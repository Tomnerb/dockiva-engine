#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
    int expected=!strcmp(argv[argc-1],"cancel-wait")?130:!strcmp(argv[argc-1],"self-term")?143:0;
    int result=vmfree_main(argc,argv);assert(result==expected);
    assert(schedule_cancellations==1);
    if(!strcmp(argv[argc-1],"cancel-read"))assert(io_read_blocks);
    if(!strcmp(argv[argc-1],"cancel-write"))assert(io_write_blocks);
    assert(!schedule_live);
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)assert(!schedule_tasks[i].saved && !schedule_tasks[i].state && !schedule_tasks[i].terminate_signal);
    for(unsigned i=0;i<PIPE_SLOTS;i++)assert(!io_pipes[i].data);
    for(unsigned i=0;i<FS_SLOTS;i++)assert(!fs_fd[i] && !fs_dirs[i]);
    puts("PASS cancelled task released memory, descriptors and pipe references");return 0;
}
