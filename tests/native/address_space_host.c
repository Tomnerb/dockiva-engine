/* Real mapping operations and interpreter fetches. */
#define main vmfree_program_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>

static void activate(RuntimeContext *context) {
    cpu=&context->cpu;address_space=&context->memory;
}
static void make_image(RuntimeContext *context,unsigned status) {
    activate(context);address_space->map_capacity=MAP_CAP;
    address_space->seg[0]=(Segment){0x10000,12288,5,backing_alloc(12288,1),0};
    assert(address_space->seg[0].backing);address_space->nseg=1;
    const uint32_t code[]={0xd2800000u|(status<<5),0xd2800ba8,0xd4000001};
    memcpy(segment_data(&address_space->seg[0]),code,sizeof(code));
    memcpy(segment_data(&address_space->seg[0])+8192,code,sizeof(code));address_space_changed(address_space);
    heap_init();assert(guest_brk(address_space->brk_base+4096)==address_space->brk_base+4096);
    assert(guest_mmap(4096,3,0x22,0)==MAP_BASE);
    store_le(MAP_BASE,status,8);store_le(address_space->brk_base,status+1,8);
}
static uint64_t protect(uint64_t addr,uint64_t length,unsigned prot) {
    cpu->gpr[0]=addr;cpu->gpr[1]=length;cpu->gpr[2]=prot;return guest_mprotect();
}
static uint64_t fixed_map(unsigned prot) {
    cpu->gpr[0]=MAP_BASE;cpu->gpr[1]=4096;cpu->gpr[2]=prot;
    cpu->gpr[3]=0x32;cpu->gpr[4]=UINT64_MAX;cpu->gpr[5]=0;
    return guest_dynamic_mmap();
}
int main(void) {
    RuntimeContext contexts[2]={0};make_image(&contexts[0],11);make_image(&contexts[1],22);
    assert(contexts[0].memory.map_backing!=contexts[1].memory.map_backing);
    assert(contexts[0].memory.brk_backing!=contexts[1].memory.brk_backing);
    for(unsigned i=0;i<100;i++) {
        unsigned status=i%2?22:11;activate(&contexts[i%2]);
        assert(read_mem(MAP_BASE,8)==status && read_mem(address_space->brk_base,8)==status+1);
        cpu->pc=0x10000;assert(run(3)==(int)status);
        ProcessImage saved;image_save(&saved);uint64_t generation=address_space->generation;
        image_clear();assert(!address_space->nseg && !address_space->map_backing);
        image_restore(&saved);assert(address_space->generation>generation);
        assert(read_mem(MAP_BASE,8)==status && lookup(0x10000,4,1));
    }
    puts("PASS: distinct address spaces, interpreter fetches, snapshot ownership and revision freshness");
    activate(&contexts[0]);
    uint64_t budget=resource_limit;resource_limit=resource_used;
    /* Anonymous memory is charged on first write, not at mmap: a fixed remap needs
       no budget and yields zero pages, as on Linux. */
    assert(fixed_map(3)==MAP_BASE && read_mem(MAP_BASE,8)==0);
    store_le(MAP_BASE,11,8);resource_limit=budget; /* its host page is still owned */
    uint64_t generation=address_space->generation;
    assert(read_mem(MAP_BASE,8)==11);
    assert(protect(MAP_BASE,8192,1)==linux_error(12));
    assert(address_space->generation==generation && lookup(MAP_BASE,8,2));
    assert(protect(MAP_BASE,4096,1)==0);
    assert(!lookup(MAP_BASE,8,2) && read_mem(MAP_BASE,8)==11);
    assert(guest_munmap(MAP_BASE,4096)==0 && !lookup(MAP_BASE,8,4));
    assert(guest_mmap(4096,3,0x22,0)==MAP_BASE && read_mem(MAP_BASE,8)==0);
    puts("PASS: failed protection is atomic; revoke/unmap/remap changes visibility");
    for(unsigned i=0;i<2;i++) {
        activate(&contexts[i]);image_free();assert(!address_space->map_backing && !address_space->nseg);
        image_free(); /* Empty release is safe and cannot double-free stale snapshots. */
    }
    assert(resource_used==0);activate(&runtime_context);
    puts("PASS: backing allocations released under the existing allocation accounting");
    return 0;
}
