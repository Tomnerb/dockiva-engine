/* Hard links inside a bind mount or named volume beside image layers (native/fs.h:
   linkat): both names are one file (one inode, two links), by absolute path, by a
   descriptor of the mount's directory, and through a symlink with AT_SYMLINK_FOLLOW; a
   missing source is ENOENT, a taken name EEXIST, and a link that would leave the mount
   (to another mount, to or from the container's layers) is EXDEV. /vol and /vol2 are
   bind mounts, /low/f a file of the image. Run by scripts/verify_overlay.py, with and
   without the memory layer, which then checks what the run left on the host. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define C1(n,a) call(n,(U)(a),0,0,0,0,0)
#define C3(n,a,b,c) call(n,(U)(a),(U)(b),(U)(c),0,0,0)
#define C4(n,a,b,c,d) call(n,(U)(a),(U)(b),(U)(c),(U)(d),0,0)
#define C5(n,a,b,c,d,e) call(n,(U)(a),(U)(b),(U)(c),(U)(d),(U)(e),0)
#define AT (-100)
#define ENOENT (-2)
#define EEXIST (-17)
#define EXDEV (-18)
static void fail_at(int line) {
    char text[40]="mountlink FAIL line ";unsigned n=20;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static unsigned long u64(const unsigned char *p) { unsigned long v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,256); }          /* AT_SYMLINK_NOFOLLOW */
static unsigned long ino(const char *path) { return stat_of(path)?0:u64(st+8); }
static long links(const char *path) { return stat_of(path)?-1:(long)(st[20]|st[21]<<8); }
static long put(const char *path) {
    long fd=C4(56,AT,path,01101,0644);if(fd<0)return fd;                         /* O_WRONLY|O_CREAT|O_TRUNC */
    long n=C3(64,fd,"x",1);C1(57,fd);return n;
}
static int has(const char *path) {
    char c[2];long fd=C4(56,AT,path,0,0);if(fd<0)return 0;
    long n=C3(63,fd,c,2);C1(57,fd);return n==1 && c[0]=='x';
}
static long link(const char *from,const char *to) { return C5(37,AT,from,AT,to,0); }

void _start(void) {
    CHECK(put("/vol/a")==1 && C3(34,AT,"/vol/d",0755)==0);
    /* inside the mount: one file under both names */
    CHECK(link("/vol/a","/vol/b")==0);
    CHECK(ino("/vol/a") && ino("/vol/a")==ino("/vol/b") && links("/vol/a")==2 && has("/vol/b"));
    CHECK(link("/vol/b","/vol/d/c")==0 && ino("/vol/d/c")==ino("/vol/a") && links("/vol/a")==3);
    long dir=C4(56,AT,"/vol",040000,0);CHECK(dir>=0);                            /* O_DIRECTORY */
    CHECK(C5(37,dir,"a",dir,"d/r",0)==0 && ino("/vol/d/r")==ino("/vol/a"));C1(57,dir);
    CHECK(C3(36,"a",AT,"/vol/s")==0 && C5(37,AT,"/vol/s",AT,"/vol/t",0x400)==0 && ino("/vol/t")==ino("/vol/a"));   /* AT_SYMLINK_FOLLOW */
    CHECK(C3(35,AT,"/vol/t",0)==0 && C3(35,AT,"/vol/d/r",0)==0 && links("/vol/a")==3 && has("/vol/a"));
    CHECK(link("/vol/none","/vol/e")==ENOENT && stat_of("/vol/e")==ENOENT);
    CHECK(link("/vol/a","/vol/b")==EEXIST && link("/vol/a","/vol/none/e")==ENOENT);
    /* never across the mount's boundary */
    CHECK(link("/vol/a","/vol2/x")==EXDEV && stat_of("/vol2/x")==ENOENT);
    CHECK(link("/vol/a","/x")==EXDEV && link("/vol/a","/low/x")==EXDEV && stat_of("/x")==ENOENT && stat_of("/low/x")==ENOENT);
    CHECK(link("/low/f","/vol/x")==EXDEV && put("/up")==1 && link("/up","/vol/x")==EXDEV && stat_of("/vol/x")==ENOENT);
    /* the layers as before: an image file is linked (copied up), a missing one is not */
    CHECK(link("/low/f","/low/g")==0 && ino("/low/g")==ino("/low/f") && links("/low/f")==2);
    CHECK(link("/none","/y")==ENOENT && link("/low/none","/low/y")==ENOENT);
    CHECK(links("/vol/a")==3);
    C3(64,1,"mountlink PASS\n",15);C1(94,0);for(;;){}
}
