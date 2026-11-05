#include "memory_backing_ops.h"
/* A worker serving a system call without the runtime lock (see guest_syscall_unlocked)
   sets syscall_unlocked; anything that would need the lock sets syscall_abort
   instead of acting, and the call is redone under the lock. */
static _Thread_local int syscall_unlocked,syscall_abort;
/* Called only while guest execution is stopped. Revisions are worker-wide and
   monotonic so restoring an old snapshot never recreates a cached generation.
   Every backend must synchronize before its next guest instruction. */
static uint64_t address_space_epoch;
/* Contents or page ownership may have changed, the layout did not (image restore,
   copy-on-write detach). */
static void address_space_touched(GuestAddressSpace *space) {
    uint64_t epoch=__atomic_add_fetch(&address_space_epoch,1,__ATOMIC_RELAXED); /* unique across workers */
    if(!epoch)fail("Address-space generation exhausted");
    space->generation=epoch;space->private_range_backing=NULL;space->private_range_previous=0;
    memset(space->lookup_cache,0,sizeof(space->lookup_cache));
}
static void address_space_changed(GuestAddressSpace *space) {
    address_space_touched(space);space->layout=space->generation;
}
/* Only mmap-arena pages [first,end) changed mapping, protection or ownership. */
static void address_space_arena_changed(GuestAddressSpace *space,unsigned first,unsigned end) {
    address_space_touched(space);
    if(space->arena_dirty_lo==space->arena_dirty_hi) { space->arena_dirty_lo=first;space->arena_dirty_hi=end; }
    else {
        if(first<space->arena_dirty_lo)space->arena_dirty_lo=first;
        if(end>space->arena_dirty_hi)space->arena_dirty_hi=end;
    }
    space->arena_rev=space->generation;
    if(end>space->map_top)space->map_top=end;
}
/* Page-flag arrays of dead images, cleared, for the next image: a large arena's is
   megabytes of anonymous memory (3 MiB for 12 GiB), and mapping and unmapping one
   per image cost more than clearing the bytes below its map_top. Only arrays the
   budget does not count (resource_metadata's mapped ones). */
#define MAP_FLAGS_POOL 16u
static struct { unsigned char *flags;uint64_t capacity; } map_flags_pool[MAP_FLAGS_POOL];
static unsigned map_flags_pool_count;
static pthread_mutex_t map_flags_pool_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char *map_flags_new(uint64_t capacity) {
    unsigned char *flags=NULL;
    pthread_mutex_lock(&map_flags_pool_lock);
    for(unsigned i=map_flags_pool_count;i--;)if(map_flags_pool[i].capacity==capacity) { flags=map_flags_pool[i].flags;map_flags_pool[i]=map_flags_pool[--map_flags_pool_count];break; }
    pthread_mutex_unlock(&map_flags_pool_lock);
    return flags?flags:resource_metadata(capacity/4096);
}
static void map_flags_free(unsigned char *flags,uint64_t capacity,unsigned top) {
    if(flags && capacity/4096>=(1u<<20) && top<=(1u<<16)) {
        memset(flags,0,top);                                /* none set at or above map_top */
        pthread_mutex_lock(&map_flags_pool_lock);
        if(map_flags_pool_count<MAP_FLAGS_POOL) { map_flags_pool[map_flags_pool_count].flags=flags;map_flags_pool[map_flags_pool_count++].capacity=capacity;flags=NULL; }
        pthread_mutex_unlock(&map_flags_pool_lock);
    }
    free(flags);
}
static int address_space_map_flags(GuestAddressSpace *space) {
    if(!space->map_flags)space->map_flags=map_flags_new(space->map_capacity);
    return space->map_flags!=NULL;
}
/* A copy of source's state: its header and the segments in use only (all of them
   are 41 KiB, and fork, exec and every task switch copy images several times). */
static void address_space_assign(GuestAddressSpace *destination,const GuestAddressSpace *source) {
    if(destination!=source)memcpy(destination,source,offsetof(GuestAddressSpace,seg)+source->nseg*sizeof(Segment));
}
static void address_space_reset(GuestAddressSpace *space) {
    uint64_t capacity=space->map_capacity;
    memset(space,0,offsetof(GuestAddressSpace,seg)+(space->nseg<=SEGMENT_LIMIT+1?space->nseg:SEGMENT_LIMIT+1)*sizeof(Segment));space->map_capacity=capacity;
    address_space_changed(space);
}
/* The image is dead: no guest code runs with it (see backing_reap). */
static void address_space_destroy(GuestAddressSpace *space) {
#ifdef __APPLE__
    backing_dead_image=1;
#endif
    for(unsigned i=0;i<space->nseg;i++)backing_release(space->seg[i].backing);
    map_flags_free(space->map_flags,space->map_capacity,space->map_top);free(space->reserve); /* file_data: dropped by load_stream */backing_release(space->brk_backing);backing_release(space->map_backing);
#ifdef __APPLE__
    backing_dead_image=0;
#endif
    address_space_reset(space);
}
static size_t address_space_backing_refs(const GuestAddressSpace *space,const MemoryBacking *backing);
/* The image is dead (exit, exec) and its memory goes later (image_destroy_later): the
   sources of the snapshots only it holds thaw now, before its parent hears of it, and
   its overlays let go of their bases. A fork's parent got SIGCHLD while still frozen,
   and its signal frame took a snapshot. */
