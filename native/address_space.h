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
/* Largest mmap arena: virtual, pages are charged on first write. Each process's arena
   takes guest-physical space under the hypervisor (1 TiB with a 40-bit IPA), so 64
   processes of 12 GiB still fit. */
#define MAP_MAX (UINT64_C(12288)*1024*1024)
/* 12 GiB: where a JVM first asks for its compressed class space (4 GiB-aligned
   attach points from 12 GiB, MAP_FIXED_NOREPLACE). The largest arena ends at 24 GiB. */
#define MAP_BASE UINT64_C(0x300000000)
/* Where Linux on arm64 (48-bit, no randomization) loads programs: a position-
   independent one at ELF_ET_DYN_BASE (2/3 of the address space), the dynamic loader
   (ld-musl, ld-linux) and a static PIE just below mmap_base (top minus 128 MiB). */
#define PIE_BASE UINT64_C(0xaaaaaaaaa000)
#define INTERPRETER_TOP UINT64_C(0xfffff8000000)
/* The end of guest user addresses: the hypervisor backend's EL1 page is at the top. A
   48-bit Linux ends user space at 1 << 48 (USER_VA_END): the 64 KiB between are shown
   as mapped (/proc/self/maps) and refuse mappings (MAP_FIXED_NOREPLACE: EEXIST), so a
   program that reserves the gaps /proc/self/maps lists, as FEX's allocator does, never
   counts on memory it could not get there. */
#define GUEST_VA_END UINT64_C(0xffffffff0000)
#define USER_VA_END (UINT64_C(1)<<48)
/* Segment flag: MAP_SHARED memory (shared anonymous mappings, System V shm). Its
   backing is never copied on write, so fork keeps every process on the same bytes. */
#define SEG_SHARED 8u
/* Segment flag: a shared segment holding an uncharged copy of a large read-only file
   mapping (mapping.h); any change to part of it makes it ordinary arena pages first. */
#define SEG_FILE_COPY 16u
/* map_flags value for arena pages lent to a shared segment (no arena access). */
#define MAP_RESERVED 0x20u

typedef struct { uint64_t base,size;unsigned flags;MemoryBacking *backing;size_t offset; } Segment;
/* Address space mapped outside the arena (mapping.h): what munmap, MAP_FIXED_NOREPLACE
   and /proc/self/maps see there. Segments inside supply its pages; the rest is
   PROT_NONE. Sorted, disjoint, adjacent ranges joined. */
typedef struct { uint64_t base,size; } Reservation;
#define RESERVE_LIMIT 65536u /* Linux's default vm.max_map_count */
/* Mappings outside the arena's page flags: the program, shared memory, mapped files
   (Go's linker maps every package archive it reads), reservations' commits. */
#define SEGMENT_LIMIT 1024u
static unsigned char *segment_data(const Segment *segment) {
    return segment->backing?backing_data(segment->backing)+segment->offset:NULL;
}
/* One authoritative mapping/backing model. Copies in ProcessImage are borrowed
   snapshots; private fork retains COW backing, while vfork shares its owning image.
   General MAP_SHARED is not implemented. */
typedef struct {
    unsigned nseg;uint64_t map_capacity; /* mmap arena bytes (--mmap-limit-mib) */
    unsigned char *file_data; /* Loader input (a read-only host mapping of the file), not a guest mapping. */
    size_t file_size;
    uint64_t auxv[48];unsigned auxv_count; /* the process's auxiliary vector, for /proc/self/auxv */
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
    /* RESERVE_LIMIT entries, allocated on first use; heap-owned like map_flags. */
    Reservation *reserve;unsigned nreserve;
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
    Segment seg[SEGMENT_LIMIT+1];     /* last: copies take the nseg in use (address_space_assign) */
} GuestAddressSpace;
#endif
