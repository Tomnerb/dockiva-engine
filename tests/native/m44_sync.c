#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
    unsigned char memory[4096]={0};seg[0]=(Segment){0x1000,sizeof(memory),6,memory};nseg=1;
    char path[]="/tmp/vmfree-sync-XXXXXX";assert(mkdtemp(path));fs_root=open(path,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    int fd=openat(fs_root,"data",O_CREAT|O_RDWR,0600);assert(fd>=0);fs_fd[0]=fd+1;
    strcpy((char *)memory,"/data");regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=0;
    assert(fs_call(48)==0);regs[2]=1;assert(fs_call(48)==linux_error(13));
    regs[2]=2;assert(fs_call(48)==linux_error(30));fs_writable=1;assert(fs_call(48)==0);
    regs[2]=8;assert(fs_call(48)==linux_error(22));regs[2]=0;
    strcpy((char *)memory,"/missing");assert(fs_call(48)==linux_error(2));
    assert(symlinkat("/etc/passwd",fs_root,"escape")==0);
    strcpy((char *)memory,"/escape");assert(fs_call(48)==linux_error(13));
    regs[2]=0x1100;regs[3]=4;assert(fs_call(78)==4);assert(!memcmp(memory+256,"/etc",4));
    regs[3]=0;assert(fs_call(78)==linux_error(22));regs[2]=0;
    strcpy((char *)memory,"../escape");assert(fs_call(48)==linux_error(13));
    memory[0]=0;regs[0]=64;regs[1]=0x1000;regs[2]=0x1200;regs[3]=4096;
    assert(fs_call(79)==0);assert(le(memory+512+48,8)==0);
    regs[3]=0;assert(fs_call(79)==linux_error(2));regs[3]=4096;regs[0]=63;assert(fs_call(79)==linux_error(9));
    regs[0]=64;regs[1]=0xffff;assert(fs_call(79)==linux_error(14));
    assert(write(fd,"durable",7)==7);regs[0]=64;assert(fs_call(82)==0);assert(fs_call(83)==0);
    assert(fs_call(57)==0);assert(fs_call(82)==linux_error(9));assert(fs_call(83)==linux_error(9));
    fd=openat(fs_root,"data",O_RDONLY);assert(fd>=0);char got[7];assert(read(fd,got,7)==7 && !memcmp(got,"durable",7));close(fd);
    unlinkat(fs_root,"data",0);unlinkat(fs_root,"escape",0);fs_cleanup();nseg=0;assert(rmdir(path)==0);
    puts("PASS access checks preserve root policy; sync, data sync, reopen and invalid descriptors");return 0;
}