static void address_space_thaw_dead(GuestAddressSpace *space) {
#ifdef __APPLE__
    for(unsigned i=0;i<space->nseg+2;i++) {
        MemoryBacking *b=i==0?space->map_backing:i==1?space->brk_backing:space->seg[i-2].backing;
        if(b && (b->source || b->base) && b->refs==address_space_backing_refs(space,b)) {
            if(b->source) { backing_thaw(b);b->source=NULL; }
            if(b->base)backing_detach(b);
        }
    }
#else
    (void)space;
#endif
}
/* Multiple views in this address space are one owner for COW purposes. */
static size_t address_space_backing_refs(const GuestAddressSpace *space,const MemoryBacking *backing) {
    size_t local=(space->brk_backing==backing)+(space->map_backing==backing);
    for(unsigned i=0;i<space->nseg;i++)local+=space->seg[i].backing==backing;
    if(!local || !backing->refs || backing->refs<local)fail("Invalid address-space backing ownership");
    return local;
}
/* Detach all local aliases together. macOS uses a VM snapshot and charges only
   host pages touched by this range; the portable reference copies the backing. */
static int address_space_shared(const GuestAddressSpace *space,const MemoryBacking *backing) {
    for(unsigned i=0;i<space->nseg;i++)if(space->seg[i].backing==backing && (space->seg[i].flags&SEG_SHARED))return 1;
    return 0;
}
/* Under the hypervisor a fork copies a private writable backing that owns at most
   fork_copy_pages host pages (a shell's data, stack, heap: a handful) instead of
   sharing it: a shared one costs each process a snapshot on its first write (a VM
   remap, new guest-physical space, every page table entry rebuilt) and the parent's
   tables a copy-on-write pass, several times what copying a few pages does. The copy
   is charged at once, where a shared page is charged on its first write.
   ponytail: a fixed page bound; a process with more private memory shares it all. */
static size_t fork_copy_pages;
/* Whether the last fork shared private writable memory: the parent's pages are then
   copy-on-write (read-only to the hypervisor until written). */
static _Thread_local int fork_shared_private;
static int address_space_writable_backing(const GuestAddressSpace *space,const MemoryBacking *backing) {
    if(space->brk_backing==backing || space->map_backing==backing)return 1;
    for(unsigned i=0;i<space->nseg;i++)if(space->seg[i].backing==backing && (space->seg[i].flags&2))return 1;
    return 0;
}
/* backing as it goes into a fork: a copy (see fork_copy_pages), or itself retained. */
static MemoryBacking *address_space_fork_backing(const GuestAddressSpace *source,MemoryBacking *backing,MemoryBacking **from,MemoryBacking **to,unsigned *copied) {
    for(unsigned i=0;i<*copied;i++)if(from[i]==backing) { if(!backing_retain(to[i],1))return NULL;return to[i]; }
#ifdef __APPLE__
    int own=!address_space_shared(source,backing) && address_space_writable_backing(source,backing) && backing->refs==address_space_backing_refs(source,backing);
    if(fork_copy_pages && *copied<8 && !backing->frozen && own) {
        size_t owned=0;
        for(size_t i=0;i<backing->used && owned<=fork_copy_pages;i++)owned+=backing->pages[i]!=NULL;
        MemoryBacking *copy=owned<=fork_copy_pages?backing_clone(backing,owned):NULL;
        if(copy) { from[*copied]=backing;to[(*copied)++]=copy;return copy; }
    }
    /* A larger one (the hypervisor's hold): the child gets an overlay of it (its pages
       read the parent's until it writes them: backing_overlay), and the parent keeps the
       memory with its guest-physical space and page tables, its owned pages read-only
       until the overlay goes (address_space_fork_private holds it). Shared instead, every
       page became copy-on-write in the parent's tables and each process snapshotted on
       its first write: Node's spawn of a child that execs at once took three snapshots
       and six remaps of tens of thousands of pages. */
    if(backing_hold_hook && *copied<8 && own && (!backing->frozen || backing->held)) {
        MemoryBacking *copy=backing_overlay(backing);
        if(copy) { from[*copied]=backing;to[(*copied)++]=copy;return copy; }
    }
#endif
    if(!backing_retain(backing,1))return NULL;
    if(address_space_writable_backing(source,backing) && !address_space_shared(source,backing))fork_shared_private=1;
    return backing;
}
/* Private fork retains backing without copying bytes (but see fork_copy_pages).
   Staging permits rollback if any reference count overflows. Destination is empty
   or a borrowed snapshot. Source must be a distinct structure; both revisions
   change before resuming (the source's only when it shares private memory now). */
