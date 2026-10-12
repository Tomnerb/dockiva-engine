#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
    int result=vmfree_main(argc,argv);assert(result==0);
    assert(schedule_switches>10 && io_read_blocks>0 && io_write_blocks>0);
    assert(!schedule_live);
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)assert(!schedule_tasks[i].saved);
    for(unsigned i=0;i<PIPE_SLOTS;i++)assert(!io_pipes[i].data);
    for(unsigned i=0;i<FS_SLOTS;i++)assert(!fs_fd[i] && !fs_dirs[i]);
    fprintf(stderr,"PASS scheduler: %" PRIu64 " switches, %" PRIu64 " read waits, %" PRIu64 " write waits; all task/pipe resources released\n",schedule_switches,io_read_blocks,io_write_blocks);
    return result;
}
