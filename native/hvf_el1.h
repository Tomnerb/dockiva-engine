/* What the EL1 handlers (hvf_el1_guest.c, compiled for the guest and run inside it by
   the exception vector) share with the runtime: one context page the runtime fills,
   read by EL1 code at HVF_EL1_VA. Plain fixed-width fields only: the guest build is
   freestanding and knows nothing of the host's structures, so the runtime publishes
   their layout (offsets) here. */
#ifndef VMFREE_HVF_EL1_H
#define VMFREE_HVF_EL1_H
#include <stdint.h>
#define HVF_EL1_MOUNTS 16
typedef struct {
    uint64_t version;                  /* even: stable; odd: the runtime is changing the mounts */
    uint64_t gen,cache;                /* EL1 addresses of the lookup cache's counters and entries (overlay.h) */
    uint32_t slots,buckets,entry,path; /* OV_CACHE_SLOTS, OV_BUCKETS, sizeof(OvCached), OV_CACHE_PATH */
    uint32_t e_seq,e_gen,e_made,e_bucket,e_layer,e_lower,e_follow,e_path,e_st; /* OvCached field offsets */
    uint32_t s_mode,s_nlink,s_ino,s_size,s_atime,s_mtime,s_ctime;           /* host struct stat: mode and nlink 16-bit, times {s, ns} */
    uint32_t mounts;                   /* bind and memory mounts below which the runtime answers; UINT32_MAX: it answers everything */
    uint32_t mount_length[HVF_EL1_MOUNTS];
    char mount[HVF_EL1_MOUNTS][256];
} HvfEl1Context;
#endif
