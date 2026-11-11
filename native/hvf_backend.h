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
/* Stage-1 tables of every process: 131072 x 4 KiB of guest-physical space, host pages
   only as tables are used (freed ones are reused first). FEX maps about 5 GiB of
   mostly untouched memory, about 2800 tables per x86 process (x86.go). */
#define HVF_TABLE_BYTES (512u<<20)
#define HVF_TABLES (HVF_TABLE_BYTES/4096)
#define HVF_CODE_BYTES (16u<<10)                    /* one host page: vectors and EL1 routines */
#define HVF_FRAME_BYTES 512u                        /* each vCPU's register frame (WORKER_LIMIT of them) */
#define HVF_SYSTEM_BYTES (HVF_CODE_BYTES+WORKER_LIMIT*HVF_FRAME_BYTES) /* code (read, execute), then frames (read, write) */
#define HVF_SYSTEM_VA UINT64_C(0xffffffff0000)      /* top of the 48-bit user range; EL1-only */
#define HVF_FRAME_VA (HVF_SYSTEM_VA+HVF_CODE_BYTES)
/* Each process's descriptor page (fd_mirror, net.h), mapped EL1-only at HVF_FDS_VA in
   its own tables; guest-physical space right after the system pages. */
#define HVF_FDS_VA (HVF_FRAME_VA+WORKER_LIMIT*HVF_FRAME_BYTES)
#define HVF_FDS_BYTES ((((PROCESS_LIMIT+2)*4096u)+16383u)&~16383u)
/* EL1 handlers (hvf_el1_guest.c, copied to HVF_EL1_CODE of the code page) and what they
   read, in the upper half of the address space (TTBR1, EL1-only, the same for every
   process): the context page, each vCPU's stack, then the lookup cache's counters
   (at 2 MiB) and entries (at 4 MiB), the process block (32 MiB) and the pack's index
   (192 MiB), shared with the runtime. */
#include "hvf_el1.h"
#include "hvf_el1_code.h"
#define HVF_EL1_VA UINT64_C(0xffff000000000000)
#define HVF_EL1_IPA (HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES+HVF_FDS_BYTES)
#define HVF_EL1_BYTES (UINT64_C(320)<<20)
#define HVF_EL1_BLOCK (UINT64_C(32)<<20)             /* proc_block (fs.h) */
#define HVF_EL1_PACK (UINT64_C(192)<<20)             /* the pack's index (pack.h) */
#define HVF_EL1_HEAD (2u<<20)                       /* context page, stacks, then (at 1 MiB) the upper half's tables */
_Static_assert(HVF_EL1_BLOCK+((sizeof(ProcBlock)+16383)&~(uint64_t)16383)<=HVF_EL1_PACK,"the process block outgrew its room in the EL1 region");
#define HVF_EL1_STACK 8192u
#define HVF_EL1_TABLES (1u<<20)
#define HVF_EL1_STUB 0xc00u                         /* in the code page: the vector's way in */
#define HVF_EL1_CODE 0x1000u
_Static_assert(HVF_EL1_CODE+sizeof(hvf_el1_code)<=(16u<<10),"EL1 code outgrew the code page");
static unsigned char *hvf_el1_head;static int hvf_el1_ready;static unsigned hvf_el1_mounts[2];
#define HVF_FIXED_IPA (HVF_EL1_IPA+HVF_EL1_BYTES) /* backings go above */
#define HVF_APPEND_OFFSET 0xbfcu                   /* fd_mirror_append, read by the vector */
#define HVF_FLUSH_OFFSET 0x800u
#define HVF_SAVE_OFFSET 0x900u
#define HVF_RESTORE_OFFSET 0x968u
#define HVF_INVALIDATE_OFFSET 0x9c4u
#define HVF_FLUSH_RESTORE_OFFSET 0xa00u
/* Frame words (SP_EL1 points at the vCPU's frame): x0..x30, then these. HF_FAST:
   the EL1 vector's own answers to syscalls 172..178, then HF_FDS: HVF_FDS_VA while it
   answers fcntl and epoll_ctl from the process's descriptor page, else 0 (hvf_fast_publish).
   HF_STAT: HVF_EL1_VA while the EL1 handlers may answer, else 0; HF_STACK: this vCPU's
   EL1 stack; HF_SVC_*: the call's ELR, SPSR and ESR while they run (a fault in them
   restores these and the call exits as before). */
enum { HF_SP=31,HF_TLS,HF_ELR,HF_SPSR,HF_ESR,HF_FAR,HF_TLBI_VA,HF_TLBI_PAGES,HF_FLUSH,HF_FAST,HF_FDS=HF_FAST+7,HF_STAT,HF_STACK,HF_SVC_ELR,HF_SVC_SPSR,HF_SVC_ESR,
       HF_TASK,HF_MM,     /* the process's descriptor tables in proc_block and its mm lock, or UINT64_MAX (hvf_el1_guest.c) */
       HF_HELD };         /* the mm lock word an EL1 handler holds, or 0: a fault in the handler releases it */
_Static_assert(HF_FAST==40 && HF_FDS==47 && HF_TASK==53 && HF_MM==54 && HF_HELD==55,"frame words hvf_el1_guest.c reads");
_Static_assert((HF_SVC_ESR+1)*8<=512,"frame words outgrew the frame");
#define HVF_BLOCKS 16384                           /* stage-2 blocks: a few backings for each of up to 1024 processes */
#define HVF_VIEWS (PROCESS_LIMIT+2)                 /* indexed by pid */
static unsigned char *hvf_tables,*hvf_system;static unsigned hvf_frames_used;
static unsigned short hvf_table_owner[HVF_TABLES];  /* 0 free, else the pid whose tables use it */
static uint32_t hvf_table_link[HVF_TABLES];        /* the next table of the same view, plus one (0: its last) */
/* Freed tables are reused first, warm in the caches and their host pages in memory.
   Taken in turn through the pool instead, a new runtime faulted in its 64 MiB a host
   page per four tables until it wrapped (a Go fork's 40-table copy 18-22 us against 7;
   Node's 80). But a walk cache may still point into a freed table (the system pages
   are mapped alike, and global, in every view): reused at once, such a walk read
   another view's entries (an EL1 routine faulted). So a freed table first waits in
   hvf_table_waiting until a full TLB invalidation begun after its free has finished
   (hvf_flush; every fork that holds memory runs one), then joins hvf_table_freed, the
   latest last. Then tables never used (hvf_table_fresh); with the pool exhausted, the
   old scan takes any free one. */
static uint32_t hvf_table_freed[HVF_TABLES],hvf_table_waiting[HVF_TABLES],hvf_table_waiting_since[HVF_TABLES];
static unsigned hvf_table_nfreed,hvf_table_fresh,hvf_table_wait_head,hvf_table_nwaiting,hvf_table_hint;
static uint32_t hvf_flushes_begun,hvf_flushes_done;  /* full invalidations begun, and the number of the latest one finished */
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
    unsigned table_chain;                     /* its first table plus one, the rest through hvf_table_link */
    Segment seg[SEGMENT_LIMIT+1];unsigned nseg;uint64_t map_capacity;
    MemoryBacking *brk_backing,*map_backing;uint64_t brk_base,brk_pages;
    uint64_t layout,arena_rev,generation;
    unsigned asid,asid_generation;            /* valid while asid_generation is hvf_asid_generation */
    struct { MemoryBacking *backing;size_t offset,size; } moved[8];unsigned nmoved; /* overlay ranges a host write copied, to map again (hvf_backing_moved) */
    uint64_t unheld_generation,unheld_epoch;int unheld; /* hvf_holding found none of its memory held at that generation and hold epoch */
} HvfView;
static HvfView hvf_views[HVF_VIEWS];
/* The view of the current process's memory: its own, or (a vfork child of a threaded
   process, sharing its memory) the owner's, which every sharer's vCPU runs on. */
