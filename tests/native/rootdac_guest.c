/* Root's CAP_DAC_OVERRIDE (native/fs.h: fs_dac): root changes the entries of a
   directory whose mode lacks write permission (0555, as tar leaves Go's module cache
   before filling it) or every permission (000), and opens files whatever their mode;
   the modes stay as set. Another user, and root without the capability, get EACCES for
   each of them. In a directory made in the container (/w), a bind mount (/vol, when
   mounted) and one the image has as 0555 (/low/ro), for files on disk and, with
   --upper-memory, files still in memory (native/upm.h). Run by scripts/verify_overlay.py,
   which then checks what the run left on the host. Hard links are made in /w only:
   linkat of a file in a bind mount answers ENOENT beside image layers, whatever the modes.
   A read-only bind mount (/ro) keeps its modes, so its 000 directory stays shut to root too. */
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
#define EACCES (-13)
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static const char *where="";
static void fail_at(int line) {
    char text[96]="rootdac FAIL line ";unsigned n=18;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]=' ';for(const char *p=where;*p && n<90;p++)text[n++]=*p;
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static int same(const void *a,const void *b,unsigned n) { const unsigned char *p=a,*q=b;for(unsigned i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1; }
static unsigned long u64(const unsigned char *p) { unsigned long v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,256); }          /* AT_SYMLINK_NOFOLLOW */
#define MEMORY (1UL<<62)                                         /* upm.h's inode numbers */
static int pending(const char *path) { return stat_of(path)==0 && u64(st+8)>=MEMORY; }
static long mode(const char *path) { return stat_of(path)?-1:(long)((st[16]|st[17]<<8)&07777); }
static long open3(const char *path,U flags,U mode) { return C4(56,AT,path,flags,mode); }
static long put(const char *path,const char *text) {
    long fd=open3(path,01101,0644);if(fd<0)return fd;                            /* O_WRONLY|O_CREAT|O_TRUNC */
    long n=C3(64,fd,text,slen(text));C1(57,fd);return n;
}
static long append(const char *path,const char *text) {
    long fd=open3(path,02001,0);if(fd<0)return fd;                               /* O_WRONLY|O_APPEND */
    long n=C3(64,fd,text,slen(text));C1(57,fd);return n;
}
static int has(const char *path,const char *text) {
    char buffer[64];long fd=open3(path,0,0);if(fd<0)return 0;
    long n=C3(63,fd,buffer,sizeof buffer);C1(57,fd);
    return n==(long)slen(text) && same(buffer,text,(unsigned)n);
}
static long mkdir(const char *path) { return C3(34,AT,path,0755); }
static long rmdir(const char *path) { return C3(35,AT,path,0x200); }
static long unlink(const char *path) { return C3(35,AT,path,0); }
static long symlink(const char *path) { return C3(36,"x",AT,path); }
static long mkfifo(const char *path) { return C4(33,AT,path,0010644,0); }
static long link(const char *from,const char *to) { return C5(37,AT,from,AT,to,0); }
static long rename(const char *from,const char *to) { return C4(38,AT,from,AT,to); }
static long chmod(const char *path,U mode) { return C3(53,AT,path,mode); }
static long chdir(const char *path) { return C1(49,path); }
/* Names in a directory (getdents64) besides . and .., or the open's error. */
static long names(const char *dir) {
    unsigned char buffer[1024];long fd=open3(dir,040000,0);if(fd<0)return fd;
    long count=0;
    for(;;) {
        long n=C3(61,fd,buffer,sizeof buffer);if(n<=0)break;
        for(long at=0;at<n;at+=buffer[at+16]|buffer[at+17]<<8) {
            const char *name=(const char *)buffer+at+19;
            if(!(name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2]))))count++;
        }
    }
    C1(57,fd);return count;
}
/* base and rest joined, in one of four buffers by turns (a call takes at most two). */
static const char *P(const char *base,const char *rest) {
    static char buffers[4][128];static unsigned next;
    char *out=buffers[next++&3];unsigned n=0;
    for(const char *p=base;*p;p++)out[n++]=*p;
    for(const char *p=rest;*p;p++)out[n++]=*p;
    out[n]=0;return out;
}
static void nap(void) { long t[2]={3,0};C2(101,t,0); }                           /* past upm.h's write-back of closed files */
static long fork(void) { return call(220,17,0,0,0,0,0); }
static void wait_ok(long pid) { int status=-1;CHECK(pid>0 && call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0); }

