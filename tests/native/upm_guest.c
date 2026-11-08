/* The writable layer's new files in memory (--upper-memory, native/upm.h) renamed
   while still open, as Linux allows (systemd-sysusers' /etc/group, debconf's
   config.dat, log rotation): over a lower file, over upper files, into a directory
   only the lower layer has, and again elsewhere; and the refusals Linux makes. The
   descriptors keep reading and writing the renamed file, each name lists once, and
   the file goes to disk once closed. Run by scripts/verify_overlay.py on an empty
   upper directory over the lower tree it makes (UPM_LOWER there), which then checks
   what the run wrote back. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define C1(n,a) call(n,(U)(a),0,0,0,0,0)
#define C2(n,a,b) call(n,(U)(a),(U)(b),0,0,0,0)
#define C3(n,a,b,c) call(n,(U)(a),(U)(b),(U)(c),0,0,0)
#define C4(n,a,b,c,d) call(n,(U)(a),(U)(b),(U)(c),(U)(d),0,0)
#define C5(n,a,b,c,d,e) call(n,(U)(a),(U)(b),(U)(c),(U)(d),(U)(e),0)
#define AT (-100)
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void say(const char *s) { C3(64,1,s,slen(s)); }
static void fail_at(int line) {
    char text[40]="upm FAIL line ";unsigned n=14;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static int scmp(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a!=*b; }
static int same(const void *a,const void *b,unsigned n) { const unsigned char *p=a,*q=b;for(unsigned i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1; }
static unsigned long u64(const unsigned char *p) { unsigned long v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,0); }
#define ST_INO u64(st+8)
#define MEMORY (1UL<<62)                                         /* upm.h's inode numbers */
static int pending(const char *path) { return stat_of(path)==0 && ST_INO>=MEMORY; }
static long open3(const char *path,U flags,U mode) { return C4(56,AT,path,flags,mode); }
static long create(const char *path) { return open3(path,01101,0644); }      /* O_WRONLY|O_CREAT|O_TRUNC */
static long put(const char *path,const char *text) {
    long fd=create(path);if(fd<0)return fd;
    long n=C3(64,fd,text,slen(text));C1(57,fd);return n;
}
static long write_text(long fd,const char *text) { return C3(64,fd,text,slen(text)); }
static int has(const char *path,const char *text) {
    char buffer[256];long fd=open3(path,0,0);if(fd<0)return 0;
    long n=C3(63,fd,buffer,sizeof buffer);C1(57,fd);
    return n==(long)slen(text) && same(buffer,text,(unsigned)n);
}
static long rename(const char *from,const char *to) { return C4(38,AT,from,AT,to); }
/* Names in a directory (getdents64) besides . and .., or whether want is one (-2: a name twice). */
static int names(const char *dir,const char *want) {
    unsigned char buffer[512];long fd=open3(dir,040000,0);if(fd<0)return -1;
    int count=0,found=0,dots=0;
    for(;;) {
        long n=C3(61,fd,buffer,sizeof buffer);if(n<=0)break;
        for(long at=0;at<n;) {
            const char *name=(const char *)buffer+at+19;unsigned size=buffer[at+16]|buffer[at+17]<<8;
            if(name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2])))dots++;
            else { count++;if(want && !scmp(name,want))found++; }
            at+=size;
        }
    }
    C1(57,fd);if(dots!=2 || found>1)return -2;
    return want?found:count;
}
void _start(void) {
    long fd,fd2;
    /* the upper layer gets /u (a file made there on disk), so new files in it are made in memory */
    CHECK(put("/u/first","1")==1 && stat_of("/u/first")==0 && ST_INO<MEMORY);
    /* over a lower file while open: the descriptor writes the renamed file */
    fd=create("/u/x.new");CHECK(fd>=0 && pending("/u/x.new") && write_text(fd,"new")==3);
    CHECK(rename("/u/x.new","/u/lower")==0 && stat_of("/u/x.new")==-2 && pending("/u/lower") && has("/u/lower","new"));
    CHECK(write_text(fd,"more")==4 && has("/u/lower","newmore") && names("/u","lower")==1 && names("/u","x.new")==0 && C1(57,fd)==0);
    /* over an upper file with a lower one below (copied up), and over one made in the container */
    fd=open3("/u/up",02002,0);CHECK(fd>=0 && write_text(fd,"+")==1 && C1(57,fd)==0 && has("/u/up","old up+"));   /* O_RDWR|O_APPEND */
    fd=create("/u/y.new");CHECK(fd>=0 && write_text(fd,"y")==1 && rename("/u/y.new","/u/up")==0 && has("/u/up","y") && C1(57,fd)==0);
    fd=create("/u/z.new");CHECK(fd>=0 && write_text(fd,"z")==1 && rename("/u/z.new","/u/first")==0 && has("/u/first","z") && C1(57,fd)==0);
    /* log rotation: the log renamed over last round's while written, then made again */
    fd=open3("/u/app.log",02101,0644);CHECK(fd>=0 && write_text(fd,"a")==1);              /* O_WRONLY|O_CREAT|O_APPEND */
    CHECK(rename("/u/app.log","/u/app.log.1")==0 && (fd2=open3("/u/app.log",02101,0644))>=0);
    CHECK(write_text(fd,"b")==1 && write_text(fd2,"c")==1 && has("/u/app.log.1","ab") && has("/u/app.log","c"));
    CHECK(names("/u","app.log")==1 && names("/u","app.log.1")==1 && C1(57,fd)==0 && C1(57,fd2)==0);
    /* into directories only the lower layer has: over its file, and as a new name */
    fd=create("/top.new");CHECK(fd>=0 && pending("/top.new") && write_text(fd,"w")==1);
    CHECK(rename("/top.new","/w/x")==0 && has("/w/x","w") && names("/w","x")==1 && C1(57,fd)==0);
    fd=create("/top2.new");CHECK(fd>=0 && write_text(fd,"v")==1);
    CHECK(rename("/top2.new","/v/made")==0 && has("/v/made","v") && has("/v/keep","v") && names("/v",0)==2 && C1(57,fd)==0);
    /* what Linux refuses: an existing name with RENAME_NOREPLACE, a directory, a missing one */
    fd=create("/u/r.new");CHECK(fd>=0);
    CHECK(C5(276,AT,"/u/r.new",AT,"/u/nr",1)==-17 && has("/u/nr","nr"));
    CHECK(rename("/u/r.new","/u/sub")==-21 && has("/u/sub/s","s"));
    CHECK(rename("/u/r.new","/nodir/r")==-2 && pending("/u/r.new") && C1(57,fd)==0);
    /* removed after taking a lower file's name: the lower one stays hidden */
    fd=create("/u/k.new");CHECK(fd>=0 && rename("/u/k.new","/u/k")==0 && C3(35,AT,"/u/k",0)==0);
    CHECK(stat_of("/u/k")==-2 && names("/u","k")==0 && C1(57,fd)==0);
    /* renamed on again: the name it took stays the lower file's whiteout */
    fd=create("/u/m.new");CHECK(fd>=0 && write_text(fd,"m")==1 && rename("/u/m.new","/u/lower2")==0);
    CHECK(rename("/u/lower2","/u/m2")==0 && stat_of("/u/lower2")==-2 && has("/u/m2","m") && C1(57,fd)==0);
    /* closed: written back first, then the host's rename */
    CHECK(put("/u/c.new","c")==1 && pending("/u/c.new") && rename("/u/c.new","/u/closed")==0 && has("/u/closed","c") && stat_of("/u/closed")==0 && ST_INO<MEMORY);
    /* written back meanwhile (closed for 2 s): read from disk, listed once */
    long wait[2]={0,100000000};
    for(int i=0;i<150 && pending("/u/lower");i++)C2(101,wait,0);
    CHECK(stat_of("/u/lower")==0 && ST_INO<MEMORY && has("/u/lower","newmore") && names("/u","lower")==1);
    say("upm PASS\n");C1(94,0);for(;;){}
}
