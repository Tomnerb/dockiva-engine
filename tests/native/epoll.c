#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char memory[4096];
static uint64_t invoke(unsigned call,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    regs[8]=call;regs[0]=a;regs[1]=b;regs[2]=c;regs[3]=d;regs[4]=e;regs[5]=8;
    assert(guest_syscall()==-3);return regs[0];
}
int main(void) {
    seg[0]=(Segment){0x1000,sizeof(memory),6,memory};nseg=1;
    uint64_t ep=invoke(20,0,0,0,0,0);assert(ep<128);
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    uint64_t fd=net_adopt(pair[0],2048);assert(fd<35);
    fs_pack(memory,0,1,4);fs_pack(memory,8,0xfedcba9876543210ULL,8);
    assert(invoke(21,ep,1,fd,0x1000,0)==0);
    uint64_t start=net_now();assert(invoke(22,ep,0x1100,1,20,0)==0);assert(net_now()-start>=15);
    assert(write(pair[1],"a",1)==1);
    assert(invoke(22,ep,0x1100,1,-1,0)==1);
    assert(le(memory+256,4)==1 && le(memory+264,8)==0xfedcba9876543210ULL);
    char c;assert(read(pair[0],&c,1)==1);assert(invoke(22,ep,0x1100,1,0,0)==0);
    fs_pack(memory,0,4,4);assert(invoke(21,ep,3,fd,0x1000,0)==0);
    assert(invoke(22,ep,0x1100,1,0,0)==1 && (le(memory+256,4)&4));
    assert(invoke(57,fd,0,0,0,0)==0);close(pair[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);assert(net_adopt(pair[0],2048)==fd);
    assert(invoke(22,ep,0x1100,1,0,0)==0); /* No stale registration after reuse. */
    fs_pack(memory,0,1u<<31,4);assert(invoke(21,ep,1,fd,0x1000,0)==linux_error(95));
    fs_pack(memory,0,1,4);assert(invoke(21,ep,1,fd,0x1000,0)==0);close(pair[1]);
    assert(invoke(22,ep,0x1100,1,0,0)==1 && (le(memory+256,4)&16));
    assert(invoke(22,ep,0x1100,1,0,0x1000)==linux_error(95));
    assert(invoke(21,ep,1,ep,0x1000,0)==linux_error(22));
    schedule_active=1;assert(invoke(20,0,0,0,0,0)==linux_error(95));schedule_active=0;
    assert(process_clone()==linux_error(95));
    assert(invoke(25,ep,2,1,0,0)==0);io_exec();assert(!epoll_live());
    for(unsigned round=0;round<3;round++) {
        for(unsigned i=0;i<EPOLL_SLOTS;i++)assert(invoke(20,0,0,0,0,0)<128);
        assert(invoke(20,0,0,0,0,0)==linux_error(24));io_cleanup();assert(!epoll_live());
    }
    net_cleanup();nseg=0;puts("PASS epoll socket readiness, timeout, HUP, close/reuse, unsupported modes, CLOEXEC and bounded cleanup");return 0;
}
