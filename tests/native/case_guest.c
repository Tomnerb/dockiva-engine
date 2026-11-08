/* Names that differ only by case, as a Linux container sees them: run by
   scripts/verify_overlay.py on an empty upper directory over a lower tree (Lower,
   inc/xt_CONNMARK.h and inc/xt_connmark.h, Dir/x), both on a case-sensitive APFS
   volume, as the engine keeps them (dockivad/store.go), with and without the memory
   layer (--upper-memory). On the Mac's case-insensitive APFS each pair is one host
   file: dpkg's NAME.dpkg-new of L and l was one symlink, the second of Bar and bar
   was dropped at write-back. The verifier then checks the upper directory. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define C1(n,a) call(n,(U)(a),0,0,0,0,0)
#define C3(n,a,b,c) call(n,(U)(a),(U)(b),(U)(c),0,0,0)
#define C4(n,a,b,c,d) call(n,(U)(a),(U)(b),(U)(c),(U)(d),0,0)
#define AT (-100)
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void say(const char *s) { C3(64,1,s,slen(s)); }
static void fail_at(int line) {
    char text[40]="case FAIL line ";unsigned n=15;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static int scmp(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a!=*b; }
static int same(const void *a,const void *b,unsigned n) { const unsigned char *p=a,*q=b;for(unsigned i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,0); }
static long lstat_of(const char *path) { return C4(79,AT,path,st,0x100); }
static long open3(const char *path,U flags,U mode) { return C4(56,AT,path,flags,mode); }
static long put(const char *path,const char *text) {
    long fd=open3(path,01101,0644);if(fd<0)return fd;               /* O_WRONLY|O_CREAT|O_TRUNC */
    long n=C3(64,fd,text,slen(text));C1(57,fd);return n;
}
static int has(const char *path,const char *text) {
    char buffer[256];long fd=open3(path,0,0);if(fd<0)return 0;
    long n=C3(63,fd,buffer,sizeof buffer);C1(57,fd);
    return n==(long)slen(text) && same(buffer,text,(unsigned)n);
}
/* Names in a directory (getdents64) besides . and .., or how often want is one. */
static int names(const char *dir,const char *want) {
    unsigned char buffer[512];long fd=open3(dir,040000,0);if(fd<0)return -1;   /* O_DIRECTORY */
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
    C1(57,fd);if(dots!=2 || found>1)return -2;                          /* each name once */
    return want?found:count;
}
void _start(void) {
    char buffer[64];
    /* the lower layer's pair (an image's unpack keeps both) */
    CHECK(has("/inc/xt_CONNMARK.h","MARK") && has("/inc/xt_connmark.h","mark") && names("/inc",0)==2);
    /* files and directories */
    CHECK(put("/Foo","F")==1 && put("/foo","f")==1 && has("/Foo","F") && has("/foo","f") && names("/","Foo")==1 && names("/","foo")==1);
    CHECK(C3(34,AT,"/D",0755)==0 && C3(34,AT,"/d",0755)==0 && put("/D/in","1")==1 && stat_of("/d/in")==-2 && names("/d",0)==0);
    /* dpkg's unpack: NAME.dpkg-new for each, then the cleanup of a stale l.dpkg-new */
    CHECK(C3(36,"x",AT,"/L.new")==0 && C3(36,"y",AT,"/l.new")==0 && C3(35,AT,"/l.new",0)==0);
    CHECK(C4(78,AT,"/L.new",buffer,sizeof buffer)==1 && buffer[0]=='x' && lstat_of("/l.new")==-2);
    /* over a lower name in another case: a new file, a whiteout of the lower one only, a new directory */
    CHECK(put("/lower","u")==1 && has("/Lower","L") && has("/lower","u"));
    CHECK(C3(35,AT,"/Lower",0)==0 && stat_of("/Lower")==-2 && has("/lower","u") && names("/","lower")==1);
    CHECK(C3(34,AT,"/dir",0755)==0 && names("/Dir",0)==1 && names("/dir",0)==0);
    /* a rename to the name in another case */
    CHECK(C4(38,AT,"/Foo",AT,"/FOO")==0 && open3("/Foo",0,0)==-2 && has("/FOO","F") && has("/foo","f"));
    /* with --upper-memory, files in memory until the run's end writes both back */
    CHECK(put("/Bar","B")==1 && put("/bar","b")==1 && has("/Bar","B") && has("/bar","b"));
    say("case PASS\n");C1(94,0);for(;;){}
}
