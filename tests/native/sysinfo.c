#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char memory[4096];
static void query(void) { regs[0]=0x1001;regs[8]=179;assert(guest_syscall()==-3 && regs[0]==0); }
int main(void) {
    memset(memory,165,sizeof(memory));seg[0]=(Segment){0x1000,sizeof(memory),6,memory};nseg=1;
    sysinfo_started_ms=net_now()-2500;query();
    assert(memory[0]==165 && memory[113]==165);
    assert(le(memory+1,8)>=2 && le(memory+1,8)<4);
    assert(le(memory+33,8)==resource_limit && le(memory+41,8)==resource_limit);
    assert(le(memory+81,2)==1 && le(memory+105,4)==1);
    for(unsigned i=8;i<32;i++)assert(memory[1+i]==0);
    for(unsigned i=48;i<80;i++)assert(memory[1+i]==0);
    for(unsigned i=82;i<104;i++)assert(memory[1+i]==0);
    for(unsigned i=108;i<112;i++)assert(memory[1+i]==0);
    void *p=malloc(1234);assert(p);query();uint64_t available=le(memory+41,8);
    assert(available<resource_limit && available==resource_limit-resource_used);
    free(p);query();assert(le(memory+41,8)==resource_limit);
    process_count=2;process_children[0].pid=2;process_children[1].pid=3;query();assert(le(memory+81,2)==3);
    process_children[0].pid=0;query();assert(le(memory+81,2)==2);
    process_count=0;process_depth=1;query();assert(le(memory+81,2)==2);process_depth=0;
    resource_used=resource_limit+1;query();assert(le(memory+41,8)==0);resource_used=0;
    regs[0]=0;regs[8]=179;assert(guest_syscall()==-3 && regs[0]==linux_error(14));
    seg[0].flags=4;memset(lookup_cache,0,sizeof(lookup_cache));regs[0]=0x1000;
    assert(guest_syscall()==-3 && regs[0]==linux_error(14));
    nseg=0;puts("PASS sysinfo LP64 layout, bounds, uptime, budget accounting, process slots and EFAULT");return 0;
}