static int address_space_fork_private(GuestAddressSpace *destination,GuestAddressSpace *source) {
    if(destination==source) { errno=EINVAL;return 0; }
    MemoryBacking *from[8],*to[8];unsigned copied=0;fork_shared_private=0;
    GuestAddressSpace copy;address_space_assign(&copy,source);
    copy.file_data=NULL;copy.brk_backing=copy.map_backing=NULL;copy.map_flags=NULL;copy.reserve=NULL;
    memset(copy.lookup_cache,0,sizeof(copy.lookup_cache));
    for(unsigned i=0;i<copy.nseg;i++)copy.seg[i].backing=NULL;
    if(source->map_flags) {
        if(!(copy.map_flags=map_flags_new(source->map_capacity)))goto failed;
        memcpy(copy.map_flags,source->map_flags,source->map_top<source->map_capacity/4096?source->map_top:source->map_capacity/4096); /* none set above map_top */
    }
    if(source->reserve) {
        if(!(copy.reserve=resource_metadata(RESERVE_LIMIT*sizeof(Reservation))))goto failed;
        memcpy(copy.reserve,source->reserve,source->nreserve*sizeof(Reservation));
    }
    for(unsigned i=0;i<copy.nseg;i++)
        if(source->seg[i].backing && !(copy.seg[i].backing=address_space_fork_backing(source,source->seg[i].backing,from,to,&copied)))goto failed;
    if(source->brk_backing && !(copy.brk_backing=address_space_fork_backing(source,source->brk_backing,from,to,&copied)))goto failed;
    if(source->map_backing && !(copy.map_backing=address_space_fork_backing(source,source->map_backing,from,to,&copied)))goto failed;
#ifdef __APPLE__
    for(unsigned i=0;i<copied;i++)if(to[i]->base==from[i])backing_hold_hook(from[i],1);   /* the parent's, while the child's overlay lives (again at every fork: pages written since are writable) */
#endif
    address_space_assign(destination,&copy);if(fork_shared_private)address_space_changed(source);
    address_space_changed(destination);return 1;
failed:
    address_space_destroy(&copy);return 0;
}
static int address_space_make_private(GuestAddressSpace *space,MemoryBacking *backing,size_t offset,size_t size) {
    if(!backing || address_space_shared(space,backing))return 1;               /* MAP_SHARED: writes are visible to every sharer */
    if(offset>backing->size || size>backing->size-offset) { errno=EINVAL;return 0; }
    if(!size)return 1;
    size_t local=backing->refs==1?1:address_space_backing_refs(space,backing);
    if(syscall_unlocked) {          /* no lock: only memory this process already owns and was charged for */
        int owned=backing->refs==local && !backing->frozen;
#ifdef __APPLE__
        for(size_t i=offset/backing->page_size;owned && i<=(offset+size-1)/backing->page_size;i++)owned=backing->pages[i] && backing->pages[i]->refs==1;
#endif
        if(!owned)syscall_abort=1;
        return owned;
    }
#ifdef __APPLE__
    int changed=0;
    if(backing->refs==local && !backing->frozen) {
        if(!backing_prepare(backing,offset,size,&changed))return 0;
        if(changed)PROFILE_ADD(profile_fault_charged,1);else PROFILE_ADD(profile_fault_owned,1);
        if(changed) {
            uint64_t previous=space->generation;address_space_touched(space);
            space->private_range_backing=backing;space->private_range_previous=previous;
            space->private_range_offset=offset/backing->page_size*backing->page_size;
            size_t end=(offset+size+backing->page_size-1)/backing->page_size*backing->page_size;
            space->private_range_size=end-space->private_range_offset;
        }
        return 1;
    }
    PROFILE_ADD(profile_fault_snapshot,1);
    MemoryBacking *copy=backing_snapshot(backing);
    if(!copy)return 0;
    if(!backing_prepare(copy,offset,size,&changed)) { backing_release(copy);return 0; }
#else
    if(backing->refs==local)return 1;
    MemoryBacking *copy=backing_copy(backing,0,backing->size);
    if(!copy)return 0;
#endif
    copy->refs=local;
    for(unsigned i=0;i<space->nseg;i++)if(space->seg[i].backing==backing)space->seg[i].backing=copy;
    if(space->brk_backing==backing)space->brk_backing=copy;
    if(space->map_backing==backing)space->map_backing=copy;
    backing_drop(backing,local);   /* the other holders may have let go meanwhile */
    address_space_changed(space);return 1;
}
static int memory_exhausted(void); /* scheduler.h: the OOM kill */
static int memory_oom;             /* set by memory_exhausted */
static int address_space_write(GuestAddressSpace *space,MemoryBacking *backing,size_t offset,size_t size) {
    if(address_space_make_private(space,backing,offset,size))return 1;
    if(syscall_abort)return 0;
    return memory_exhausted();
}
/* Host bytes for a lookup of [offset, offset+size) of *ref (after address_space_write for
   one that writes). A read of an overlay's pages it has not copied reads its base's
   (frozen: as the fork left them); a read of both kinds copies them now (make_private: the
   guest's tables follow), which unlocked is the locked path's job. NULL if that fails. */
