/* Mach's copy remap creates a private snapshot without copying an arena. All
   guest stores must prepare their range before writing, including backend stores.
   Charges are conservative logical host-page ownership, not measured RSS. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
static BackingPageGroup *backing_page_group(size_t count,size_t page_size,int charge) {
    if(!count || count>(SIZE_MAX-sizeof(BackingPageGroup))/sizeof(BackingPage))return NULL;
    BackingPageGroup *group=malloc(sizeof(*group)+count*sizeof(BackingPage));
    if(!group)return NULL;
    if(count>UINT64_MAX/page_size || (charge && !resource_charge(count*page_size))) { free(group);errno=ENOMEM;return NULL; }
    group->live=group->count=count;group->uncharged=!charge;
    for(size_t i=0;i<count;i++)group->pages[i]=(BackingPage){1,group};
    return group;
}
static void backing_page_release(BackingPage *page,size_t page_size) {
    /* Pages shared after fork are released by whichever worker runs each process. */
    size_t left=__atomic_sub_fetch(&page->refs,1,__ATOMIC_ACQ_REL);
    if(left==SIZE_MAX)fail("Invalid COW page ownership");
    if(!left) {
        BackingPageGroup *group=page->group;if(!group->uncharged)resource_uncharge(page_size);
        if(!__atomic_sub_fetch(&group->live,1,__ATOMIC_ACQ_REL))free(group);
    }
}
static MemoryBacking *backing_descriptor(size_t size) {
    size_t page=(size_t)vm_page_size;
    if(!size || size>SIZE_MAX-(page-1)) { errno=ENOMEM;return NULL; }
    size_t mapped=(size+page-1)/page*page,count=mapped/page;
    if(count>(SIZE_MAX-sizeof(MemoryBacking))/sizeof(BackingPage *)) { errno=ENOMEM;return NULL; }
    MemoryBacking *backing=resource_metadata(sizeof(*backing)+count*sizeof(BackingPage *));
    if(backing) { backing->refs=1;backing->size=size;backing->mapped_size=mapped;backing->page_size=page; }
    return backing;
}
static void backing_vm_release(MemoryBacking *backing) {
    if(backing->bytes && mach_vm_deallocate(mach_task_self(),(mach_vm_address_t)backing->bytes,backing->mapped_size)!=KERN_SUCCESS)
        fail("Cannot release guest VM backing");
    for(size_t i=0;i<backing->used;i++)
        if(backing->pages[i])backing_page_release(backing->pages[i],backing->page_size);
    free(backing);
}
static void backing_free_now(MemoryBacking *backing) {
    if(backing_release_hook)backing_release_hook(backing);
    backing_vm_release(backing);
}
/* With backing_reaping set (the hypervisor backend), a freed backing's host memory
   goes back on a host thread of its own: unmapping its guest-physical space and
   deallocating its VM took most of every exit and exec, on the path of the process
   that runs next (a shell waiting for its child). Its pages are uncharged at once.
   Recycling: the memory of a dead image (exit, exec) is kept instead, zeroed, still
   mapped in guest-physical space, for the next backing of its size (a process's
   stack, heap, mmap arena and data take a new one each, and allocating, mapping,
   unmapping and freeing them cost more than the rest of a short process's life).
   Only memory every holder let go of as a dead image, where no guest code still runs
   (so no stale page table entry is used), never a snapshot or a snapshot's source
   (their host pages are copy-on-write), and only with few pages to zero. */
