#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static uint64_t mapping(uint64_t a,uint64_t len,unsigned prot,unsigned flags,long fd,uint64_t off) {
    cpu->gpr[0]=a;cpu->gpr[1]=len;cpu->gpr[2]=prot;cpu->gpr[3]=flags;cpu->gpr[4]=(uint64_t)fd;cpu->gpr[5]=off;return guest_dynamic_mmap();
}
int main(void) {
    uint64_t a=mapping(0,8192,3,34,-1,0);assert(a==MAP_BASE);
    assert(lookup(a,8192,2));store_le(a,42,8);
    assert(mapping(a,4096,7,50,-1,0)==linux_error(22));assert(read_mem(a,8)==42);
    assert(mapping(a-4096,4096,3,50,-1,0)==linux_error(22));
    assert(mapping(a,4096,3,18,999,0)==linux_error(9));assert(read_mem(a,8)==42);
    cpu->gpr[0]=a;cpu->gpr[1]=4096;cpu->gpr[2]=1;assert(guest_mprotect()==0);
    assert(!lookup(a,1,2) && lookup(a,1,4) && lookup(a+4096,1,2));
    cpu->gpr[0]=a;cpu->gpr[1]=8192;cpu->gpr[2]=5;assert(guest_mprotect()==0);assert(lookup(a,4,1));
    assert(!lookup(a,1,2));cpu->gpr[2]=7;assert(guest_mprotect()==linux_error(22));
    assert(guest_munmap(a,4096)==0);cpu->gpr[0]=a;cpu->gpr[1]=8192;cpu->gpr[2]=3;
    assert(guest_mprotect()==linux_error(12));assert(!lookup(a+4096,1,2));
    FILE *f=tmpfile();assert(f);assert(fwrite("hello",1,5,f)==5);fflush(f);fs_fd[0]=dup(fileno(f))+1;fs_guest[0]=64;fd_bind(64,FD_FILE,0); /* guest fd 64 names file slot 0 */
    assert(mapping(a,4096,1,18|0x1800,64,0)==a);assert(!memcmp(lookup(a,5,4),"hello",5));assert(!lookup(a,1,2));
    assert(lookup(a+5,1,4)[0]==0);
    assert(mapping(0,4096,1,2,64,UINT64_C(0x7ffffffffffff000))==linux_error(22));
    close(fs_fd[0]-1);fs_fd[0]=0;fd_unbind(64);fclose(f);address_space_destroy(address_space);
    address_space->map_capacity=32u*1024u*1024u;
    /* The arena is charged as pages are written: a mapping at its end succeeds under
       a budget smaller than the arena, and only touching it costs memory. */
    resource_limit=16u*1024u*1024u;
    assert(mapping(MAP_BASE+address_space->map_capacity-4096,4096,3,50,-1,0)==MAP_BASE+address_space->map_capacity-4096);
    assert(address_space->map_backing && resource_used<64u*1024u);
    address_space_destroy(address_space);address_space->map_capacity=32u*1024u*1024u;
    resource_limit=256u*1024u*1024u;
    uint64_t last=MAP_BASE+address_space->map_capacity-4096;
    assert(mapping(last,4096,3,50,-1,0)==last);
    store_le(last,123,8);assert(read_mem(last,8)==123);
    assert(!lookup(MAP_BASE+address_space->map_capacity,1,4));
    assert(mapping(last,8192,3,50,-1,0)==linux_error(22));assert(read_mem(last,8)==123);
    assert(guest_munmap(last,4096)==0);assert(!lookup(last,1,4));
    address_space_destroy(address_space);assert(resource_used==0);
    puts("PASS guest mapping bounds, W^X, atomic errors, protections, holes and file contents");return 0;
}
