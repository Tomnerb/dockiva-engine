#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
    static unsigned char memory[4096];seg[0]=(Segment){0x1000,sizeof(memory),6,memory};nseg=1;
    char path[]="/tmp/vmfree-m18-XXXXXX";assert(mkdtemp(path));fs_root=open(path,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    int host=openat(fs_root,"a",O_CREAT|O_WRONLY,0600);assert(host>=0);assert(write(host,"abc",3)==3);close(host);
    strcpy((char *)memory,"/");regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=16384;
    uint64_t fd=fs_call(56);assert(fd==64);
    regs[0]=fd;regs[1]=0;regs[2]=24;assert(fs_call(61)==linux_error(14));
    regs[1]=0x1100;regs[2]=23;assert(fs_call(61)==linux_error(22));
    regs[2]=24;unsigned entries=0,found=0;
    for(;;) {
        uint64_t got=fs_call(61);if(!got)break;assert(got==24);
        unsigned char *record=memory+256;assert(le(record+16,2)==24);assert(record[23]==0);
        if(!strcmp((char *)record+19,"a"))found++;
        assert(++entries<=3);
    }
    assert(entries==3 && found==1);
    regs[1]=0;regs[2]=0;assert(fs_call(62)==0);
    regs[1]=0x1100;regs[2]=24;assert(fs_call(61)==24);
    assert(fs_call(57)==0);assert(!fs_dirs[0]);assert(fs_call(61)==linux_error(9));
    strcpy((char *)memory,"/a");regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=0;regs[3]=0;
    assert(fs_call(79)==linux_error(14));regs[2]=0x1100;assert(fs_call(79)==0);assert(le(memory+256+48,8)==3);
    regs[3]=1;assert(fs_call(79)==linux_error(95));regs[3]=256;
    assert(symlinkat("a",fs_root,"link")==0);strcpy((char *)memory,"/link");assert(fs_call(79)==0);
    strcpy((char *)memory,"../outside");assert(fs_call(79)==linux_error(2)); /* .. clamps at the root */
    regs[0]=999;regs[1]=0;regs[2]=1;assert(guest_writev()==linux_error(9));
    regs[0]=1;regs[2]=17;assert(guest_writev()==linux_error(22));
    regs[2]=1;assert(guest_writev()==linux_error(14));
    regs[1]=0x1200;fs_pack(memory+512,0,0,8);fs_pack(memory+512,8,1,8);assert(guest_writev()==linux_error(14));
    fs_pack(memory+512,8,0,8);assert(guest_writev()==0);
    unlinkat(fs_root,"link",0);unlinkat(fs_root,"a",0);fs_cleanup();assert(rmdir(path)==0);
    puts("PASS getdents layout, small-buffer continuation, rewind, close, pointers, stat restrictions and writev bounds");
    return 0;
}
