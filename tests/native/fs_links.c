/* Guest symlinks resolve inside fs_root: relative, absolute, chained, ".."
   clamped at the root, loops bounded, and never reaching host paths. */
#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char *memory;
static uint64_t call4(uint64_t n,uint64_t a,const char *path,uint64_t c,uint64_t d) {
    strcpy((char *)memory,path);
    cpu->gpr[0]=a;cpu->gpr[1]=0x1000;cpu->gpr[2]=c;cpu->gpr[3]=d;return fs_call(n);
}
static uint64_t stat_mode(const char *path,uint64_t flags) {
    uint64_t r=call4(79,(uint64_t)-100,path,0x1400,flags);
    return r?r:le(memory+0x400+16,4)&0170000;
}
static uint64_t stat_size(const char *path) {
    assert(call4(79,(uint64_t)-100,path,0x1400,0)==0);return le(memory+0x400+48,8);
}
static void read_file(const char *path,const char *want) {
    uint64_t fd=call4(56,(uint64_t)-100,path,0,0);assert(fd>=FS_FIRST && fd<FS_FIRST+FS_SLOTS);
    cpu->gpr[0]=fd;cpu->gpr[1]=0x1800;cpu->gpr[2]=64;uint64_t n=fs_call(63);
    assert(n==strlen(want) && !memcmp(memory+0x800,want,n));
    cpu->gpr[0]=fd;assert(fs_call(57)==0);
}
static void chdir_to(const char *path,const char *want) {
    strcpy((char *)memory,path);cpu->gpr[0]=0x1000;assert(fs_chdir()==0);
    cpu->gpr[0]=0x1c00;cpu->gpr[1]=1024;assert(fs_getcwd()==strlen(want)+1);
    assert(!strcmp((char *)memory+0xc00,want));
}
static void put(const char *path,const char *data) {
    int fd=openat(fs_root,path,O_CREAT|O_WRONLY|O_TRUNC,0700);assert(fd>=0);
    assert(write(fd,data,strlen(data))==(ssize_t)strlen(data));close(fd);
}
int main(void) {
    MemoryBacking *backing=backing_alloc(8192,1);assert(backing);memory=backing_data(backing);
    address_space->seg[0]=(Segment){0x1000,8192,6,backing,0};address_space->nseg=1;address_space_changed(address_space);
    char host[]="/tmp/vmfree-links-XXXXXX";assert(mkdtemp(host));
    char root[64];snprintf(root,sizeof root,"%s/root",host);assert(mkdir(root,0700)==0);
    char outside[64];snprintf(outside,sizeof outside,"%s/outside",host);
    int o=open(outside,O_CREAT|O_WRONLY,0600);assert(o>=0 && write(o,"host",4)==4);close(o);
    fs_root=open(root,O_RDONLY|O_DIRECTORY);assert(fs_root>=0);fs_writable=1;
    assert(mkdirat(fs_root,"bin",0700)==0 && mkdirat(fs_root,"lib",0700)==0 && mkdirat(fs_root,"etc",0700)==0);
    assert(mkdirat(fs_root,"usr",0700)==0 && mkdirat(fs_root,"usr/bin",0700)==0);
    put("bin/busybox","busybox");put("lib/real.so","elf");put("etc/passwd","inside");
    assert(symlinkat("busybox",fs_root,"bin/sh")==0);              /* relative */
    assert(symlinkat("/lib/real.so",fs_root,"lib/abs")==0);         /* absolute */
    assert(symlinkat("../../bin/sh",fs_root,"usr/bin/up")==0);      /* chained, .. */
    assert(symlinkat("/lib",fs_root,"dirlink")==0);                 /* directory */
    assert(symlinkat("../../../../etc/passwd",fs_root,"escape")==0);/* clamps at root */
    assert(symlinkat("../outside",fs_root,"hostrel")==0);
    assert(symlinkat(outside,fs_root,"hostabs")==0);                /* host path is a guest path */
    assert(symlinkat("loop",fs_root,"loop")==0);

    assert(stat_mode("/bin/sh",0)==0100000 && stat_size("/bin/sh")==7);
    assert(stat_mode("/bin/sh",256)==0120000);
    read_file("/usr/bin/up","busybox");
    read_file("/dirlink/real.so","elf");
    read_file("/lib/abs","elf");
    uint64_t dir=call4(56,(uint64_t)-100,"/",2048|16384|524288|131072,0);    /* glibc opendir flags */
    assert(dir>=FS_FIRST && dir<FS_FIRST+FS_SLOTS);cpu->gpr[0]=dir;assert(fs_call(57)==0);
    read_file("/escape","inside");
    read_file("/bin/../etc/passwd","inside");
    assert(stat_mode("/hostrel",0)==linux_error(2));
    assert(stat_mode("/hostabs",0)==linux_error(2));
    assert(stat_mode("../outside",0)==linux_error(2));
    assert(stat_mode("/loop",0)==linux_error(40));
    assert(call4(56,(uint64_t)-100,"/bin/sh",32768,0)==linux_error(40)); /* O_NOFOLLOW */

    assert(call4(78,(uint64_t)-100,"/bin/sh",0x1800,64)==7 && !memcmp(memory+0x800,"busybox",7));

    chdir_to("/usr/bin","/usr/bin");
    read_file("up","busybox");
    chdir_to("/dirlink","/lib");
    read_file("abs","elf");
    chdir_to("..","/");
    chdir_to("../..","/");
    chdir_to("usr/../bin","/bin");

    int fd=fs_open_root_file("/lib/abs");assert(fd>=0);close(fd);
    assert(fs_open_root_file("/hostabs")<0);

    assert(call4(35,(uint64_t)-100,"/bin/sh",0,0)==0);            /* unlink removes the link */
    assert(stat_mode("/bin/busybox",0)==0100000 && stat_mode("/bin/sh",256)==linux_error(2));

    char cmd[128];snprintf(cmd,sizeof cmd,"rm -rf %s",host);assert(system(cmd)==0);
    puts("fs_links ok");return 0;
}
