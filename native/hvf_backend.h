/* Experimental native execution (milestone 1): Linux ARM64 EL0 code runs directly
   on the host CPU under Hypervisor.framework. There is no guest kernel. Guest
   memory is the interpreter's own address space: each MemoryBacking is mapped
   once into guest-physical space (stage 2), and 4 KiB stage-1 tables built here
   give the guest its Linux virtual layout. Every guest exception lands on an
   EL1-only vector page whose routine saves the guest's registers to this
   vCPU's frame and executes HVC, so SVC and faults exit to this process, which
   runs the shared Linux syscall layer and resumes the guest at EL0 through the
   frame (hvf_put_regs). Hardware stage-2 translation keeps guest code out of host memory.
   Guest processes share the one VM: every process keeps its own stage-1 tables
   (tagged with an ASID of its own, see hvf_asid) and every backing stays mapped in stage 2
   until it is freed, so a process switch only loads TTBR0; tables are rebuilt
   when a process's own layout changes. hvf_slice() is the scheduler's CPU slice;
   a watchdog preempts CPU-bound tasks. Threads (CLONE_THREAD) of a process share
   its tables and run on several vCPUs at once. */
#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>
#define HVF_IPA_BASE UINT64_C(0x80000000)
#define HVF_TABLE_BYTES (64u<<20)                   /* stage-1 tables of every process: 16384 x 4 KiB */
#define HVF_TABLES (HVF_TABLE_BYTES/4096)
#define HVF_CODE_BYTES (16u<<10)                    /* one host page: vectors and EL1 routines */
#define HVF_FRAME_BYTES 512u                        /* each vCPU's register frame (WORKER_LIMIT of them) */
#define HVF_SYSTEM_BYTES (HVF_CODE_BYTES+WORKER_LIMIT*HVF_FRAME_BYTES) /* code (read, execute), then frames (read, write) */
#define HVF_SYSTEM_VA UINT64_C(0xffffffff0000)      /* top of the 48-bit user range; EL1-only */
#define HVF_FRAME_VA (HVF_SYSTEM_VA+HVF_CODE_BYTES)
#define HVF_FLUSH_OFFSET 0x800u
#define HVF_SAVE_OFFSET 0x900u
#define HVF_RESTORE_OFFSET 0x968u
#define HVF_INVALIDATE_OFFSET 0x9c4u
#define HVF_FLUSH_RESTORE_OFFSET 0xa00u
/* Frame words (SP_EL1 points at the vCPU's frame): x0..x30, then these. HF_FAST:
   the EL1 vector's own answers to syscalls 172..178 (hvf_fast_publish). */
enum { HF_SP=31,HF_TLS,HF_ELR,HF_SPSR,HF_ESR,HF_FAR,HF_TLBI_VA,HF_TLBI_PAGES,HF_FLUSH,HF_FAST };
#define HVF_BLOCKS 16384                           /* stage-2 blocks: a few backings for each of up to 1024 processes */
#define HVF_VIEWS (PROCESS_LIMIT+2)                 /* indexed by pid */
static unsigned char *hvf_tables,*hvf_system;static unsigned hvf_frames_used;
static unsigned short hvf_table_owner[HVF_TABLES];  /* 0 free, else the pid whose tables use it */
static unsigned hvf_table_hint;
typedef struct { const unsigned char *host;size_t size;uint64_t ipa; } HvfBlock;
static HvfBlock hvf_blocks[HVF_BLOCKS];
static unsigned hvf_nblocks,hvf_block_last;
static uint64_t hvf_ipa_top;                        /* above every block: new ones go here while they fit */
static uint64_t hvf_ipa_limit=UINT64_C(1)<<36;
static _Thread_local int hvf_exhausted;      /* a build ran out of table pages or guest-physical space */
/* The table pool and the stage-2 blocks are shared by every worker; each process's
   own tables are only ever changed by the worker running it. Taken inside
   runtime_mutex, never the other way round. */
static pthread_mutex_t hvf_mutex=PTHREAD_MUTEX_INITIALIZER;
/* An update replaced or removed a valid descriptor, or mapped code: the TLBs (and
   instruction caches) need maintenance. New mappings of unmapped addresses do
   not, as invalid entries are never cached. */
static _Thread_local int hvf_stale_tlb;
static _Thread_local int hvf_stale_code; /* and executable pages were mapped: instruction caches too */
static _Thread_local uint64_t hvf_pending_va;static _Thread_local unsigned hvf_pending_pages; /* to invalidate as the guest resumes */
static _Thread_local int hvf_resume_el1;     /* hvf_unlocked_exit left the vCPU in an EL1 routine (hvf_fault_resume) */
/* A process's stage-1 tables and the mapping-relevant layout they were built from.
   The scheduler restores a blocked process's image on every resume, which bumps
   the generation without changing anything; comparing the layout (and its
   revision, for the mmap arena) avoids rebuilding the tables then. Page sharing
   (copy-on-write) can change without the layout changing, so stale marks the
   view out of date after the process's own fork and after a copy-on-write fault
   that replaced a backing. Other processes can only make pages more private, and
   a read-only view of a private page merely faults once more.
   HVF_STALE_COW: update in place (hvf_sync_partial). HVF_STALE_NEW: the tables may
   belong to an earlier process or image in this slot, or ran out: rebuild them. */
#define HVF_STALE_COW 1
#define HVF_STALE_NEW 2
typedef struct {
    uint64_t root;unsigned tables;int stale;  /* root 0: no tables */
    unsigned table_low,table_high;            /* its tables lie in hvf_table_owner[table_low..table_high] */
    Segment seg[SEGMENT_LIMIT+1];unsigned nseg;uint64_t map_capacity;
    MemoryBacking *brk_backing,*map_backing;uint64_t brk_base,brk_pages;
    uint64_t layout,arena_rev,generation;
    unsigned asid,asid_generation;            /* valid while asid_generation is hvf_asid_generation */
} HvfView;
static HvfView hvf_views[HVF_VIEWS];
static int hvf_active;                       /* run() executes natively */
/* Each worker's vCPU (Worker.backend), created on and used only by that worker's
   thread. view: the tables its TTBR0 holds (ttbr: their root, which a rebuild
   elsewhere may have replaced). frame: its register frame (host address); framed:
   the guest's registers are there (saved by the vector, or put to resume through it),
   not in the vCPU's own registers (a preemption at EL0). */
typedef struct {
    hv_vcpu_t vcpu;hv_vcpu_exit_t *exit;HvfView *view;uint64_t ttbr;unsigned asid_flushed; /* the ASID generation its TLBs were last flushed in */
    uint64_t *frame;int framed,fp_in_cpu,fp_dirty;
    int preempt;                             /* the watchdog's slice ended: yield at the next safe point (atomic) */
    unsigned tso;                            /* ACTLR_EL1.EnTSO as last set: the running thread's mem_model */
} HvfCpu;
#define hvf_cpu ((HvfCpu *)worker->backend)
#define hvf_vcpu (hvf_cpu->vcpu)
#define hvf_exit (hvf_cpu->exit)
#define hvf_view (hvf_cpu->view)
#define hvf_frame (hvf_cpu->frame)
#define hvf_fp_in_cpu (hvf_cpu->fp_in_cpu)
#define hvf_fp_dirty (hvf_cpu->fp_dirty)
static void hvf_check(hv_return_t result,const char *what) {
    if(result!=HV_SUCCESS) { fprintf(stderr,"dockiva: %s failed (0x%x)\n",what,(unsigned)result);fail("Hypervisor.framework call failed"); }
}
static void *hvf_host_pages(size_t size) {
    void *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(p==MAP_FAILED)fail("Cannot allocate hypervisor memory");
    return p;
}
/* Stage 2: give a backing guest-physical space (bump, then first fit), mapping it on
   first use; it stays mapped until the backing is freed. A backing larger than
   HVF_CHUNK takes it a chunk at a time, as its pages are used: a 12 GiB mmap arena
   reserved whole by every process would fill the 1 TiB space at about 85 processes.
   0 when out of space. */
#define HVF_CHUNK (UINT64_C(256)<<20)
static uint64_t hvf_chunk_ipa_locked(MemoryBacking *backing,size_t chunk) {
    const unsigned char *host=backing_data(backing)+chunk*HVF_CHUNK;
    if(hvf_block_last<hvf_nblocks && hvf_blocks[hvf_block_last].host==host)return hvf_blocks[hvf_block_last].ipa;
    for(unsigned i=0;i<hvf_nblocks;i++)if(hvf_blocks[i].host==host) { hvf_block_last=i;return hvf_blocks[i].ipa; }
    uint64_t size=backing->mapped_size<=HVF_CHUNK?backing->mapped_size:backing->mapped_size-chunk*HVF_CHUNK;
    if(size>HVF_CHUNK && backing->mapped_size>HVF_CHUNK)size=HVF_CHUNK;
    uint64_t ipa=hvf_ipa_top?hvf_ipa_top:HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES;
    if(ipa+size>hvf_ipa_limit) {                                            /* then first fit */
        ipa=HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES;
        for(unsigned i=0;i<hvf_nblocks;i++)
            if(ipa<hvf_blocks[i].ipa+hvf_blocks[i].size && hvf_blocks[i].ipa<ipa+size) { ipa=hvf_blocks[i].ipa+hvf_blocks[i].size;i=UINT_MAX; }
    }
    if(hvf_nblocks==HVF_BLOCKS || ipa+size>hvf_ipa_limit) { hvf_exhausted=1;return 0; }
    hvf_check(hv_vm_map((void *)host,ipa,size,HV_MEMORY_READ|HV_MEMORY_WRITE|HV_MEMORY_EXEC),"hv_vm_map");
    hvf_block_last=hvf_nblocks;hvf_blocks[hvf_nblocks++]=(HvfBlock){host,size,ipa};
    if(ipa+size>hvf_ipa_top)hvf_ipa_top=ipa+size;
    return ipa;
}
/* Each backing remembers its addresses, so building tables (every page of a fork)
   needs no lock: hvf_ipa_epoch moves whenever a block is unmapped, which only happens
   for a freed backing or with a single thread left. */
static uint64_t hvf_ipa_epoch=1;
/* The guest-physical address of byte offset of backing, or 0. */
static uint64_t hvf_ipa_at(MemoryBacking *backing,size_t offset) {
    uint64_t epoch=__atomic_load_n(&hvf_ipa_epoch,__ATOMIC_ACQUIRE);
    size_t chunk=backing->mapped_size<=HVF_CHUNK?0:offset/HVF_CHUNK;
    uint64_t *cache=backing->mapped_size>HVF_CHUNK?__atomic_load_n(&backing->hvf_chunks,__ATOMIC_ACQUIRE):NULL,*slot=cache?cache+2*chunk:&backing->hvf_ipa;
    if(backing->mapped_size<=HVF_CHUNK || cache) {
        if(__atomic_load_n(&slot[1],__ATOMIC_ACQUIRE)==epoch)return slot[0]+(offset-chunk*HVF_CHUNK);
    }
    pthread_mutex_lock(&hvf_mutex);
    if(backing->mapped_size>HVF_CHUNK && !backing->hvf_chunks) {          /* (address, epoch) per chunk */
        uint64_t *table=calloc(2*((backing->mapped_size+HVF_CHUNK-1)/HVF_CHUNK),sizeof(uint64_t));
        if(!table) { pthread_mutex_unlock(&hvf_mutex);hvf_exhausted=1;return 0; }
        __atomic_store_n(&backing->hvf_chunks,table,__ATOMIC_RELEASE);
    }
    if(backing->mapped_size>HVF_CHUNK)slot=backing->hvf_chunks+2*(offset/HVF_CHUNK);
    uint64_t ipa=hvf_chunk_ipa_locked(backing,chunk);
    if(ipa) { slot[0]=ipa;__atomic_store_n(&slot[1],hvf_ipa_epoch,__ATOMIC_RELEASE); }
    pthread_mutex_unlock(&hvf_mutex);
    return ipa?ipa+(offset-chunk*HVF_CHUNK):0;
}
static void hvf_block_unmap(unsigned i) {
    hvf_check(hv_vm_unmap(hvf_blocks[i].ipa,hvf_blocks[i].size),"hv_vm_unmap");
    hvf_blocks[i]=hvf_blocks[--hvf_nblocks];if(!hvf_nblocks)hvf_ipa_top=0;
    __atomic_fetch_add(&hvf_ipa_epoch,1,__ATOMIC_RELEASE);
}
/* backing_release_hook: no live process maps a freed backing, and the tables of
   a process that used to are rebuilt (its layout changed) before it runs again.
   Its blocks are unmapped without hvf_mutex (every table build takes it): meanwhile
   they have no host (no lookup finds them) but keep their guest-physical space. */
static void hvf_backing_gone(MemoryBacking *backing) {
    const unsigned char *host=backing_data(backing);
    for(;;) {
        uint64_t ipa[16],size[16];unsigned n=0;
        pthread_mutex_lock(&hvf_mutex);
        for(unsigned i=0;i<hvf_nblocks && n<16;i++)if(hvf_blocks[i].host>=host && hvf_blocks[i].host<host+backing->mapped_size) {
            hvf_blocks[i].host=NULL;ipa[n]=hvf_blocks[i].ipa;size[n++]=hvf_blocks[i].size;
        }
        pthread_mutex_unlock(&hvf_mutex);
        if(!n)break;
        for(unsigned k=0;k<n;k++)hvf_check(hv_vm_unmap(ipa[k],size[k]),"hv_vm_unmap");
        pthread_mutex_lock(&hvf_mutex);
        for(unsigned k=0;k<n;k++)for(unsigned i=0;i<hvf_nblocks;i++)if(!hvf_blocks[i].host && hvf_blocks[i].ipa==ipa[k]) {
            hvf_blocks[i]=hvf_blocks[--hvf_nblocks];if(!hvf_nblocks)hvf_ipa_top=0;
            break;
        }
        __atomic_fetch_add(&hvf_ipa_epoch,1,__ATOMIC_RELEASE);
        pthread_mutex_unlock(&hvf_mutex);
    }
    free(backing->hvf_chunks);backing->hvf_chunks=NULL;
}
/* The last leaf table a thread found (consecutive pages share it); hvf_table_epoch
   changes whenever table pages are freed, which invalidates every cached one. */