static unsigned char *address_space_bytes(GuestAddressSpace *space,MemoryBacking **ref,size_t offset,size_t size,unsigned permission) {
    if(!*ref)return NULL;
#ifdef __APPLE__
    MemoryBacking *b=*ref;
    if(b->base && size && !(permission&2)) {
        size_t ps=b->page_size,first=offset/ps,last=(offset+size-1)/ps,inherited=0;
        for(size_t i=first;i<=last;i++)inherited+=(size_t)backing_inherits(b,i);
        if(inherited==last-first+1)return BACKING_BASE(b)->bytes+offset;
        if(inherited) {
            if(syscall_unlocked) { syscall_abort=1;return NULL; }
            if(!address_space_write(space,b,offset,size))return NULL;
        }
    }
#else
    (void)space;(void)size;(void)permission;
#endif
    return backing_data(*ref)+offset;
}
/* This address space's own MemoryBacking for the bytes it shares with another
   address space after fork (a VM snapshot; nothing is charged or copied yet). */
static MemoryBacking *address_space_unshare(GuestAddressSpace *space,MemoryBacking *backing) {
    size_t local=backing->refs==1?1:address_space_backing_refs(space,backing);
#ifdef __APPLE__
    if(backing->refs==local && !backing->frozen)return backing;
#else
    if(backing->refs==local)return backing;
#endif
#ifdef __APPLE__
    MemoryBacking *copy=backing_snapshot(backing);
#else
    MemoryBacking *copy=backing_copy(backing,0,backing->size);
#endif
    if(!copy)return NULL;
    copy->refs=local;
    for(unsigned i=0;i<space->nseg;i++)if(space->seg[i].backing==backing)space->seg[i].backing=copy;
    if(space->brk_backing==backing)space->brk_backing=copy;
    if(space->map_backing==backing)space->map_backing=copy;
    backing_drop(backing,local);
    address_space_changed(space);return copy;
}
/* Zero [offset,offset+size) of the mmap arena, as Linux does for fresh anonymous
   pages and MADV_DONTNEED. Whole host pages are released (and uncharged); the
   partial host pages at either end are zeroed in place. */
static int address_space_zero_partial(GuestAddressSpace *space,MemoryBacking *backing,size_t begin,size_t end) {
#ifdef __APPLE__
    for(size_t page=begin/backing->page_size*backing->page_size;page<end;page+=backing->page_size) {
        size_t low=begin>page?begin:page,high=end<page+backing->page_size?end:page+backing->page_size;
        if(!backing->pages[page/backing->page_size] && !backing_inherits(backing,page/backing->page_size))continue; /* unowned: already zero (an overlay's base page is not) */
        if(!address_space_make_private(space,backing,low,high-low))return 0;
        memset(backing->bytes+low,0,high-low);
    }
#else
    if(end>begin) { if(!address_space_make_private(space,backing,begin,end-begin))return 0;memset(backing->bytes+begin,0,end-begin); }
#endif
    return 1;
}
/* Zero [offset,offset+size) of one of this space's private backings: whole host pages
   are released (and uncharged), partial ones zeroed in place. */
static int address_space_discard(GuestAddressSpace *space,MemoryBacking *shared,size_t offset,size_t size) {
    if(!shared || !size)return 1;
    MemoryBacking *backing=address_space_unshare(space,shared);if(!backing)return 0;
    size_t end=offset+size;
#ifdef __APPLE__
    size_t low=(offset+backing->page_size-1)/backing->page_size*backing->page_size,high=end/backing->page_size*backing->page_size;
    if(low<high) {
        if(!address_space_zero_partial(space,backing,offset,low) || !address_space_zero_partial(space,backing,high,end))return 0;
        backing_discard(backing,low,high-low);return 1;
    }
#endif
    return address_space_zero_partial(space,backing,offset,end);
}
static int address_space_discard_map(GuestAddressSpace *space,size_t offset,size_t size) {
    return address_space_discard(space,space->map_backing,offset,size);
}
