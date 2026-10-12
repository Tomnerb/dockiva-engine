#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
    assert(argc==2);fs_root=open(argv[1],O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    unsigned char *memory=calloc(131072,1);assert(memory);seg[0]=(Segment){0x1000,131072,6,memory};nseg=1;
    strcpy((char *)memory,"/probe.elf");regs[0]=0x1000;regs[1]=1;regs[2]=0;
    assert(process_exec()==linux_error(14));regs[1]=0;assert(process_exec()==linux_error(22));
    regs[1]=0x1100;fs_pack(memory+256,0,1,8);assert(process_exec()==linux_error(14));
    for(unsigned i=0;i<65;i++)fs_pack(memory+256,i*8,0x1400,8);
    assert(process_exec()==linux_error(7));
    memset(memory+256,0,65*8);fs_pack(memory+256,0,0x2000,8);memset(memory+4096,'a',65536);
    assert(process_exec()==linux_error(7));
    regs[1]=UINT64_MAX-3;assert(process_exec()==linux_error(14));
    regs[0]=17;regs[1]=1;regs[2]=regs[3]=regs[4]=0;assert(process_clone()==linux_error(95));
    /* An injected directory fd lacks fs_open shared-cursor ownership metadata. */
    regs[1]=0;fs_fd[0]=dup(fs_root)+1;assert(fs_fd[0]>0);assert(process_clone()==linux_error(95));close(fs_fd[0]-1);fs_fd[0]=0;
    process_count=PROCESS_LIMIT;assert(process_clone()==linux_error(11));process_count=0;
    regs[0]=(uint64_t)-1;assert(process_wait()==linux_error(10));
    regs[0]=0;assert(process_wait()==linux_error(95));
    image_free();fs_cleanup();puts("PASS M19 malformed vectors, argument bounds, clone restrictions and wait errors");return 0;
}