static uint64_t hvf_table_epoch;
static _Thread_local struct { uint64_t root,key,epoch;uint64_t *leaf; } hvf_walk;
static uint64_t hvf_table(void) {
    unsigned owner=(unsigned)(hvf_view-hvf_views);
    pthread_mutex_lock(&hvf_mutex);
    for(unsigned n=0;n<HVF_TABLES;n++) {
        unsigned i=(hvf_table_hint+n)%HVF_TABLES;
        if(hvf_table_owner[i])continue;
        hvf_table_owner[i]=(unsigned short)owner;hvf_table_hint=i+1;
        if(!hvf_view->tables || i<hvf_view->table_low)hvf_view->table_low=i;
        if(!hvf_view->tables || i>hvf_view->table_high)hvf_view->table_high=i;
        pthread_mutex_unlock(&hvf_mutex);
        hvf_view->tables++;memset(hvf_tables+(size_t)i*4096,0,4096);return HVF_IPA_BASE+(uint64_t)i*4096;
    }
    pthread_mutex_unlock(&hvf_mutex);
    hvf_exhausted=1;return 0;
}
static void hvf_tables_free(HvfView *v) {
    unsigned owner=(unsigned)(v-hvf_views);
    __atomic_add_fetch(&hvf_table_epoch,1,__ATOMIC_ACQ_REL); /* cached leaf tables may be reused */
    pthread_mutex_lock(&hvf_mutex);
    if(v->tables)for(unsigned i=v->table_low;i<=v->table_high;i++)if(hvf_table_owner[i]==owner)hvf_table_owner[i]=0;
    pthread_mutex_unlock(&hvf_mutex);
    v->tables=0;v->root=0;
}
/* Stage 1 of the current view, 4 KiB granule, four levels. Normal write-back
   memory, inner shareable. flags are ELF-style: 4 read, 2 write, 1 execute. Guest
   pages are non-global (tagged with the view's ASID); EL1 never executes them. */
static uint64_t *hvf_leaf(HvfView *v,uint64_t va,int create) {
    uint64_t epoch=__atomic_load_n(&hvf_table_epoch,__ATOMIC_ACQUIRE);
    if(hvf_walk.leaf && hvf_walk.root==v->root && hvf_walk.key==va>>21 && hvf_walk.epoch==epoch)return hvf_walk.leaf;
    uint64_t table=v->root;
    for(int level=0;level<3;level++) {
        uint64_t *entry=&((uint64_t *)(hvf_tables+(table-HVF_IPA_BASE)))[(va>>(39-9*level))&511];
        if(!(*entry&1)) {
            if(!create)return NULL;
            uint64_t next=hvf_table();if(!next)return NULL;*entry=next|3;
        }
        table=*entry&UINT64_C(0x0000fffffffff000);
    }
    hvf_walk.root=v->root;hvf_walk.key=va>>21;hvf_walk.epoch=epoch;hvf_walk.leaf=(uint64_t *)(hvf_tables+(table-HVF_IPA_BASE));
    return hvf_walk.leaf;
}
/* A page descriptor's bits besides the address. */
static uint64_t hvf_descriptor_bits(unsigned flags,int system) {
    uint64_t descriptor=3|(UINT64_C(3)<<8)|(UINT64_C(1)<<10);
    if(system)return descriptor|UINT64_C(1)<<54;                            /* EL1 only, never EL0-executable */
    descriptor|=(UINT64_C(1)<<6)|(UINT64_C(1)<<11)|(UINT64_C(1)<<53);       /* EL0 access; nG; PXN */
    if(!(flags&2))descriptor|=UINT64_C(1)<<7;                               /* read-only */
    if(!(flags&1))descriptor|=UINT64_C(1)<<54;                              /* UXN */
    return descriptor;
}
static void hvf_map_page_in(HvfView *v,uint64_t va,uint64_t ipa,unsigned flags,int system) {
    uint64_t *leaf=hvf_leaf(v,va,1);if(!leaf)return;
    uint64_t descriptor=ipa|hvf_descriptor_bits(flags,system);
    uint64_t *slot=&leaf[(va>>12)&511];
    if(((*slot&1) && *slot!=descriptor) || (flags&1))hvf_stale_tlb=1;
    if(flags&1)hvf_stale_code=1;
    *slot=descriptor;
}
static void hvf_map_page(uint64_t va,uint64_t ipa,unsigned flags,int system) { hvf_map_page_in(hvf_view,va,ipa,flags,system); }
/* Copy-on-write pages are mapped read-only while shared, and lazily charged pages
   while unowned; a write fault detaches (or charges) them. */
static unsigned hvf_effective_flags(const GuestAddressSpace *space,MemoryBacking *backing,unsigned flags,size_t offset) {
    if(flags&SEG_SHARED)return flags&7;                                        /* shared memory is never copy-on-write */
    if(!(flags&2) || !backing)return flags;
    if(backing->frozen)return flags&~2u;                                       /* writes go to a fresh copy */
    if(backing->refs>1 && backing->refs>address_space_backing_refs(space,backing))return flags&~2u;
    const BackingPage *page=backing->pages[offset/backing->page_size];
    if(!page || page->refs>1)return flags&~2u;
    return flags;
}
/* hvf_effective_flags for every page, a leaf table (2 MiB) at a time: a build maps
   every page of every segment (a stack alone is 2048), so the per-page work is a
   store. Pages differ only by ownership, where the backing is private and writable. */
static void hvf_map_range(const GuestAddressSpace *space,uint64_t base,uint64_t size,unsigned flags,MemoryBacking *backing,size_t offset) {
    if(!size || !flags || !backing)return;
    if((base|size|offset)&4095)fail("Hypervisor backend needs page-aligned guest mappings (use --dynamic)");
    HvfView *v=hvf_view;uint64_t ipa=0;
    int per_page=0;unsigned uniform=flags;                   /* as hvf_effective_flags decides */
    if(flags&SEG_SHARED)uniform=flags&7;
    else if(flags&2) {
        if(backing->frozen || (backing->refs>1 && backing->refs>address_space_backing_refs(space,backing)))uniform=flags&~2u;
        else per_page=1;
    }
    uint64_t bits[2]={hvf_descriptor_bits(flags&~2u,0),hvf_descriptor_bits(flags,0)},single=hvf_descriptor_bits(uniform,0);
    int stale=0;
    for(uint64_t p=0;p<size;) {
        if(!ipa || !((offset+p)%HVF_CHUNK)) { if(!(ipa=hvf_ipa_at(backing,offset+(size_t)p)))break;ipa-=p; } /* each chunk's address */
        uint64_t *leaf=hvf_leaf(v,base+p,1);if(!leaf)break;
        uint64_t end=p+4096*(512-((base+p)>>12&511)),chunk=p+(HVF_CHUNK-(offset+p)%HVF_CHUNK);
        if(end>chunk)end=chunk;
        if(end>size)end=size;
        for(;p<end;p+=4096) {
            uint64_t descriptor=ipa+p;
            if(per_page) {
                const BackingPage *page=backing->pages[(offset+p)/backing->page_size];
                descriptor|=bits[page && page->refs==1];
            } else descriptor|=single;
            uint64_t *slot=&leaf[((base+p)>>12)&511];
            stale|=(*slot&1) && *slot!=descriptor;
            *slot=descriptor;
        }
    }
    if(stale || (flags&1))hvf_stale_tlb=1;
    if(flags&1)hvf_stale_code=1;
}
static void hvf_flush(void);
static void hvf_resume_el0(uint64_t pc,uint64_t spsr);
static void hvf_siblings_out(void);
static int hvf_view_matches(const GuestAddressSpace *space) {
    const HvfView *v=&hvf_views[process_pid];
    if(!v->root || v->stale || v->nseg!=space->nseg || v->map_capacity!=space->map_capacity ||
       v->brk_backing!=space->brk_backing || v->map_backing!=space->map_backing ||
       v->brk_base!=space->brk_base || v->brk_pages!=space->brk_pages)return 0;
    for(unsigned i=0;i<space->nseg;i++) {
        const Segment *a=&v->seg[i],*b=&space->seg[i];
        if(a->base!=b->base || a->size!=b->size || a->flags!=b->flags || a->backing!=b->backing || a->offset!=b->offset)return 0;
    }
    return v->layout==space->layout;
}
static void hvf_view_record(HvfView *v,const GuestAddressSpace *space) {
    v->nseg=space->nseg;v->map_capacity=space->map_capacity;
    v->brk_backing=space->brk_backing;v->map_backing=space->map_backing;
    v->brk_base=space->brk_base;v->brk_pages=space->brk_pages;
    memcpy(v->seg,space->seg,space->nseg*sizeof(Segment));
    v->layout=space->layout;v->generation=space->generation;v->stale=0;
}
/* Returns how many pages from va on are now unmapped: 1, or up to the next 2 MiB
   boundary when no leaf table covers va (Go reserves and frees arenas of PROT_NONE
   pages that were never mapped; walking each of them from the root dominated syncs). */
static unsigned hvf_unmap_page(uint64_t va) {
    uint64_t *leaf=hvf_leaf(hvf_view,va,0);if(!leaf)return 512-(unsigned)((va>>12)&511);
    uint64_t *slot=&leaf[(va>>12)&511];
    if(*slot&1)hvf_stale_tlb=1;
    *slot=0;return 1;
}
/* The view now reflects every mmap-arena change: start a fresh dirty range. */
static void hvf_arena_synced(GuestAddressSpace *space) {
    hvf_view->arena_rev=space->arena_rev;
    space->arena_dirty_lo=space->arena_dirty_hi=0;space->arena_dirty_since=space->arena_rev;
}
static void hvf_map_arena_page(GuestAddressSpace *space,unsigned i) {
    unsigned flags=space->map_flags[i]&7;uint64_t ipa=flags?hvf_ipa_at(space->map_backing,(size_t)i*4096):0;
    if(ipa)hvf_map_page(MAP_BASE+(uint64_t)i*4096,ipa,hvf_effective_flags(space,space->map_backing,flags,(size_t)i*4096),0);
    else hvf_unmap_page(MAP_BASE+(uint64_t)i*4096);                       /* PROT_NONE stays unmapped */
}
/* Apply mmap/munmap/mprotect/madvise changes to just the arena pages they touched.
   Pages to unmap are cleared a leaf table (2 MiB) at a time, eight flag bytes at a
   time where none of them is mapped or lent to a shared segment: Go reserves and
   frees gigabytes of PROT_NONE pages, and a dirty range spans every change since the
   last sync (walking it page by page dominated the start of every Go process). */
static void hvf_remap_arena(GuestAddressSpace *space) {
    int ipa=space->map_backing!=NULL;                                     /* arena pages can be mapped */
    const unsigned char *flags=space->map_flags;
    const uint64_t kept=ipa?UINT64_C(0x2727272727272727):UINT64_C(0x2020202020202020); /* mapped or reserved: not cleared */
    for(unsigned i=space->arena_dirty_lo,hi=space->arena_dirty_hi;i<hi;) {
        unsigned char f=flags?flags[i]:0;
        if(f&MAP_RESERVED) { i++;continue; }                                    /* a shared segment's page */
        if(ipa && (f&7)) { hvf_map_arena_page(space,i);i++;continue; }
        uint64_t va=MAP_BASE+(uint64_t)i*4096;
        unsigned end=i+512-(unsigned)((va>>12)&511);if(end>hi)end=hi;          /* this leaf table's last page */
        uint64_t *leaf=hvf_leaf(hvf_view,va,0);
        while(i<end) {
            if(!(i&7) && i+8<=end) {
                uint64_t w=0;if(flags)memcpy(&w,flags+i,8);
                if(!(w&kept)) {
                    if(leaf)for(unsigned k=0;k<8;k++) { uint64_t *slot=&leaf[((MAP_BASE>>12)+i+k)&511];if(*slot&1)hvf_stale_tlb=1;*slot=0; }
                    i+=8;continue;
                }
            }
            f=flags?flags[i]:0;
            if(ipa && (f&7))break;                                              /* mapped: the outer loop maps it */
            if(leaf && !(f&MAP_RESERVED)) { uint64_t *slot=&leaf[((MAP_BASE>>12)+i)&511];if(*slot&1)hvf_stale_tlb=1;*slot=0; }
            i++;
        }
    }
}
/* ASIDs. Apple's vCPUs have 8-bit ones (ID_AA64MMFR0_EL1.ASIDBits 0), fewer than the
   processes, so they are handed out as Linux's allocator does: a view takes a fresh
   one when it is next loaded in a new generation; when all 255 are taken a new
   generation begins, keeping those of the views loaded on a vCPU, and every vCPU
   flushes its TLBs once before it loads a view in it (hvf_asid_flush). No two
   views ever hold one ASID at a time, and an ASID is reused only after a flush. */
