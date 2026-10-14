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
    MemoryBacking *backing=calloc(1,sizeof(*backing)+count*sizeof(BackingPage *));
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
/* charge 0: memory the budget does not count (read-only program text, which Linux
   keeps in its reclaimable page cache); a write copies, and charges, a page. */
static MemoryBacking *backing_alloc_charged(size_t size,int charge) {
    MemoryBacking *backing=backing_descriptor(size);if(!backing)return NULL;
    size_t count=backing->mapped_size/backing->page_size;
    BackingPageGroup *group=backing_page_group(count,backing->page_size,charge);
    if(!group) { free(backing);return NULL; }
    for(size_t i=0;i<count;i++)backing->pages[i]=&group->pages[i];
    backing->used=count;
    mach_vm_address_t address=0;
    if(mach_vm_allocate(mach_task_self(),&address,backing->mapped_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS) {
        backing_vm_release(backing);errno=ENOMEM;return NULL;
    }
    backing->bytes=(unsigned char *)address;return backing;
}
static MemoryBacking *backing_alloc(size_t size,int clear) { (void)clear;return backing_alloc_charged(size,1); } /* Mach pages start zero */
static MemoryBacking *backing_alloc_text(size_t size) { return backing_alloc_charged(size,0); }
/* A lazily charged backing: no host page is owned (or charged) until a write
   prepares it. Unowned pages read as zero. */
static MemoryBacking *backing_reserve(size_t size) {
    MemoryBacking *backing=backing_descriptor(size);if(!backing)return NULL;
    mach_vm_address_t address=0;
    if(mach_vm_allocate(mach_task_self(),&address,backing->mapped_size,VM_FLAGS_ANYWHERE)!=KERN_SUCCESS) {
        free(backing);errno=ENOMEM;return NULL;
    }
    backing->bytes=(unsigned char *)address;return backing;
}
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
    copy->bytes=(unsigned char *)address;
    for(size_t i=0;i<count;i++) { copy->pages[i]=source->pages[i];if(copy->pages[i])__atomic_fetch_add(&copy->pages[i]->refs,1,__ATOMIC_ACQ_REL); }
    copy->used=count;
    return copy;
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