static unsigned hvf_view_pid(void) { return schedule_active?mm_id((unsigned)process_pid-1):(unsigned)process_pid; }
#define hvf_own_view (&hvf_views[hvf_view_pid()])
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
    uint64_t ipa=hvf_ipa_top?hvf_ipa_top:HVF_FIXED_IPA;
    if(ipa+size>hvf_ipa_limit) {                                            /* then first fit */
        ipa=HVF_FIXED_IPA;
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
/* The guest-physical address a process's tables give byte offset of backing: an
   overlay's page it has not copied is its base's (mapped read-only: a write copies it),
   whose stage-2 entries its parent's use keeps warm; else its own. */
static uint64_t hvf_page_ipa(MemoryBacking *backing,size_t offset) {
    if(backing->base && backing_inherits(backing,offset/backing->page_size))return hvf_ipa_at(BACKING_BASE(backing),offset);
    return hvf_ipa_at(backing,offset);
}
static void hvf_block_unmap(unsigned i) {
    hvf_check(hv_vm_unmap(hvf_blocks[i].ipa,hvf_blocks[i].size),"hv_vm_unmap");
    hvf_blocks[i]=hvf_blocks[--hvf_nblocks];if(!hvf_nblocks)hvf_ipa_top=0;
    __atomic_fetch_add(&hvf_ipa_epoch,1,__ATOMIC_RELEASE);
}
/* Fork hold (backing_hold_hook, MemoryBacking.held): the parent keeps a
   backing whose snapshot its child took, but must not write it while the snapshot
   lives (frozen). Its own page tables lose write permission on the backing's owned
   pages (those it maps writable: a few thousand entries, where a copy-on-write pass
   rewrote every mapped page); a write fault on it waits for the snapshot to go
   (hvf_held_sleep), and the first one after that gives the permission back the same way
   (hvf_hold). Only the process's own view maps the backing (it was not shared).
   Hypervisor-level (stage-2) protection did the same with no table pass, but
   hv_vm_protect took 30-150 us per 256 MiB block each way. */
static unsigned hvf_held_live;                          /* backings held now (atomic) */
static uint64_t hvf_hold_epoch;                         /* bumped after a backing becomes held (atomic): hvf_holding's cached answers expire */
static uint64_t hvf_held_thaws;                         /* thaws so far, under hvf_held_lock */
static pthread_mutex_t hvf_held_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hvf_held_cond=PTHREAD_COND_INITIALIZER;
static void hvf_held_wake(void) { pthread_mutex_lock(&hvf_held_lock);hvf_held_thaws++;pthread_cond_broadcast(&hvf_held_cond);pthread_mutex_unlock(&hvf_held_lock); }
static uint64_t *hvf_leaf(HvfView *v,uint64_t va,int create);
static void hvf_flush(void);
/* Clear (protect) or give back (0, pages its process owns alone) write permission on
   the view's pages of backing's owned host pages it maps writable. 1 if any changed. */
static int hvf_hold(HvfView *v,const GuestAddressSpace *space,MemoryBacking *backing,int protect) {
    if(!v->root)return 0;
    int changed=0;size_t ps=backing->page_size;
    for(unsigned r=0;r<space->nseg+2;r++) {
        uint64_t base,size;size_t offset;unsigned flags;int arena=0;
        if(r==0) { if(space->map_backing!=backing || !space->map_flags)continue;base=MAP_BASE;size=space->map_capacity;offset=0;flags=6;arena=1; }
        else if(r==1) { if(space->brk_backing!=backing)continue;base=space->brk_base;size=space->brk_pages;offset=0;flags=6; }
        else { const Segment *g=&space->seg[r-2];if(g->backing!=backing)continue;base=g->base;size=g->size;offset=g->offset;flags=g->flags; }
        if(!(flags&2) || (flags&SEG_SHARED))continue;
        size_t end=offset+size,last=(end+ps-1)/ps;if(last>backing->used)last=backing->used;
        for(size_t h=backing_next(backing,offset/ps,last);h<last;h=backing_next(backing,h+1,last)) {
            const BackingPage *p=backing->pages[h];
            if(!protect && p->refs!=1)continue;
            for(size_t off=h*ps;off<(h+1)*ps;off+=4096) {
                if(off<offset || off>=end)continue;
                uint64_t va=base+(off-offset);
                if(arena && (space->map_flags[(va-MAP_BASE)/4096]&(2|MAP_RESERVED))!=2)continue;
                uint64_t *leaf=hvf_leaf(v,va,0);if(!leaf)continue;
                uint64_t *slot=&leaf[(va>>12)&511];
                if(!(*slot&1) || !(*slot&(UINT64_C(1)<<7))==!protect)continue;   /* unmapped, or already so */
                *slot^=UINT64_C(1)<<7;changed=1;
            }
        }
    }
    return changed;
}
static void hvf_backing_hold(MemoryBacking *backing,int protect) {
    if(!protect) { hvf_held_wake();return; }                    /* thawed: the parent's next write fault gives permission back */
    /* Every fork's pass, held already or not: pages written since the last one (the
       parent's writes after a thaw remap a page at a time when the locked path serves
       them) are writable again. */
    if(!__atomic_exchange_n(&backing->held,1,__ATOMIC_ACQ_REL))__atomic_add_fetch(&hvf_held_live,1,__ATOMIC_ACQ_REL);
    __atomic_add_fetch(&hvf_hold_epoch,1,__ATOMIC_ACQ_REL);         /* after held: a reader that sees the new epoch sees it held */
    if(hvf_hold(hvf_own_view,address_space,backing,1))hvf_flush();
    PROFILE_ADD(profile_holds,1);
}
/* backing_release_hook: no live process maps a freed backing, and the tables of
   a process that used to are rebuilt (its layout changed) before it runs again.
   Its blocks are unmapped without hvf_mutex (every table build takes it): meanwhile
   they have no host (no lookup finds them) but keep their guest-physical space. */
static void hvf_backing_gone(MemoryBacking *backing) {
    const unsigned char *host=backing_data(backing);
    if(__atomic_exchange_n(&backing->held,0,__ATOMIC_ACQ_REL)) { __atomic_sub_fetch(&hvf_held_live,1,__ATOMIC_ACQ_REL);hvf_held_wake(); }   /* freed while held */
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
/* Table i, freed when since full invalidations had begun, waits (under hvf_mutex). */
static void hvf_table_wait(unsigned i,uint32_t since) {
    if(hvf_table_nwaiting==HVF_TABLES)return;                         /* (only duplicates: the scan finds it) */
    unsigned at=(hvf_table_wait_head+hvf_table_nwaiting++)%HVF_TABLES;
    hvf_table_waiting[at]=i;hvf_table_waiting_since[at]=since;
}
/* A table for view v, under hvf_mutex (see hvf_table_freed); HVF_TABLES when none is
   left. An index listed twice (the scan took it meanwhile) is skipped once owned. */
static unsigned hvf_table_take_one(HvfView *v) {
    uint32_t done=__atomic_load_n(&hvf_flushes_done,__ATOMIC_ACQUIRE);
    while(hvf_table_nwaiting && (int32_t)(done-hvf_table_waiting_since[hvf_table_wait_head])>0) {
        unsigned w=hvf_table_waiting[hvf_table_wait_head];hvf_table_wait_head=(hvf_table_wait_head+1)%HVF_TABLES;hvf_table_nwaiting--;
        if(!hvf_table_owner[w] && hvf_table_nfreed<HVF_TABLES)hvf_table_freed[hvf_table_nfreed++]=w;
    }
    unsigned i=HVF_TABLES;
    while(i==HVF_TABLES && hvf_table_nfreed) { i=hvf_table_freed[--hvf_table_nfreed];if(hvf_table_owner[i])i=HVF_TABLES; }
    if(i==HVF_TABLES && hvf_table_fresh<HVF_TABLES)i=hvf_table_fresh++;
    for(unsigned n=0;i==HVF_TABLES && n<HVF_TABLES;n++) { unsigned k=(hvf_table_hint+n)%HVF_TABLES;if(!hvf_table_owner[k]) { i=k;hvf_table_hint=k+1; } }
    if(i==HVF_TABLES)return i;
    hvf_table_owner[i]=(unsigned short)(v-hvf_views);hvf_table_link[i]=v->table_chain;v->table_chain=i+1;v->tables++;
    return i;
}
static uint64_t hvf_table_of(HvfView *v,int zero) {
    pthread_mutex_lock(&hvf_mutex);unsigned i=hvf_table_take_one(v);pthread_mutex_unlock(&hvf_mutex);
    if(i==HVF_TABLES) { hvf_exhausted=1;return 0; }
    if(zero)memset(hvf_tables+(size_t)i*4096,0,4096);
    return HVF_IPA_BASE+(uint64_t)i*4096;
}
static uint64_t hvf_table(void) { return hvf_table_of(hvf_view,1); }
static void hvf_tables_free(HvfView *v) {
    __atomic_add_fetch(&hvf_table_epoch,1,__ATOMIC_ACQ_REL); /* cached leaf tables may be reused */
    pthread_mutex_lock(&hvf_mutex);
    uint32_t since=__atomic_load_n(&hvf_flushes_begun,__ATOMIC_ACQUIRE);
    for(unsigned i=v->table_chain;i;i=hvf_table_link[i-1]) { hvf_table_owner[i-1]=0;hvf_table_wait(i-1,since); }
    pthread_mutex_unlock(&hvf_mutex);
    v->table_chain=0;v->tables=0;v->root=0;
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
    if(system)return descriptor|UINT64_C(1)<<54|(system==2?UINT64_C(1)<<11:0); /* EL1 only, never EL0-executable; 2: per process (nG) */
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
    HvfView *v=hvf_view;uint64_t ipa=0,bipa=0;
    MemoryBacking *over=backing->base?BACKING_BASE(backing):NULL;  /* an overlay's base (same size, same chunks) */
    int per_page=0;unsigned uniform=flags;                   /* as hvf_effective_flags decides */
    if(flags&SEG_SHARED)uniform=flags&7;
    else if(flags&2) {
        if(backing->frozen || (backing->refs>1 && backing->refs>address_space_backing_refs(space,backing)))uniform=flags&~2u;
        else per_page=1;
    }
    uint64_t bits[2]={hvf_descriptor_bits(flags&~2u,0),hvf_descriptor_bits(flags,0)},single=hvf_descriptor_bits(uniform,0);
    int stale=0;
    for(uint64_t p=0;p<size;) {
        if(!ipa || !((offset+p)%HVF_CHUNK)) {                                       /* each chunk's address (and its base's) */
            if(!(ipa=hvf_ipa_at(backing,offset+(size_t)p)))break;ipa-=p;
            if(over) { if(!(bipa=hvf_ipa_at(over,offset+(size_t)p)))break;bipa-=p; }
        }
        uint64_t *leaf=hvf_leaf(v,base+p,1);if(!leaf)break;
        uint64_t end=p+4096*(512-((base+p)>>12&511)),chunk=p+(HVF_CHUNK-(offset+p)%HVF_CHUNK);
        if(end>chunk)end=chunk;
        if(end>size)end=size;
        for(;p<end;p+=4096) {
            uint64_t descriptor=(over && backing_inherits(backing,(offset+p)/backing->page_size)?bipa:ipa)+p;
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
    const HvfView *v=hvf_own_view;
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
    v->layout=space->layout;v->generation=space->generation;v->stale=0;v->nmoved=0;
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
    unsigned flags=space->map_flags[i]&7;uint64_t ipa=flags?hvf_page_ipa(space->map_backing,(size_t)i*4096):0;
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
        if(!leaf && flags) {           /* nothing mapped here to clear: just find a page to map (Go probes 64 MiB at a time) */
            unsigned j=i;
            for(;j+64<=end;j+=64) { uint64_t w[8],any=0;memcpy(w,flags+j,64);for(unsigned k=0;k<8;k++)any|=w[k];if(any&UINT64_C(0x0707070707070707))break; }
            if(j+64>end) { while(j<end && !(flags[j]&7))j++; }
            if(j==end || !ipa) { i=end;continue; }
            i=j;
        }
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
/* Whether this vCPU runs v as it stands: its tables, under the ASID v holds now. A
   view rebuilt on the same root table (freed tables are reused first: a fork's child
   often gets its predecessor's root) has a fresh ASID to take, or the TLBs still hold
   the earlier process's translations. */
static int hvf_view_current(const HvfView *v) {
    return v==hvf_view && v->root && hvf_cpu->ttbr==v->root && v->asid_generation==__atomic_load_n(&hvf_asid_generation,__ATOMIC_RELAXED);
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
        for(unsigned w=0;w<worker_count;w++) {
            HvfCpu *c=workers[w]!=worker?workers[w]->backend:NULL;
            if(workers[w]!=worker && (workers[w]->task+1==(int)i || (c && c->view==&hvf_views[i])))in_use=1;
        }
        if(!in_use)hvf_tables_free(&hvf_views[i]);
    }
    if(!worker_threads) { pthread_mutex_lock(&hvf_mutex);for(unsigned i=hvf_nblocks;i--;)if(hvf_blocks[i].host)hvf_block_unmap(i);pthread_mutex_unlock(&hvf_mutex); } /* (no host: being unmapped) */
}
static void hvf_map_arena_all(GuestAddressSpace *space);
static void hvf_build(GuestAddressSpace *space) {
    hvf_exhausted=0;hvf_tables_free(hvf_view);
    if(!(hvf_view->root=hvf_table()))return;
    for(unsigned i=0;i<HVF_CODE_BYTES/4096;i++)hvf_map_page(HVF_SYSTEM_VA+i*4096,HVF_IPA_BASE+HVF_TABLE_BYTES+i*4096,0,1);   /* vectors, routines, EL1 handlers */
    for(unsigned i=0;i<WORKER_LIMIT*HVF_FRAME_BYTES/4096;i++)hvf_map_page(HVF_FRAME_VA+i*4096,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_CODE_BYTES+i*4096,0,1);
    hvf_map_page(HVF_FDS_VA,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES+(uint64_t)(hvf_view-hvf_views)*4096,0,2); /* the view's own process */
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
/* hvf_map_arena_page for every mapped arena page, with what it decides per page
   (the backing's sharing, each chunk's guest-physical address) decided once: after
   a fork of a large process (Node: tens of thousands of pages) it ran on every one. */
static void hvf_map_arena_pass(GuestAddressSpace *space,uint64_t *mapped);
static void hvf_map_arena_all(GuestAddressSpace *space) {
    if(!profiling) { uint64_t n=0;hvf_map_arena_pass(space,&n);return; }
    uint64_t at=profile_now_ns(),n=0;hvf_map_arena_pass(space,&n);
    PROFILE_ADD(profile_arena_passes,1);PROFILE_ADD(profile_arena_ns,profile_now_ns()-at);PROFILE_ADD(profile_arena_pages,n);
}
static void hvf_map_arena_pass(GuestAddressSpace *space,uint64_t *mapped) {
    MemoryBacking *backing=space->map_backing;
    if(!space->map_flags || !backing)return;
    unsigned pages=space->map_capacity/4096;if(space->map_top<pages)pages=space->map_top;
    int shared=backing->frozen || (backing->refs>1 && backing->refs>address_space_backing_refs(space,backing));
    uint64_t bits[8];for(unsigned f=0;f<8;f++)bits[f]=hvf_descriptor_bits(f,0);
    size_t chunk=SIZE_MAX;uint64_t ipa=0,bipa=0,leaf_key=UINT64_MAX,*leaf=NULL;
    MemoryBacking *over=backing->base?BACKING_BASE(backing):NULL;   /* an overlay's base */
    for(unsigned i=0;i<pages;) {
        if(!(i&7) && i+8<=pages) {                                        /* V8 and Go reserve gigabytes they never map */
            unsigned span=!(i&63) && i+64<=pages?64:8;uint64_t any=0;
            for(unsigned k=0;k<span;k+=8) { uint64_t w;memcpy(&w,space->map_flags+i+k,8);any|=w; }
            if(!(any&UINT64_C(0x0707070707070707))) { i+=span;continue; }
        }
        unsigned flags=space->map_flags[i++]&7;if(!flags)continue;
        size_t offset=(size_t)(i-1)*4096,at=backing->mapped_size<=HVF_CHUNK?0:offset/HVF_CHUNK;
        uint64_t va=MAP_BASE+offset;
        if(at!=chunk) { chunk=at;ipa=hvf_ipa_at(backing,at*HVF_CHUNK);bipa=over?hvf_ipa_at(over,at*HVF_CHUNK):0; }
        uint64_t use=over && backing_inherits(backing,offset/backing->page_size)?bipa:ipa;   /* an overlay's page it has not copied: its base's */
        if(!use) { hvf_unmap_page(va);leaf_key=UINT64_MAX;continue; }
        if(flags&2) {                                                     /* as hvf_effective_flags */
            const BackingPage *page=backing->pages[offset/backing->page_size];
            if(shared || !page || page->refs>1)flags&=~2u;
        }
        if(va>>21!=leaf_key) { if(!(leaf=hvf_leaf(hvf_view,va,1)))return;leaf_key=va>>21; }
        uint64_t descriptor=(use+(offset-at*HVF_CHUNK))|bits[flags],*slot=&leaf[(va>>12)&511];   /* as hvf_map_page */
        if(((*slot&1) && *slot!=descriptor) || (flags&1))hvf_stale_tlb=1;
        if(flags&1)hvf_stale_code=1;
        *slot=descriptor;(*mapped)++;
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
        if(kept && s->backing && s->flags && s->size && hvf_mapped_ipa(s->base)!=hvf_page_ipa(s->backing,s->offset))kept=0;
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
/* Map backing's [offset, offset+size) again wherever space maps it, into v. */
static void hvf_remap_backing(const GuestAddressSpace *space,HvfView *v,MemoryBacking *backing,size_t offset,size_t size) {
    for(unsigned r=0;r<space->nseg+2;r++) {
        uint64_t base,length;size_t from;unsigned flags;int arena=0;
        if(r==0) { if(space->map_backing!=backing || !space->map_flags)continue;base=MAP_BASE;length=space->map_capacity;from=0;flags=0;arena=1; }
        else if(r==1) { if(space->brk_backing!=backing)continue;base=space->brk_base;length=space->brk_pages;from=0;flags=6; }
        else { const Segment *g=&space->seg[r-2];if(g->backing!=backing)continue;base=g->base;length=g->size;from=g->offset;flags=g->flags; }
        size_t low=offset>from?offset:from,high=offset+size<from+length?offset+size:from+(size_t)length;
        for(size_t at=low&~(size_t)4095;at<high;at+=4096) {
            uint64_t va=base+(at-from);
            unsigned f=arena?space->map_flags[(va-MAP_BASE)/4096]:flags;
            if(arena && (f&MAP_RESERVED))continue;
            if(arena)f&=7;
            if(!(f&7))continue;
            uint64_t ipa=hvf_page_ipa(backing,at);if(!ipa)continue;
            hvf_map_page_in(v,va,ipa,hvf_effective_flags(space,backing,f,at),0);
        }
    }
}
/* backing_moved_hook: an overlay's pages a host write copied (a read() into memory the
   fork's child had not written yet) are still its base's in the running process's
   tables; mapped again before its guest code runs (hvf_sync_tables). A write fault maps
   what it copies itself (hvf_faulting). */
static _Thread_local int hvf_faulting;
static void hvf_backing_moved(MemoryBacking *backing,size_t offset,size_t size) {
    if(hvf_faulting)return;
    HvfView *v=hvf_own_view;if(!v->root)return;
    if(v->nmoved<sizeof(v->moved)/sizeof(*v->moved)) { v->moved[v->nmoved].backing=backing;v->moved[v->nmoved].offset=offset;v->moved[v->nmoved].size=size;v->nmoved++; }
    else v->stale=HVF_STALE_COW;                         /* many: every private page again */
}
/* n tables for view v, taken in one pass under hvf_mutex (their numbers in idx);
   fewer when the pool runs out. */
static unsigned hvf_tables_take(HvfView *v,unsigned *idx,unsigned n) {
    unsigned got=0;
    pthread_mutex_lock(&hvf_mutex);
    while(got<n) { unsigned i=hvf_table_take_one(v);if(i==HVF_TABLES)break;idx[got++]=i; }
    pthread_mutex_unlock(&hvf_mutex);
    return got;
}
/* A copy for view to of the table at from (level 0 the root) and every table below it,
   into the tables idx names (next: the first unused; n of them). */
static uint64_t hvf_table_copy(HvfView *to,uint64_t from,int level,const unsigned *idx,unsigned *next,unsigned n) {
    if(*next==n)return 0;
    uint64_t copy=HVF_IPA_BASE+(uint64_t)idx[(*next)++]*4096;(void)to;
    const uint64_t *src=(const uint64_t *)(void *)(hvf_tables+(from-HVF_IPA_BASE));uint64_t *dst=(uint64_t *)(void *)(hvf_tables+(copy-HVF_IPA_BASE));
    memcpy(dst,src,4096);
    if(level<3)
        for(unsigned i=0;i<512;i++)if(src[i]&1) {                 /* a table descriptor (no blocks here) */
            uint64_t below=hvf_table_copy(to,src[i]&UINT64_C(0x0000fffffffff000),level+1,idx,next,n);
            if(!below)return 0;
            dst[i]=below|(src[i]&~UINT64_C(0x0000fffffffff000));
        }
    return copy;
}
/* A fork's child starts on a copy of its parent's tables (Node's: some 80 pages) instead
   of a build that maps every page again (Node's text and heap, 38k entries: ~140 us of
   the child's start). They are right for it: memory it shares maps the same, an
   overlay's pages read the parent's backing (read-only since the fork's hold), and only
   the memory the fork copied (a small backing's clone, an overlay's pages copied
   already) is mapped again. 0 when the parent's tables lag its memory (or the pool ran
   out): the child builds its own. Under the parent's locks, after its fork. */
static int hvf_view_fork(uint64_t pid) {
    HvfView *from=hvf_own_view,*to=&hvf_views[pid];
    const GuestAddressSpace *parent=address_space,*child=&schedule_tasks[pid-1].image.memory;
    if(to==from || !from->root || from->nmoved || from->arena_rev!=parent->arena_rev || !hvf_view_matches(parent) || child->nseg!=parent->nseg)return 0;
    hvf_tables_free(to);hvf_exhausted=0;
    /* As many tables as the parent's (all in its tree: a view frees them only all at
       once), taken together: one at a time, each took the pool's lock. */
    unsigned idx[512],n=from->tables,next=0;
    if(n>sizeof(idx)/sizeof(*idx) || hvf_tables_take(to,idx,n)!=n) { hvf_tables_free(to);return 0; }
    uint64_t root=hvf_table_copy(to,from->root,0,idx,&next,n);
    if(!root) { hvf_tables_free(to);return 0; }
    if(next<n) {           /* fewer in its tree after all: the rest back (the chain's first, as taken last) */
        pthread_mutex_lock(&hvf_mutex);
        for(unsigned i=n;i-->next;) { hvf_table_owner[idx[i]]=0;if(hvf_table_nfreed<HVF_TABLES)hvf_table_freed[hvf_table_nfreed++]=idx[i]; }  /* never walked */
        to->table_chain=next?idx[next-1]+1:0;to->tables-=n-next;
        pthread_mutex_unlock(&hvf_mutex);
    }
    to->root=root;
    HvfView *was=hvf_view;hvf_view=to;                            /* the mapping calls below work on hvf_view */
    hvf_map_page(HVF_FDS_VA,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES+(uint64_t)(to-hvf_views)*4096,0,2); /* its own process page */
    for(unsigned r=0;r<child->nseg+2 && !hvf_exhausted;r++) {
        MemoryBacking *c=r==0?child->map_backing:r==1?child->brk_backing:child->seg[r-2].backing;
        MemoryBacking *p=r==0?parent->map_backing:r==1?parent->brk_backing:parent->seg[r-2].backing;
        if(c==p || !c)continue;                                    /* shared as it is */
        if(c->base==p) {                                          /* an overlay: pages it copied already (the fork's own writes) */
            for(size_t i=backing_next(c,0,c->used);i<c->used;i=backing_next(c,i+1,c->used))hvf_remap_backing(child,to,c,i*c->page_size,c->page_size);
            continue;
        }
        if(r==0)hvf_map_arena_all((GuestAddressSpace *)child);   /* a clone */
        else if(r==1)hvf_map_range(child,child->brk_base,child->brk_pages,6,c,0);
        else hvf_map_range(child,child->seg[r-2].base,child->seg[r-2].size,child->seg[r-2].flags,c,child->seg[r-2].offset);
    }
    hvf_view=was;
    if(hvf_exhausted) { hvf_tables_free(to);return 0; }
    hvf_view_record(to,child);to->arena_rev=child->arena_rev;to->asid_generation=0;  /* a fresh ASID: nothing of an earlier process's in the TLBs */
    PROFILE_ADD(profile_view_copies,1);
    return 1;
}
/* Bring the current process's tables up to date; returns whether TLBs need maintenance:
   1 for this view's ASID, 2 for every one (the tables were rebuilt). */
static int hvf_sync_tables(GuestAddressSpace *space,HvfView *v) {
    if(hvf_view_matches(space)) {
        if(!hvf_view_current(v))hvf_activate(v);                                    /* a process switch */
        int stale=0;
        if(v->arena_rev!=space->arena_rev) {
            /* A range that started after this view (or a large table set) rebuilds. */
            if(v->arena_rev<space->arena_dirty_since || v->tables>HVF_TABLES/4)goto rebuild;
            hvf_exhausted=0;hvf_stale_tlb=0;hvf_remap_arena(space);
            if(hvf_exhausted)goto rebuild;
            hvf_arena_synced(space);stale=hvf_stale_tlb;
        }
        if(v->nmoved) {                                  /* overlay pages a host write copied: off their base */
            hvf_exhausted=0;hvf_stale_tlb=0;
            for(unsigned i=0;i<v->nmoved;i++)hvf_remap_backing(space,v,v->moved[i].backing,v->moved[i].offset,v->moved[i].size);
            v->nmoved=0;
            if(hvf_exhausted)goto rebuild;
            stale|=hvf_stale_tlb;
        }
        v->generation=space->generation;return stale;
    }
rebuild:
    if(v->root)hvf_siblings_out();                       /* threads of this process on other vCPUs use these tables */
    uint64_t profile_at=profiling?profile_now_ns():0;
    if(hvf_sync_partial(space,v)) {
        if(profiling) { PROFILE_ADD(profile_partials,1);PROFILE_ADD(profile_partial_ns,profile_now_ns()-profile_at); }
        hvf_activate(v);hvf_view_record(v,space);hvf_arena_synced(space);return 1;
    }
    if(profiling)profile_at=profile_now_ns();
    hvf_view=v;hvf_build(space);
    if(profiling) { PROFILE_ADD(profile_builds,1);PROFILE_ADD(profile_build_ns,profile_now_ns()-profile_at); }
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
    HvfView *v=hvf_own_view;
    if(hvf_view_current(v) && v->generation==space->generation)return;
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
        if(f && (ipa=hvf_page_ipa(backing,offset))) { hvf_map_page(page,ipa,hvf_effective_flags(space,backing,f,offset),0);if(!first)first=page;last=page; }
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
    if(hvf_view!=hvf_own_view || !hvf_view_current(hvf_view))return 0;
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
   share them). Off while syscalls are traced, counted or filtered. Descriptor
   answers (HF_FDS) need the process's own tables: not for a vfork child running on
   its parent's (its page is the parent's). */
/* This process has memory protected for a fork (hvf_backing_hold): the EL1 handlers,
   some of which write guest memory, stay off meanwhile. */
/* Called on every resume (hvf_fast_publish). A backing stays held after its snapshot
   goes until its process writes it again, so once some fork held memory that is not
   written again (FEX's large allocations), hvf_held_live stays up and every resume of
   every process walked all its segments: the Go linker under FEX, with a segment per
   mapped archive (over a thousand), spent most of its time here. An answer of none is
   kept until this process's memory changes or another backing becomes held; a held
   answer is checked again each time (a thaw only makes it none). */
static int hvf_holding(void) {
    if(!__atomic_load_n(&hvf_held_live,__ATOMIC_ACQUIRE))return 0;
    const GuestAddressSpace *s=address_space;HvfView *v=hvf_own_view;
    uint64_t epoch=__atomic_load_n(&hvf_hold_epoch,__ATOMIC_ACQUIRE);
    if(v->unheld && v->unheld_generation==s->generation && v->unheld_epoch==epoch)return 0;
#define HVF_HELD(b) ((b) && __atomic_load_n(&(b)->held,__ATOMIC_ACQUIRE) && __atomic_load_n(&(b)->frozen,__ATOMIC_ACQUIRE))
    int held=HVF_HELD(s->map_backing) || HVF_HELD(s->brk_backing);
    for(unsigned i=0;!held && i<s->nseg;i++)held=HVF_HELD(s->seg[i].backing);
#undef HVF_HELD
    v->unheld=!held;v->unheld_generation=s->generation;v->unheld_epoch=epoch;
    return held;
}
static void hvf_fast_publish(uint64_t *frame,uint64_t tid) {
    int on=!trace && !profiling && !seccomp_active();
    const uint64_t answers[7]={process_pid,UINT64_MAX,guest_ids.ruid,guest_uid,guest_ids.rgid,guest_gid,tid};
    for(unsigned i=0;i<7;i++)frame[HF_FAST+i]=on?answers[i]:UINT64_MAX;
    frame[HF_FDS]=on && schedule_active && fd_mirror_base && hvf_view_pid()==process_pid?HVF_FDS_VA:0;
    frame[HF_STAT]=on && hvf_el1_ready && fs_mount_count==hvf_el1_mounts[0] && mem_mount_count==hvf_el1_mounts[1] && !hvf_holding()?HVF_EL1_VA:0;
    unsigned task=schedule_active && process_pid?(unsigned)process_pid-1:0;     /* its own tables, in proc_block: EL1 opens and closes */
    int tables=frame[HF_STAT] && schedule_active && fs_table==&proc_block->fs[task] && fd_table==&proc_block->fds[task];
    frame[HF_TASK]=tables?task:UINT64_MAX;frame[HF_MM]=tables?mm_id(task)-1:UINT64_MAX;
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
    /* EL0t, with PSTATE.SSBS set as Linux starts every task (speculative store bypass
       allowed unless a task asks for the Spectre v4 mitigation): with it clear, code
       that reloads what it just stored (JIT-compiled JavaScript's stack and context
       slots) ran up to 2.7 times slower. */
    f[HF_SPSR]=((uint64_t)cpu->flag_n<<31)|((uint64_t)cpu->flag_z<<30)|((uint64_t)cpu->flag_c<<29)|((uint64_t)cpu->flag_v<<28)|(UINT64_C(1)<<12);
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
    uint32_t number=__atomic_add_fetch(&hvf_flushes_begun,1,__ATOMIC_ACQ_REL);     /* before its TLBI (hvf_table_freed) */
    /* A preemption requested as the previous guest run ended can still be pending;
       it cancels this run before the routine starts, so simply run again. */
    do hvf_check(hv_vcpu_run(hvf_vcpu),"hv_vcpu_run");
    while(hvf_exit->reason==HV_EXIT_REASON_CANCELED || hvf_exit->reason==HV_EXIT_REASON_VTIMER_ACTIVATED);
    if(hvf_exit->reason!=HV_EXIT_REASON_EXCEPTION || (hvf_exit->exception.syndrome>>26)!=0x16 || (hvf_exit->exception.syndrome&0xffff)!=1)
        fail("Hypervisor TLB/cache maintenance did not complete");
    for(uint32_t done=__atomic_load_n(&hvf_flushes_done,__ATOMIC_ACQUIRE);(int32_t)(number-done)>0 && !__atomic_compare_exchange_n(&hvf_flushes_done,&done,number,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE););
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
/* A guest write to memory protected for a fork (hvf_backing_hold) waits for the
   child's snapshot to go: a child that execs or exits does within microseconds, and
   the parent then writes its own memory as it was. After HVF_HELD_WAIT_NS on one
   instruction (a child that lives on, a server's worker), the locked path gives the
   process its own copy instead (lookup snapshots it). */
#define HVF_HELD_WAIT_NS UINT64_C(1000000)
#define HVF_HELD_GAP_NS UINT64_C(200000)
static _Thread_local uint64_t hvf_held_pc,hvf_held_since,hvf_held_last;
/* How long the write at pc has waited (now): a new wait unless this thread was waiting
   for it just now (Node's spawn faults on the same instruction at every fork). */
static uint64_t hvf_held_waited(uint64_t pc,uint64_t now) {
    if(pc!=hvf_held_pc || now-hvf_held_last>HVF_HELD_GAP_NS) { hvf_held_pc=pc;hvf_held_since=now; }
    hvf_held_last=now;return now-hvf_held_since;
}
static uint64_t hvf_held_seen(void) { pthread_mutex_lock(&hvf_held_lock);uint64_t seen=hvf_held_thaws;pthread_mutex_unlock(&hvf_held_lock);return seen; }
/* Sleep until a thaw after seen (1: retry the write) or the instruction at pc has
   waited long enough (0). */
static int hvf_held_sleep(uint64_t seen,uint64_t pc) {
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if(hvf_held_waited(pc,now)>=HVF_HELD_WAIT_NS)return 0;
    uint64_t since=profiling?profile_now_ns():0;
    pthread_mutex_lock(&hvf_held_lock);
    while(hvf_held_thaws==seen && (now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW))-hvf_held_since<HVF_HELD_WAIT_NS) {
        uint64_t left=HVF_HELD_WAIT_NS-(now-hvf_held_since);
        struct timespec pause={(time_t)(left/1000000000),(long)(left%1000000000)};
        (void)pthread_cond_timedwait_relative_np(&hvf_held_cond,&hvf_held_lock,&pause);
    }
    int woken=hvf_held_thaws!=seen;
    pthread_mutex_unlock(&hvf_held_lock);
    hvf_held_last=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if(profiling) { PROFILE_ADD(profile_hold_waits,1);PROFILE_ADD(profile_hold_wait_ns,profile_now_ns()-since);PROFILE_ADD(profile_hold_timeouts,woken?0:1); }
    return woken;
}
/* A write fault on held memory reached the locked path (a pending signal sent it, or
   the unlocked wait ran out). The runtime lock is the one the child needs to exec or
   exit, so this path cannot wait: 1 while the instruction at pc has waited less than
   HVF_HELD_WAIT_NS (the thread yields, the write to run again: a pending signal goes
   first, then the unlocked path waits), 0 to copy the memory now. Under the mm lock. */
static int hvf_held_yield(uint64_t far,uint64_t pc) {
    if(!__atomic_load_n(&hvf_held_live,__ATOMIC_ACQUIRE))return 0;
    uint64_t low=0,high=0;size_t start=0;unsigned flags=0;
    MemoryBacking *b=hvf_region(address_space,far,&low,&high,&start,&flags);
    if(!b || !__atomic_load_n(&b->held,__ATOMIC_ACQUIRE) || !__atomic_load_n(&b->frozen,__ATOMIC_ACQUIRE))return 0;
    return hvf_held_waited(pc,clock_gettime_nsec_np(CLOCK_UPTIME_RAW))<HVF_HELD_WAIT_NS;
}
/* A write fault on memory a fork held that has thawed: give this view's write permission
   back in one pass and resume the write through an ASID flush (1; the vCPU is then in
   an EL1 routine). 0 when nothing was restored here. Under the mm lock, framed. A later
   fork that ran out of copy and overlay slots keeps a held backing as it is
   (address_space_fork_backing): shared then, so the write takes the copy-on-write path
   (0) instead of making the memory another process maps writable in place. */
static int hvf_held_restore(MemoryBacking *b,uint64_t elr,uint64_t spsr) {
    if(__atomic_load_n(&b->frozen,__ATOMIC_ACQUIRE) || !__atomic_exchange_n(&b->held,0,__ATOMIC_ACQ_REL))return 0;
    __atomic_sub_fetch(&hvf_held_live,1,__ATOMIC_ACQ_REL);
    size_t refs=__atomic_load_n(&b->refs,__ATOMIC_ACQUIRE);
    if(refs>1 && refs>address_space_backing_refs(address_space,b))return 0;
    if(hvf_view!=hvf_own_view || !hvf_hold(hvf_view,address_space,b,0))return 0;
    hvf_resume_el0(elr,spsr);
    hvf_frame[HF_FLUSH]=(uint64_t)hvf_view->asid<<48;
    hvf_check(hv_vcpu_set_reg(hvf_vcpu,HV_REG_PC,HVF_SYSTEM_VA+HVF_FLUSH_RESTORE_OFFSET),"set pc");
    return 1;
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
    /* TTBR1 (the upper half): the EL1 region alone, the same 48 bits, 4 KiB granule. */
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_TCR_EL1,16|(1u<<8)|(1u<<10)|(3u<<12)|(UINT64_C(16)<<16)|(UINT64_C(1)<<24)|(UINT64_C(1)<<26)|(UINT64_C(3)<<28)|(UINT64_C(2)<<30)|(UINT64_C(2)<<32)|(UINT64_C(1)<<37)),"set tcr");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_TTBR1_EL1,HVF_EL1_IPA+HVF_EL1_TABLES),"set ttbr1");
    hvf_check(hv_vcpu_set_sys_reg(c->vcpu,HV_SYS_REG_TPIDR_EL1,HVF_FRAME_VA+index*HVF_FRAME_BYTES),"set tpidr_el1"); /* the fault vector's frame */
    c->frame[HF_STACK]=HVF_EL1_VA+16384+(uint64_t)(index+1)*HVF_EL1_STACK;
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
/* The EL1 region (HVF_EL1_VA): its tables map the head (context page and stacks) and,
   when the root's layers have a lookup cache, the cache too; the handlers answer only
   then (hvf_el1_ready) and only while the mounts are the ones published. */
static void hvf_el1_map(uint64_t offset,uint64_t ipa,uint64_t bytes) {
    uint64_t *tables=(uint64_t *)(void *)(hvf_el1_head+HVF_EL1_TABLES),base=HVF_EL1_IPA+HVF_EL1_TABLES;  /* L0, L1, L2, leaves */
    tables[0]=(base+4096)|3;tables[512]=(base+8192)|3;
    for(uint64_t p=0;p<bytes;p+=4096) {
        uint64_t page=(offset+p)>>12,leaf=page/512;
        tables[1024+leaf]=(base+(3+leaf)*4096)|3;
        tables[(3+leaf)*512+page%512]=(ipa+p)|hvf_descriptor_bits(0,1);
    }
}
static void hvf_el1_setup(void) {
    hvf_el1_head=hvf_host_pages(HVF_EL1_HEAD);
    hvf_check(hv_vm_map(hvf_el1_head,HVF_EL1_IPA,HVF_EL1_HEAD,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map el1");
    hvf_el1_map(0,HVF_EL1_IPA,16384+WORKER_LIMIT*HVF_EL1_STACK);
    HvfEl1Context *c=(HvfEl1Context *)(void *)hvf_el1_head;c->mounts=UINT32_MAX;
    if(!ov_gen || !ov_cache || fs_root<0 || fs_lower<0)return;
    uint64_t gen_bytes=((2+OV_BUCKETS)*sizeof(uint64_t)+16383)&~UINT64_C(16383),cache_bytes=(OV_CACHE_SLOTS*sizeof(OvCached)+16383)&~UINT64_C(16383);
    if((4u<<20)+cache_bytes>HVF_EL1_BYTES || HVF_EL1_TABLES+(3+((4u<<20)+cache_bytes)/(2u<<20)+1)*4096>HVF_EL1_HEAD)return;
    /* The counters are a shared file mapping (dockivad counts in it too); a host that
       refuses to map one leaves the handlers off. */
    if(hv_vm_map(ov_gen,HVF_EL1_IPA+(2u<<20),gen_bytes,HV_MEMORY_READ)!=HV_SUCCESS ||
       hv_vm_map(ov_cache,HVF_EL1_IPA+(4u<<20),cache_bytes,HV_MEMORY_READ)!=HV_SUCCESS)return;
    hvf_el1_map(2u<<20,HVF_EL1_IPA+(2u<<20),gen_bytes);hvf_el1_map(4u<<20,HVF_EL1_IPA+(4u<<20),cache_bytes);
    c->gen=HVF_EL1_VA+(2u<<20);c->cache=HVF_EL1_VA+(4u<<20);
    c->slots=OV_CACHE_SLOTS;c->buckets=OV_BUCKETS;c->entry=sizeof(OvCached);c->path=OV_CACHE_PATH;
    c->e_seq=offsetof(OvCached,seq);c->e_gen=offsetof(OvCached,at.gen);c->e_made=offsetof(OvCached,at.made);c->e_bucket=offsetof(OvCached,at.bucket);
    c->e_layer=offsetof(OvCached,layer);c->e_lower=offsetof(OvCached,lower);c->e_follow=offsetof(OvCached,follow);
    c->e_path=offsetof(OvCached,path);c->e_st=offsetof(OvCached,st);
    c->s_mode=offsetof(struct stat,st_mode);c->s_nlink=offsetof(struct stat,st_nlink);c->s_ino=offsetof(struct stat,st_ino);c->s_size=offsetof(struct stat,st_size);
    c->s_atime=offsetof(struct stat,st_atimespec);c->s_mtime=offsetof(struct stat,st_mtimespec);c->s_ctime=offsetof(struct stat,st_ctimespec);
    _Static_assert(sizeof(((struct stat *)0)->st_mode)==2 && sizeof(((struct stat *)0)->st_nlink)==2 && sizeof(struct timespec)==16,"stat layout the EL1 code reads");
    unsigned n=0,ok=fs_mount_count+mem_mount_count<=HVF_EL1_MOUNTS;
    for(unsigned i=0;ok && i<fs_mount_count+mem_mount_count;i++) {
        const char *path=i<fs_mount_count?fs_mounts[i].path:mem_mounts[i-fs_mount_count]->path;size_t length=strlen(path);
        if(length>=sizeof(c->mount[0]))ok=0;else { memcpy(c->mount[n],path,length+1);c->mount_length[n++]=(uint32_t)length; }
    }
    if(!ok)return;
    c->mounts=n;hvf_el1_mounts[0]=fs_mount_count;hvf_el1_mounts[1]=mem_mount_count;hvf_el1_ready=1;
    /* The process block: descriptor tables, mm locks, region caches (fs.h). */
    uint64_t block_bytes=proc_block_bytes();
    if(HVF_EL1_BLOCK+block_bytes>HVF_EL1_PACK || hv_vm_map(proc_block,HVF_EL1_IPA+HVF_EL1_BLOCK,block_bytes,HV_MEMORY_READ|HV_MEMORY_WRITE)!=HV_SUCCESS)return;
    hvf_el1_map(HVF_EL1_BLOCK,HVF_EL1_IPA+HVF_EL1_BLOCK,block_bytes);
    c->host_block=(uint64_t)(uintptr_t)proc_block;
    c->b_mm=offsetof(ProcBlock,mm);c->b_locks=offsetof(ProcBlock,locks_held);c->b_hint=offsetof(ProcBlock,pack_hint);
    c->b_fs=offsetof(ProcBlock,fs);c->b_fds=offsetof(ProcBlock,fds);c->b_pack=offsetof(ProcBlock,pack);c->b_mmc=offsetof(ProcBlock,mm_cache);c->b_futex=offsetof(ProcBlock,futex_waiters);c->b_spin=offsetof(ProcBlock,spinners);c->b_epoch=offsetof(ProcBlock,futex_epoch);
    c->b_sig=offsetof(ProcBlock,sig);c->sig_size=sizeof(HvfSignals);c->sig_ok=(job_control_enabled?1u:0u)|(vdso_backing?2u:0u);
    c->fs_size=sizeof(FsTable);c->fds_size=sizeof(FdTable);
#define FS_FIELD(f) (uint32_t)(offsetof(FsTable,slot)+offsetof(FsSlot,f))
    c->t_fd=FS_FIELD(fd);c->t_cloexec=FS_FIELD(cloexec);c->t_ro=FS_FIELD(ro);c->t_dev=FS_FIELD(dev);c->t_sole=FS_FIELD(sole);
    c->t_dirs=FS_FIELD(dir);c->t_guest=FS_FIELD(guest);c->t_pack=FS_FIELD(pack);c->t_top=offsetof(FsTable,top);c->slot_size=sizeof(FsSlot);c->d_kind=offsetof(FdTable,kind);c->d_index=offsetof(FdTable,index);
    c->fd_file=FD_FILE;c->mirror_file=FD_MIRROR_FILE;c->mirror_cloexec=FD_MIRROR_CLOEXEC;c->fs_slots=FS_SLOTS;c->fd_limit=GUEST_FD_LIMIT;c->file_limit=fs_file_limit;
    _Static_assert(sizeof(((FsSlot *)0)->fd)==4 && sizeof(((FsTable *)0)->top)==4 && sizeof(((FsSlot *)0)->guest)==2 && sizeof(((FsSlot *)0)->dir)==8 && sizeof(((FsSlot *)0)->pack)==8 && sizeof(((FdTable *)0)->index[0])==2 && sizeof(((ProcBlock *)0)->locks_held[0])==2,"table fields hvf_el1_guest.c writes");
    __atomic_store_n(&c->block,HVF_EL1_VA+HVF_EL1_BLOCK,__ATOMIC_RELEASE);
    /* Image files in the pack: opens and closes (its index). */
    if(!pack_base)return;
    const PackHeader *h=(const PackHeader *)(const void *)pack_base;
    uint64_t index_bytes=(h->data+16383)&~UINT64_C(16383);
    if(HVF_EL1_PACK+index_bytes>HVF_EL1_BYTES || HVF_EL1_TABLES+(3+(HVF_EL1_PACK+index_bytes)/(2u<<20)+1)*4096>HVF_EL1_HEAD)return;
    /* The index is copied: the hypervisor refuses to map a read-only file mapping. */
    void *index=mmap(NULL,index_bytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(index==MAP_FAILED)return;
    memcpy(index,pack_base,h->data);
    if(hv_vm_map(index,HVF_EL1_IPA+HVF_EL1_PACK,index_bytes,HV_MEMORY_READ)!=HV_SUCCESS)return;
    hvf_el1_map(HVF_EL1_PACK,HVF_EL1_IPA+HVF_EL1_PACK,index_bytes);
    c->pack_entries=HVF_EL1_VA+HVF_EL1_PACK+h->entries;c->pack_strings=HVF_EL1_VA+HVF_EL1_PACK+h->strings;
    c->marker=(uint32_t)pack_placeholder+1;
    __atomic_store_n(&c->pack_count,pack_count,__ATOMIC_RELEASE);
}
/* A64 encodings for the stub hvf_init writes. */
static uint32_t a64_b(unsigned from,unsigned to,uint32_t op) { return op|(((uint32_t)((int)to-(int)from)/4)&0x3ffffffu); }
static uint32_t a64_b19(unsigned from,unsigned to,uint32_t op) { return op|((((uint32_t)((int)to-(int)from)/4)&0x7ffffu)<<5); }
static uint32_t a64_sp(uint32_t op,unsigned offset,unsigned rt) { return op|((offset/8)<<10)|(31u<<5)|rt; }      /* ldr/str xT, [sp, #offset] */
static uint32_t a64_pair(uint32_t op,unsigned offset,unsigned a,unsigned b) { return op|((offset/8)<<15)|(b<<10)|(31u<<5)|a; }
/* hvf_init_vm is the part of hvf_init that depends on no option: the VM, its tables,
   frames and descriptor mirror, and the system code. A prestarted runtime (runtime.c)
   does it, and makes the main thread's vCPU, before its run is known (hvf_preinit):
   about 0.6 ms of a run's start. */
static int hvf_preinitialized;
static void hvf_init_vm(void);
static void hvf_init(void) {
    if(!hvf_preinitialized)hvf_init_vm();
    uint32_t vcpus=0;
    if(hv_vm_get_max_vcpu_count(&vcpus)==HV_SUCCESS && vcpus && worker_count>vcpus)worker_count=vcpus;
    hvf_el1_setup();                                  /* the lookup cache and pack (--layers-gen, --lower-pack): the run's */
    if(!hvf_preinitialized)worker->backend=hvf_cpu_new(1);
    if(vdso_backing)vdso_start(hvf_vtimer_offset);
    pthread_t watchdog;
    if(pthread_create(&watchdog,NULL,hvf_watchdog,NULL) || pthread_detach(watchdog))fail("Cannot start hypervisor watchdog");
}
static void hvf_preinit(void) {
    hvf_init_vm();
    worker->backend=hvf_cpu_new(1);                   /* the main thread's: the run's first process runs on it */
    hvf_preinitialized=1;
}
/* hvf_unpreinit gives them back before this process execs another image of itself
   (a named container's run, runtime.c): a VM and a vCPU belong to the process and
   its thread, which exec keeps. */
static void hvf_unpreinit(void) {
    hvf_check(hv_vcpu_destroy(hvf_cpu->vcpu),"hv_vcpu_destroy");
    hvf_check(hv_vm_destroy(),"hv_vm_destroy");
}
static void hvf_init_vm(void) {
    /* Every process's memory stays mapped: ask for the largest guest-physical space (up to the 40-bit TCR_EL1.IPS). */
    hv_vm_config_t config=hv_vm_config_create();uint32_t bits=0;
    if(config && hv_vm_config_get_max_ipa_size(&bits)==HV_SUCCESS && bits>=36) {
        if(bits>40)bits=40;
        if(hv_vm_config_set_ipa_size(config,bits)==HV_SUCCESS)hvf_ipa_limit=UINT64_C(1)<<bits;
    }
    hvf_check(hv_vm_create(config),"hv_vm_create");
    if(config)os_release(config);
    backing_release_hook=hvf_backing_gone;backing_hold_hook=hvf_backing_hold;backing_moved_hook=hvf_backing_moved;worker_interrupt=hvf_interrupt;workers_supported=1;
    hvf_futex_init();
    hvf_tables=hvf_host_pages(HVF_TABLE_BYTES);hvf_system=hvf_host_pages(HVF_SYSTEM_BYTES);fd_mirror_base=hvf_host_pages(HVF_FDS_BYTES);
    hvf_check(hv_vm_map(hvf_tables,HVF_IPA_BASE,HVF_TABLE_BYTES,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map tables");
    hvf_check(hv_vm_map(hvf_system,HVF_IPA_BASE+HVF_TABLE_BYTES,HVF_CODE_BYTES,HV_MEMORY_READ|HV_MEMORY_EXEC),"hv_vm_map system");
    hvf_check(hv_vm_map(hvf_system+HVF_CODE_BYTES,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_CODE_BYTES,HVF_SYSTEM_BYTES-HVF_CODE_BYTES,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map frames");
    hvf_check(hv_vm_map(fd_mirror_base,HVF_IPA_BASE+HVF_TABLE_BYTES+HVF_SYSTEM_BYTES,HVF_FDS_BYTES,HV_MEMORY_READ|HV_MEMORY_WRITE),"hv_vm_map descriptors"); /* EL1 opens write it */
    fd_mirror_append=(uint32_t *)(void *)(hvf_system+HVF_APPEND_OFFSET);
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
       an exit; fcntl and epoll_ctl of files at 0xb00 from the descriptor page.
       Assembled with clang --target=aarch64-linux-gnu, rust-objcopy -O binary:
       0x400: stp x0, x1, [sp]; mrs x0, esr_el1; lsr x0, x0, #26; cmp x0, #0x15; b.ne save
              cmp x8, #25; b.eq fds; cmp x8, #21; b.eq fds
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
    static const uint32_t lower_sync[]={0xa90007e0,0xd5385200,0xd35afc00,0xf100541f,0x54002781,0xf100651f,0x54003740,0xf100551f,0x54003700,
        0xd102b100,0xf100181f,0x540026a8,0x910503e1,0xf8607820,0xb7f82640,0xf94007e1,0xd69f03e0};
    static const uint32_t frame_code[]={
        0xa9010fe2,0xa90217e4,0xa9031fe6,0xa90427e8,0xa9052fea,0xa90637ec,0xa9073fee,0xa90847f0,0xa9094ff2,0xa90a57f4,0xa90b5ff6,0xa90c67f8,0xa90d6ffa,0xa90e77fc,
        0xd5384100,0xa90f03fe,0xd53bd040,0xd5384021,0xa91007e0,0xd5384000,0xd5385201,0xa91107e0,0xd5386000,0xf90093e0,0xd4000082,0x17ffffff,
        0xa94f87e0,0xd5184100,0xd51bd041,0xa95087e0,0xd5184020,0xd5184001,0xa94007e0,0xa9410fe2,0xa94217e4,0xa9431fe6,0xa94427e8,0xa9452fea,
        0xa94637ec,0xa9473fee,0xa94847f0,0xa9494ff2,0xa94a57f4,0xa94b5ff6,0xa94c67f8,0xa94d6ffa,0xa94e77fc,0xf9407bfe,0xd69f03e0,
        0xa95287e0,0xd50883a0,0x91000400,0xf1000421,0x54ffffa1,0xd5033b9f,0x17ffffe3};
    memcpy(hvf_system+0x400,lower_sync,sizeof(lower_sync));
    /* Calls 172..178 aside, newfstatat (79), statx (291), readlinkat (78), openat (56), close (57), mmap (222), munmap (215) and futex (98) go to the EL1 handlers while
       HF_STAT says so: the stub saves what C code may clobber (x2..x18, x30) and the
       call's ELR, SPSR and ESR, runs el1_entry(frame, context) on the vCPU's stack, and
       returns to the guest (handled) or takes the exit at save. A fault in the handlers
       (current EL, at 0x200) restores the call's state and takes the same way out. */
    {
        uint32_t *w=(uint32_t *)(void *)hvf_system;
        w[0x42c/4]=a64_b19(0x42c,HVF_EL1_STUB,0x54000008);              /* b.hi: no longer straight to save */
        unsigned at=HVF_EL1_STUB,give_up,taken;
#define EMIT(x) (w[at/4]=(x),at+=4)
        static const unsigned answered[]={79,291,78,56,57,222,215,98,134};   /* newfstatat, statx, readlinkat, openat, close, mmap, munmap, futex, rt_sigaction */
        unsigned taken_at[sizeof(answered)/sizeof(*answered)],calls=sizeof(answered)/sizeof(*answered);
        for(unsigned k=0;k+1<calls;k++) { EMIT(0xf100001fu|(answered[k]<<10)|(8u<<5));taken_at[k]=at;at+=4; }   /* cmp x8, #call; b.eq (below) */
        EMIT(0xf100001fu|(answered[calls-1]<<10)|(8u<<5));EMIT(a64_b19(at,HVF_SAVE_OFFSET,0x54000001));      /* the last: b.ne save */
        for(unsigned k=0;k+1<calls;k++)w[taken_at[k]/4]=a64_b19(taken_at[k],at,0x54000000);
        EMIT(a64_sp(0xf9400000u,HF_STAT*8,0));EMIT(a64_b19(at,HVF_SAVE_OFFSET,0xb4000000)); /* ldr x0, HF_STAT; cbz x0, save */
        for(unsigned r=2;r<=18;r+=2)EMIT(a64_pair(0xa9000000u,r*8,r,r+1));    /* stp x2, x3 ... x18, x19 */
        EMIT(a64_sp(0xf9000000u,240,30));                                    /* str x30 */
        EMIT(0xd5384029);EMIT(a64_sp(0xf9000000u,HF_SVC_ELR*8,9));          /* elr_el1, spsr_el1, esr_el1 */
        EMIT(0xd5384009);EMIT(a64_sp(0xf9000000u,HF_SVC_SPSR*8,9));
        EMIT(0xd5385209);EMIT(a64_sp(0xf9000000u,HF_SVC_ESR*8,9));
        EMIT(0x910003f3);EMIT(0xaa0003e1);EMIT(0x910003e0);                  /* mov x19, sp; mov x1, x0; mov x0, sp */
        EMIT(a64_sp(0xf9400000u,HF_STACK*8,9));EMIT(0x9100013f);             /* mov sp, the vCPU's EL1 stack */
        EMIT(a64_b(at,HVF_EL1_CODE+HVF_EL1_ENTRY,0x94000000));              /* bl el1_entry */
        EMIT(0x9100027f);                                                    /* mov sp, x19 */
        give_up=at;
        for(unsigned r=2;r<=18;r+=2)EMIT(a64_pair(0xa9400000u,r*8,r,r+1));    /* ldp x2, x3 ... x18, x19 */
        EMIT(a64_sp(0xf9400000u,240,30));
        taken=at;at+=4;                                                      /* cbz x0, (below) */
        EMIT(0xa94007e0);EMIT(0xd69f03e0);                                   /* ldp x0, x1, [sp]; eret */
        w[taken/4]=a64_b19(taken,at,0xb4000000);
        EMIT(0xa94007e0);EMIT(a64_b(at,HVF_SAVE_OFFSET,0x14000000));        /* ldp x0, x1, [sp]; b save */
        if(at>HVF_EL1_CODE)fail("EL1 stub outgrew its room");
        /* 0x200, synchronous exceptions at EL1: from the handlers, back to the frame (TPIDR_EL1),
           the call's registers restored, "not answered"; anything else exits as before (hvc #3). */
        uint64_t code=HVF_SYSTEM_VA+HVF_EL1_CODE;at=0x200;
        EMIT(0xd5384029);                                                    /* mrs x9, elr_el1 */
        EMIT(0xd2800000u|((uint32_t)(code&0xffff)<<5)|10);EMIT(0xf2a00000u|((uint32_t)((code>>16)&0xffff)<<5)|10);
        EMIT(0xf2c00000u|((uint32_t)((code>>32)&0xffff)<<5)|10);           /* x10: the handlers' address */
        EMIT(0xcb0a0129);EMIT(0xd2800000u|((uint32_t)sizeof(hvf_el1_code)<<5)|11);EMIT(0xeb0b013f);   /* sub x9, x9, x10; mov x11, #size; cmp x9, x11 */
        unsigned other=at;at+=4;                                             /* b.hs (below) */
        EMIT(0xd538d089);EMIT(0x9100013f);                                   /* mrs x9, tpidr_el1; mov sp, x9 */
        EMIT(a64_sp(0xf9400000u,HF_HELD*8,9));EMIT(0xb4000069);              /* ldr x9, HF_HELD; cbz x9, (past the release) */
        EMIT(0x889ffd3f);EMIT(a64_sp(0xf9000000u,HF_HELD*8,31));             /* stlr wzr, [x9]; str xzr, HF_HELD */
        EMIT(a64_sp(0xf9400000u,HF_SVC_ELR*8,9));EMIT(0xd5184029);
        EMIT(a64_sp(0xf9400000u,HF_SVC_SPSR*8,9));EMIT(0xd5184009);
        EMIT(a64_sp(0xf9400000u,HF_SVC_ESR*8,9));EMIT(0xd5185209);
        EMIT(0xd2800000);EMIT(a64_b(at,give_up,0x14000000));                 /* mov x0, #0; b (restore, not answered) */
        w[other/4]=a64_b19(other,at,0x54000002);
        EMIT(0xd4000062);EMIT(0x17ffffff);                                   /* hvc #3; b .-4 */
        if(at>0x280)fail("EL1 fault vector outgrew its room");
#undef EMIT
        memcpy(hvf_system+HVF_EL1_CODE,hvf_el1_code,sizeof(hvf_el1_code));
    }
    memcpy(hvf_system+HVF_SAVE_OFFSET,frame_code,sizeof(frame_code));
    /* 0xa00, hvf_sync_memory's: ldr x0, [sp, #312] (HF_FLUSH); tbz x0, #0, 1f; ic ialluis; 1: tbz x0, #1, 2f
       tlbi vmalle1is; b 3f; 2: and x1, x0, #0xffff000000000000; tlbi aside1is, x1; 3: dsb ish; isb; b restore */
    static const uint32_t flush_restore[]={0xf9409fe0,0x36000040,0xd508711f,0x36080060,0xd508831f,0x14000003,0x92503c01,0xd5088341,0xd5033b9f,0xd5033fdf,0x17ffffd0};
    memcpy(hvf_system+HVF_FLUSH_RESTORE_OFFSET,flush_restore,sizeof(flush_restore));
    /* 0xb00, fcntl (x8 25) and epoll_ctl (21) answered from the byte fd_mirror keeps for
       each descriptor (net.h), the host's answers exactly; anything else goes to save:
       fds:   ldr x0, [sp, #376] (HF_FDS); cbz x0, save; ldr x1, [sp]; cmp x1, #4095; b.hi save
              ldrb w0, [x0, x1]; cmp x8, #21; b.eq epoll; tbz w0, #7, save (a file); ldr x1, [sp, #8]
              cmp x1, #1; b.ne 1f; ubfx x0, x0, #5, #1; b done                      F_GETFD: close-on-exec
       1:     cmp x1, #2; b.ne 2f; ubfx x1, x0, #5, #1; cmp x1, x2; b.ne save; mov x0, #0; b done  F_SETFD: unchanged
       2:     ldr w1, appended (0xbfc); cbnz w1, save; ldr x1, [sp, #8]; cmp x1, #3; b.ne 3f
              and x1, x0, #3; tst x0, #4; orr x0, x1, #0x20000; b.eq done; orr x0, x0, #0x400; b done
       3:     cmp x1, #4; b.ne save; ubfx x1, x2, #10, #1; ubfx x0, x0, #2, #1; cmp x0, x1; b.ne save
              mov x0, #0                                                      F_SETFL: O_APPEND unchanged
       done:  ldr x1, [sp, #8]; eret
       epoll: tbz w0, #6, save; cmp x2, #4095; b.hi save; ldr x0, [sp, #376]; ldrb w0, [x0, x2]
              tbz w0, #7, save; ldr x1, [sp, #8]; sub w1, w1, #1; cmp w1, #2
              mov x0, #-1 (EPERM); mov x1, #-22 (EINVAL); csel x0, x0, x1, ls; b done */
    static const uint32_t fds[]={
        0xf940bfe0,0xb4ffefe0,0xf94003e1,0xf13ffc3f,0x54ffef88,0x38616800,0xf100551f,0x54000440,0x363fef00,0xf94007e1,
        0xf100043f,0x54000061,0xd3451400,0x1400001a,0xf100083f,0x540000c1,0xd3451401,0xeb02003f,0x54ffedc1,0xd2800000,0x14000013,
        0x18000541,0x35ffed41,0xf94007e1,0xf1000c3f,0x540000e1,0x92400401,0xf27e001f,0xb26f0020,0x54000140,0xb2760000,0x14000008,
        0xf100103f,0x54ffebe1,0xd34a2841,0xd3420800,0xeb01001f,0x54ffeb61,0xd2800000,0xf94007e1,0xd69f03e0,
        0x3637eae0,0xf13ffc5f,0x54ffeaa8,0xf940bfe0,0x38626800,0x363fea40,0xf94007e1,0x51000421,0x7100083f,0x92800000,0x928002a1,0x9a819000,0x17fffff2};
    memcpy(hvf_system+0xb00,fds,sizeof(fds));
    const uint32_t flush[]={UINT32_C(0xd508711f),UINT32_C(0xd508831f),UINT32_C(0xd5033b9f),UINT32_C(0xd5033fdf),UINT32_C(0xd4000022),UINT32_C(0x17ffffff)};
    memcpy(hvf_system+HVF_FLUSH_OFFSET,flush,sizeof(flush));
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
    if(!schedule_active || !worker_threads)return;
    /* Threads of this process, and vfork children sharing its memory: the vCPUs on its
       view (one entering guest code activates its view under this mm lock first). */
    unsigned mm=hvf_view_pid();int task;
#define HVF_SIBLING(w) ((w)!=worker && (task=__atomic_load_n(&(w)->task,__ATOMIC_ACQUIRE))>=0 && mm_id((unsigned)task)==mm)
    for(unsigned i=0;i<worker_count;i++)if(HVF_SIBLING(workers[i]))hvf_interrupt(workers[i]);
    for(unsigned i=0;i<worker_count;i++)
        while(HVF_SIBLING(workers[i]) && atomic_load(&workers[i]->in_guest))sched_yield();
#undef HVF_SIBLING
}
/* A threaded process's memory moves to a shared image while it has one thread, so
   its vfork children can share it in place later (process_clone) while its other
   threads run. */
static void hvf_share_image(void) {
    ThreadTable *p=hvf_task()->threads;
    if(shared_image) { if(!shared_image->owner && shared_image->refs==1)shared_image->owner=(unsigned)process_pid;return; }
    for(unsigned i=0;p && i<p->top;i++)if((int)i!=thread_now && (p->thread[i].state || p->thread[i].worker))return; /* not alone: it stays put */
    SharedImage *shared=calloc(1,sizeof(*shared));if(!shared)return;
    shared->refs=1;shared->owner=(unsigned)process_pid;
    address_space_assign(&shared->state.memory,address_space);address_space=&shared->state.memory;shared_image=shared;
}
/* The process's thread table, created on first use: thread 0 is the caller. */
static ThreadTable *hvf_threads_ensure(void) {
    ScheduledTask *t=hvf_task();
    if(!t->threads) {
        ThreadTable *p=resource_metadata(sizeof(*p));if(!p)return NULL;   /* room for THREAD_LIMIT threads: pages as they are used */
        Thread *th=&p->thread[0];
        th->state=1;th->tid=(unsigned)process_pid;th->clear_tid=hvf_main_clear_tid[process_pid];
        th->signal_mask=t->signal_mask;net_wait_copy(&th->network,&net_wait);th->worker=t->worker;th->wakeups=t->wakeups;
        p->top=1;t->worker=0;t->running=1;t->threads=p;thread_now=0;net_wait_now=&th->network;
        hvf_share_image();
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
    if(signal==9 || signal==19 || (!task_sig(t)->handler[signal-1] && !(th->signal_mask&bit))) { schedule_signal(t,(unsigned)signal);return 0; }
    if(task_sig(t)->ignored&bit)return 0;
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
/* Futex waits served in place: a thread whose futex wait is likely short waits on
   its own worker, holding no lock, instead of going back to the scheduler
   (hvf_futex_unlocked). It is queued in a bucket by its key: (process, image,
   address), or (memory, offset) for a futex in shared memory; wakers take only the
   bucket's lock. */
typedef struct HvfFutexWaiter { struct HvfFutexWaiter *next;uint64_t pid,epoch,address;const MemoryBacking *backing;size_t offset;uint32_t bits;int woken;InplaceWait *wait;HvfSpinner *spinner;uint32_t spin; } HvfFutexWaiter; /* wait: its worker's (scheduler.h) */
#define HVF_FUTEX_BUCKETS 256u
static struct { pthread_mutex_t lock;HvfFutexWaiter *head; } hvf_futex_buckets[HVF_FUTEX_BUCKETS];
static void hvf_futex_init(void) { for(unsigned i=0;i<HVF_FUTEX_BUCKETS;i++)pthread_mutex_init(&hvf_futex_buckets[i].lock,NULL); }
static unsigned hvf_futex_bucket(uint64_t pid,uint64_t address,const MemoryBacking *backing,size_t offset) {
    if(backing) { uint64_t k=(uint64_t)(uintptr_t)backing^((uint64_t)offset*0x9e3779b1u);return (unsigned)((k>>4)^(k>>12)^(k>>20))%HVF_FUTEX_BUCKETS; }
    return (unsigned)((address>>2)^(address>>12)^(pid*0x9e37u))%HVF_FUTEX_BUCKETS;
}
/* The EL1 wake's count (hvf_el1.h) for a waiter on address, raised; then a full
   barrier, so the waiter's read of the word comes after it (a waker stores the word,
   then reads the count). */
static uint32_t hvf_futex_dummy_count;
static uint32_t *hvf_futex_count_up(uint64_t address,const MemoryBacking *shared) {
    uint32_t *c=!proc_block?&hvf_futex_dummy_count:shared?&proc_block->futex_waiters[HVF_EL1_FUTEX_SLOTS]:&proc_block->futex_waiters[hvf_el1_futex_slot(process_pid,address)];
    __atomic_fetch_add(c,1,__ATOMIC_SEQ_CST);__atomic_thread_fence(__ATOMIC_SEQ_CST);return c;
}
static void hvf_futex_count_down(uint32_t *c) { __atomic_fetch_sub(c,1,__ATOMIC_SEQ_CST); }
/* Wake up to count in-place waiters on address: this process's (this image's), or
   any process's for a futex in shared memory (shared, offset). */
static unsigned hvf_futex_wake_inplace(uint64_t address,const MemoryBacking *shared,size_t offset,unsigned count,uint32_t bits) {
    if(!count || !schedule_active)return 0;
    uint64_t pid=process_pid,epoch=hvf_task()->futex_epoch;unsigned woken=0,b=hvf_futex_bucket(pid,address,shared,offset);
    pthread_mutex_lock(&hvf_futex_buckets[b].lock);
    for(HvfFutexWaiter **link=&hvf_futex_buckets[b].head;*link && woken<count;) {
        HvfFutexWaiter *w=*link;
        int match=shared?w->backing==shared && w->offset==offset:!w->backing && w->pid==pid && w->epoch==epoch && w->address==address;
        if(!match || !(w->bits&bits)) { link=&w->next;continue; }
        if(w->spinner) {                               /* spinning: claimed here, unless EL1 took it (it leaves on its own) */
            uint32_t seen=w->spin;
            if(!__atomic_compare_exchange_n(&w->spinner->state,&seen,(w->spin&~3u)|3,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && seen==((w->spin&~3u)|2)) { link=&w->next;continue; }
        }
        InplaceWait *iw=w->wait;                       /* w is gone once it sees woken */
        *link=w->next;pthread_mutex_lock(&iw->lock);__atomic_store_n(&w->woken,1,__ATOMIC_RELEASE);pthread_cond_signal(&iw->cond);pthread_mutex_unlock(&iw->lock);woken++;
    }
    pthread_mutex_unlock(&hvf_futex_buckets[b].lock);
    return woken;
}
/* All guest memory is process-private, where Linux gives private and shared futex
   keys the same identity: CLONE_CHILD_CLEARTID's shared wake must reach a joiner's
   FUTEX_PRIVATE wait. A futex in shared memory has waiters in any process. */
static unsigned hvf_futex_wake(uint64_t address,unsigned count,uint32_t bits) {
    unsigned woken=0;size_t offset=0;MemoryBacking *shared=hvf_futex_shared(address,&offset);
    woken=hvf_futex_wake_inplace(address,shared,offset,count,bits);
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
    moved=hvf_futex_wake_inplace(address,shared,offset,move,UINT32_MAX); /* woken rather than moved: they retry */
    for(unsigned i=0;i<=process_count && moved<move;i++) {
        ScheduledTask *t=&schedule_tasks[i];
        if(!t->threads || (!shared && t!=hvf_task()))continue;
        for(unsigned j=0;j<t->threads->top && moved<move;j++) {
            Thread *th=&t->threads->thread[j];
            if(th->state!=2)continue;
            if(shared?th->futex_backing==shared && th->futex_offset==offset:!th->futex_backing && th->futex_address==address) {
                th->futex_address=target;th->futex_backing=target_shared;th->futex_offset=target_offset;moved++;
                uint32_t *was=th->futex_count;th->futex_count=hvf_futex_count_up(target,target_shared);if(was)hvf_futex_count_down(was);
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
    hvf_share_image();
    unsigned i=0;while(i<THREAD_LIMIT && (p->thread[i].state || p->thread[i].worker))i++; /* a killed thread may still be leaving a worker */
    if(i==THREAD_LIMIT || hvf_next_tid>1048575)return linux_error(11);
    if(i>=p->top)p->top=i+1;
    Thread *t=&p->thread[i];uint64_t mask=*signal_mask_now();memset(t,0,offsetof(Thread,network)+offsetof(NetWait,fds));
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
    size_t shared_offset=0;MemoryBacking *shared=hvf_futex_shared(cpu->gpr[0],&shared_offset);
    uint32_t *count=hvf_futex_count_up(cpu->gpr[0],shared);
    if(__atomic_load_n((const uint32_t *)(const void *)word,__ATOMIC_SEQ_CST)!=(uint32_t)cpu->gpr[2]) { hvf_futex_count_down(count);return linux_error(11); }
    if(cpu->gpr[3] && deadline<=thread_time_ns(realtime)) { hvf_futex_count_down(count);return linux_error(110); }
    if(!schedule_active) { hvf_futex_count_down(count);return linux_error(38); }
    /* Even a single-threaded process waits (until its timeout, as on Linux). */
    ThreadTable *p=hvf_threads_ensure();if(!p) { hvf_futex_count_down(count);return linux_error(12); }
    ScheduledTask *task=hvf_task();Thread *t=&p->thread[thread_now];
    if(job_control_enabled && ((task->signal_pending|t->signal_pending)&~t->signal_mask&~task_sig(task)->ignored)) { hvf_futex_count_down(count);return linux_error(4); }
    t->state=2;t->futex_address=cpu->gpr[0];t->deadline_ns=deadline;t->realtime=(unsigned)realtime;t->bitset=bits;t->private_wait=(op&128)!=0;
    t->futex_backing=shared;t->futex_offset=shared_offset;
    if(t->futex_count)hvf_futex_count_down(t->futex_count);
    t->futex_count=count;
    *waiting=1;return 0;
}
/* A successful exec in a threaded process ends every other thread; the caller
   becomes the main thread. */
static void hvf_exec_threads(void) {
    ScheduledTask *t=hvf_task();hvf_main_clear_tid[process_pid]=0;hvf_views[process_pid].stale=HVF_STALE_NEW;
    t->futex_epoch=__atomic_add_fetch(&schedule_futex_epochs,1,__ATOMIC_RELAXED); /* in-place waiters of the old image wake no one */
    __atomic_store_n(&proc_block->futex_epoch[t-schedule_tasks],t->futex_epoch,__ATOMIC_RELEASE);
    if(!t->threads)return;
    for(unsigned i=0;i<t->threads->top;i++)if((int)i!=thread_now) { t->threads->thread[i].state=0;futex_uncount(&t->threads->thread[i]); }
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
        uint64_t tgid=call==131?(uint64_t)(int64_t)(int32_t)cpu->gpr[0]:process_pid,target=(uint64_t)(int64_t)(int32_t)cpu->gpr[call==131?1:0],signal=(uint32_t)cpu->gpr[call==131?2:1]; /* ints (syscall_int_args) */
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
/* Whether address lies in a mapping of any protection, PROT_NONE included: a fault
   there is SEGV_ACCERR, as on Linux, and elsewhere SEGV_MAPERR. FEX tells a fault on
   the page it protects to interrupt its JIT code (a deferred signal) by SEGV_ACCERR. */
static int guest_mapped(uint64_t address) {
    const GuestAddressSpace *space=address_space;
    if(map_in_arena(address,1) && space->map_flags && (space->map_flags[(address-MAP_BASE)/4096]&8))return 1;
    for(unsigned i=0;i<space->nseg;i++)if(address>=space->seg[i].base && address-space->seg[i].base<space->seg[i].size)return 1;
    if(address>=space->brk_base && address-space->brk_base<space->brk_pages)return 1;
    return reserve_find(address,1)>=0;
}
/* Under the scheduler a guest fault is a SIGSEGV, SIGBUS or SIGILL for that process: its
   handler runs if it has one and has not blocked the signal (OpenSSL probes CPU
   features this way, JVMs and V8 catch faults on purpose); else the process ends. */
static int hvf_fault(unsigned signal,uint64_t pc,uint64_t address,unsigned ec) {
    if(schedule_active && schedule_tasks[process_pid-1].terminate_signal==9)return -1; /* OOM-killed: the fault was the page it could not get */
    if(schedule_active) {
        ScheduledTask *task=&schedule_tasks[process_pid-1];unsigned index=signal-1;uint64_t bit=UINT64_C(1)<<index;
        if(job_control_enabled && task_sig(task)->handler[index] && !(*signal_mask_now()&bit) && !(task_sig(task)->ignored&bit)) {
            /* ILL_ILLOPC at the instruction; BUS_ADRALN, and SEGV_ACCERR on a mapped page, else SEGV_MAPERR, at the address. */
            /* Per thread: JVM safepoints fault many threads at once, and each handler
               needs its own si_code and si_addr. */
            unsigned code=signal!=11?1u:guest_mapped(address)?2u:1u;uint64_t where=signal==4?pc:address; /* ILL_ILLOPC, BUS_ADRALN */
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
    return ((task->signal_pending|(th?th->signal_pending:0)) & ~*signal_mask_now() & ~task_sig(task)->ignored)!=0;
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
    uint64_t since=profiling?profile_now_ns():0;
    mm_unlock(task);(void)nanosleep(&pause,NULL);mm_lock(task);profile_wait(2,since);
    hvf_frame[0]=0;hvf_resume_el0(hvf_frame[HF_ELR],hvf_frame[HF_SPSR]);return 1;
}
/* futex without the runtime lock (the process's mm lock is held), for a process
   with a thread table: WAKE of a private futex when no thread of the process is
   parked in the scheduler on the address (parking takes this mm lock too, so none
   appears meanwhile; a shared one takes the runtime lock if it is free), and WAIT
   in place while no thread waits for a worker, for up to
   HVF_FUTEX_INPLACE_NS (or its own timeout; longer waits held workers a Go build
   wanted, measured: 1 ms was no better). The waiter is registered as its
   worker's in-place wait: a thread that becomes runnable with no worker free evicts
   it, a signal or the process's end ends it. Returns 0 for the locked path (which
   rereads the call): anything else, or a wait that did not end in time (it parks
   then, after reading the word again). */
#define HVF_FUTEX_INPLACE_NS 200000u
static int runtime_trylock(void);
static int hvf_futex_unlocked(void) {
    if(!schedule_active || trace)return 0;
    ScheduledTask *task=hvf_task();ThreadTable *p=task->threads;
    unsigned op=(unsigned)cpu->gpr[1],command=op&127;uint64_t address=cpu->gpr[0];size_t offset=0;
    if(!p || (op&~255u) || (command!=0 && command!=1 && command!=9 && command!=10) || (address&3))return 0; /* not FUTEX_CLOCK_REALTIME */
    uint32_t bits=command>=9?(uint32_t)cpu->gpr[5]:UINT32_MAX;
    const unsigned char *word=lookup(address,4,4);
    if(!bits || !word)return 0;
    const MemoryBacking *shared=hvf_futex_shared(address,&offset);
    if(command==1 || command==10) {
        if((int32_t)cpu->gpr[2]<0)return 0;
        /* In shared memory its waiters may be parked in any process: the full wake,
           under the runtime lock if that is free now (PostgreSQL's semaphores). */
        if(shared) {
            if(!runtime_trylock())return 0;
            cpu->gpr[0]=hvf_futex_wake(address,(unsigned)cpu->gpr[2],bits);runtime_unlock();return 1;
        }
        for(unsigned j=0;j<p->top;j++) {
            const Thread *th=&p->thread[j];
            if(th->state==2 && !th->futex_backing && th->futex_address==address && (th->bitset&bits))return 0;
        }
        cpu->gpr[0]=hvf_futex_wake_inplace(address,NULL,0,(unsigned)cpu->gpr[2],bits);return 1;
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
    if(hvf_must_stop(task) || !schedule_inplace_allowed(0))return 0;
    InplaceWait *iw=&schedule_inplace[worker->index];
    HvfFutexWaiter w;memset(&w,0,sizeof(w));
    w.pid=process_pid;w.epoch=task->futex_epoch;w.address=address;w.backing=shared;w.offset=offset;w.bits=bits;w.wait=iw;
    unsigned b=hvf_futex_bucket(w.pid,address,shared,offset);
    uint32_t *count=hvf_futex_count_up(address,shared);
    pthread_mutex_lock(&hvf_futex_buckets[b].lock);
    if(__atomic_load_n((const uint32_t *)word,__ATOMIC_SEQ_CST)!=(uint32_t)cpu->gpr[2]) {
        pthread_mutex_unlock(&hvf_futex_buckets[b].lock);hvf_futex_count_down(count);
        cpu->gpr[0]=linux_error(11);return 1;
    }
    if(!shared && proc_block && worker->index<HVF_EL1_SPINNERS) {  /* EL1 may wake it while it spins (hvf_el1.h); before it is linked */
        HvfSpinner *sp=&proc_block->spinners[worker->index];
        w.spin=((__atomic_load_n(&sp->state,__ATOMIC_RELAXED)>>2)+1)<<2|1;
        sp->address=address;sp->epoch=w.epoch;sp->pid=(uint32_t)w.pid;sp->bits=bits;
        __atomic_store_n(&sp->state,w.spin,__ATOMIC_RELEASE);w.spinner=sp;
    }
    w.next=hvf_futex_buckets[b].head;hvf_futex_buckets[b].head=&w;
    pthread_mutex_lock(&iw->lock);iw->task=task;iw->thread=task_thread(task);iw->futex=1;iw->polling=0;__atomic_store_n(&iw->state,1,__ATOMIC_RELEASE);pthread_mutex_unlock(&iw->lock);
    __atomic_add_fetch(&schedule_inplace_count,1,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&hvf_futex_buckets[b].lock);
    mm_unlock(task);
    uint64_t since=profiling?profile_now_ns():0;
    /* A wake usually follows within microseconds (a lock hand-off, a channel ping-pong):
       spin briefly before sleeping, as a condition variable's wake costs several. */
    uint64_t start=thread_time_ns(0),spin_end=start+(wait<SCHEDULE_INPLACE_SPIN_NS?wait:SCHEDULE_INPLACE_SPIN_NS);
    while(!__atomic_load_n(&w.woken,__ATOMIC_ACQUIRE) && __atomic_load_n(&iw->state,__ATOMIC_ACQUIRE)==1 && thread_time_ns(0)<spin_end &&
          (!w.spinner || __atomic_load_n(&w.spinner->state,__ATOMIC_ACQUIRE)==w.spin)) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    int guest_woken=0;                                         /* EL1 took it: no runtime wake comes; it leaves the bucket below */
    if(w.spinner) { uint32_t seen=w.spin;if(!__atomic_compare_exchange_n(&w.spinner->state,&seen,(w.spin&~3u)|3,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))guest_woken=seen==((w.spin&~3u)|2); }
    pthread_mutex_lock(&iw->lock);
    for(;!guest_woken;) {
        if(__atomic_load_n(&w.woken,__ATOMIC_ACQUIRE) || __atomic_load_n(&iw->state,__ATOMIC_ACQUIRE)!=1)break;
        uint64_t at=thread_time_ns(0);if(at-start>=wait)break;
        struct timespec pause={(time_t)((wait-(at-start))/1000000000),(long)((wait-(at-start))%1000000000)};
        (void)pthread_cond_timedwait_relative_np(&iw->cond,&iw->lock,&pause);
    }
    pthread_mutex_unlock(&iw->lock);
    profile_wait(1,since);
    if(!__atomic_load_n(&w.woken,__ATOMIC_ACQUIRE)) {      /* evicted, ended or timed out: leave the bucket unless a wake came meanwhile */
        pthread_mutex_lock(&hvf_futex_buckets[b].lock);
        if(!w.woken)for(HvfFutexWaiter **link=&hvf_futex_buckets[b].head;*link;link=&(*link)->next)if(*link==&w) { *link=w.next;break; }
        pthread_mutex_unlock(&hvf_futex_buckets[b].lock);
    }
    pthread_mutex_lock(&iw->lock);__atomic_store_n(&iw->state,0,__ATOMIC_RELEASE);iw->futex=0;pthread_mutex_unlock(&iw->lock);
    if(w.spinner)__atomic_store_n(&w.spinner->state,w.spin&~3u,__ATOMIC_RELEASE);   /* idle; the next wait takes the next gen */
    __atomic_sub_fetch(&schedule_inplace_count,1,__ATOMIC_RELAXED);hvf_futex_count_down(count);
    mm_lock(task);
    PROFILE_ADD(profile_futex_inplace,1);
    if(w.woken || guest_woken) { PROFILE_ADD(profile_futex_inplace_woken,1);cpu->gpr[0]=0;return 1; }
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
        if(__atomic_load_n(&hvf_held_live,__ATOMIC_ACQUIRE)) {             /* memory held for a fork (hvf_backing_hold) */
            uint64_t low=0,high=0;size_t start=0;unsigned flags=0;
            MemoryBacking *b=hvf_region(address_space,far,&low,&high,&start,&flags);
            if(b && __atomic_load_n(&b->held,__ATOMIC_ACQUIRE)) {
                int kept=1;
                if(__atomic_load_n(&b->frozen,__ATOMIC_ACQUIRE)) {
                    ScheduledTask *task=hvf_task();uint64_t seen=hvf_held_seen();
                    mm_unlock(task);int again=hvf_held_sleep(seen,elr);mm_lock(task);
                    /* Another thread's write may have replaced the backing meanwhile (a
                       snapshot under the lock) and freed it: b is used only if the layout held. */
                    kept=address_space->layout==layout;
                    /* Still frozen (another fork's thaw woke it): the write again. Thawed: its
                       permission back now (below), not after a second fault. */
                    if(again && kept && __atomic_load_n(&b->frozen,__ATOMIC_ACQUIRE)) { hvf_resume_el1=hvf_fault_resume(elr,spsr);return 1; }
                }
                if(kept && hvf_held_restore(b,elr,spsr)) { hvf_resume_el1=1;return 1; }
            }
        }
        if(!hvf_spurious_write(far)) {
            hvf_faulting=1;                                              /* what it copies is remapped below */
            if(!lookup(far,1,2)) { hvf_faulting=0;return 0; }
            address_space_touched(address_space);
            if(address_space->layout==layout && hvf_view==hvf_own_view && hvf_view_matches(address_space)) {
                unsigned around=hvf_fault_around(address_space,far);PROFILE_ADD(profile_fault_around,around);
                hvf_remap_host_page(address_space,far,1+around);hvf_view->generation=address_space->generation;
            } else hvf_own_view->stale=HVF_STALE_COW;
            hvf_faulting=0;
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
    if(!memory && call!=21 && call!=22 && call!=73 && call!=25 && call!=57 && call!=56 && call!=34 && call!=35 && call!=38 && call!=48 && call!=439 && call!=78 && call!=88 && call!=132 && call!=134 && call!=135 && call!=172 && call!=178 && call!=79 && call!=80 && (call<62 || call>68) && call!=82 && call!=83 && call!=206 && call!=207 && call!=71 && call!=291 && call!=221 && call!=166 &&
       !(mem_active && (call==61 || call==36 || call==37 || call==33 || call==52 || call==53 || call==54 || call==55 || call==46 || call==47 ||
                             call==45 || call==276 || call==69 || call==70 || call==286 || call==287 || call==71 || call==285 || call==43 || call==44)))return 0; /* 221: execve builds its image (exec_build); the rest: memfs */
    hvf_get_regs(1,elr,spsr);
    SyscallInts ints;syscall_ints_take(&ints);
    int served=memory?guest_memory_unlocked():guest_syscall_unlocked();
    ov_changed();                                                              /* overlay.h: whether served or retried locked */
    syscall_ints_give(&ints);
    if(!served) { if(ints.changed&1)cpu->gpr[0]=ints.saved[0];return 0; }   /* the locked path rereads the same exit */
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
    if(!count || !net_unlocked_wait.timeout || hvf_must_stop(task) || !schedule_inplace_allowed(0))return 0;
    unsigned budget=net_unlocked_wait.timeout<HVF_DIRECT_WAIT_MS?net_unlocked_wait.timeout:HVF_DIRECT_WAIT_MS;
    /* As an in-place waiter: a signal, the process's end or a thread that needs this
       worker end the wait at once; so does an event of the thread (it retries). */
    static _Thread_local struct pollfd fds[NET_DIRECT_FDS+1];                       /* net_suspend gives at most NET_DIRECT_FDS */
    memcpy(fds,net_unlocked_wait.fds,count*sizeof(*fds));
    int woken=schedule_inplace_block(task,task_thread(task),fds,count,(uint64_t)budget*1000000,0)==2;
    mm_lock(task);
    return woken && !hvf_must_stop(task);
}
/* The worker must leave guest code: the run or the process is ending, the process
   stops, this thread was ended, or a signal awaits delivery. Read without the
   runtime lock: whoever sets these interrupts workers that are in guest code. */
static int hvf_must_stop(ScheduledTask *t) { return t && task_must_stop(t,task_thread(t)); }
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
    int fast=0,in_vector=0,mm_held=0,resumed=0; /* resumed: woken from an in-place wait, holding only the mm lock */
    for(;;) {
        if(!resumed && job_control_enabled && schedule_active && !fast && signal_deliverable()) {
            if(!mm_held) { mm_lock(task);mm_held=1; }
            hvf_fp_pull(); /* a signal frame saves and restores the whole CPU state */
            signal_deliver(0);
            if(task->terminate_signal || task->stop_signal)return -1;
            hvf_put_regs();
        }
        fast=0;
        Worker *self=worker;hv_vcpu_t vcpu=hvf_vcpu;
        hv_return_t ran=HV_SUCCESS;int stopped=0;
        if(!resumed) {
            if(mm_held)mm_unlock(task);
            runtime_unlock();                          /* guest code runs in parallel with other workers */
            mm_lock(task);mm_held=1;
        }
        resumed=0;
        for(;;) {
            /* in_guest is set under the mm lock, so a sibling holding it sees every
               worker of this process that may enter guest code. */
            atomic_store(&self->in_guest,1);atomic_thread_fence(memory_order_seq_cst);
            /* A tick yields only while no worker is idle: an idle one takes any thread
               that becomes runnable (a sleeper's timer) itself. */
            if(!in_vector && (hvf_must_stop(task) || (schedule_active && __atomic_exchange_n(&hvf_cpu->preempt,0,__ATOMIC_ACQ_REL) && !(schedule_idle || schedule_polling)))) {
                atomic_store(&self->in_guest,0);stopped=1;break;  /* (the guest's registers are in the frame or the vCPU) */
            }
            in_vector=0;
            hvf_sync_memory(address_space);
            mm_unlock(task);
            fs_close_pending();
            profile_exit_done();
            uint64_t entered=profiling?profile_now_ns():0;
            /* An interruption (preemption tick, timer) inside EL1 code is resumed at once,
               before the mm lock: an EL1 handler may hold it (hvf_el1_guest.c). */
            for(;;) {
                ran=hv_vcpu_run(vcpu);
                if(ran!=HV_SUCCESS || (hvf_exit->reason!=HV_EXIT_REASON_CANCELED && hvf_exit->reason!=HV_EXIT_REASON_VTIMER_ACTIVATED))break;
                uint64_t mode=0;hvf_check(hv_vcpu_get_reg(vcpu,HV_REG_CPSR,&mode),"get cpsr");
                if(!(mode&0xf))break;
            }
            atomic_store(&self->in_guest,0);
            hvf_cpu->framed=ran==HV_SUCCESS && hvf_exit->reason==HV_EXIT_REASON_EXCEPTION && (hvf_exit->exception.syndrome>>26)==0x16 && (hvf_exit->exception.syndrome&0xffff)==4; /* hvc #4 */
            if(profiling) {
                profile_exit_at=profile_now_ns();__atomic_fetch_add(&profile_guest_ns,profile_exit_at-entered,__ATOMIC_RELAXED);
                unsigned ec=(unsigned)(hvf_frame[HF_ESR]>>26);
                profile_class=!hvf_cpu->framed?(ran==HV_SUCCESS && hvf_exit->reason==HV_EXIT_REASON_EXCEPTION?514:513):ec==0x15?(int)(hvf_frame[8]&511):ec==0x24 && (hvf_frame[HF_ESR]&(UINT64_C(1)<<6))?512:514;
                profile_locked=0;profile_waited_ns=0;
            }
            mm_lock(task);
            if(!worker_threads || ran!=HV_SUCCESS || hvf_must_stop(task)) { profile_locked=1;break; }
            hvf_resume_el1=0;
            if(!hvf_unlocked_exit() && !(hvf_direct_wait(task) && hvf_unlocked_exit())) { profile_locked=1;break; }
            in_vector=hvf_resume_el1;
        }
        /* The runtime lock comes before the mm lock. Waiting for the mm lock with the
           runtime lock held (a sibling thread in a long unlocked call) stalls every
           worker, so take the runtime lock while still holding mm when it is free,
           and otherwise wait for each lock while holding neither. */
        for(unsigned tries=0;!runtime_trylock();tries++) {
            mm_unlock(task);runtime_lock(self);
            if(tries>=2 || !schedule_active || mm_trylock(task)) { if(tries>=2)mm_lock(task);break; }
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
                hvf_own_view->stale=HVF_STALE_NEW;hvf_own_view->generation=UINT64_MAX;
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
                   snapshotted or replaced (a vfork child shares it instead). A
                   process whose memory a vfork child still uses execs once the child
                   has: the child runs on its page tables, which the exec rebuilds. */
                int share=fork && flags==(17|0x100|0x4000) && process_vfork_shares();
                if(exec && shared_image && shared_image->owner==process_pid && shared_image->refs>1) { cpu->pc-=4;return -2; }
                if((fork && !share) || exec)hvf_siblings_out();
                result=guest_syscall();
                if((call>=143 && call<=147) || call==149 || call==277)hvf_fast_push(); /* set*id, seccomp: sibling threads' answers change */
                if(result==-3 && fork && (int64_t)cpu->gpr[0]>0) {     /* the child starts single-threaded, on new tables */
                    hvf_main_clear_tid[cpu->gpr[0]]=0;
                    if(share || fork_shared_private || !hvf_view_fork(cpu->gpr[0]))hvf_views[cpu->gpr[0]].stale=HVF_STALE_NEW; /* else a copy of the parent's */
                    if(fork_shared_private)hvf_own_view->stale=HVF_STALE_COW; /* the fork shared this process's private pages */
                }
                if(exec && process_exec_count!=execs)hvf_exec_threads();
            }
            if(result!=-3) {
                if(result==-2 && !schedule_active)fail("Blocking syscalls need the scheduler (--process-tree)");
                if(result==-2 && schedule_inplace_wait(task)) { hvf_put_regs();resumed=1;continue; }  /* woken in place: the call again */
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
            if(schedule_active && hvf_held_yield(far,elr)) { hvf_get_regs(1,elr,spsr);return -1; }   /* held for a fork: later */
            if(__atomic_load_n(&hvf_held_live,__ATOMIC_ACQUIRE)) {             /* held, now thawed: all of it writable again */
                uint64_t low=0,high=0;size_t start=0;unsigned flags=0;
                MemoryBacking *b=hvf_region(address_space,far,&low,&high,&start,&flags);
                if(b && __atomic_load_n(&b->held,__ATOMIC_ACQUIRE) && hvf_held_restore(b,elr,spsr)) { in_vector=1;fast=1;continue; }
            }
            hvf_faulting=1;                                              /* what it copies is remapped here */
            if(lookup(far,1,2)) {
                /* The page is private (and charged) now: remap its host page writable.
                   A new backing (a fork snapshot) changes the layout: rebuild then. */
                address_space_touched(address_space);
                if(address_space->layout==layout && hvf_view==hvf_own_view && hvf_view_matches(address_space)) {
                    unsigned around=hvf_fault_around(address_space,far);PROFILE_ADD(profile_fault_around,around);
                    hvf_remap_host_page(address_space,far,1+around);hvf_view->generation=address_space->generation;
                } else hvf_own_view->stale=HVF_STALE_COW;
                hvf_faulting=0;in_vector=hvf_fault_resume(elr,spsr);fast=1;continue;
            }
            hvf_faulting=0;
        }
        /* A read or instruction fetch that the address space allows (lookup: 4 read,
           1 execute) faulted: the tables are stale, which is the runtime's fault, not
           the guest's. Rebuild them and retry once; report it, so it can be found. */
        if((ec==0x24 && !(esr&(UINT64_C(1)<<6)) && !HVF_ALIGNMENT(esr) && lookup(far,1,4)) || (ec==0x20 && lookup(far,1,1))) {
            static _Thread_local uint64_t stale_pc,stale_far,stale_generation;static int reported;
            if(elr!=stale_pc || far!=stale_far || address_space->generation!=stale_generation) {
                stale_pc=elr;stale_far=far;stale_generation=address_space->generation;
                if(!reported) { reported=1;fprintf(stderr,"dockiva: stale page tables at pc=0x%" PRIx64 " address=0x%" PRIx64 "; rebuilt (please report)\n",elr,far); }
                hvf_own_view->stale=HVF_STALE_NEW;hvf_own_view->generation=UINT64_MAX;
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
    if(profiling) {
        uint64_t now=profile_now_ns();
        if(profile_slice_end) { uint64_t ns=now-profile_slice_end;__atomic_fetch_add(&profile_sched_ns,ns>profile_waited_ns?ns-profile_waited_ns:0,__ATOMIC_RELAXED); }
        profile_waited_ns=0;__atomic_fetch_add(&profile_slices,1,__ATOMIC_RELAXED);
    }
    int result=hvf_slice_run();
    hvf_fp_pull();mm_unlock(task);fs_close_pending();
    if(profiling) { profile_locked=1;profile_exit_done();profile_slice_end=profile_now_ns();profile_waited_ns=0; }
    return result;
}
static int hvf_run(uint64_t steps) {
    hvf_init();hvf_active=1;
    return run(steps); /* the scheduler (if enabled) calls run(), which slices natively */
}
