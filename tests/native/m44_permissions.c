#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
    mode_t previous=umask(0);unsigned char memory[4096]={0};
    seg[0]=(Segment){0x1000,sizeof(memory),6,memory};nseg=1;
    char path[]="/tmp/vmfree-mode-XXXXXX";assert(mkdtemp(path));fs_root=open(path,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    fs_writable=1;guest_uid=guest_gid=70;
    regs[0]=0027;assert(fs_call(166)==0022);
    strcpy((char *)memory,"/data");regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=64|2;regs[3]=0666;
    uint64_t fd=fs_open();assert(fd>=FS_FIRST && fd<FS_FIRST+FS_SLOTS);
    regs[0]=fd;regs[1]=0x1100;assert(fs_call(80)==0);
    assert(le(memory+256+16,4)==0100640 && le(memory+256+24,4)==70 && le(memory+256+28,4)==70);
    ProcessImage parent;image_save(&parent);regs[0]=0777;assert(fs_call(166)==0027);
    image_restore(&parent);assert(fs_umask==0027);
    regs[0]=fd;regs[1]=0600;assert(fs_call(52)==0);
    regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=1;assert(fs_call(48)==linux_error(13));
    regs[2]=6;assert(fs_call(48)==0);
    regs[0]=fd;regs[1]=04755;assert(fs_call(52)==linux_error(95));
    fs_writable=0;regs[1]=0400;assert(fs_call(52)==linux_error(30));fs_writable=1;
    regs[0]=(uint64_t)-100;regs[1]=0x1000;regs[2]=0400;assert(fs_call(53)==0);
    regs[2]=2;assert(fs_call(48)==linux_error(13));regs[2]=4;assert(fs_call(48)==0);
    assert(symlinkat("/etc/passwd",fs_root,"escape")==0);strcpy((char *)memory,"/escape");
    regs[2]=0600;assert((int64_t)fs_call(53)<0);
    strcpy((char *)memory,"/directory");regs[2]=0777;assert(fs_call(34)==0);
    struct stat st;assert(fstatat(fs_root,"directory",&st,AT_SYMLINK_NOFOLLOW)==0 && (st.st_mode&0777)==0750);
    unlinkat(fs_root,"data",0);unlinkat(fs_root,"escape",0);unlinkat(fs_root,"directory",AT_REMOVEDIR);
    fs_cleanup();nseg=0;assert(rmdir(path)==0);umask(previous);
    puts("PASS umask, mapped ownership, actual modes, task snapshot, chmod, access and link boundary");return 0;
}
