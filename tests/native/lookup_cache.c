#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static void same(uint64_t addr,uint64_t size,unsigned permission) {
    lookup_cache_enabled=0;unsigned char *expected=lookup(addr,size,permission);
    lookup_cache_enabled=1;unsigned char *actual=lookup(addr,size,permission);
    assert(actual==expected);
}
int main(void) {
    MemoryBacking *buffers[3];for(unsigned i=0;i<3;i++) { buffers[i]=backing_alloc(4096,1);assert(buffers[i]); }
    unsigned char *a=backing_data(buffers[0]),*b=backing_data(buffers[1]);
    address_space->seg[0]=(Segment){0x10000,4096,5,buffers[0],0};
    address_space->seg[1]=(Segment){0x11000,4096,6,buffers[1],0};
    address_space->seg[2]=(Segment){0x20000,4096,4,buffers[2],0};address_space->nseg=3;
    profiling=1;
    for(unsigned pass=0;pass<3;pass++) {
        for(unsigned permission=0;permission<10;permission++) {
            const uint64_t sizes[]={0,1,2,8,4096,4097,LIMIT,LIMIT+1,UINT64_MAX};
            const uint64_t bases[]={0,0xffff,0x10000,0x10ff8,0x10fff,0x11000,0x11fff,0x12000,0x20000,0x20fff,0x21000,UINT64_MAX};
            for(unsigned i=0;i<sizeof(bases)/sizeof(*bases);i++)
                for(unsigned j=0;j<sizeof(sizes)/sizeof(*sizes);j++)same(bases[i],sizes[j],permission);
        }
    }
    /* Populate the later segment first, then check zero-size shared boundary. */
    assert(lookup(0x11000,1,2)==b);
    assert(lookup(0x11000,0,2)==NULL);
    assert(lookup(0x10000,1,1)==a);assert(lookup(0x10000,1,1)==a);
    assert(profile_cache_hits>0);
    heap_init();assert(guest_brk(address_space->brk_base+8192)==address_space->brk_base+8192);
    same(address_space->brk_base,1,4);same(address_space->brk_base+4096,1,2);
    guest_brk(address_space->brk_base);same(address_space->brk_base,1,4);assert(lookup(address_space->brk_base,1,4)==NULL);
    uint64_t mapped=guest_mmap(8192,3,0x22,0);assert(mapped==MAP_BASE);
    same(mapped,8192,2);assert(lookup(mapped,1,2)!=NULL);
    guest_munmap(mapped,4096);same(mapped,1,4);assert(lookup(mapped,1,4)==NULL);
    same(mapped+4096,1,4);assert(lookup(mapped+4096,1,4)!=NULL);
    assert(guest_mmap(4096,0,0x22,0)==mapped);same(mapped,1,4);assert(lookup(mapped,1,4)==NULL);
    address_space_destroy(address_space);assert(resource_used==0);puts("PASS cached/reference equivalence, bounds, permissions, zero-length boundaries, heap shrink, unmap and reuse");
    return 0;
}
