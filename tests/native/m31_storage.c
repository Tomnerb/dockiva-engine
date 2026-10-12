#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
    char directory[]="/tmp/vmfree-storage-XXXXXX";assert(mkdtemp(directory));
    fs_root=open(directory,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    int fd=openat(fs_root,"existing",O_CREAT|O_RDWR,0600);assert(fd>=0);assert(ftruncate(fd,FS_STORAGE_LIMIT-2)==0);close(fd);
    assert(fs_scan_budget(fs_root,0)==0 && fs_storage_bytes==FS_STORAGE_LIMIT-2);
    fd=openat(fs_root,"new",O_CREAT|O_RDWR,0600);assert(fd>=0);fs_fd[0]=fd+1;fs_writable=1;
    unsigned char buffer[16]="data";seg[0]=(Segment){0x1000,16,6,buffer};nseg=1;
    regs[0]=64;regs[1]=0x1000;regs[2]=3;assert(fs_call(64)==linux_error(28));
    regs[2]=2;assert(fs_call(64)==2 && fs_storage_bytes==FS_STORAGE_LIMIT);
    regs[1]=3;assert(fs_call(46)==linux_error(28));
    regs[0]=64;assert(fs_call(57)==0);nseg=0;
    fs_storage_bytes=0;fs_entries=0;assert(fs_scan_budget(fs_root,0)==0 && fs_storage_bytes==FS_STORAGE_LIMIT);
    fd=openat(fs_root,"existing",O_RDWR);assert(fd>=0);assert(ftruncate(fd,FS_STORAGE_LIMIT)==0);close(fd);
    fs_storage_bytes=0;fs_entries=0;assert(fs_scan_budget(fs_root,0)<0);
    assert(unlinkat(fs_root,"new",0)==0);assert(unlinkat(fs_root,"existing",0)==0);close(fs_root);fs_root=-1;assert(rmdir(directory)==0);
    puts("PASS persistent root scan and write/truncate growth limits");return 0;
}