/* base/a, 0755 still: directories and files that reach the disk before its mode changes. */
static void setup(const char *b) {
    where=b;
    CHECK(mkdir(P(b,"/a"))==0 && mkdir(P(b,"/a/pre"))==0 && mkdir(P(b,"/a/pre2"))==0 && mkdir(P(b,"/a/mvd"))==0);
    CHECK(put(P(b,"/a/d1"),"d")==1 && put(P(b,"/a/d2"),"d")==1 && put(P(b,"/a/d3"),"d")==1 && put(P(b,"/a/d4"),"d")==1 && put(P(b,"/a/d5"),"d")==1);
}
/* Root, in base/a once it is 0555, then 000. */
static void root(const char *b) {
    where=b;
    int layer=b[1]=='w';
    int memory=pending(P(b,"/a/d1"));                                            /* the memory layer holds this run's new files */
    if(memory) { nap();CHECK(!pending(P(b,"/a/d1")) && !pending(P(b,"/a/d5"))); }
    CHECK(put(P(b,"/a/y1"),"y")==1 && put(P(b,"/a/y2"),"y")==1 && put(P(b,"/a/y3"),"y")==1 && pending(P(b,"/a/y1"))==memory);
    CHECK(chmod(P(b,"/a/mvd"),0555)==0 && chmod(P(b,"/a"),0555)==0);
    CHECK(mkdir(P(b,"/a/d"))==0);
    CHECK(symlink(P(b,"/a/l"))==0);
    CHECK(mkfifo(P(b,"/a/p"))==0);
    CHECK(rmdir(P(b,"/a/pre"))==0 && stat_of(P(b,"/a/pre"))==-2);
    CHECK(put(P(b,"/a/f"),"new")==3);                                            /* a new file, renamed and removed */
    CHECK(rename(P(b,"/a/f"),P(b,"/a/g"))==0 && has(P(b,"/a/g"),"new") && unlink(P(b,"/a/g"))==0 && stat_of(P(b,"/a/g"))==-2);
    CHECK(rename(P(b,"/a/d1"),P(b,"/a/d1b"))==0 && has(P(b,"/a/d1b"),"d"));      /* files on disk */
    CHECK(unlink(P(b,"/a/d2"))==0 && stat_of(P(b,"/a/d2"))==-2);
    CHECK(rename(P(b,"/a/y1"),P(b,"/a/y1b"))==0 && has(P(b,"/a/y1b"),"y"));      /* files made just before the chmod */
    CHECK(unlink(P(b,"/a/y2"))==0 && stat_of(P(b,"/a/y2"))==-2);
    CHECK(!layer || (link(P(b,"/a/d4"),P(b,"/a/h"))==0 && has(P(b,"/a/h"),"d")));
    CHECK(rename(P(b,"/a/mvd"),P(b,"/out"))==0 && mode(P(b,"/out"))==0555);      /* a 0555 directory to another parent: its ".." is written */
    CHECK(put(P(b,"/a/keep"),"kept")==4);
    CHECK(mode(P(b,"/a"))==0555);
    /* files open whatever their mode */
    CHECK(chmod(P(b,"/a/d3"),0444)==0 && append(P(b,"/a/d3"),"+")==1 && mode(P(b,"/a/d3"))==0444);
    CHECK(chmod(P(b,"/a/d3"),0)==0 && has(P(b,"/a/d3"),"d+") && append(P(b,"/a/d3"),"+")==1 && mode(P(b,"/a/d3"))==0);
    CHECK(C2(45,P(b,"/a/d3"),2)==0 && has(P(b,"/a/d3"),"d+") && mode(P(b,"/a/d3"))==0);   /* truncate */
    /* no permission at all: listed, searched, entered, changed */
    CHECK(chmod(P(b,"/a"),0)==0);
    CHECK(names(P(b,"/a"))==11+layer);                                           /* d l p pre2 d1b d3 d4 d5 y1b y3 keep, and h */
    CHECK(put(P(b,"/a/z"),"z")==1 && has(P(b,"/a/z"),"z") && mkdir(P(b,"/a/d/in"))==0 && mode(P(b,"/a"))==0);
    CHECK(chdir(P(b,"/a"))==0 && stat_of("z")==0 && put("cwd","c")==1 && names(".")==13+layer && chdir("/")==0 && mode(P(b,"/a"))==0);
    CHECK(chmod(P(b,"/a/."),0700)==0 && mode(P(b,"/a"))==0700);                  /* the call's own chmod is what stays */
    CHECK(chmod(P(b,"/a"),0555)==0);
}
/* Anyone the mode binds (another user; root without CAP_DAC_OVERRIDE): refused, in base/a at 0555 and 000. */
static void bound(const char *b,int user) {
    where=b;
    CHECK(mkdir(P(b,"/a/e"))==EACCES && symlink(P(b,"/a/m"))==EACCES && mkfifo(P(b,"/a/q"))==EACCES);
    CHECK(rmdir(P(b,"/a/pre2"))==EACCES && put(P(b,"/a/new"),"n")==EACCES);
    CHECK(rename(P(b,"/a/d5"),P(b,"/a/d5x"))==EACCES && unlink(P(b,"/a/d4"))==EACCES);
    CHECK(rename(P(b,"/a/y3"),P(b,"/a/y3x"))==EACCES && unlink(P(b,"/a/y3"))==EACCES);
    CHECK((b[1]!='w' || link(P(b,"/a/d5"),P(b,"/a/h2"))==EACCES) && rename(P(b,"/out"),P(b,"/a/in"))==EACCES);
    CHECK(append(P(b,"/a/d5"),"")==0);                                           /* the file's own mode allows it */
    if(!user)return;                                                             /* (the test's root child stops here: a chmod is its to undo) */
    CHECK(chmod(P(b,"/a"),0)==0);
    CHECK(names(P(b,"/a"))==EACCES && put(P(b,"/a/zz"),"z")==EACCES && stat_of(P(b,"/a/d5"))==EACCES && chdir(P(b,"/a"))==EACCES);
    CHECK(chmod(P(b,"/a"),0555)==0 && chmod(P(b,"/a/d5"),0)==0 && open3(P(b,"/a/d5"),0,0)==EACCES && chmod(P(b,"/a/d5"),0644)==0);
}
void _start(void) {
    int vol=mode("/vol")>=0;
    CHECK(mkdir("/w")==0);
    setup("/w");if(vol)setup("/vol");
    root("/w");if(vol)root("/vol");
    /* a 0555 directory of the image: a whiteout, a new directory and a new file in its upper copy */
    where="/low/ro";
    CHECK(mode("/low/ro")==0555 && unlink("/low/ro/x")==0 && stat_of("/low/ro/x")==-2 && rmdir("/low/ro/sub")==0 && mkdir("/low/ro/new")==0);
    CHECK(put("/low/ro/f","f")==1 && rename("/low/ro/mv","/low/ro/mv2")==0 && has("/low/ro/mv2","mv") && mode("/low/ro")==0555 && names("/low/ro")==4);   /* keep new f mv2 */
    where="/";CHECK(chmod("/",0555)==0 && mkdir("/top")==0 && rmdir("/top")==0 && mode("/")==0555 && chmod("/",0755)==0);
    where="/ro";CHECK(mode("/ro")<0 || (names("/ro/locked")==EACCES && stat_of("/ro/locked/f")==EACCES && mkdir("/ro/new")==-30));
    long pid=fork();
    if(!pid) {                                                                   /* another user */
        CHECK(C2(159,0,0)==0 && C1(144,1000)==0 && C1(146,1000)==0);
        bound("/w",1);if(vol)bound("/vol",1);
        where="/low";CHECK(unlink("/low/ro/keep")==EACCES && mkdir("/low/ro/e")==EACCES && mkdir("/low/ro2/e")==EACCES && unlink("/low/ro2/x")==EACCES);
        C1(94,0);
    }
    wait_ok(pid);
    pid=fork();
    if(!pid) {                                                                   /* root having dropped CAP_DAC_OVERRIDE (capability 1) */
        unsigned header[2]={0x20080522,0},data[6]={~2u,~2u,0,0x1ff,0x1ff,0};
        CHECK(C2(91,header,data)==0);
        bound("/w",0);if(vol)bound("/vol",0);
        C1(94,0);
    }
    wait_ok(pid);
    where="";CHECK(mode("/w/a")==0555 && mkdir("/w/a/last")==0 && (!vol || mkdir("/vol/a/last")==0));   /* the parent still holds it */
    C3(64,1,"rootdac PASS\n",13);C1(94,0);for(;;){}
}
