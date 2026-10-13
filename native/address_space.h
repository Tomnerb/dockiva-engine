#ifndef VMFREE_ADDRESS_SPACE_H
#define VMFREE_ADDRESS_SPACE_H
#include <stdint.h>
#include "memory_backing.h"
#define GUEST_PAGE UINT64_C(4096)
#define BRK_CAP (8u*1024u*1024u)
/* The main thread's stack: 8 MiB below STACK_TOP, charged as pages are used. */
#define STACK_TOP UINT64_C(0x7ffffff00000)
#define STACK_SIZE (8u*1024u*1024u)
/* exec limits: argument and environment bytes and entries (Linux allows a quarter
   of the stack limit). */
#define EXEC_STRINGS (1024u*1024u)
#define EXEC_ENTRIES 16384u
#define MAP_CAP (16u*1024u*1024u)
#define MAP_MAX (2048u*1024u*1024u) /* virtual arena; pages are charged on first write */
#define MAP_BASE UINT64_C(0x1000000000)
/* Segment flag: MAP_SHARED memory (shared anonymous mappings, System V shm). Its
   backing is never copied on write, so fork keeps every process on the same bytes. */
#define SEG_SHARED 8u
/* map_flags value for arena pages lent to a shared segment (no arena access). */
#define MAP_RESERVED 0x20u

typedef struct { uint64_t base,size;unsigned flags;MemoryBacking *backing;size_t offset; } Segment;
static unsigned char *segment_data(const Segment *segment) {
    return segment->backing?backing_data(segment->backing)+segment->offset:NULL;
}
/* One authoritative mapping/backing model. Copies in ProcessImage are borrowed
   snapshots; private fork retains COW backing, while vfork shares its owning image.
   General MAP_SHARED is not implemented. */
typedef struct {
    Segment seg[129];
    unsigned nseg,map_capacity;
    unsigned char *file_data; /* Loader input, not a guest mapping. */
    MemoryBacking *brk_backing,*map_backing;
    uint64_t entry,phaddr,phcount,interpreter_base;
    uint64_t brk_base,brk_current,brk_pages;
    /* Per guest page in the mmap arena: 8 mapped, 4 read, 2 write, 1 execute,
       16 file-backed. Heap-owned by this process (scheduler images borrow it). */
    unsigned char *map_flags;
    unsigned map_free_hint;           /* every arena page below it is mapped */
    /* no map_large_pages consecutive free pages lie in [map_free_hint, map_large_from)
       (0 pages: unknown); first fit for at least that many skips most of it */
    unsigned map_large_from,map_large_pages;
    unsigned map_top;                 /* no arena page at or above it has ever been mapped */
    uint64_t layout; /* unique revision of the mapping layout (not of page contents) */
    /* mmap-arena changes since revision arena_dirty_since all lie in pages
       [arena_dirty_lo,arena_dirty_hi); a backend may update just that range. */
    uint64_t arena_rev,arena_dirty_since;unsigned arena_dirty_lo,arena_dirty_hi;
    Segment *lookup_cache[8];
    uint64_t generation;
    /* Optional single-revision COW permission delta. Other mutations invalidate
       this hint; a backend that missed a revision must rebuild its full view. */
    MemoryBacking *private_range_backing;
    size_t private_range_offset,private_range_size;
    uint64_t private_range_previous;
} GuestAddressSpace;
#endif
