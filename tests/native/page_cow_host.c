/* macOS host-page COW: large arenas, cross-page ranges and stable pointers. */
#define main vmfree_program_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
#ifndef __APPLE__
    puts("SKIP: Mach host-page COW requires macOS");return 77;
#else
    GuestAddressSpace parent={.map_capacity=MAP_MAX},child={0},grandchild={0};
    address_space=&parent;
    size_t length=64u*1024*1024;
    parent.seg[0]=(Segment){0x10000,length,6,backing_alloc(length,1),0};parent.nseg=1;
    assert(parent.seg[0].backing);address_space_changed(&parent);
    size_t page=parent.seg[0].backing->page_size;
    store_le(0x10000,11,8);store_le(0x10000+page,22,8);store_le(0x10000+length-8,33,8);
    uint64_t base=resource_used;
    assert(address_space_fork_private(&child,&parent));assert(resource_used==base);
    address_space=&child;
    unsigned char *first=lookup(0x10000,8,2);assert(first);fs_pack(first,0,44,8);
    uint64_t first_growth=resource_used-base;
    assert(first_growth<page+length/page*sizeof(void *)+1024);
    uint64_t used=resource_used;
    assert(lookup(0x10008,8,2)==first+8 && resource_used==used);
    /* A later write to another page cannot invalidate an earlier output pointer. */
    unsigned char *other=lookup(0x10000+2*page,8,2);assert(other);fs_pack(other,0,55,8);fs_pack(first,0,66,8);
    uint64_t second_growth=resource_used-used;
    assert(second_growth>=page && second_growth<page+256);
    assert(read_mem(0x10000,8)==66 && read_mem(0x10000+2*page,8)==55);
    assert(address_space_fork_private(&grandchild,&child));
    address_space=&grandchild;store_le(0x10000,77,8);
    address_space=&parent;assert(read_mem(0x10000,8)==11 && read_mem(0x10000+2*page,8)==0);
    store_le(0x10000+page,88,8);
    address_space=&child;assert(read_mem(0x10000+page,8)==22);
    /* New shared pages require an all-or-nothing budget reservation. */
    used=resource_used;uint64_t revision=child.generation,budget=resource_limit;
    resource_limit=used+page; /* insufficient for two host pages plus metadata */
    assert(!address_space_make_private(&child,child.seg[0].backing,4*page-8,16));
    assert(resource_used==used && child.generation==revision);
    resource_limit=budget;
    memset(lookup(0x10000+4*page-8,16,2),0xa5,16);
    assert(resource_used-used>=2*page && resource_used-used<2*page+256);
    address_space=&parent;assert(read_mem(0x10000+4*page-8,8)==0);
    /* Parent teardown cannot invalidate snapshots. (A borrowed pointer lasts until its
       backing is copied again: the child's write above replaced its backing, which the
       grandchild's fork had made a snapshot source.) */
    address_space_destroy(&parent);address_space=&child;
    assert(read_mem(0x10000,8)==66 && read_mem(0x10000+length-8,8)==33);
    address_space_destroy(&child);address_space=&grandchild;
    assert(read_mem(0x10000,8)==77 && read_mem(0x10000+length-8,8)==33);
    address_space_destroy(&grandchild);address_space=&runtime_context.memory;
    assert(resource_used==0);
    printf("PASS: arena=%zu host_page=%zu first_write_charge=%" PRIu64 " second_write_charge=%" PRIu64 "\n",length,page,first_growth,second_growth);
    puts("PASS: stable pointers, nested snapshots, cross-host-page COW, transactional OOM and final release");
    return 0;
#endif
}
