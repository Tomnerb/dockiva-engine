#include <sys/socket.h>
static int observed_backlog=-1;
static int observed_listen(int fd,int backlog) { observed_backlog=backlog;return listen(fd,backlog); }
#define listen observed_listen
#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#undef listen
#include <assert.h>
static uint64_t invoke(uint64_t fd,uint64_t backlog) { regs[0]=fd;regs[1]=backlog;return net_call(201); }
int main(void) {
    const uint64_t input[]={0,1,128,129,511,2147483647,UINT64_MAX,2147483648,4294967296,4294967807};
    const int expected[]={0,1,128,128,128,128,128,128,0,128};
    uint64_t fd=net_socket(2,1,0);assert(fd<35);
    assert(invoke(fd,511)==linux_error(13) && observed_backlog==-1); /* No implicit wildcard bind. */
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(net_host(fd),(struct sockaddr *)&address,sizeof(address))==0);
    for(unsigned i=0;i<sizeof(input)/sizeof(input[0]);i++) {
        assert(invoke(fd,input[i])==0 && observed_backlog==expected[i]);
    }
    net_cleanup();assert(invoke(fd,511)==linux_error(9));
    fd=net_socket(2,1,0);assert(fd<35);address.sin_addr.s_addr=htonl(INADDR_ANY);
    assert(bind(net_host(fd),(struct sockaddr *)&address,sizeof(address))==0);
    observed_backlog=-1;assert(invoke(fd,511)==linux_error(13) && observed_backlog==-1);
    net_cleanup();puts("PASS capped host backlog arguments, int ABI, repeated listen, closed descriptors and wildcard guards (ASan/UBSan)");return 0;
}
