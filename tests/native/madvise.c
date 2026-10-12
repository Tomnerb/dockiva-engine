#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static uint64_t advise(uint64_t address,uint64_t length,uint64_t hint) {
    regs[0]=address;regs[1]=length;regs[2]=hint;regs[8]=233;
    assert(guest_syscall()==-3);return regs[0];
}
int main(void) {
    unsigned char memory[8192];memset(memory,90,sizeof(memory));
    seg[0]=(Segment){0x1000,sizeof(memory),0,memory};nseg=1;
    for(unsigned hint=0;hint<3;hint++)assert(advise(0x1000,8192,hint)==0);
    assert(advise(0x1000,1,UINT64_C(0x100000001))==0); /* int ABI truncation */
    assert(advise(0x1001,1,0)==linux_error(22));
    assert(advise(0x1000,UINT64_MAX,0)==linux_error(22));
    assert(advise(UINT64_MAX-4095,4096,0)==linux_error(22));
    assert(advise(0,0,0)==0);assert(advise(0,4096,0)==linux_error(12));
    assert(advise(0x2000,8192,0)==linux_error(12));
    for(unsigned i=0;i<sizeof(memory);i++)assert(memory[i]==90);
    assert(advise(0x1000,4096,8)==linux_error(22));
    assert(advise(0x1000,4096,4)==linux_error(22));
    assert(advise(0x1000,4096,UINT64_MAX)==linux_error(22));
    heap_init();assert(guest_brk(brk_base+4096)==brk_base+4096);
    assert(advise(brk_base,4096,0)==0);assert(advise(brk_base+4096,4096,0)==linux_error(12));
    uint64_t p=guest_mmap(12288,0,34,0);assert(p==MAP_BASE);
    assert(advise(p,12288,0)==0);assert(guest_munmap(p+4096,4096)==0);
    assert(advise(p,12288,0)==linux_error(12));assert(advise(p+8192,4096,0)==0);
    nseg=0;heap_cleanup();assert(resource_used==0);
    puts("PASS madvise bounds, occupied ranges, PROT_NONE, int ABI, unsupported advice, unchanged bytes and cleanup");
    return 0;
}