static unsigned hvf_asid_generation=1,hvf_asid_next=1;
static unsigned char hvf_asid_taken[256];
static _Thread_local int hvf_asid_flush;
static unsigned hvf_asid(HvfView *v) {
    pthread_mutex_lock(&hvf_mutex);
    if(v->asid_generation!=hvf_asid_generation) {
        while(hvf_asid_next<256 && hvf_asid_taken[hvf_asid_next])hvf_asid_next++;
        if(hvf_asid_next==256) {                           /* a new generation */
            hvf_asid_generation++;hvf_asid_next=1;memset(hvf_asid_taken,0,sizeof(hvf_asid_taken));
            for(unsigned w=0;w<worker_count;w++) {
                HvfCpu *c=workers[w]?workers[w]->backend:NULL;HvfView *u=c?c->view:NULL;
                if(u && u!=v && u->asid_generation==hvf_asid_generation-1) { u->asid_generation=hvf_asid_generation;hvf_asid_taken[u->asid]=1; }
            }
            while(hvf_asid_taken[hvf_asid_next])hvf_asid_next++;
        }
        v->asid=hvf_asid_next;v->asid_generation=hvf_asid_generation;hvf_asid_taken[hvf_asid_next++]=1;
    }
    if(hvf_cpu->asid_flushed!=hvf_asid_generation) { hvf_cpu->asid_flushed=hvf_asid_generation;hvf_asid_flush=1; }
    unsigned asid=v->asid;
    pthread_mutex_unlock(&hvf_mutex);
    return asid;
}
static void hvf_activate(HvfView *v) {
    hvf_view=v;hvf_cpu->ttbr=v->root;
    hvf_check(hv_vcpu_set_sys_reg(hvf_vcpu,HV_SYS_REG_TTBR0_EL1,v->root|((uint64_t)hvf_asid(v)<<48)),"set ttbr0");
}
/* Give every recycled backing back (the hypervisor ran out of guest-physical space). */
static void backing_pool_drain(void) {
    for(;;) {
        pthread_mutex_lock(&backing_reap_lock);
        MemoryBacking *backing=backing_pool_count?backing_pool[--backing_pool_count]:NULL;
        if(backing)backing_pool_large-=(unsigned)backing_large(backing);
        pthread_mutex_unlock(&backing_reap_lock);
        if(!backing)return;
        backing_free_now(backing);
    }
}
/* Other processes' tables go (each is rebuilt on its next run), except those another
   worker's vCPU may be using; with one worker every backing also leaves stage 2. */
static void hvf_evict(void) {
    backing_pool_drain();                                /* recycled memory holds guest-physical space */
    for(unsigned i=0;i<HVF_VIEWS && i<=process_count+1;i++) {
        int in_use=0;
        for(unsigned w=0;w<worker_count;w++)if(workers[w]!=worker && workers[w]->task+1==(int)i)in_use=1;
        if(!in_use)hvf_tables_free(&hvf_views[i]);
    }
    if(!worker_threads) { pthread_mutex_lock(&hvf_mutex);for(unsigned i=hvf_nblocks;i--;)if(hvf_blocks[i].host)hvf_block_unmap(i);pthread_mutex_unlock(&hvf_mutex); } /* (no host: being unmapped) */
}
static void hvf_map_arena_all(GuestAddressSpace *space);
static void hvf_build(GuestAddressSpace *space) {
    hvf_exhausted=0;hvf_tables_free(hvf_view);
    if(!(hvf_view->root=hvf_table()))return;
    hvf_map_page(HVF_SYSTEM_VA,HVF_IPA_BASE+HVF_TABLE_BYTES,0,1);
    for(unsigned i=0;i<WORKER_LIMIT*HVF_FRAME_BYTES/4096;i++)hvf_map_page(HVF_FRAME_VA+i*4096,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_CODE_BYTES+i*4096,0,1);
    for(unsigned i=0;i<space->nseg;i++)hvf_map_range(space,space->seg[i].base,space->seg[i].size,space->seg[i].flags,space->seg[i].backing,space->seg[i].offset);
    hvf_map_range(space,space->brk_base,space->brk_pages,6,space->brk_backing,0);
    hvf_map_arena_all(space);
}
static int hvf_same_segment(const Segment *a,const Segment *b) {
    return a->base==b->base && a->size==b->size && a->flags==b->flags && a->backing==b->backing && a->offset==b->offset;
}
static void hvf_unmap_range(uint64_t base,uint64_t size) { for(uint64_t p=0;p<size;)p+=4096*(uint64_t)hvf_unmap_page(base+p); }
/* The guest-physical address va maps to in the current view, or 0. */
static uint64_t hvf_mapped_ipa(uint64_t va) {
    uint64_t *leaf=hvf_leaf(hvf_view,va,0);if(!leaf)return 0;
    uint64_t descriptor=leaf[(va>>12)&511];
    return (descriptor&1)?descriptor&UINT64_C(0x0000fffffffff000):0;
}
static void hvf_map_arena_all(GuestAddressSpace *space) {
    int ipa=space->map_flags && space->map_backing;
    unsigned pages=space->map_capacity/4096;if(space->map_top<pages)pages=space->map_top;
    if(ipa)for(unsigned i=0;i<pages;i++) {
        if(!(i&7) && i+8<=pages) { uint64_t w;memcpy(&w,space->map_flags+i,8);if(!(w&UINT64_C(0x0707070707070707))) { i+=7;continue; } }
        if(space->map_flags[i]&7)hvf_map_arena_page(space,i);
    }
}
/* Update v to space in place, touching only what changed since v was recorded:
   removed and changed segments, the brk heap and the mmap arena. After this
   process's fork or a backing copy (HVF_STALE_COW) every private page is mapped
   again, as it may be copy-on-write now; shared segments never change permissions
   and keep their entries. This spares a fork-heavy server from re-mapping all of
   its memory (Postgres: its shared buffers) several times per new process.
   Returns 0 when only a full rebuild can do it. */
static int hvf_sync_partial(GuestAddressSpace *space,HvfView *v) {
    if(!v->root || v->stale==HVF_STALE_NEW || v->map_capacity!=space->map_capacity || v->tables>HVF_TABLES/4)return 0;
    if(v->map_backing && !space->map_backing)return 0;
    int arena_dirty=v->arena_rev!=space->arena_rev;
    if(arena_dirty && v->arena_rev<space->arena_dirty_since)return 0;   /* arena changes this view did not see */
    int all=v->stale==HVF_STALE_COW;
    hvf_view=v;hvf_exhausted=0;hvf_stale_tlb=0;
    uint64_t removed_low[129],removed_high[129];unsigned removed=0;
    for(unsigned i=0;i<v->nseg;i++) {
        int kept=0;
        for(unsigned j=0;j<space->nseg && !kept;j++)kept=hvf_same_segment(&v->seg[i],&space->seg[j]);
        if(kept)continue;
        if(removed==129)return 0;                        /* many segments went (an munmap of many commits): rebuild */
        hvf_unmap_range(v->seg[i].base,v->seg[i].size);
        removed_low[removed]=v->seg[i].base;removed_high[removed++]=v->seg[i].base+v->seg[i].size;
    }
    for(unsigned j=0;j<space->nseg;j++) {
        const Segment *s=&space->seg[j];int kept=0;
        for(unsigned i=0;i<v->nseg && !kept;i++)kept=hvf_same_segment(s,&v->seg[i]);
        for(unsigned r=0;kept && r<removed;r++)if(s->base<removed_high[r] && removed_low[r]<s->base+s->size)kept=0;
        /* A backing freed and another allocated at its address keeps the pointer, not the block. */
        if(kept && s->backing && s->flags && s->size && hvf_mapped_ipa(s->base)!=hvf_ipa_at(s->backing,s->offset))kept=0;
        if(!kept || (all && !(s->flags&SEG_SHARED)))hvf_map_range(space,s->base,s->size,s->flags,s->backing,s->offset);
    }
    if(v->brk_backing!=space->brk_backing || v->brk_base!=space->brk_base || v->brk_pages!=space->brk_pages) {
        hvf_unmap_range(v->brk_base,v->brk_pages);
        hvf_map_range(space,space->brk_base,space->brk_pages,6,space->brk_backing,0);
    } else if(all)hvf_map_range(space,space->brk_base,space->brk_pages,6,space->brk_backing,0);
    if(arena_dirty)hvf_remap_arena(space);                       /* pages mapped, unmapped or reprotected since */
    if(all || v->map_backing!=space->map_backing)hvf_map_arena_all(space);
    return !hvf_exhausted;
}
/* Bring the current process's tables up to date; returns whether TLBs need maintenance:
   1 for this view's ASID, 2 for every one (the tables were rebuilt). */
static int hvf_sync_tables(GuestAddressSpace *space,HvfView *v) {
    if(hvf_view_matches(space)) {
        if(v!=hvf_view || hvf_cpu->ttbr!=v->root)hvf_activate(v);             /* a process switch */
        int stale=0;
        if(v->arena_rev!=space->arena_rev) {
            /* A range that started after this view (or a large table set) rebuilds. */
            if(v->arena_rev<space->arena_dirty_since || v->tables>HVF_TABLES/4)goto rebuild;
            hvf_exhausted=0;hvf_stale_tlb=0;hvf_remap_arena(space);
            if(hvf_exhausted)goto rebuild;
            hvf_arena_synced(space);stale=hvf_stale_tlb;
        }
        v->generation=space->generation;return stale;
    }
rebuild:
    if(v->root)hvf_siblings_out();                       /* threads of this process on other vCPUs use these tables */
    if(hvf_sync_partial(space,v)) { hvf_activate(v);hvf_view_record(v,space);hvf_arena_synced(space);return 1; }
    hvf_view=v;hvf_build(space);
    if(hvf_exhausted) {
        hvf_evict();hvf_build(space);
        if(hvf_exhausted)fail("Guest memory exceeds the hypervisor's page tables or guest-physical space");
    }
    hvf_activate(v);hvf_view_record(v,space);hvf_arena_synced(space);
    return 2;                                            /* drop every ASID's stale translations */
}
/* TLB (and, for new code, instruction cache) maintenance runs on the way back to the
   guest when it resumes through the frame: the EL1 routine "ic ialluis (if code was
   mapped); tlbi aside1is (this view's ASID) or tlbi vmalle1is; dsb ish; isb" then
   restores it. munmap and mprotect paid a second vCPU run each for hvf_flush, and
   flushed every process's translations on every vCPU. */
static void hvf_sync_memory(GuestAddressSpace *space) {
    HvfView *v=&hvf_views[process_pid];
    if(v==hvf_view && v->root && hvf_cpu->ttbr==v->root && v->generation==space->generation)return;
    hvf_stale_code=0;
    int stale=hvf_sync_tables(space,v);
    if(!stale && !hvf_asid_flush)return;
    uint64_t pc=0;
    if(hvf_cpu->framed)hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_PC,&pc),"get pc");
    if(pc==HVF_SYSTEM_VA+HVF_RESTORE_OFFSET) {
        int all=stale==2 || hvf_asid_flush;
        hvf_frame[HF_FLUSH]=((uint64_t)v->asid<<48)|(all?2:0)|(all || hvf_stale_code?1:0);
        hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_FLUSH_RESTORE_OFFSET),"set pc");
    } else hvf_flush();
    hvf_asid_flush=0;
}
/* The mapping holding va: its backing, guest range [*low,*high), the backing offset
   at *low and (outside the mmap arena, whose pages have their own) its flags. */
static MemoryBacking *hvf_region(const GuestAddressSpace *space,uint64_t va,uint64_t *low,uint64_t *high,size_t *start,unsigned *flags) {
    *start=0;*flags=0;
    if(space->map_flags && va>=MAP_BASE && va-MAP_BASE<space->map_capacity) { *low=MAP_BASE;*high=MAP_BASE+space->map_capacity;return space->map_backing; }
    if(va>=space->brk_base && va-space->brk_base<space->brk_pages) { *low=space->brk_base;*high=*low+space->brk_pages;*flags=6;return space->brk_backing; }
    for(unsigned i=0;i<space->nseg;i++)
        if(va>=space->seg[i].base && va-space->seg[i].base<space->seg[i].size) {
            *low=space->seg[i].base;*high=*low+space->seg[i].size;*start=space->seg[i].offset;*flags=space->seg[i].flags;return space->seg[i].backing;
        }
    return NULL;
}
/* After a write fault made a page private (or charged it), remap the guest pages
   that share its host page, and the hosts-1 host pages after it, instead of
   rebuilding every table. */
static void hvf_remap_host_page(const GuestAddressSpace *space,uint64_t far,unsigned hosts) {
    uint64_t va=far&~UINT64_C(4095),low=0,high=0;size_t start=0;unsigned flags=0;
    MemoryBacking *backing=hvf_region(space,va,&low,&high,&start,&flags);
    if(!backing)return;
    hvf_exhausted=0;hvf_stale_tlb=0;hvf_stale_code=0;
    uint64_t ipa=1,first=0,last=0;
    size_t host=(start+(size_t)(va-low))/backing->page_size*backing->page_size;
    for(size_t offset=host;ipa && offset<host+hosts*backing->page_size;offset+=4096) {
        if(offset<start)continue;
        uint64_t page=low+(offset-start);if(page>=high)break;
        unsigned f=space->map_flags && low==MAP_BASE?space->map_flags[(page-MAP_BASE)/4096]&7:flags;
        if(f && (ipa=hvf_ipa_at(backing,offset))) { hvf_map_page(page,ipa,hvf_effective_flags(space,backing,f,offset),0);if(!first)first=page;last=page; }
    }
    if(hvf_exhausted) { hvf_view->stale=HVF_STALE_NEW;return; } /* the next sync rebuilds (and evicts) */
    if(hvf_stale_tlb && (hvf_stale_code || !first))hvf_flush();
    else if(hvf_stale_tlb) { hvf_pending_va=first;hvf_pending_pages=(unsigned)((last-first)/4096+1); }
}
/* Fault-around: a write fault made host page h private and the page before it is
   private too (memory being filled in order: a heap, a buffer). Take the next host
   pages now, up to HVF_FAULT_AROUND of them, while each still needs a write fault
   (not owned yet, or shared since a fork) and every guest page in it is mapped
   writable and private to this process (no shared or file-copy segment, no arena
   page lent to one, no read-only or PROT_NONE page). Each costs a charge it would
   have taken at its first write; a fault on each is saved. Only when the backing is
   already this process's alone (no snapshot is taken here). Returns how many. */
