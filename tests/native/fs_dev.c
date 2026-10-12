/* Minimal /dev: null, zero and urandom behave like Linux, stay writable on a
   read-only root, skip the storage/write budgets and resolve through links. */
#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char *memory;
static uint64_t open_path(const char *path,uint64_t flags) {
    strcpy((char *)memory,path);cpu->gpr[0]=(uint64_t)-100;cpu->gpr[1]=0x1000;cpu->gpr[2]=flags;cpu->gpr[3]=0;return fs_call(56);
}
static uint64_t transfer(uint64_t fd,uint64_t call,uint64_t length) {
    cpu->gpr[0]=fd;cpu->gpr[1]=0x2000;cpu->gpr[2]=length;return fs_call(call);
}
int main(void) {
    MemoryBacking *backing=backing_alloc(0x110000,1);assert(backing);memory=backing_data(backing);
    address_space->seg[0]=(Segment){0x1000,0x110000,6,backing,0};address_space->nseg=1;address_space_changed(address_space);
    char root[]="/tmp/vmfree-dev-XXXXXX";assert(mkdtemp(root));
    fs_root=open(root,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);
    assert(mkdirat(fs_root,"dev",0700)==0 && mkdirat(fs_root,"var",0700)==0);
    assert(symlinkat("/dev/null",fs_root,"var/log")==0);
    fs_writable=0; /* read-only root */

    uint64_t null=open_path("/dev/null",1|64|512);assert(null>=FS_FIRST && null<FS_FIRST+FS_SLOTS);
    for(unsigned i=0;i<100;i++)assert(transfer(null,64,1024*1024)==1024*1024); /* 100 MiB > 64 MiB budget */
    assert(fs_written==0 && fs_storage_bytes==0);
    cpu->gpr[0]=null;assert(fs_call(57)==0);
    null=open_path("/var/log",2);assert(null>=FS_FIRST);              /* through a symlink */
    assert(transfer(null,63,16)==0 && transfer(null,64,5)==5);

    uint64_t zero=open_path("/dev/zero",0);memset(memory+0x1000,0xff,64);
    assert(transfer(zero,63,64)==64);for(unsigned i=0;i<64;i++)assert(memory[0x1000+i]==0);
    uint64_t random=open_path("/dev/urandom",0);memset(memory+0x1000,0,64);
    assert(transfer(random,63,64)==64);unsigned nonzero=0;for(unsigned i=0;i<64;i++)nonzero+=memory[0x1000+i]!=0;assert(nonzero>32);

    strcpy((char *)memory,"/dev/null");cpu->gpr[0]=(uint64_t)-100;cpu->gpr[1]=0x1000;cpu->gpr[2]=0x1800;cpu->gpr[3]=0;
    assert(fs_call(79)==0 && (le(memory+0x800+16,4)&0170000)==0020000);
    cpu->gpr[0]=null;cpu->gpr[1]=0x1800;assert(fs_call(80)==0 && (le(memory+0x800+16,4)&0170000)==0020000);

    strcpy((char *)memory,"/");cpu->gpr[0]=(uint64_t)-100;cpu->gpr[1]=0x1000;cpu->gpr[2]=0755;
    assert(fs_call(34)==linux_error(17));                             /* mkdir -p walks through "/" */
    strcpy((char *)memory,"/var");assert(fs_call(34)==linux_error(17)); /* EEXIST precedes EROFS */
    strcpy((char *)memory,"/new");assert(fs_call(34)==linux_error(30));
    assert(open_path("/dev/null",16384)==linux_error(20));          /* O_DIRECTORY */
    assert(open_path("/dev/tty",0)==linux_error(2));                 /* only the minimal set */
    assert(open_path("/etc/new",1|64)==linux_error(2) || open_path("/etc/new",1|64)==linux_error(30));

    fs_cleanup();char cmd[64];snprintf(cmd,sizeof cmd,"rm -rf %s",root);assert(system(cmd)==0);
    puts("fs_dev ok");return 0;
}
