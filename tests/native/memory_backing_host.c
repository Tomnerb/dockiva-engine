#define main vmfree_program_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>

static uint64_t protect_middle(void) {
    cpu->gpr[0]=0x11000;cpu->gpr[1]=4096;cpu->gpr[2]=1;return guest_mprotect();
}
int main(void) {
    assert(!backing_alloc(SIZE_MAX,0));assert(resource_used==0);
    MemoryBacking *backing=backing_alloc(12288,1);assert(backing);
    address_space->seg[0]=(Segment){0x10000,12288,6,backing,0};address_space->nseg=1;
    address_space_changed(address_space);heap_init();
    store_le(0x10000,11,8);store_le(0x11000,22,8);store_le(0x12000,33,8);
    uint64_t generation=address_space->generation,used=resource_used;
    backing->refs=SIZE_MAX;
    assert(protect_middle()==linux_error(12));
    assert(backing->refs==SIZE_MAX && address_space->nseg==1 && address_space->generation==generation);
    backing->refs=1;
    uint64_t budget=resource_limit;resource_limit=used;
    assert(protect_middle()==0);resource_limit=budget;
    assert(backing->refs==3 && resource_used==used && address_space->nseg==3);
    assert(!lookup(0x11000,8,2) && lookup(0x10000,8,2) && lookup(0x12000,8,2));
    assert(read_mem(0x10000,8)==11 && read_mem(0x11000,8)==22 && read_mem(0x12000,8)==33);
    puts("PASS: mprotect splits retain offset views without copying or double charging");

    assert(guest_brk(address_space->brk_base+4096)==address_space->brk_base+4096);
    assert(guest_mmap(4096,3,0x22,0)==MAP_BASE);
    store_le(address_space->brk_base,44,8);store_le(MAP_BASE,55,8);
    GuestAddressSpace copy={.map_capacity=MAP_CAP,.generation=123};
    used=resource_used;generation=address_space->generation;
    /* Reject a late retain after segment/heap references were acquired. */
    size_t map_refs=address_space->map_backing->refs;address_space->map_backing->refs=SIZE_MAX;
    assert(!address_space_fork_private(&copy,address_space));
    address_space->map_backing->refs=map_refs;
    assert(resource_used==used && backing->refs==3 && address_space->generation==generation);
    assert(copy.generation==123 && !copy.nseg && !copy.map_backing);
    assert(!backing_copy(backing,backing->size,1));assert(resource_used==used);
    resource_limit=used;assert(address_space_fork_private(&copy,address_space));resource_limit=budget; /* nothing copied */
    assert(resource_used==used && backing->refs==6 && copy.map_flags==address_space->map_flags);
    assert(copy.seg[0].backing==backing && copy.seg[0].offset==4096);
    assert(copy.brk_backing==address_space->brk_backing && copy.map_backing==address_space->map_backing);
    GuestAddressSpace *parent=address_space;address_space=&copy;
    assert(read_mem(0x11000,8)==22 && read_mem(address_space->brk_base,8)==44 && read_mem(MAP_BASE,8)==55);
    store_le(0x10000,99,8);store_le(address_space->brk_base,88,8);store_le(MAP_BASE,77,8);
    address_space=parent;
    assert(read_mem(0x10000,8)==11 && read_mem(address_space->brk_base,8)==44 && read_mem(MAP_BASE,8)==55);
    address_space_destroy(&copy);assert(resource_used==used);
    puts("PASS: COW fork retains offsets/heap/mmap and shares page flags, copying nothing; reference overflow rolls back");

    GuestAddressSpace retained={.map_capacity=MAP_CAP};
    retained.seg[0]=address_space->seg[0];retained.nseg=1;
    assert(backing_retain(backing,1));address_space_changed(&retained);
    address_space_destroy(parent);assert(backing->refs==1);
    address_space=&retained;assert(read_mem(0x11000,8)==22);
    address_space_destroy(&retained);assert(resource_used==0);
    address_space_destroy(&retained);assert(resource_used==0);
    address_space=parent;
    puts("PASS: retained view survives original-owner teardown; final release frees backing exactly once");
    const char *args[]={"probe","argument"},*env[]={"TEST=value"};
    startup(2,args,1,env);
    assert(read_mem(cpu->sp,8)==2);
    assert(!strcmp((char *)lookup(read_mem(cpu->sp+8,8),6,4),args[0]));
    assert(!strcmp((char *)lookup(read_mem(cpu->sp+16,8),9,4),args[1]));
    assert(!strcmp((char *)lookup(read_mem(cpu->sp+32,8),11,4),env[0]));
    address_space_destroy(parent);assert(resource_used==0);
    puts("PASS: startup arguments, environment and random bytes use backing data offsets");
    return 0;
}