static int backing_reaping;
static _Thread_local int backing_dead_image;    /* address_space_destroy is dropping a dead image's memory */
static pthread_mutex_t backing_reap_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backing_reap_cond=PTHREAD_COND_INITIALIZER;
static MemoryBacking *backing_reap_queue;
#define BACKING_POOL 32u
#define BACKING_POOL_LARGE 4u                   /* of more than 256 MiB (mmap arenas): guest-physical space */
#define BACKING_POOL_PAGES 64u                  /* host pages to zero, at most */
static MemoryBacking *backing_pool[BACKING_POOL];
static unsigned backing_pool_count,backing_pool_large;
static BackingPage backing_zero_mark;           /* pages[i] of a recycled backing: owned until now, to zero */
static int backing_large(const MemoryBacking *backing) { return backing->mapped_size>(UINT64_C(256)<<20); }
static void *backing_reaper(void *unused) {
    (void)unused;
    pthread_mutex_lock(&backing_reap_lock);
    for(;;) {
        while(!backing_reap_queue)pthread_cond_wait(&backing_reap_cond,&backing_reap_lock);
        MemoryBacking *dead=backing_reap_queue;backing_reap_queue=NULL;
        pthread_mutex_unlock(&backing_reap_lock);
        while(dead) {
            MemoryBacking *next=dead->reap_next;
            if(dead->recycle) {
                for(size_t i=0;i<dead->used;i++)if(dead->pages[i]) { memset(dead->bytes+i*dead->page_size,0,dead->page_size);dead->pages[i]=NULL; }
                dead->used=0;
                pthread_mutex_lock(&backing_reap_lock);
                if(backing_pool_count<BACKING_POOL && (!backing_large(dead) || backing_pool_large<BACKING_POOL_LARGE)) {
                    backing_pool_large+=(unsigned)backing_large(dead);backing_pool[backing_pool_count++]=dead;dead=NULL;
                }
                pthread_mutex_unlock(&backing_reap_lock);
            }
            if(dead)backing_free_now(dead);            /* its pages went already: used is 0 */
            dead=next;
        }
        pthread_mutex_lock(&backing_reap_lock);
    }
    return NULL;
}
/* A freed backing (refs 0): 1 when the reaper takes it. */
static int backing_reap(MemoryBacking *backing) {
    static int started;
    if(!backing_reaping)return 0;
    size_t owned=0;
    for(size_t i=0;i<backing->used;i++)owned+=backing->pages[i]!=NULL;
    backing->recycle=backing_dead_image && !__atomic_load_n(&backing->unpooled,__ATOMIC_RELAXED) && !backing->frozen && !backing->cow_copy && backing->bytes && owned<=BACKING_POOL_PAGES;
    for(size_t i=0;i<backing->used;i++)if(backing->pages[i]) {
        backing_page_release(backing->pages[i],backing->page_size);backing->pages[i]=backing->recycle?&backing_zero_mark:NULL;
    }
    if(!backing->recycle)backing->used=0;
    pthread_mutex_lock(&backing_reap_lock);
    if(!started) {
        pthread_t thread;
        if(pthread_create(&thread,NULL,backing_reaper,NULL) || pthread_detach(thread))fail("Cannot start the memory reaper");
        started=1;
    }
    if(!backing_reap_queue)pthread_cond_signal(&backing_reap_cond); /* the reaper takes the whole queue */
    backing->reap_next=backing_reap_queue;backing_reap_queue=backing;
    pthread_mutex_unlock(&backing_reap_lock);
    return 1;
}
/* Recycled memory of size's host-page size, or NULL. */
static MemoryBacking *backing_pool_take(size_t size) {
    size_t page=(size_t)vm_page_size,mapped=(size+page-1)/page*page;MemoryBacking *backing=NULL;
    if(!backing_reaping || !size || size>SIZE_MAX-(page-1))return NULL;
    pthread_mutex_lock(&backing_reap_lock);
    for(unsigned i=backing_pool_count;i--;)if(backing_pool[i]->mapped_size==mapped) {
        backing=backing_pool[i];backing_pool[i]=backing_pool[--backing_pool_count];backing_pool_large-=(unsigned)backing_large(backing);break;
    }
    pthread_mutex_unlock(&backing_reap_lock);
    if(backing) { backing->refs=1;backing->size=size;backing->frozen=backing->cow_copy=backing->unpooled=backing->recycle=0; }
    return backing;
}
/* A backing of size with host memory and no owned page: recycled, or new. */
static MemoryBacking *backing_region(size_t size) {
    MemoryBacking *backing=backing_pool_take(size);if(backing)return backing;
    if(!(backing=backing_descriptor(size)))return NULL;
    mach_vm_address_t address=0;
    if(mach_vm_allocate(mach_task_self(),&address,backing->mapped_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS) { free(backing);errno=ENOMEM;return NULL; }
    backing->bytes=(unsigned char *)address;return backing;
}
/* charge 0: memory the budget does not count (read-only program text, which Linux
   keeps in its reclaimable page cache); a write copies, and charges, a page. */
static MemoryBacking *backing_alloc_charged(size_t size,int charge) {
    MemoryBacking *backing=backing_region(size);if(!backing)return NULL;
    size_t count=backing->mapped_size/backing->page_size;
    BackingPageGroup *group=backing_page_group(count,backing->page_size,charge);
    if(!group) { backing_free_now(backing);return NULL; }
    for(size_t i=0;i<count;i++)backing->pages[i]=&group->pages[i];
    backing->used=count;return backing;
}
static MemoryBacking *backing_alloc(size_t size,int clear) { (void)clear;return backing_alloc_charged(size,1); } /* Mach pages start zero */
static MemoryBacking *backing_alloc_text(size_t size) { return backing_alloc_charged(size,0); }
/* A lazily charged backing: no host page is owned (or charged) until a write
   prepares it. Unowned pages read as zero. */
static MemoryBacking *backing_reserve(size_t size) { return backing_region(size); }
/* Zero whole host pages and give them back: ownership and charge are released and
   the host may reclaim the memory. The caller owns the backing privately. */
static void backing_discard(MemoryBacking *backing,size_t offset,size_t size) {
    for(size_t i=offset/backing->page_size;i<(offset+size)/backing->page_size;i++) {
        if(!backing->pages[i])continue;
        unsigned char *bytes=backing->bytes+i*backing->page_size;
        memset(bytes,0,backing->page_size);(void)madvise(bytes,backing->page_size,MADV_FREE_REUSABLE);
        backing_page_release(backing->pages[i],backing->page_size);backing->pages[i]=NULL;
    }
}
static MemoryBacking *backing_snapshot(MemoryBacking *source) {
    MemoryBacking *copy=backing_descriptor(source->size);if(!copy)return NULL;
    size_t count=source->used;
    for(size_t i=0;i<count;i++)if(source->pages[i] && source->pages[i]->refs==SIZE_MAX) { free(copy);errno=ENOMEM;return NULL; }
    mach_vm_address_t address=0;vm_prot_t current=0,maximum=0;
    source->frozen=1;
    kern_return_t result=mach_vm_remap(mach_task_self(),&address,source->mapped_size,0,
        VM_FLAGS_ANYWHERE,mach_task_self(),(mach_vm_address_t)source->bytes,TRUE,&current,&maximum,VM_INHERIT_NONE);
    if(result!=KERN_SUCCESS) { free(copy);errno=ENOMEM;return NULL; }
    copy->bytes=(unsigned char *)address;copy->cow_copy=1;
    for(size_t i=0;i<count;i++) { copy->pages[i]=source->pages[i];if(copy->pages[i])__atomic_fetch_add(&copy->pages[i]->refs,1,__ATOMIC_ACQ_REL); }
    copy->used=count;
    return copy;
}
/* A private copy of source's owned pages, charged now (the fork of a small process:
   cheaper than two snapshots, see address_space_fork_private). NULL on failure. */
static MemoryBacking *backing_clone(const MemoryBacking *source,size_t owned) {
    MemoryBacking *copy=backing_region(source->size);if(!copy)return NULL;
    BackingPageGroup *group=owned?backing_page_group(owned,source->page_size,1):NULL;
    if(owned && !group) { backing_free_now(copy);return NULL; }
    for(size_t i=0,next=0;i<source->used;i++)if(source->pages[i]) {
        copy->pages[i]=&group->pages[next++];copy->used=i+1;
        memcpy(copy->bytes+i*copy->page_size,source->bytes+i*copy->page_size,copy->page_size);
    }
    return copy;
}
/* Read-only program text mapped from its file (offset a host page multiple) instead
   of copied: pages come in from the host's page cache as the guest uses them. A
   first exec of a 100 MB program (node) from a fresh container root otherwise read
   and copied all of it. Owned and uncharged, as backing_alloc_text; copy-on-write
   with the file (MAP_PRIVATE), so never recycled. NULL when it cannot be mapped. */
static MemoryBacking *backing_map_file(int fd,off_t offset,size_t size) {
    MemoryBacking *backing=backing_descriptor(size);if(!backing)return NULL;
    size_t count=backing->mapped_size/backing->page_size;
    BackingPageGroup *group=backing_page_group(count,backing->page_size,0);
    if(!group) { free(backing);return NULL; }
    void *bytes=mmap(NULL,backing->mapped_size,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,offset);
    if(bytes==MAP_FAILED) { free(group);free(backing);return NULL; }
    for(size_t i=0;i<count;i++)backing->pages[i]=&group->pages[i];
    backing->used=count;backing->bytes=bytes;backing->cow_copy=1;return backing;
}
/* Stage the entire charge before changing metadata. A failed multi-page range
   leaves pointers, ownership and all guest bytes untouched. */
static int backing_prepare(MemoryBacking *backing,size_t offset,size_t size,int *changed) {
    if(offset>backing->size || size>backing->size-offset) { errno=EINVAL;return 0; }
    if(!size)return 1;
    size_t first=offset/backing->page_size,last=(offset+size-1)/backing->page_size,count=0;
    for(size_t i=first;i<=last;i++)count+=!backing->pages[i] || backing->pages[i]->refs>1; /* unowned or shared */
    if(!count)return 1;
    BackingPageGroup *group=backing_page_group(count,backing->page_size,1);if(!group)return 0;
    size_t next=0;
    for(size_t i=first;i<=last;i++)if(!backing->pages[i] || backing->pages[i]->refs>1) {
        if(backing->pages[i])backing_page_release(backing->pages[i],backing->page_size);
        backing->pages[i]=&group->pages[next++];
    }
    if(last+1>backing->used)backing->used=last+1;
    *changed=1;return 1;
}
