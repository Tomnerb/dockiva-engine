/* What the EL1 handlers (hvf_el1_guest.c, compiled for the guest and run inside it by
   the exception vector) share with the runtime: one context page the runtime fills,
   read by EL1 code at HVF_EL1_VA. Plain fixed-width fields only: the guest build is
   freestanding and knows nothing of the host's structures, so the runtime publishes
   their layout (offsets) here. */
#ifndef VMFREE_HVF_EL1_H
#define VMFREE_HVF_EL1_H
#include <stdint.h>
#define HVF_EL1_MOUNTS 16
/* An open description of a file in the image's pack (pack.h), in a pool both sides
   allocate from: entry indexes the pack's index; real is the host descriptor once a
   call needed the real file (-1 until then); st is the host's struct stat of it. */
#define HVF_EL1_PACK_OPENS 32768u
typedef struct { uint64_t entry,offset;int32_t real;uint32_t refs,used,spare;uint64_t st[18]; } HvfPackOpen;
/* Small anonymous regions a process maps and soon unmaps (musl's malloc): the runtime
   registers each it makes (base, pages: 4-way buckets by page number), an EL1 munmap of
   exactly one parks it, still mapped, and an EL1 mmap of as many pages hands a parked
   one back zeroed. Any other mapping change over a registered or parked range first
   unmaps the parked regions there and forgets the registered ones (mm_cache_range). */
#define HVF_EL1_REGISTERED 256u
#define HVF_EL1_PARKED 64u
#define HVF_EL1_PARK_PAGES 256u                      /* at most 1 MiB parked per process */
#define HVF_EL1_REGION_PAGES 16u                     /* regions of at most 64 KiB */
typedef struct {
    uint64_t base[HVF_EL1_REGISTERED];uint32_t pages[HVF_EL1_REGISTERED];
    uint64_t park_base[HVF_EL1_PARKED];uint32_t park_pages[HVF_EL1_PARKED];uint32_t parked,parked_pages;
} HvfMmCache;
/* Futex waiters, counted for EL1's FUTEX_WAKE (hvf_el1_guest.c): a slot per
   (process, address) hash, raised by every waiter before it reads the futex word and
   lowered once it waits no more, and one count for every futex in shared memory
   (keyed by its memory, so no address finds it). A wake whose slot and the shared
   count are both zero has no one to wake: 0, with no exit. Too high is only slower. */
#define HVF_EL1_FUTEX_SLOTS 4096u
static inline uint32_t hvf_el1_futex_slot(uint64_t pid,uint64_t address) {
    return (uint32_t)(((address>>2)^(address>>14)^(pid*UINT64_C(0x9e3779b1)))&(HVF_EL1_FUTEX_SLOTS-1));
}
/* A thread waiting in place on a private futex while it spins (hvf_futex_unlocked),
   one slot per worker: EL1's FUTEX_WAKE of one waiter claims it by changing state from
   gen<<2|1 to gen<<2|2, with no exit; the runtime's wakes claim it with gen<<2|3 (also
   what the waiter sets when it stops spinning to sleep), so each waiter is woken once.
   gen changes with every wait: a slot read between two waits never matches. */
#define HVF_EL1_SPINNERS 64u
typedef struct { uint64_t address,epoch;uint32_t pid,bits,state,pad; } HvfSpinner;
/* An entry of the pack's index (pack.h), sorted by inode. */
typedef struct { uint64_t ino,size,offset;int64_t mtime,mtime_ns;uint32_t path,path_length; } HvfPackEntry;
typedef struct {
    uint64_t version;                  /* even: stable; odd: the runtime is changing the mounts */
    uint64_t gen,cache;                /* EL1 addresses of the lookup cache's counters and entries (overlay.h) */
    uint32_t slots,buckets,entry,path; /* OV_CACHE_SLOTS, OV_BUCKETS, sizeof(OvCached), OV_CACHE_PATH */
    uint32_t e_seq,e_gen,e_made,e_bucket,e_layer,e_lower,e_follow,e_path,e_st; /* OvCached field offsets */
    uint32_t s_mode,s_nlink,s_ino,s_size,s_atime,s_mtime,s_ctime;           /* host struct stat: mode and nlink 16-bit, times {s, ns} */
    uint32_t mounts;                   /* bind and memory mounts below which the runtime answers; UINT32_MAX: it answers everything */
    uint32_t mount_length[HVF_EL1_MOUNTS];
    char mount[HVF_EL1_MOUNTS][256];
    /* Opens and closes of image files in the pack (zero pack_count: the runtime's):
       the process block (fs.h) at block, the runtime's own address of it at host_block
       (descriptor slots hold its pointers to pool entries), and its layout. */
    uint64_t block,host_block,pack_entries,pack_strings,pack_count,file_limit;
    uint32_t b_mm,b_locks,b_hint,b_fs,b_fds,b_pack,fs_size,fds_size;
    uint32_t t_fd,t_cloexec,t_ro,t_dev,t_sole,t_dirs,t_guest,t_pack,d_kind,d_index;
    uint32_t marker,fd_file,mirror_file,mirror_cloexec,fs_slots,fd_limit;
    uint32_t b_mmc;                                   /* the block's HvfMmCache array, by mm lock index */
    uint32_t b_futex;                                 /* the block's futex counts: HVF_EL1_FUTEX_SLOTS, then the shared one */
    uint32_t b_spin,b_epoch;                          /* the block's HvfSpinner array and each task's futex epoch (uint64) */
} HvfEl1Context;
#endif
