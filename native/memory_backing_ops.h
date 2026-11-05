#ifdef __APPLE__
#include "memory_backing_mach.h"
#else
static MemoryBacking *backing_alloc(size_t size,int clear) {
    if(size>SIZE_MAX-sizeof(MemoryBacking)) { errno=ENOMEM;return NULL; }
    MemoryBacking *backing=malloc(sizeof(*backing)+size);
    if(!backing)return NULL;
    backing->refs=1;backing->size=size;
    if(clear)memset(backing->bytes,0,size);
    return backing;
}
static MemoryBacking *backing_reserve(size_t size) { return backing_alloc(size,1); }
static MemoryBacking *backing_alloc_text(size_t size) { return backing_alloc(size,1); }
static int backing_inherits(const MemoryBacking *b,size_t page) { (void)b;(void)page;return 0; }   /* overlays are the Mach backing's */
#endif
static int backing_retain(MemoryBacking *backing,size_t count) {
    if(!backing || !backing->refs || count>SIZE_MAX-backing->refs) { errno=ENOMEM;return 0; }
    __atomic_fetch_add(&backing->refs,count,__ATOMIC_ACQ_REL);return 1;
}
/* Drop count references; holders in other processes may drop theirs concurrently. */
static void backing_drop(MemoryBacking *backing,size_t count) {
    if(!backing)return;
    size_t refs=__atomic_load_n(&backing->refs,__ATOMIC_ACQUIRE);
    if(refs<count)fail("Invalid memory backing ownership");
#ifdef __APPLE__
    if(!backing_dead_image)__atomic_store_n(&backing->unpooled,1,__ATOMIC_RELAXED);
#endif
    if(!__atomic_sub_fetch(&backing->refs,count,__ATOMIC_ACQ_REL)) {
#ifdef __APPLE__
        if(backing_orphan(backing))return;           /* overlays still read it: the last frees it */
        if(backing_reap(backing))return;
#endif
        if(backing_release_hook)backing_release_hook(backing);
#ifdef __APPLE__
        backing_vm_release(backing);
#else
        free(backing);
#endif
    }
}
static void backing_release(MemoryBacking *backing) { backing_drop(backing,1); }
static inline MemoryBacking *backing_copy(const MemoryBacking *source,size_t offset,size_t size) {
    if(!source || offset>source->size || size>source->size-offset) { errno=EINVAL;return NULL; }
    MemoryBacking *copy=backing_alloc(size,0);
    if(copy)memcpy(copy->bytes,source->bytes+offset,size);
    return copy;
}
