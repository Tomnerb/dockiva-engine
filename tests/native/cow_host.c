/* COW isolation through real guest stores, syscalls, mappings and backend entry. */
#define main vmfree_program_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static GuestAddressSpace parent,child,grandchild;
static void activate(GuestAddressSpace *space) { address_space=space; }
static void fixture(void) {
    activate(&parent);parent.map_capacity=MAP_CAP;
    parent.seg[0]=(Segment){0x10000,4096,5,backing_alloc(4096,1),0};
    parent.seg[1]=(Segment){0x20000,12288,6,backing_alloc(12288,1),0};parent.nseg=2;
    assert(parent.seg[0].backing && parent.seg[1].backing);
    /* str x19,[x20]; mov x0,#7; mov x8,#93; svc #0 */
    const uint32_t code[]={0xf9000293,0xd28000e0,0xd2800ba8,0xd4000001};
    memcpy(segment_data(&parent.seg[0]),code,sizeof(code));address_space_changed(&parent);
    heap_init();assert(guest_brk(parent.brk_base+8192)==parent.brk_base+8192);
    assert(guest_mmap(8192,3,0x22,0)==MAP_BASE);
    store_le(0x20000,11,8);store_le(0x21000,22,8);store_le(0x22000,33,8);
    store_le(parent.brk_base+4096,44,8);store_le(MAP_BASE,55,8);store_le(MAP_BASE+4096,66,8);
    cpu->gpr[0]=0x21000;cpu->gpr[1]=4096;cpu->gpr[2]=1;assert(guest_mprotect()==0);
}
static void destroy_all(void) {
    address_space_destroy(&grandchild);address_space_destroy(&child);address_space_destroy(&parent);
    activate(&runtime_context.memory);assert(resource_used==0);
}
static uint64_t fixed_map(void) {
    cpu->gpr[0]=MAP_BASE;cpu->gpr[1]=4096;cpu->gpr[2]=3;cpu->gpr[3]=0x32;cpu->gpr[4]=UINT64_MAX;cpu->gpr[5]=0;
    return guest_dynamic_mmap();
}
int main(int argc,char **argv) {
    (void)argv;
    if(argc>1) {
        fixture();assert(address_space_fork_private(&child,&parent));activate(&child);
        assert(!atexit(destroy_all));resource_limit=resource_used;
        store_le(0x20000,999,8);assert(0); /* Must terminate without writing peer bytes. */
    }
    fixture();uint64_t used=resource_used,budget=resource_limit;
    assert(!address_space_fork_private(&parent,&parent) && errno==EINVAL);
    uint64_t flags=MAP_CAP/4096+sizeof(ResourceHeader); /* each fork copies only the arena's page flags */
    resource_limit=used+2*flags;
    assert(address_space_fork_private(&child,&parent));
    assert(address_space_fork_private(&grandchild,&child));
    assert(resource_used==used+2*flags);used=resource_used;
    printf("PASS: two private forks add 0 backing bytes to %" PRIu64 " tracked bytes\n",used);
    activate(&child);uint64_t revision=child.generation;
    assert(read_mem(0x20000,8)==11 && !lookup(0x21000,8,2));
    assert(child.generation==revision && resource_used==used);
    assert(!address_space_make_private(&child,child.seg[1].backing,0,8));
    assert(child.generation==revision && child.seg[1].backing==parent.seg[1].backing && resource_used==used);
    resource_limit=budget;
    /* Warm the read cache, then execute a real guest store through the write barrier. */
    cpu->pc=0x10000;cpu->gpr[19]=77;cpu->gpr[20]=0x20000;assert(run(4)==7);
    assert(read_mem(0x20000,8)==77 && read_mem(0x21000,8)==22);
    assert(child.seg[1].backing!=parent.seg[1].backing && child.seg[1].backing->refs==3);
    assert(child.generation>revision);
    used=resource_used;store_le(0x22000,88,8);assert(resource_used==used);
    activate(&parent);store_le(0x20000,99,8);
    activate(&grandchild);assert(read_mem(0x20000,8)==11 && read_mem(0x22000,8)==33);
    puts("PASS: nested forks, parent/child stores and split views remain isolated");

    activate(&child);assert(guest_brk(child.brk_base+4096)==child.brk_base+4096);
    used=resource_used;revision=child.generation;resource_limit=used;
    assert(guest_brk(child.brk_base+8192)==child.brk_base+4096);
    assert(child.generation==revision && resource_used==used);resource_limit=budget;
    assert(guest_brk(child.brk_base+8192)==child.brk_base+8192);
    assert(read_mem(child.brk_base+4096,8)==0);
    activate(&parent);assert(read_mem(parent.brk_base+4096,8)==44);
    activate(&child);used=resource_used;revision=child.generation;
    resource_limit=used+sizeof(ResourceHeader)+4096; /* temporary fixed-map bytes only */
    assert(fixed_map()==linux_error(12));resource_limit=budget;
    assert(child.generation==revision && resource_used==used && read_mem(MAP_BASE,8)==55);
    assert(fixed_map()==MAP_BASE && read_mem(MAP_BASE,8)==0 && read_mem(MAP_BASE+4096,8)==66);
    activate(&parent);assert(read_mem(MAP_BASE,8)==55);
    activate(&grandchild);assert(guest_munmap(MAP_BASE,4096)==0);
    assert(!lookup(MAP_BASE,8,4));
    used=resource_used;resource_limit=used; /* fresh anonymous pages cost nothing until written */
    assert(guest_mmap(4096,3,0x22,0)==MAP_BASE && read_mem(MAP_BASE,8)==0 && resource_used==used);resource_limit=budget;
    activate(&child);assert(read_mem(MAP_BASE+4096,8)==66);
    activate(&parent);assert(read_mem(MAP_BASE,8)==55);
    puts("PASS: heap regrowth, MAP_FIXED, unmap/reuse and allocation-failure rollback preserve peers");
    destroy_all();

    fixture();
    /* readv's input table and both destinations share a backing. The first
       writable lookup moves all local views; later lookups must not stale it. */
    store_le(0x20000,0x20100,8);store_le(0x20008,3,8);
    store_le(0x20010,0x22100,8);store_le(0x20018,3,8);
    assert(address_space_fork_private(&child,&parent));activate(&child);
    FILE *input=tmpfile();assert(input);assert(fwrite("abcdef",1,6,input)==6);rewind(input);
    int fd=io_free_slot(0);assert(fd>=0);IO(fd)=(GuestIO){IO_FILE,0,dup(fileno(input)),NULL,NULL,NULL,NULL,NULL,NULL};assert(IO(fd).host>=0);
    cpu->gpr[0]=(uint64_t)fd;cpu->gpr[1]=0x20000;cpu->gpr[2]=2;assert(guest_readv()==6);
    io_release(&IO(fd));fd_unbind((uint64_t)fd);fclose(input);
    assert(!memcmp(lookup(0x20100,3,4),"abc",3) && !memcmp(lookup(0x22100,3,4),"def",3));
    activate(&parent);assert(read_mem(0x20100,3)==0 && read_mem(0x22100,3)==0);
    puts("PASS: syscall readv writes detach backing and preserve multiple output pointers");
    destroy_all();

    fixture();cpu->pc=0x10000;cpu->gpr[19]=123;cpu->gpr[20]=0x20000;
    cpu->gpr[0]=17;cpu->gpr[1]=0;process_steps=100;
    assert(process_clone()==2);assert(read_mem(0x20000,8)==11);
    cpu->gpr[0]=2;cpu->gpr[1]=0x20000;cpu->gpr[2]=cpu->gpr[3]=0;
    assert(process_wait()==2 && read_mem(0x20000,4)==(7<<8));
    puts("PASS: actual process clone/store/exit/wait path preserves the parent image");
    destroy_all();
    return 0;
}