#define HVF_FAULT_AROUND 8u
static unsigned hvf_fault_around(GuestAddressSpace *space,uint64_t far) {
    uint64_t low=0,high=0;size_t start=0;unsigned flags=0;
    MemoryBacking *backing=hvf_region(space,far,&low,&high,&start,&flags);
    if(!backing || (flags&SEG_SHARED) || backing->frozen || address_space_shared(space,backing))return 0;
    if(backing->refs!=1 && backing->refs!=address_space_backing_refs(space,backing))return 0;
    size_t size=backing->page_size,host=(start+(size_t)(far-low))/size;
    if(!host || !backing->pages[host-1] || backing->pages[host-1]->refs!=1)return 0;
    int arena=space->map_flags && low==MAP_BASE;unsigned n=0;
    if(!arena && !(flags&2))return 0;
    for(size_t h=host+1;n<HVF_FAULT_AROUND && (h+1)*size<=backing->size && h*size>=start;h++,n++) {
        if(backing->pages[h] && backing->pages[h]->refs==1)break;     /* private already */
        uint64_t page=low+(h*size-start);
        if(page>=high || high-page<size)break;
        for(size_t k=0;arena && k<size/4096;k++)if((space->map_flags[(page-MAP_BASE)/4096+k]&(8|2|MAP_RESERVED))!=(8|2))return n && address_space_make_private(space,backing,(host+1)*size,n*size)?n:0;
    }
    return n && address_space_make_private(space,backing,(host+1)*size,n*size)?n:0;
}
/* Resume the guest after a write fault (its registers are in the frame). Pages that
   became writable are invalidated on the way: the vCPU runs the EL1 routine
   "1: tlbi vale1is, x0; add x0, x0, #1; subs x1, x1, #1; b.ne 1b; dsb ish" (operand
   and page count from the frame) and then restores the guest, instead of a separate
   run for the invalidation. Returns 1 then: the vCPU is at EL1 until it runs (the
   caller treats it as in the vector). */
static int hvf_fault_resume(uint64_t elr,uint64_t spsr) {
    unsigned pages=hvf_pending_pages;hvf_pending_pages=0;
    if(pages && !hvf_cpu->framed)hvf_flush();
    if(!pages || !hvf_cpu->framed) { hvf_resume_el0(elr,spsr);return 0; }
    hvf_frame[HF_TLBI_VA]=((uint64_t)hvf_view->asid<<48)|((hvf_pending_va>>12)&UINT64_C(0xfffffffffff));hvf_frame[HF_TLBI_PAGES]=pages;
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_INVALIDATE_OFFSET),"set pc");
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,0x3c5),"set cpsr");           /* EL1h, interrupts masked */
    return 1;
}
/* A write fault on a page this view already maps writable: a TLB still held its
   read-only entry. Invalidate it on the way back (the caller resumes the guest). */
static int hvf_spurious_write(uint64_t far) {
    if(hvf_view!=&hvf_views[process_pid] || !hvf_view->root || hvf_cpu->ttbr!=hvf_view->root)return 0;
    const uint64_t *leaf=hvf_leaf(hvf_view,far,0);if(!leaf)return 0;
    uint64_t descriptor=leaf[(far>>12)&511];
    if(!(descriptor&1) || (descriptor&(UINT64_C(1)<<7)))return 0;
    PROFILE_ADD(profile_spurious_faults,1);
    hvf_pending_va=far&~UINT64_C(4095);hvf_pending_pages=1;return 1;
}
/* Register transfer. The EL1 vector saves the guest's general registers, SP_EL0,
   TPIDR_EL0, ELR, SPSR, ESR and FAR to the vCPU's frame (plain memory) before its
   HVC, and every resume goes through the EL1 routine that loads them back from it
   and ERETs: an exit costs no register calls (each hv_vcpu_get_reg is a library call;
   reading 36 of them at every exit was a quarter of a system call's cost). Only a
   preemption at EL0 leaves them in the vCPU (framed 0). FP/SIMD state stays in
   the vCPU until something needs the whole CPU state (returning to the scheduler,
   a thread switch, signal delivery, clone, exec, sigreturn): hvf_fp_pull() copies
   it into cpu and marks it to be written back on the next resume. */
/* cpu now holds a complete state the vCPU does not have (new task or thread). */
static void hvf_regs_invalidate(void) { hvf_fp_in_cpu=1;hvf_fp_dirty=1; }
static void hvf_fp_pull(void) {
    if(!hvf_fp_in_cpu) {
        uint64_t value=0;
        hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_FPCR,&value),"get fpcr");cpu->fpcr=(uint32_t)value;
        hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_FPSR,&value),"get fpsr");cpu->fpsr=(uint32_t)value;
        for(unsigned i=0;i<32;i++) {
            hv_simd_fp_uchar16_t q;hvf_check(hv_vcpu_get_simd_fp_reg(hvf_vcpu,(hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0+i),&q),"get q");
            memcpy(cpu->simd[i],&q,16);
        }
        hvf_fp_in_cpu=1;
    }
    hvf_fp_dirty=1; /* the caller may change it */
}
/* The answers the EL1 vector gives getpid, (not getppid: a parent can exit),
   getuid, geteuid, getgid, getegid and gettid without an exit: frame words
   HF_FAST+call-172, -1 for "ask the host". Written on every resume, and pushed to
   the frames of the process's other running threads when its IDs change (they
   share them). Off while syscalls are traced, counted or filtered. */
static void hvf_fast_publish(uint64_t *frame,uint64_t tid) {
    int on=!trace && !profiling && !seccomp_active();
    const uint64_t answers[7]={process_pid,UINT64_MAX,guest_ids.ruid,guest_uid,guest_ids.rgid,guest_gid,tid};
    for(unsigned i=0;i<7;i++)frame[HF_FAST+i]=on?answers[i]:UINT64_MAX;
}
static void hvf_fast_push(void) {
    for(unsigned w=0;w<worker_count;w++) {
        HvfCpu *c=workers[w] && workers[w]!=worker && workers[w]->task==(int)process_pid-1?workers[w]->backend:NULL;
        if(c)hvf_fast_publish(c->frame,c->frame[HF_FAST+6]);
    }
}
static void hvf_put_regs(void) {
    uint64_t *f=hvf_frame;
    memcpy(f,cpu->gpr,31*sizeof(uint64_t));
    f[HF_SP]=cpu->sp;f[HF_TLS]=cpu->tls;f[HF_ELR]=cpu->pc;
    f[HF_SPSR]=((uint64_t)cpu->flag_n<<31)|((uint64_t)cpu->flag_z<<30)|((uint64_t)cpu->flag_c<<29)|((uint64_t)cpu->flag_v<<28); /* EL0t */
    Thread *th=thread_current();hvf_fast_publish(f,th?th->tid:process_pid);
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_RESTORE_OFFSET),"set pc");
    if(!hvf_cpu->framed)hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,0x3c5),"set cpsr"); /* EL1h, interrupts masked (framed: it is) */
    hvf_cpu->framed=1;
    if(cpu->mem_model!=hvf_cpu->tso) {                                         /* only set where guest_tso said it works */
        if(__builtin_available(macOS 15.0,*))hvf_check(hv_vcpu_set_sys_reg(hvf_vcpu,HV_SYS_REG_ACTLR_EL1,cpu->mem_model?2:0),"set actlr");
        hvf_cpu->tso=cpu->mem_model;
    }
    if(hvf_fp_dirty) {
        if(!hvf_fp_in_cpu)fail("Hypervisor FP state lost");
        hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_FPCR,cpu->fpcr),"set fpcr");
        hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_FPSR,cpu->fpsr),"set fpsr");
        for(unsigned i=0;i<32;i++) {
            hv_simd_fp_uchar16_t value;memcpy(&value,cpu->simd[i],16);
            hvf_check(hv_vcpu_set_simd_fp_reg(hvf_vcpu,(hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0+i),value),"set q");
        }
        hvf_fp_dirty=0;
    }
}
/* From the frame; else (a preemption, or another vector) from the vCPU: after an
   EL0 exception the guest's return address and flags are ELR/SPSR_EL1 (passed in),
   after a preemption the live PC/CPSR. */
static void hvf_get_regs(int exception,uint64_t elr,uint64_t spsr) {
    uint64_t value=spsr;
    if(hvf_cpu->framed) {
        const uint64_t *f=hvf_frame;
        memcpy(cpu->gpr,f,31*sizeof(uint64_t));cpu->pc=f[HF_ELR];value=f[HF_SPSR];cpu->sp=f[HF_SP];cpu->tls=f[HF_TLS];
    } else {
        for(unsigned i=0;i<31;i++)hvf_check(hv_vcpu_get_reg(hvf_vcpu,(hv_reg_t)(HV_REG_X0+i),&cpu->gpr[i]),"get x");
        if(exception)cpu->pc=elr;
        else {
            hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_PC,&cpu->pc),"get pc");
            hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_CPSR,&value),"get cpsr");
        }
        hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_SP_EL0,&cpu->sp),"get sp");
        hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_TPIDR_EL0,&cpu->tls),"get tpidr");
    }
    cpu->gpr[31]=0;
    cpu->flag_n=(value>>31)&1;cpu->flag_z=(value>>30)&1;cpu->flag_c=(value>>29)&1;cpu->flag_v=(value>>28)&1;
    hvf_fp_in_cpu=0; /* the guest ran: the vCPU's FP state is newer */
}
/* The exception an exit reports: from the frame, else (another vector) from the vCPU. */
static void hvf_exception(uint64_t *esr,uint64_t *elr,uint64_t *spsr,uint64_t *far) {
    if(hvf_cpu->framed) { *esr=hvf_frame[HF_ESR];*elr=hvf_frame[HF_ELR];*spsr=hvf_frame[HF_SPSR];*far=hvf_frame[HF_FAR];return; }
    hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_ESR_EL1,esr),"get esr");
    hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_ELR_EL1,elr),"get elr");
    hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_SPSR_EL1,spsr),"get spsr");
    hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,HV_SYS_REG_FAR_EL1,far),"get far");
    if(*spsr&0xf)fail("Hypervisor EL1 routine faulted");                      /* not the guest's exception */
}
/* Run the EL1 routine "ic ialluis; tlbi vmalle1is; dsb ish; isb; hvc #1" so the
   guest sees rebuilt tables and freshly written code on every vCPU, then return to
   the EL0 state. */
static void hvf_flush(void) {
    uint64_t pc=0,cpsr=0;
    hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_PC,&pc),"get pc");hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_CPSR,&cpsr),"get cpsr");
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_FLUSH_OFFSET),"set pc");
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,0x3c5),"set cpsr");           /* EL1h, interrupts masked */
    /* A preemption requested as the previous guest run ended can still be pending;
       it cancels this run before the routine starts, so simply run again. */
    do hvf_check(hv_vcpu_run(hvf_vcpu),"hv_vcpu_run");
    while(hvf_exit->reason==HV_EXIT_REASON_CANCELED || hvf_exit->reason==HV_EXIT_REASON_VTIMER_ACTIVATED);
    if(hvf_exit->reason!=HV_EXIT_REASON_EXCEPTION || (hvf_exit->exception.syndrome>>26)!=0x16 || (hvf_exit->exception.syndrome&0xffff)!=1)
        fail("Hypervisor TLB/cache maintenance did not complete");
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,pc),"set pc");hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,cpsr),"set cpsr");
}
/* Back to the guest at pc with flags spsr, its registers otherwise unchanged. */
static void hvf_resume_el0(uint64_t pc,uint64_t spsr) {
    if(hvf_cpu->framed) {
        hvf_frame[HF_ELR]=pc;hvf_frame[HF_SPSR]=spsr;
        hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_RESTORE_OFFSET),"set pc");
        hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,0x3c5),"set cpsr");
        return;
    }
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,pc),"set pc");hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_CPSR,spsr),"set cpsr");
}
/* Stop a worker's guest code soon (preemption, a signal for its task, the run ending). */
static void hvf_interrupt(Worker *w) {
    HvfCpu *c=w->backend;
    if(c && atomic_load(&w->in_guest))(void)hv_vcpus_exit(&c->vcpu,1);
}
/* Preempt CPU-bound guests so the scheduler can switch tasks (10 ms slices), and
   keep the vDSO's realtime offset current (only this thread refreshes it). The
   request is sticky: an interrupt alone is lost when the worker is serving a
   system call (not in guest code) or the vCPU is in an EL1 routine (which runs on
   into its exit), and a thread looping on system calls is almost always in one of
   those; hvf_slice_run yields at its next safe point instead. */
