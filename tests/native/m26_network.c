#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
    int result=vmfree_main(argc,argv);assert(result==0);
    assert(net_wait_blocks>0 && !net_wait.active && !schedule_live);
    for(unsigned i=0;i<NET_SLOTS;i++)assert(!net_fd[i]);
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)assert(!schedule_tasks[i].state && !schedule_tasks[i].saved);
    for(unsigned i=0;i<PIPE_SLOTS;i++)assert(!io_pipes[i].data);
    printf("PASS %" PRIu64 " network suspensions; guest sockets, tasks and pipes released\n",net_wait_blocks);
    return 0;
}
