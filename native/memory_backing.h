#ifndef VMFREE_MEMORY_BACKING_H
#define VMFREE_MEMORY_BACKING_H
#include <stddef.h>
#ifdef __APPLE__
/* Contiguous private VM views preserve borrowed syscall pointers. Physical COW
   and accounting use host pages (16 KiB on Apple Silicon), not guest pages. */
typedef struct BackingPageGroup BackingPageGroup;
typedef struct { size_t refs;BackingPageGroup *group; } BackingPage;
struct BackingPageGroup { size_t live,count;BackingPage pages[]; };
/* frozen: the backing was the source of a snapshot. Its memory is never written
   again (each holder writes its own copy instead): with several vCPUs first-touching
   pages at once, a write to a snapshot's source can show through in the snapshot. */
typedef struct {
    size_t refs,size,mapped_size,page_size;
    unsigned frozen;
    unsigned char *bytes;
    BackingPage *pages[];
} MemoryBacking;
#else
/* Portable reference implementation retains backing-granular COW. */
typedef struct {
    size_t refs,size;
    _Alignas(max_align_t) unsigned char bytes[];
} MemoryBacking;
#endif
static unsigned char *backing_data(MemoryBacking *backing) {
    return backing?backing->bytes:NULL;
}
#endif
