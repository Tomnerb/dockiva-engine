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
}
static int address_space_map_flags(GuestAddressSpace *space) {
    if(!space->map_flags)space->map_flags=calloc(space->map_capacity/4096,1);
    return space->map_flags!=NULL;
}
static void address_space_reset(GuestAddressSpace *space) {
    unsigned capacity=space->map_capacity;
    memset(space,0,sizeof(*space));space->map_capacity=capacity;
    address_space_changed(space);
}
static void address_space_destroy(GuestAddressSpace *space) {
    for(unsigned i=0;i<space->nseg;i++)backing_release(space->seg[i].backing);
    free(space->file_data);free(space->map_flags);backing_release(space->brk_backing);backing_release(space->map_backing);
    address_space_reset(space);
}
/* Private fork retains backing without copying bytes. Staging permits rollback
   if any reference count overflows. Destination is empty or a borrowed snapshot.
   Source must be a distinct structure; both revisions change before resuming. */
static int address_space_fork_private(GuestAddressSpace *destination,GuestAddressSpace *source) {
    if(destination==source) { errno=EINVAL;return 0; }
    GuestAddressSpace copy=*source;
    copy.file_data=NULL;copy.brk_backing=copy.map_backing=NULL;copy.map_flags=NULL;
    memset(copy.lookup_cache,0,sizeof(copy.lookup_cache));
    for(unsigned i=0;i<copy.nseg;i++)copy.seg[i].backing=NULL;
    if(source->map_flags) {
        if(!(copy.map_flags=malloc(source->map_capacity/4096)))goto failed;
        memcpy(copy.map_flags,source->map_flags,source->map_capacity/4096);
    }
    for(unsigned i=0;i<copy.nseg;i++) {
        if(!backing_retain(source->seg[i].backing,1))goto failed;
        copy.seg[i].backing=source->seg[i].backing;
    }
    if(source->brk_backing) {
        if(!backing_retain(source->brk_backing,1))goto failed;
        copy.brk_backing=source->brk_backing;
    }
    if(source->map_backing) {
        if(!backing_retain(source->map_backing,1))goto failed;
        copy.map_backing=source->map_backing;
    }
    *destination=copy;address_space_changed(source);address_space_changed(destination);return 1;
failed:
    address_space_destroy(&copy);return 0;
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
        if(changed) {
            uint64_t previous=space->generation;address_space_touched(space);
            space->private_range_backing=backing;space->private_range_previous=previous;
            space->private_range_offset=offset/backing->page_size*backing->page_size;
            size_t end=(offset+size+backing->page_size-1)/backing->page_size*backing->page_size;
            space->private_range_size=end-space->private_range_offset;
        }
        return 1;
    }
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
static int address_space_write(GuestAddressSpace *space,MemoryBacking *backing,size_t offset,size_t size) {
    if(address_space_make_private(space,backing,offset,size))return 1;
    if(syscall_abort)return 0;
    fail("Guest memory budget exhausted (--memory-limit-mib)");return 0;
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
        if(!backing->pages[page/backing->page_size])continue; /* unowned: already zero */
        if(!address_space_make_private(space,backing,low,high-low))return 0;
        memset(backing->bytes+low,0,high-low);
    }
#else
    if(end>begin) { if(!address_space_make_private(space,backing,begin,end-begin))return 0;memset(backing->bytes+begin,0,end-begin); }
#endif
    return 1;
}
static int address_space_discard_map(GuestAddressSpace *space,size_t offset,size_t size) {
    if(!space->map_backing || !size)return 1;
    MemoryBacking *backing=address_space_unshare(space,space->map_backing);if(!backing)return 0;
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
