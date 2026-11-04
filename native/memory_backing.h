#ifndef VMFREE_MEMORY_BACKING_H
#define VMFREE_MEMORY_BACKING_H
#include <stddef.h>
#ifdef __APPLE__
/* Contiguous private VM views preserve borrowed syscall pointers. Physical COW
   and accounting use host pages (16 KiB on Apple Silicon), not guest pages. */
typedef struct BackingPageGroup BackingPageGroup;
typedef struct { size_t refs;BackingPageGroup *group; } BackingPage;
struct BackingPageGroup { size_t live,count;unsigned uncharged;BackingPage pages[]; }; /* uncharged: read-only program text */
/* frozen: the backing was the source of a snapshot. Its memory is not written while
   one lives (each holder writes its own copy instead): with several vCPUs first-
   touching pages at once, a write to a snapshot's source can show through in the
   snapshot. Once its last snapshot's host memory is gone it thaws (backing_thaw):
   the parent of a fork whose child copied, then exec'd, writes its own memory again
   instead of copying all of it too. source, source_serial: a snapshot's source. */
/* hvf_ipa, hvf_epoch: the hypervisor backend's guest-physical address for it, valid
   while hvf_epoch matches; hvf_chunks: the same per chunk of a large one (see hvf_ipa_at). */
/* used: one past the highest page ever owned; every page above it is unowned, so
   snapshots and releases need not walk the rest (a 12 GiB arena is 786,432 pages,
   and a forked process usually touches a few hundred). */
/* cow_copy: a snapshot (its host pages are copy-on-write with its source's).
   unpooled: a reference to it went other than with a dead image (see backing_reap). */
typedef struct {
    size_t refs,size,mapped_size,page_size,used;
    unsigned frozen,cow_copy,unpooled,recycle;
    size_t warm;                      /* pooled: bytes of its host memory still resident, zeroed (backing_pool_take) */
    unsigned char *bytes;
    uint64_t hvf_ipa,hvf_epoch,*hvf_chunks;
    void *reap_next;                  /* in backing_reap's queue */
    void *source;uint64_t source_serial;
    BackingPage *pages[];
} MemoryBacking;
#else
/* Portable reference implementation retains backing-granular COW. */
typedef struct {
    size_t refs,size;
    _Alignas(max_align_t) unsigned char bytes[];
} MemoryBacking;
#endif
static void (*backing_release_hook)(MemoryBacking *); /* the hypervisor backend forgets the memory first */
static unsigned char *backing_data(MemoryBacking *backing) {
    return backing?backing->bytes:NULL;
}
#endif
