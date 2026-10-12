#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>

int main(void) {
    address_space->seg[0]=(Segment){0x1000,4096,6,backing_alloc(4096,1),0};assert(address_space->seg[0].backing);address_space->nseg=1;
    heap_init();assert(guest_brk(address_space->brk_base+4096)==address_space->brk_base+4096);
    GuestAddressSpace peer={0};assert(address_space_fork_private(&peer,address_space));
    shared_image=calloc(1,sizeof(*shared_image));assert(shared_image);shared_image->refs=2;
    ProcessImage parent,child;cpu->gpr[0]=99;image_save(&parent);child=parent;
    image_restore(&child);cpu->gpr[0]=17;
    uint64_t address=guest_mmap(4096,3,0x22,0);assert(address==MAP_BASE);
    store_le(address,42,8);store_le(address_space->brk_base,73,8);image_save(&child);
    image_restore(&parent);assert(cpu->gpr[0]==99);
    assert(read_mem(address,8)==42 && read_mem(address_space->brk_base,8)==73);
    assert(guest_munmap(address,4096)==0);image_save(&parent);
    image_restore(&child);assert(cpu->gpr[0]==17 && !lookup(address,8,4));
    image_free();assert(shared_image==NULL);
    image_restore(&parent);assert(read_mem(address_space->brk_base,8)==73);
    image_free();
    GuestAddressSpace *active=address_space;address_space=&peer;
    assert(read_mem(address_space->brk_base,8)==0);address_space_destroy(&peer);address_space=active;
    assert(resource_used==0);
    puts("PASS shared image mapping metadata, private registers, detach and final release");
    return 0;
}