static void *hvf_watchdog(void *unused) {
    (void)unused;
    for(;;) {
        struct timespec slice={0,10000000};nanosleep(&slice,NULL);
        if(vdso_backing)vdso_tick();
        for(unsigned i=0;i<worker_count;i++) {
            HvfCpu *c=workers[i]->backend;
            if(c)__atomic_store_n(&c->preempt,1,__ATOMIC_RELEASE);
            hvf_interrupt(workers[i]);
        }
    }
    return NULL;
}
static uint64_t hvf_vtimer_offset;
static void hvf_futex_init(void);
/* The calling worker's vCPU: every vCPU shares one virtual counter (the vDSO's clock). */
static HvfCpu *hvf_cpu_new(int first) {
    HvfCpu *c=calloc(1,sizeof(*c));if(!c)fail("Cannot allocate a vCPU");
    c->fp_in_cpu=c->fp_dirty=1;
    unsigned index=__atomic_fetch_add(&hvf_frames_used,1,__ATOMIC_RELAXED);
    if(index>=WORKER_LIMIT)fail("Too many vCPUs");
    c->frame=(uint64_t *)(void *)(hvf_system+HVF_CODE_BYTES+index*HVF_FRAME_BYTES);
    hvf_check(hv_vcpu_create(&c->vcpu,&c->exit,NULL),"hv_vcpu_create");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_SP_EL1,HVF_FRAME_VA+index*HVF_FRAME_BYTES),"set sp_el1"); /* the vector's frame */
    if(first)hvf_check(hv_vcpu_get_vtimer_offset(c->vcpu,&hvf_vtimer_offset),"get vtimer offset");
    else hvf_check(hv_vcpu_set_vtimer_offset(c->vcpu,hvf_vtimer_offset),"set vtimer offset");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_MAIR_EL1,0xff),"set mair");     /* attr 0: normal write-back */
    /* T0SZ=16 (48-bit), WB walks, inner shareable, 4 KiB granule, no TTBR1 walks, 40-bit IPA, TBI0; 8-bit ASIDs from TTBR0. */
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_TCR_EL1,16|(1u<<8)|(1u<<10)|(3u<<12)|(1u<<23)|(UINT64_C(2)<<32)|(UINT64_C(1)<<37)),"set tcr");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_VBAR_EL1,HVF_SYSTEM_VA),"set vbar");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_CPACR_EL1,UINT64_C(3)<<20),"set cpacr"); /* FP/SIMD at EL0 */
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_CNTKCTL_EL1,3),"set cntkctl");   /* EL0 may read the counters */
    /* MMU, caches, SP alignment; EL0 may use CTR_EL0, DC ZVA, cache maintenance and WFI/WFE. */
    uint64_t sctlr=UINT64_C(0x30d00800)|1|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<12)|(1u<<14)|(1u<<15)|(1u<<16)|(1u<<18)|(1u<<26);
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_SCTLR_EL1,sctlr),"set sctlr");
    /* TSO (compat.h's PR_SET_MEM_MODEL): public from macOS 15, which lets a vCPU set
       only ACTLR_EL1.EnTSO (bit 1). The first vCPU checks the bit sticks. */
    if(__builtin_available(macOS 15.0,*)) {
        uint64_t actlr=0;
        int sticks=hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_ACTLR_EL1,2)==HV_SUCCESS && hv_vcpu_get_sys_reg(c->vcpu,HV_SYS_REG_ACTLR_EL1,&actlr)==HV_SUCCESS && (actlr&2);
        if(first)guest_tso=sticks;
        if(sticks)hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_ACTLR_EL1,0),"set actlr");
    }
    return c;
}
static void hvf_init(void) {
    /* Every process's memory stays mapped: ask for the largest guest-physical space (up to the 40-bit TCR_EL1.IPS). */
    hv_vm_config_t config=hv_vm_config_create();uint32_t bits=0;
    if(config && hv_vm_config_get_max_ipa_size(&bits)==HV_SUCCESS && bits>=36) {
        if(bits>40)bits=40;
        if(hv_vm_config_set_ipa_size(config,bits)==HV_SUCCESS)hvf_ipa_limit=UINT64_C(1)<<bits;
    }
    hvf_check(hv_vm_create(config),"hv_vm_create");
    if(config)os_release(config);
    uint32_t vcpus=0;
    if(hv_vm_get_max_vcpu_count(&vcpus)==HV_SUCCESS && vcpus && worker_count>vcpus)worker_count=vcpus;
    backing_release_hook=hvf_backing_gone;worker_interrupt=hvf_interrupt;workers_supported=1;
    hvf_futex_init();
    hvf_tables=hvf_host_pages(HVF_TABLE_BYTES);hvf_system=hvf_host_pages(HVF_SYSTEM_BYTES);
    hvf_check(hv_vm_map(hvf_tables,HVF_IPA_BASE,HVF_TABLE_BYTES,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map tables");
    hvf_check(hv_vm_map(hvf_system,HVF_IPA_BASE+HVF_TABLE_BYTES,HVF_CODE_BYTES,HV_MEMORY_READ|HV_MEMORY_EXEC),"hv_vm_map system");
    hvf_check(hv_vm_map(hvf_system+HVF_CODE_BYTES,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_CODE_BYTES,HVF_SYSTEM_BYTES-HVF_CODE_BYTES,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map frames");
    /* Each HVC is followed by "b .-4": under load Hypervisor.framework can resume a
       vCPU past an HVC without reporting its exit (the next instruction, zeros, then
       faulted at EL1 and the guest saw SIGILL at the vector). Looping back takes the
       exit on the retry; ESR/ELR_EL1 are unchanged, and the exit PC is HVC+4 either way.
       Never "hvc #0": that is an Arm SMCCC call, and Hypervisor.framework answers some
       function IDs itself, overwriting the guest's live x0 with 0xffffffff
       (NOT_SUPPORTED) when it reads like one (0xC1xxxxxx: an mmap address 0x3c1xxxxxx
       passed to munmap, a pointer in a faulting thread). Non-zero immediates are not
       SMCCC calls, so every register reaches the exit intact. */
    for(unsigned i=0;i<16;i++) {
        ((uint32_t *)hvf_system)[i*0x80/4]=UINT32_C(0xd4000062);                       /* hvc #3 */
        ((uint32_t *)hvf_system)[i*0x80/4+1]=UINT32_C(0x17ffffff);                     /* b .-4 */
    }
    /* Synchronous exceptions from EL0 (0x400) save the guest to the frame at SP_EL1
       and exit with "hvc #4" (0x900); the routine at 0x968 resumes the guest from the
       frame, the one at 0x9c4 invalidates hvf_fault_resume's pages first. Getpid,
       gettid and the uid/gid getters are answered at 0x400 from the frame, without
       an exit. Assembled with clang --target=aarch64-linux-gnu, rust-objcopy -O binary:
       0x400: stp x0, x1, [sp]; mrs x0, esr_el1; lsr x0, x0, #26; cmp x0, #0x15; b.ne save
              sub x0, x8, #172; cmp x0, #6; b.hi save; add x1, sp, #320 (HF_FAST)
              ldr x0, [x1, x0, lsl #3]; tbnz x0, #63, save; ldr x1, [sp, #8]; eret
       save:  stp x2, x3, [sp, #16] ... stp x28, x29, [sp, #224]
              mrs x0, sp_el0; stp x30, x0, [sp, #240]; mrs x0, tpidr_el0; mrs x1, elr_el1; stp x0, x1, [sp, #256]
              mrs x0, spsr_el1; mrs x1, esr_el1; stp x0, x1, [sp, #272]; mrs x0, far_el1; str x0, [sp, #288]
              hvc #4; b .-4
       restore: ldp x0, x1, [sp, #248]; msr sp_el0, x0; msr tpidr_el0, x1
              ldp x0, x1, [sp, #264]; msr elr_el1, x0; msr spsr_el1, x1
              ldp x0, x1, [sp] ... ldp x28, x29, [sp, #224]; ldr x30, [sp, #240]; eret
       invalidate: ldp x0, x1, [sp, #296]; 1: tlbi vale1is, x0; add x0, x0, #1; subs x1, x1, #1; b.ne 1b; dsb ish; b restore */
    static const uint32_t lower_sync[]={0xa90007e0,0xd5385200,0xd35afc00,0xf100541f,0x54002781,0xd102b100,0xf100181f,0x54002728,0x910503e1,0xf8607820,0xb7f826c0,0xf94007e1,0xd69f03e0};
    static const uint32_t frame_code[]={
        0xa9010fe2,0xa90217e4,0xa9031fe6,0xa90427e8,0xa9052fea,0xa90637ec,0xa9073fee,0xa90847f0,0xa9094ff2,0xa90a57f4,0xa90b5ff6,0xa90c67f8,0xa90d6ffa,0xa90e77fc,
        0xd5384100,0xa90f03fe,0xd53bd040,0xd5384021,0xa91007e0,0xd5384000,0xd5385201,0xa91107e0,0xd5386000,0xf90093e0,0xd4000082,0x17ffffff,
        0xa94f87e0,0xd5184100,0xd51bd041,0xa95087e0,0xd5184020,0xd5184001,0xa94007e0,0xa9410fe2,0xa94217e4,0xa9431fe6,0xa94427e8,0xa9452fea,
        0xa94637ec,0xa9473fee,0xa94847f0,0xa9494ff2,0xa94a57f4,0xa94b5ff6,0xa94c67f8,0xa94d6ffa,0xa94e77fc,0xf9407bfe,0xd69f03e0,
        0xa95287e0,0xd50883a0,0x91000400,0xf1000421,0x54ffffa1,0xd5033b9f,0x17ffffe3};
    memcpy(hvf_system+0x400,lower_sync,sizeof(lower_sync));
    memcpy(hvf_system+HVF_SAVE_OFFSET,frame_code,sizeof(frame_code));
    /* 0xa00, hvf_sync_memory's: ldr x0, [sp, #312] (HF_FLUSH); tbz x0, #0, 1f; ic ialluis; 1: tbz x0, #1, 2f
       tlbi vmalle1is; b 3f; 2: and x1, x0, #0xffff000000000000; tlbi aside1is, x1; 3: dsb ish; isb; b restore */
    static const uint32_t flush_restore[]={0xf9409fe0,0x36000040,0xd508711f,0x36080060,0xd508831f,0x14000003,0x92503c01,0xd5088341,0xd5033b9f,0xd5033fdf,0x17ffffd0};
    memcpy(hvf_system+HVF_FLUSH_RESTORE_OFFSET,flush_restore,sizeof(flush_restore));
    const uint32_t flush[]={UINT32_C(0xd508711f),UINT32_C(0xd508831f),UINT32_C(0xd5033b9f),UINT32_C(0xd5033fdf),UINT32_C(0xd4000022),UINT32_C(0x17ffffff)};
    memcpy(hvf_system+HVF_FLUSH_OFFSET,flush,sizeof(flush));
    worker->backend=hvf_cpu_new(1);
    if(vdso_backing)vdso_start(hvf_vtimer_offset);
    pthread_t watchdog;
    if(pthread_create(&watchdog,NULL,hvf_watchdog,NULL) || pthread_detach(watchdog))fail("Cannot start hypervisor watchdog");
}
/* ---- Threads (CLONE_THREAD) ----
   The scheduler runs every thread of a process on its own (scheduler.h), so
   threads of one process run on several vCPUs at once; these are the thread
   system calls. Runtime work of a process's threads is serialized by its mm lock.
   Futex operations: WAIT, WAKE, WAIT_BITSET, WAKE_BITSET. */
#define HVF_UNHANDLED (-100)
static uint64_t hvf_main_clear_tid[PROCESS_LIMIT+2];
static unsigned hvf_next_tid=65536;
static ScheduledTask *hvf_task(void) { return &schedule_tasks[process_pid-1]; }
/* Threads of this process running guest code on other workers leave it, and stay
   out while the caller holds the process's mm lock (they take it to re-enter).
   Needed before their page tables are freed or their memory is snapshotted. */
static void hvf_siblings_out(void) {
    if(!schedule_active || !worker_threads || !hvf_task()->threads)return;
    int index=(int)(process_pid-1);
    for(unsigned i=0;i<worker_count;i++)if(workers[i]!=worker && workers[i]->task==index)hvf_interrupt(workers[i]);
    for(unsigned i=0;i<worker_count;i++)
        while(workers[i]!=worker && workers[i]->task==index && atomic_load(&workers[i]->in_guest))sched_yield();
}
/* The process's thread table, created on first use: thread 0 is the caller. */
static ThreadTable *hvf_threads_ensure(void) {
    ScheduledTask *t=hvf_task();
    if(!t->threads) {
        ThreadTable *p=calloc(1,sizeof(*p));if(!p)return NULL;
        Thread *th=&p->thread[0];
        th->state=1;th->tid=(unsigned)process_pid;th->clear_tid=hvf_main_clear_tid[process_pid];
        th->signal_mask=t->signal_mask;th->network=net_wait;th->worker=t->worker;th->wakeups=t->wakeups;
        p->top=1;t->worker=0;t->running=1;t->threads=p;thread_now=0;net_wait_now=&th->network;
    }
    return t->threads;
}
static Thread *hvf_thread_find(ScheduledTask *t,uint64_t tid) {
    for(unsigned i=0;t->threads && i<t->threads->top;i++)if(t->threads->thread[i].state && t->threads->thread[i].tid==tid)return &t->threads->thread[i];
    return NULL;
}
/* tgkill/tkill within a threaded process: pending on that thread, which leaves a
   futex wait (EINTR) or a blocked call to take it. A signal with no handler that
   the thread does not block takes its default action on the whole process. */
static uint64_t hvf_thread_signal(ScheduledTask *t,uint64_t tid,uint64_t signal) {
    Thread *th=hvf_thread_find(t,tid);
    if(!th)return linux_error(3);
    if(!signal)return 0;
    uint64_t bit=UINT64_C(1)<<(signal-1);
    if(signal==9 || signal==19 || (!t->signal_handler[signal-1] && !(th->signal_mask&bit))) { schedule_signal(t,(unsigned)signal);return 0; }
    if(t->signal_ignored&bit)return 0;
    th->signal_pending|=bit;t->signal_sender[signal-1]=(unsigned)process_pid;
    if(!(th->signal_mask&bit)) {
        if(th->state==2) { th->state=1;th->regs.gpr[0]=linux_error(4); }
        else if(th->state==3)th->state=1;
        th->wakeups++;schedule_events++;
        if(th->worker && th!=thread_current())worker_interrupt(workers[th->worker-1]);
    }
    return 0;
}
/* The futex key of address: (memory, offset) inside a shared segment, else NULL. */
static MemoryBacking *hvf_futex_shared(uint64_t address,size_t *offset) {
    for(unsigned i=0;i<address_space->nseg;i++) {
        const Segment *s=&address_space->seg[i];
        if((s->flags&SEG_SHARED) && address>=s->base && address-s->base<s->size) { *offset=s->offset+(size_t)(address-s->base);return s->backing; }
    }
    return NULL;
}
/* Futex waits served in place: a thread whose futex wait is likely short while
   workers are idle waits on its own worker, holding no lock, instead of going back
   to the scheduler (hvf_futex_unlocked). It is queued in a bucket by (process,
   image, address); wakers take only the bucket's lock. Private futexes only. */
typedef struct HvfFutexWaiter { struct HvfFutexWaiter *next;uint64_t pid,epoch,address;uint32_t bits;int woken;pthread_cond_t cond; } HvfFutexWaiter;
#define HVF_FUTEX_BUCKETS 256u
static struct { pthread_mutex_t lock;HvfFutexWaiter *head; } hvf_futex_buckets[HVF_FUTEX_BUCKETS];
static void hvf_futex_init(void) { for(unsigned i=0;i<HVF_FUTEX_BUCKETS;i++)pthread_mutex_init(&hvf_futex_buckets[i].lock,NULL); }
static unsigned hvf_futex_bucket(uint64_t pid,uint64_t address) { return (unsigned)((address>>2)^(address>>12)^(pid*0x9e37u))%HVF_FUTEX_BUCKETS; }
/* Wake up to count of this process's (this image's) in-place waiters on address. */
static unsigned hvf_futex_wake_inplace(uint64_t address,unsigned count,uint32_t bits) {
    if(!count || !schedule_active)return 0;
    uint64_t pid=process_pid,epoch=hvf_task()->futex_epoch;unsigned woken=0,b=hvf_futex_bucket(pid,address);
    pthread_mutex_lock(&hvf_futex_buckets[b].lock);
    for(HvfFutexWaiter **link=&hvf_futex_buckets[b].head;*link && woken<count;) {
        HvfFutexWaiter *w=*link;
        if(w->pid!=pid || w->epoch!=epoch || w->address!=address || !(w->bits&bits)) { link=&w->next;continue; }
        *link=w->next;w->woken=1;pthread_cond_signal(&w->cond);woken++;
    }
    pthread_mutex_unlock(&hvf_futex_buckets[b].lock);
    return woken;
}
/* All guest memory is process-private, where Linux gives private and shared futex
   keys the same identity: CLONE_CHILD_CLEARTID's shared wake must reach a joiner's
   FUTEX_PRIVATE wait. A futex in shared memory has waiters in any process. */
static unsigned hvf_futex_wake(uint64_t address,unsigned count,uint32_t bits) {
    unsigned woken=0;size_t offset=0;MemoryBacking *shared=hvf_futex_shared(address,&offset);
    if(!shared)woken=hvf_futex_wake_inplace(address,count,bits);
    for(unsigned i=0;i<=process_count && woken<count;i++) {
        ScheduledTask *t=&schedule_tasks[i];
        if(!t->threads || (!shared && t!=hvf_task()))continue;
        for(unsigned j=0;j<t->threads->top && woken<count;j++) {
            Thread *th=&t->threads->thread[j];
            if(th->state!=2 || !(th->bitset&bits))continue;
            if(shared?th->futex_backing==shared && th->futex_offset==offset:!th->futex_backing && th->futex_address==address) {
                th->state=1;th->regs.gpr[0]=0;woken++;
            }
        }
    }
    if(woken)schedule_events++;                        /* idle workers pick the woken threads */
    return woken;
}
/* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: wake up to wake waiters on address and move up
   to move others to wait on target (musl's condition variables move their waiters
   to the mutex this way). Returns the number woken plus moved, as Linux does. */
static unsigned hvf_futex_requeue(uint64_t address,unsigned wake,unsigned move,uint64_t target) {
    unsigned woken=hvf_futex_wake(address,wake,UINT32_MAX),moved=0;
    size_t offset=0,target_offset=0;
    MemoryBacking *shared=hvf_futex_shared(address,&offset),*target_shared=hvf_futex_shared(target,&target_offset);
    if(!shared)moved=hvf_futex_wake_inplace(address,move,UINT32_MAX); /* woken rather than moved: they retry */
    for(unsigned i=0;i<=process_count && moved<move;i++) {
        ScheduledTask *t=&schedule_tasks[i];
        if(!t->threads || (!shared && t!=hvf_task()))continue;
        for(unsigned j=0;j<t->threads->top && moved<move;j++) {
            Thread *th=&t->threads->thread[j];
            if(th->state!=2)continue;
            if(shared?th->futex_backing==shared && th->futex_offset==offset:!th->futex_backing && th->futex_address==address) {
                th->futex_address=target;th->futex_backing=target_shared;th->futex_offset=target_offset;moved++;
            }
        }
    }
    return woken+moved;
}
static uint64_t hvf_thread_clone(void) {
    uint64_t flags=cpu->gpr[0];
    /* VM, FS, FILES, SIGHAND, THREAD required; SYSVSEM, SETTLS, PARENT_SETTID,
       CHILD_CLEARTID, CHILD_SETTID and DETACHED (ignored by Linux) are optional. */
    if((flags&0x10f00)!=0x10f00 || (flags&~UINT64_C(0x17d0f00)))return linux_error(95);
    if(!cpu->gpr[1] || (cpu->gpr[1]&15) || !lookup(cpu->gpr[1]-16,16,2))return linux_error(22);
    unsigned char *parent=(flags&0x100000)?lookup(cpu->gpr[2],4,2):NULL,*child=(flags&0x1000000)?lookup(cpu->gpr[4],4,2):NULL;
    if(((flags&0x100000) && !parent) || ((flags&0x1000000) && !child))return linux_error(14);
    if(!schedule_active)return linux_error(11);
    ThreadTable *p=hvf_threads_ensure();if(!p)return linux_error(12);
    unsigned i=0;while(i<THREAD_LIMIT && (p->thread[i].state || p->thread[i].worker))i++; /* a killed thread may still be leaving a worker */
    if(i==THREAD_LIMIT || hvf_next_tid>1048575)return linux_error(11);
    if(i>=p->top)p->top=i+1;
    Thread *t=&p->thread[i];uint64_t mask=*signal_mask_now();memset(t,0,sizeof(*t));
    t->regs=*cpu;t->regs.gpr[0]=0;t->regs.sp=cpu->gpr[1];if(flags&0x80000)t->regs.tls=cpu->gpr[3];
    t->regs.altstack_sp=t->regs.altstack_size=0; /* a new thread has no alternate signal stack */
    t->signal_mask=mask;t->epoch=io_epoch;    /* and inherits the creator's mask */
    t->tid=hvf_next_tid++;t->state=1;t->clear_tid=(flags&0x200000)?cpu->gpr[4]:0;
    if(parent)fs_pack(parent,0,t->tid,4);
    if(child)fs_pack(child,0,t->tid,4);
    schedule_events++;
    return t->tid;
}
/* Futex for the current thread. Returns the syscall result; *waiting when the
   thread now waits (state 2 until a wake, its deadline or a signal). */
static uint64_t hvf_futex(int *waiting) {
    unsigned op=(unsigned)cpu->gpr[1],command=op&127;*waiting=0;
    if(op&~511u)return linux_error(38);
    if(command!=0 && command!=1 && command!=3 && command!=4 && command!=9 && command!=10)return linux_error(38);
    if((op&256) && command!=9)return linux_error(38);
    if(cpu->gpr[0]&3)return linux_error(22);
    unsigned char *word=lookup(cpu->gpr[0],4,4);if(!word)return linux_error(14);
    if(command==3 || command==4) { /* x2: to wake, x3: to move, x4: the target futex, x5: the expected value */
        if((int32_t)cpu->gpr[2]<0 || (int32_t)cpu->gpr[3]<0 || (cpu->gpr[4]&3))return linux_error(22);
        if(!lookup(cpu->gpr[4],4,4))return linux_error(14);
        if(command==4 && le(word,4)!=(uint32_t)cpu->gpr[5])return linux_error(11);
        return hvf_futex_requeue(cpu->gpr[0],(unsigned)cpu->gpr[2],(unsigned)cpu->gpr[3],cpu->gpr[4]);
    }
    uint32_t bits=(command==9 || command==10)?(uint32_t)cpu->gpr[5]:UINT32_MAX;
    if(!bits)return linux_error(22);
    if(command==1 || command==10) {
        if((int32_t)cpu->gpr[2]<0)return linux_error(22);
        return hvf_futex_wake(cpu->gpr[0],(unsigned)cpu->gpr[2],bits);
    }
    uint64_t deadline=0;int realtime=(op&256)!=0;
    if(cpu->gpr[3]) {
        unsigned char *ts=lookup(cpu->gpr[3],16,4);if(!ts)return linux_error(14);
        uint64_t sec=le(ts,8),ns=le(ts+8,8);
        if((int64_t)sec<0 || ns>=1000000000 || sec>(UINT64_MAX-ns)/1000000000)return linux_error(22);
        deadline=sec*1000000000+ns;
        if(command==0) { uint64_t now=thread_time_ns(0);if(deadline>UINT64_MAX-now)return linux_error(22);deadline+=now; }
    }
    if(le(word,4)!=(uint32_t)cpu->gpr[2])return linux_error(11);
    if(cpu->gpr[3] && deadline<=thread_time_ns(realtime))return linux_error(110);
    if(!schedule_active)return linux_error(38);
    /* Even a single-threaded process waits (until its timeout, as on Linux). */
    ThreadTable *p=hvf_threads_ensure();if(!p)return linux_error(12);
    ScheduledTask *task=hvf_task();Thread *t=&p->thread[thread_now];
    if(job_control_enabled && ((task->signal_pending|t->signal_pending)&~t->signal_mask&~task->signal_ignored))return linux_error(4);
    t->state=2;t->futex_address=cpu->gpr[0];t->deadline_ns=deadline;t->realtime=(unsigned)realtime;t->bitset=bits;t->private_wait=(op&128)!=0;
    t->futex_backing=hvf_futex_shared(cpu->gpr[0],&t->futex_offset);
    *waiting=1;return 0;
}
/* A successful exec in a threaded process ends every other thread; the caller
   becomes the main thread. */
static void hvf_exec_threads(void) {
    ScheduledTask *t=hvf_task();hvf_main_clear_tid[process_pid]=0;hvf_views[process_pid].stale=HVF_STALE_NEW;
    t->futex_epoch=__atomic_add_fetch(&schedule_futex_epochs,1,__ATOMIC_RELAXED); /* in-place waiters of the old image wake no one */
    if(!t->threads)return;
    for(unsigned i=0;i<t->threads->top;i++)if((int)i!=thread_now)t->threads->thread[i].state=0;
    Thread *th=task_thread(t);th->tid=(unsigned)process_pid;th->clear_tid=0;
}
/* Thread-aware syscalls, before the shared Linux layer. */
static int hvf_thread_call(uint64_t call) {
    ScheduledTask *task=schedule_active?hvf_task():NULL;
    Thread *th=task?task_thread(task):NULL;
    unsigned tid=th?th->tid:(unsigned)process_pid;
    if(trace && (call==220 || call==178 || call==96 || call==98 || call==124 || call==93 || call==94))
        fprintf(stderr,"hvf thread pid=%" PRIu64 " tid=%u syscall %" PRIu64 " x0..3=[%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 "]\n",process_pid,tid,call,cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3]);
    if(call==220 && (cpu->gpr[0]&0x10000)) { cpu->gpr[0]=hvf_thread_clone();return -3; }
    if(call==178) { cpu->gpr[0]=tid;return -3; }
    if((call==130 || call==131) && job_control_enabled) {               /* tkill(tid,sig), tgkill(tgid,tid,sig) */
        uint64_t tgid=call==131?cpu->gpr[0]:process_pid,target=cpu->gpr[call==131?1:0],signal=cpu->gpr[call==131?2:1];
        if((int64_t)target<=0 || (int64_t)tgid<=0 || signal>64) { cpu->gpr[0]=linux_error(22);return -3; }
        if(tgid==process_pid && th) { cpu->gpr[0]=hvf_thread_signal(task,target,signal);return -3; }
        if(tgid==process_pid && target!=process_pid) { cpu->gpr[0]=linux_error(3);return -3; }
        if(tgid!=target && tgid!=process_pid) { cpu->gpr[0]=linux_error(3);return -3; }
        cpu->gpr[0]=guest_signal_process(target,signal);               /* this process, or another one */
        return -3;
    }
    if(call==96) {
        if(th)th->clear_tid=cpu->gpr[0];else hvf_main_clear_tid[process_pid]=cpu->gpr[0];
        cpu->gpr[0]=tid;return -3;
    }
    if(call==98) {
        int waiting=0;uint64_t result=hvf_futex(&waiting);
        cpu->gpr[0]=waiting?0:result;
        return waiting?-2:-3;
    }
    if(call==93 && th) { /* exit of one thread while others live */
        unsigned alive=0;for(unsigned i=0;i<task->threads->top;i++)alive+=(int)i!=thread_now && task->threads->thread[i].state;
        if(alive) {
            unsigned char *word=th->clear_tid?lookup(th->clear_tid,4,2):NULL;
            if(word) { fs_pack(word,0,0,4);(void)hvf_futex_wake(th->clear_tid,1,UINT32_MAX); }
            return THREAD_EXIT;
        }
    }
    /* membarrier: running sibling threads pass through an exception, a full barrier. */
    if(call==283 && th && cpu->gpr[0])hvf_siblings_out();
    return HVF_UNHANDLED;
}
/* An EL0 MRS of the ID register space sys_reg(3,0,0,CRm,op2) traps (ESR class 0x18);
   as Linux does (it sets HWCAP_CPUID), answer it: MIDR_EL1, MPIDR_EL1 with only its
   RES1 bit, and the AArch64 feature registers with only the fields Linux shows user
   space (no pointer authentication or BTI: the runtime does not enable them).
   ponytail: other fields, and registers this SDK cannot read (ISAR2, MMFR3), read 0
   where Linux shows their safe values. FEX picks its code generation from these. */
static int hvf_id_register(uint64_t esr,uint64_t *value) {
    unsigned op0=(unsigned)(esr>>20)&3,op2=(unsigned)(esr>>17)&7,op1=(unsigned)(esr>>14)&7,crn=(unsigned)(esr>>10)&15,crm=(unsigned)(esr>>1)&15;
    if(!(esr&1) || op0!=3 || op1 || crn)return 0;
    static const struct { unsigned crm,op2;hv_sys_reg_t reg;uint64_t visible; } ids[]={
        {0,0,HV_SYS_REG_MIDR_EL1,~UINT64_C(0)},
        {4,0,HV_SYS_REG_ID_AA64PFR0_EL1,UINT64_C(0x000F000F00FFFFFF)},   /* DIT, SVE, AdvSIMD, FP, EL3..EL0 */
        {4,1,HV_SYS_REG_ID_AA64PFR1_EL1,UINT64_C(0xF0)},                 /* SSBS */
        {6,0,HV_SYS_REG_ID_AA64ISAR0_EL1,UINT64_C(0xF0FFFFFFF0FFFFF0)},  /* all but TLB */
        {6,1,HV_SYS_REG_ID_AA64ISAR1_EL1,UINT64_C(0xF0FFF0FF00FFF00F)},  /* all but XS, SPECRES and pointer authentication */
        {7,0,HV_SYS_REG_ID_AA64MMFR0_EL1,UINT64_C(0xF000000000000000)},  /* ECV */
        {7,1,HV_SYS_REG_ID_AA64MMFR1_EL1,UINT64_C(0x0000F00000000000)},  /* AFP */
        {7,2,HV_SYS_REG_ID_AA64MMFR2_EL1,UINT64_C(0x0000000F00000000)}}; /* AT (LSE2) */
    *value=!crm && op2==5?UINT64_C(0x80000000):0;
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++)if(ids[i].crm==crm && ids[i].op2==op2) {
        hvf_check(hv_vcpu_get_sys_reg(hvf_vcpu,ids[i].reg,value),"get id register");*value&=ids[i].visible;
    }
    return 1;
}
/* A data abort for an unaligned access (an LDAXR, or an LSE2 atomic crossing 16
   bytes): SIGBUS, BUS_ADRALN, as Linux sends; FEX backpatches x86 atomics on it. */
#define HVF_ALIGNMENT(esr) (((esr)>>26)==0x24 && ((esr)&0x3f)==0x21)
/* Under the scheduler a guest fault is a SIGSEGV, SIGBUS or SIGILL for that process: its
   handler runs if it has one and has not blocked the signal (OpenSSL probes CPU
   features this way, JVMs and V8 catch faults on purpose); else the process ends. */
static int hvf_fault(unsigned signal,uint64_t pc,uint64_t address,unsigned ec) {
    if(schedule_active && schedule_tasks[process_pid-1].terminate_signal==9)return -1; /* OOM-killed: the fault was the page it could not get */
    if(schedule_active) {
        ScheduledTask *task=&schedule_tasks[process_pid-1];unsigned index=signal-1;uint64_t bit=UINT64_C(1)<<index;
        if(job_control_enabled && task->signal_handler[index] && !(*signal_mask_now()&bit) && !(task->signal_ignored&bit)) {
            /* ILL_ILLOPC at the instruction; BUS_ADRALN, and SEGV_ACCERR on a mapped page, else SEGV_MAPERR, at the address. */
            /* Per thread: JVM safepoints fault many threads at once, and each handler
               needs its own si_code and si_addr. */
            unsigned code=signal!=11?1u:(lookup(address,1,4) || lookup(address,1,1))?2u:1u;uint64_t where=signal==4?pc:address; /* ILL_ILLOPC, BUS_ADRALN */
            Thread *th=thread_current();
            if(th) { th->fault_signal=signal;th->fault_code=code;th->fault_addr=where; }
            else { task->signal_fault_code[index]=code;task->signal_fault_addr[index]=where; }
            if(th)th->signal_pending|=bit;else task->signal_pending|=bit;
            return -1; /* delivered before the instruction runs again (signal_deliver) */
        }
        /* Like a kernel's segfault line: always reported, so container logs explain a 139. */
        fprintf(stderr,"dockiva: process %" PRIu64 " killed by signal %u: fault at pc=0x%" PRIx64 " address=0x%" PRIx64 " (exception class 0x%x)\n",process_pid,signal,pc,address,ec);
        schedule_tasks[process_pid-1].terminate_signal=signal;return -1;
    }
    fprintf(stderr,"dockiva: guest fault at pc=0x%" PRIx64 " address=0x%" PRIx64 " (exception class 0x%x)\n",pc,address,ec);
    fail("Guest memory or instruction fault");return 125;
}
/* Right after a guest exit, without the runtime lock (the process's mm lock is
   held): serve the system call if it is one guest_syscall_unlocked takes, and say
   whether the guest can just resume. Changed tables are synced before it does. */
/* Whether signal_deliver would act now: a pending signal this thread does not block or ignore. */
static int signal_deliverable(void) {
    if(!job_control_enabled || !schedule_active)return 0;
    ScheduledTask *task=&schedule_tasks[process_pid-1];Thread *th=thread_current();
    return ((task->signal_pending|(th?th->signal_pending:0)) & ~*signal_mask_now() & ~task->signal_ignored)!=0;
}
/* nanosleep and relative clock_nanosleep shorter than a millisecond: sleep on the
   host, holding no lock. Through the scheduler a sleep lasts at least its poll
   granularity (1 ms), and Go sleeps microseconds at a time (usleep(3) while
   stealing work, usleep(20) in sysmon). A signal for the thread waits for the end
   of the sleep (under a millisecond); Linux would end the sleep with EINTR. */
#define HVF_SHORT_SLEEP_NS 1000000u
static int hvf_must_stop(ScheduledTask *t);
static int hvf_short_sleep(uint64_t call) {
    uint64_t x0=hvf_frame[0];
    if(call==115) {
        if(x0>1 || hvf_frame[1])return 0;                           /* REALTIME or MONOTONIC, relative */
        x0=hvf_frame[2];
    }
    const unsigned char *ts=lookup(x0,16,4);if(!ts)return 0;
    uint64_t sec=le(ts,8),ns=le(ts+8,8);
    if(sec || ns>=HVF_SHORT_SLEEP_NS || hvf_must_stop(schedule_active?hvf_task():NULL))return 0;
    ScheduledTask *task=schedule_active?hvf_task():NULL;
    struct timespec pause={0,(long)ns};
    mm_unlock(task);(void)nanosleep(&pause,NULL);mm_lock(task);
    hvf_frame[0]=0;hvf_resume_el0(hvf_frame[HF_ELR],hvf_frame[HF_SPSR]);return 1;
}
/* futex without the runtime lock (the process's mm lock is held), for private
   futexes of a threaded process: WAKE when no thread of the process is parked in
   the scheduler on the address (parking takes this mm lock too, so none appears
   meanwhile), and WAIT in place while workers are idle, for up to
   HVF_FUTEX_INPLACE_NS (or its own timeout). Returns 0 for the locked path (which
   rereads the call): anything else, or a wait that did not end in time (it parks
   then, after reading the word again). A signal for an in-place waiter is taken
   when that wait ends. */
#define HVF_FUTEX_INPLACE_NS 200000u
static int hvf_futex_unlocked(void) {
    if(!schedule_active || trace)return 0;
    ScheduledTask *task=hvf_task();ThreadTable *p=task->threads;
    unsigned op=(unsigned)cpu->gpr[1],command=op&127;uint64_t address=cpu->gpr[0];size_t offset=0;
    if(!p || (op&~255u) || (command!=0 && command!=1 && command!=9 && command!=10) || (address&3))return 0; /* not FUTEX_CLOCK_REALTIME */
    uint32_t bits=command>=9?(uint32_t)cpu->gpr[5]:UINT32_MAX;
    const unsigned char *word=lookup(address,4,4);
    if(!bits || !word || hvf_futex_shared(address,&offset))return 0;
    if(command==1 || command==10) {
        if((int32_t)cpu->gpr[2]<0)return 0;
        for(unsigned j=0;j<p->top;j++) {
            const Thread *th=&p->thread[j];
            if(th->state==2 && !th->futex_backing && th->futex_address==address && (th->bitset&bits))return 0;
        }
        cpu->gpr[0]=hvf_futex_wake_inplace(address,(unsigned)cpu->gpr[2],bits);return 1;
    }
    uint64_t now=thread_time_ns(0),deadline=0,wait=HVF_FUTEX_INPLACE_NS;
    if(cpu->gpr[3]) {
        const unsigned char *ts=lookup(cpu->gpr[3],16,4);if(!ts)return 0;
        uint64_t sec=le(ts,8),ns=le(ts+8,8);
        if((int64_t)sec<0 || ns>=1000000000 || sec>=UINT64_C(1)<<32)return 0;
        deadline=sec*1000000000+ns+(command==0?now:0);
        if(deadline<=now)return 0;
        if(deadline-now<wait)wait=deadline-now;
    }
    if(!(schedule_idle || schedule_polling) || hvf_must_stop(task))return 0;
    HvfFutexWaiter w;memset(&w,0,sizeof(w));
    w.pid=process_pid;w.epoch=task->futex_epoch;w.address=address;w.bits=bits;
    if(pthread_cond_init(&w.cond,NULL))return 0;
    unsigned b=hvf_futex_bucket(w.pid,address);
    pthread_mutex_lock(&hvf_futex_buckets[b].lock);
    if(__atomic_load_n((const uint32_t *)word,__ATOMIC_SEQ_CST)!=(uint32_t)cpu->gpr[2]) {
        pthread_mutex_unlock(&hvf_futex_buckets[b].lock);pthread_cond_destroy(&w.cond);
        cpu->gpr[0]=linux_error(11);return 1;
    }
    w.next=hvf_futex_buckets[b].head;hvf_futex_buckets[b].head=&w;
    mm_unlock(task);
    struct timespec pause={0,(long)wait};
    (void)pthread_cond_timedwait_relative_np(&w.cond,&hvf_futex_buckets[b].lock,&pause);
    if(!w.woken)for(HvfFutexWaiter **link=&hvf_futex_buckets[b].head;*link;link=&(*link)->next)if(*link==&w) { *link=w.next;break; }
    pthread_mutex_unlock(&hvf_futex_buckets[b].lock);
    mm_lock(task);pthread_cond_destroy(&w.cond);
    PROFILE_ADD(profile_futex_inplace,1);
    if(w.woken) { PROFILE_ADD(profile_futex_inplace_woken,1);cpu->gpr[0]=0;return 1; }
    if(deadline && thread_time_ns(0)>=deadline) { cpu->gpr[0]=linux_error(110);return 1; }
    return 0;
}
static int hvf_unlocked_exit(void) {
    if(!hvf_cpu->framed)return 0;                                             /* only the frame's exits */
    uint64_t esr=0,elr=0,spsr=0,far=0;
    hvf_exception(&esr,&elr,&spsr,&far);
    unsigned ec=(unsigned)(esr>>26);
    if(ec==0x24 && (esr&(UINT64_C(1)<<6)) && !HVF_ALIGNMENT(esr)) {
        if(profiling)__atomic_fetch_add(&profile_write_faults,1,__ATOMIC_RELAXED);
        /* A write to a copy-on-write or not yet charged page: the same handling as
           under the lock. A genuine fault goes to the locked path. */
        uint64_t layout=address_space->layout;
        if(!hvf_spurious_write(far)) {
            if(!lookup(far,1,2))return 0;
            address_space_touched(address_space);
            if(address_space->layout==layout && hvf_view==&hvf_views[process_pid] && hvf_view_matches(address_space)) {
                unsigned around=hvf_fault_around(address_space,far);PROFILE_ADD(profile_fault_around,around);
                hvf_remap_host_page(address_space,far,1+around);hvf_view->generation=address_space->generation;
            } else hvf_views[process_pid].stale=HVF_STALE_COW;
        }
        hvf_resume_el1=hvf_fault_resume(elr,spsr);return 1;
    }
    if(ec!=0x15 || seccomp_active())return 0;                                 /* SVC from EL0; filtered calls take the locked path */
    uint64_t call=hvf_frame[8];
    if((call==101 || call==115) && hvf_short_sleep(call))return 1;
    /* sched_yield while workers are idle: no runnable thread waits for a CPU (an idle
       worker would take it), so, as on Linux, it returns at once. */
    if(call==124 && (schedule_idle || schedule_polling)) {
        PROFILE_ADD(profile_unlocked_calls,1);
        if(profiling)__atomic_fetch_add(&profile_syscalls[124],1,__ATOMIC_RELAXED);
        hvf_frame[0]=0;hvf_resume_el0(hvf_frame[HF_ELR],hvf_frame[HF_SPSR]);return 1;
    }
    if(call==98) {
        hvf_get_regs(1,elr,spsr);
        if(!hvf_futex_unlocked())return 0;
        PROFILE_ADD(profile_unlocked_calls,1);
        if(profiling)__atomic_fetch_add(&profile_syscalls[98],1,__ATOMIC_RELAXED);
        hvf_put_regs();return 1;
    }
    int memory=call==222 || call==215 || call==226 || call==233 || call==214;
    if(!memory && call!=22 && call!=25 && call!=57 && call!=56 && call!=34 && call!=35 && call!=38 && call!=48 && call!=439 && call!=78 && call!=88 && call!=132 && call!=134 && call!=135 && call!=172 && call!=178 && call!=79 && call!=80 && (call<62 || call>68) && call!=82 && call!=83 && call!=206 && call!=207 && call!=291 && call!=221)return 0; /* 221: execve builds its image (exec_build) */
    hvf_get_regs(1,elr,spsr);
    if(!(memory?guest_memory_unlocked():guest_syscall_unlocked()))return 0;   /* the locked path rereads the same exit */
    PROFILE_ADD(profile_unlocked_calls,1);
    if(profiling && call<512)__atomic_fetch_add(&profile_syscalls[call],1,__ATOMIC_RELAXED);
    hvf_put_regs();return 1;
}
/* An unlocked call would block on sockets. While other workers are idle, wait for
   them here, briefly and without any lock, instead of a round trip through the
   scheduler under the runtime lock (a server's next request often arrives within
   microseconds). Returns whether one became ready; the caller then retries the call.
   Called and returns with the process's mm lock held. */
#define HVF_DIRECT_WAIT_MS 2
static int hvf_must_stop(ScheduledTask *t);
static int hvf_direct_wait(ScheduledTask *task) {
    unsigned count=net_unlocked_wait.count;net_unlocked_wait.count=0;
    if(!count || !net_unlocked_wait.timeout || !(schedule_idle || schedule_polling) || hvf_must_stop(task))return 0;
    int budget=net_unlocked_wait.timeout<HVF_DIRECT_WAIT_MS?(int)net_unlocked_wait.timeout:HVF_DIRECT_WAIT_MS;
    mm_unlock(task);
    int ready=poll(net_unlocked_wait.fds,count,budget);
    mm_lock(task);
    return ready>0 && !hvf_must_stop(task);
}
/* The worker must leave guest code: the run or the process is ending, the process
   stops, this thread was ended, or a signal awaits delivery. Read without the
   runtime lock: whoever sets these interrupts workers that are in guest code. */
static int hvf_must_stop(ScheduledTask *t) {
    if(!t)return 0;
    if(schedule_done || t->exit_pending || t->terminate_signal || t->stop_signal || t->state==3)return 1;
    Thread *th=task_thread(t);
    if(th && !th->state)return 1;
    if(!job_control_enabled)return 0;
    uint64_t mask=th?th->signal_mask:t->signal_mask;
    return ((t->signal_pending|(th?th->signal_pending:0))&~mask&~t->signal_ignored)!=0;
}
/* Take the runtime lock if it is free now (never blocks, so it may be tried with an mm lock held). */
static int runtime_trylock(void) {
    if(!worker_threads)return 1;
    if(pthread_mutex_trylock(&runtime_mutex))return 0;
    runtime_held=1;return 1;
}
/* One CPU slice for the current thread, with run()'s contract: a syscall result
   other than "continue" (-3) is returned; preemption yields (-1). Called with the
   runtime lock held (the mm lock is taken only when needed: not while the runtime
   lock waits on it), returns with both held. Guest code runs
   with neither; system calls served without the runtime lock hold the mm lock. */
static int hvf_slice_run(void) {
    if(!worker->backend)worker->backend=hvf_cpu_new(0); /* a worker's first slice, on its own thread */
    __atomic_store_n(&hvf_cpu->preempt,0,__ATOMIC_RELAXED); /* a full slice from now */
    hvf_regs_invalidate(); /* the scheduler handed over a complete CPU state */
    hvf_put_regs();
    ScheduledTask *task=schedule_active?hvf_task():NULL;
    /* fast: the vCPU was resumed in place (write fault, EL1 preemption); cpu is not
       current, so signal delivery waits for the next full exit. in_vector: it must
       first run on into its HVC exit. */
    int fast=0,in_vector=0,mm_held=0;
    for(;;) {
        if(job_control_enabled && schedule_active && !fast && signal_deliverable()) {
            if(!mm_held) { mm_lock(task);mm_held=1; }
            hvf_fp_pull(); /* a signal frame saves and restores the whole CPU state */
            signal_deliver(0);
            if(task->terminate_signal || task->stop_signal)return -1;
            hvf_put_regs();
        }
        fast=0;
        Worker *self=worker;hv_vcpu_t vcpu=hvf_vcpu;
        hv_return_t ran=HV_SUCCESS;int stopped=0;
        if(mm_held)mm_unlock(task);
        runtime_unlock();                              /* guest code runs in parallel with other workers */
        mm_lock(task);mm_held=1;
        for(;;) {
            /* in_guest is set under the mm lock, so a sibling holding it sees every
               worker of this process that may enter guest code. */
            atomic_store(&self->in_guest,1);atomic_thread_fence(memory_order_seq_cst);
            if(!in_vector && (hvf_must_stop(task) || (schedule_active && __atomic_exchange_n(&hvf_cpu->preempt,0,__ATOMIC_ACQ_REL)))) {
                atomic_store(&self->in_guest,0);stopped=1;break;  /* (the guest's registers are in the frame or the vCPU) */
            }
            in_vector=0;
            hvf_sync_memory(address_space);
            mm_unlock(task);
            ran=hv_vcpu_run(vcpu);
            atomic_store(&self->in_guest,0);
            hvf_cpu->framed=ran==HV_SUCCESS && hvf_exit->reason==HV_EXIT_REASON_EXCEPTION && (hvf_exit->exception.syndrome>>26)==0x16 && (hvf_exit->exception.syndrome&0xffff)==4; /* hvc #4 */
            mm_lock(task);
            if(!worker_threads || ran!=HV_SUCCESS || hvf_must_stop(task))break;
            hvf_resume_el1=0;
            if(!hvf_unlocked_exit() && !(hvf_direct_wait(task) && hvf_unlocked_exit()))break;
            in_vector=hvf_resume_el1;
        }
        /* The runtime lock comes before the mm lock. Waiting for the mm lock with the
           runtime lock held (a sibling thread in a long unlocked call) stalls every
           worker, so take the runtime lock while still holding mm when it is free,
           and otherwise wait for each lock while holding neither. */
        for(unsigned tries=0;!runtime_trylock();tries++) {
            mm_unlock(task);runtime_lock(self);
            if(tries>=2 || !schedule_active || !pthread_mutex_trylock(&task->mm)) { if(tries>=2)mm_lock(task);break; }
            runtime_unlock();mm_lock(task);
        }
        if(stopped) { hvf_get_regs(0,0,0);return -1; }
        hvf_check(ran,"hv_vcpu_run");
        if(task && (task->exit_pending || (task_thread(task) && !task_thread(task)->state)))return -1; /* ended meanwhile */
        if(hvf_exit->reason==HV_EXIT_REASON_CANCELED || hvf_exit->reason==HV_EXIT_REASON_VTIMER_ACTIVATED) {
            /* A preemption can land between an EL0 exception and the vector's HVC.
               The vCPU is then at EL1 in the vector page, not in guest code: let it
               finish into its HVC exit instead of saving an EL1 PC as guest state. */
            uint64_t mode=0;hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_CPSR,&mode),"get cpsr");
            if(!schedule_active || (mode&0xf)) { fast=in_vector=1;continue; }
            hvf_get_regs(0,0,0);return -1;
        }
        uint64_t syndrome=hvf_exit->exception.syndrome;
        PROFILE_ADD(profile_locked_exits,1);
        if(hvf_exit->reason==HV_EXIT_REASON_EXCEPTION && ((syndrome>>26)==0x20 || (syndrome>>26)==0x24) && (syndrome&0x3c)==0x4) {
            /* A stage-2 translation fault: the tables still name a backing that was
               just freed (a sibling thread's copy-on-write replaced it, and this vCPU
               ran before the rebuild reached it). Rebuild this process's tables and
               retry the instruction; the same fault again on unchanged memory is a bug. */
            static _Thread_local uint64_t retry_pc,retry_va,retry_generation;
            uint64_t pc=0;hvf_check(hv_vcpu_get_reg(hvf_vcpu,HV_REG_PC,&pc),"get pc");
            if(pc!=retry_pc || hvf_exit->exception.virtual_address!=retry_va || address_space->generation!=retry_generation) {
                retry_pc=pc;retry_va=hvf_exit->exception.virtual_address;retry_generation=address_space->generation;
                hvf_views[process_pid].stale=HVF_STALE_NEW;hvf_views[process_pid].generation=UINT64_MAX;
                fast=1;continue;
            }
        }
        if(hvf_exit->reason!=HV_EXIT_REASON_EXCEPTION || (syndrome>>26)!=0x16) {
            uint64_t pc=0;hv_vcpu_get_reg(hvf_vcpu,HV_REG_PC,&pc);
            fprintf(stderr,"dockiva: unexpected hypervisor exit: reason %u syndrome 0x%llx address 0x%llx pc 0x%llx\n",(unsigned)hvf_exit->reason,
                    (unsigned long long)hvf_exit->exception.syndrome,(unsigned long long)hvf_exit->exception.virtual_address,(unsigned long long)pc);
            fail("Unexpected hypervisor exit");
        }
        uint64_t esr=0,elr=0,spsr=0,far=0;
        hvf_exception(&esr,&elr,&spsr,&far);
        unsigned ec=(unsigned)(esr>>26);
        if(ec==0x15) { /* SVC from EL0 */
            hvf_get_regs(1,elr,spsr);
            uint64_t call=cpu->gpr[8],flags=cpu->gpr[0];unsigned execs=process_exec_count;
            int fork=call==220 && !(flags&0x10000);
            int exec=call==221 || call==281;                          /* execve, execveat */
            if(call==220 || exec || call==139)hvf_fp_pull(); /* clone/fork copy, exec and sigreturn replace the whole CPU state */
            int denied=seccomp_check();
            int result=denied?denied:hvf_thread_call(call);
            if(profiling && result!=HVF_UNHANDLED && call<512)__atomic_fetch_add(&profile_syscalls[call],1,__ATOMIC_RELAXED);   /* served here, not by the dispatcher */
            if(result==HVF_UNHANDLED) {
                /* Sibling threads stay out of guest code while their memory is
                   snapshotted or replaced. */
                if(fork || exec)hvf_siblings_out();
                result=guest_syscall();
                if((call>=143 && call<=147) || call==149 || call==277)hvf_fast_push(); /* set*id, seccomp: sibling threads' answers change */
                if(result==-3 && fork && (int64_t)cpu->gpr[0]>0) {     /* the child starts single-threaded, on new tables */
                    hvf_main_clear_tid[cpu->gpr[0]]=0;hvf_views[cpu->gpr[0]].stale=HVF_STALE_NEW;
                    if(fork_shared_private)hvf_views[process_pid].stale=HVF_STALE_COW; /* the fork shared this process's private pages */
                }
                if(exec && process_exec_count!=execs)hvf_exec_threads();
            }
            if(result!=-3) {
                if(result==-2 && !schedule_active)fail("Blocking syscalls need the scheduler (--process-tree)");
                return result;
            }
            /* The scheduler asked for a switch (vfork parent, sched_yield): yield now,
               as the interpreter does at its next instruction. */
            if(schedule_active && !schedule_slice)return -1;
            hvf_put_regs();
            continue;
        }
        if(ec==0x24 && (esr&(UINT64_C(1)<<6)) && !HVF_ALIGNMENT(esr)) {
        if(profiling)__atomic_fetch_add(&profile_write_faults,1,__ATOMIC_RELAXED);
            /* Write to a read-only view: lookup() detaches a copy-on-write page when the
               guest mapping is writable; a genuine read-only page stays a fault. */
            uint64_t layout=address_space->layout;
            if(hvf_spurious_write(far)) { in_vector=hvf_fault_resume(elr,spsr);fast=1;continue; }
            if(lookup(far,1,2)) {
                /* The page is private (and charged) now: remap its host page writable.
                   A new backing (a fork snapshot) changes the layout: rebuild then. */
                address_space_touched(address_space);
                if(address_space->layout==layout && hvf_view==&hvf_views[process_pid] && hvf_view_matches(address_space)) {
                    unsigned around=hvf_fault_around(address_space,far);PROFILE_ADD(profile_fault_around,around);
                    hvf_remap_host_page(address_space,far,1+around);hvf_view->generation=address_space->generation;
                } else hvf_views[process_pid].stale=HVF_STALE_COW;
                in_vector=hvf_fault_resume(elr,spsr);fast=1;continue;
            }
        }
        /* A read or instruction fetch that the address space allows (lookup: 4 read,
           1 execute) faulted: the tables are stale, which is the runtime's fault, not
           the guest's. Rebuild them and retry once; report it, so it can be found. */
        if((ec==0x24 && !(esr&(UINT64_C(1)<<6)) && !HVF_ALIGNMENT(esr) && lookup(far,1,4)) || (ec==0x20 && lookup(far,1,1))) {
            static _Thread_local uint64_t stale_pc,stale_far,stale_generation;static int reported;
            if(elr!=stale_pc || far!=stale_far || address_space->generation!=stale_generation) {
                stale_pc=elr;stale_far=far;stale_generation=address_space->generation;
                if(!reported) { reported=1;fprintf(stderr,"dockiva: stale page tables at pc=0x%" PRIx64 " address=0x%" PRIx64 "; rebuilt (please report)\n",elr,far); }
                hvf_views[process_pid].stale=HVF_STALE_NEW;hvf_views[process_pid].generation=UINT64_MAX;
                hvf_resume_el0(elr,spsr);fast=1;continue;
            }
        }
        hvf_get_regs(1,elr,spsr);
        uint64_t id;
        if(ec==0x18 && hvf_id_register(esr,&id)) {                              /* MRS of an ID register: emulated */
            unsigned rt=(unsigned)(esr>>5)&31;if(rt<31)cpu->gpr[rt]=id;
            cpu->pc+=4;hvf_put_regs();continue;
        }
        if(ec==0x22 || ec==0x26)far=elr;                                       /* PC or SP alignment */
        return hvf_fault(HVF_ALIGNMENT(esr) || ec==0x22 || ec==0x26?7:ec==0x24 || ec==0x20?11:4,elr,far,ec);
    }
}
/* The scheduler's CPU slice: whatever happens, hand back a complete CPU state. */
static int hvf_slice(void) {
    ScheduledTask *task=schedule_active?hvf_task():NULL;
    int result=hvf_slice_run();
    hvf_fp_pull();mm_unlock(task);
    return result;
}
static int hvf_run(uint64_t steps) {
    hvf_init();hvf_active=1;
    return run(steps); /* the scheduler (if enabled) calls run(), which slices natively */
}
