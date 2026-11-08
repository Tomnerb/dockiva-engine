/* Image layers (--root UPPER --lower LOWER, native/overlay.h) as Docker's overlay2
   shows them to a container: run on an empty upper directory over the lower tree
   scripts/verify_overlay.py makes (contents listed there). Copy-up keeps mode and
   times, whiteouts hide lower files and directories, a directory made again is
   opaque, rename and link work across the layers, directories list merged, and a
   lower file stays readable while open after its unlink. The verifier then checks
   the lower tree is untouched. */
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
    char text[40]="overlay FAIL line ";unsigned n=18;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static int scmp(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a!=*b; }
static int same(const void *a,const void *b,unsigned n) { const unsigned char *p=a,*q=b;for(unsigned i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1; }
static unsigned long u64(const unsigned char *p) { unsigned long v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned u32(const unsigned char *p) { return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,0); }
static long lstat_of(const char *path) { return C4(79,AT,path,st,0x100); }
#define ST_INO u64(st+8)
#define ST_MODE u32(st+16)
#define ST_NLINK u32(st+20)
#define ST_SIZE u64(st+48)
#define ST_MTIME u64(st+88)
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
/* Names in a directory (getdents64) besides . and .., or whether want is one. */
static int names(const char *dir,const char *want) {
    unsigned char buffer[512];long fd=open3(dir,040000,0);if(fd<0)return -1;   /* O_DIRECTORY; small reads: several calls */
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
    char buffer[8192];long fd;
    /* reads, through lower symlinks too */
    CHECK(has("/f","lower\n") && stat_of("/f")==0 && (ST_MODE&07777)==0644 && ST_MTIME==1000000000);
    CHECK(has("/ln","lower\n") && has("/abs/a","a") && has("/deep/1/2/3/f","deep") && names("/d",0)==3 && names("/d","sub")==1);
    /* a write copies the file up; a metadata change keeps its mode (setuid too) and times */
    unsigned long ino=(stat_of("/f"),ST_INO);
    fd=open3("/f",02002,0);CHECK(fd>=0 && C3(64,fd,"more\n",5)==5 && C1(57,fd)==0);  /* O_RDWR|O_APPEND */
    CHECK(has("/f","lower\nmore\n") && stat_of("/f")==0 && (ST_MODE&07777)==0644 && ST_INO!=ino);
    { unsigned char times[32]={0};times[0]=5;times[24]=0xfe;times[25]=0xff;times[26]=0xff;times[27]=0x3f; /* atime 5, mtime UTIME_OMIT */
      CHECK(C4(88,AT,"/suid",times,0)==0 && stat_of("/suid")==0 && (ST_MODE&07777)==04755 && ST_MTIME==1000000000 && u64(st+72)==5); }
    CHECK(C3(53,AT,"/ch",0600)==0 && stat_of("/ch")==0 && (ST_MODE&07777)==0600 && ST_MTIME==1000000000 && has("/ch","chmod"));
    { unsigned char times[32]={0};times[16]=200;CHECK(C4(88,AT,"/ut",times,0)==0 && stat_of("/ut")==0 && ST_MTIME==200 && has("/ut","utimes")); }
    CHECK(C2(45,"/t1",4)==0 && has("/t1","trun"));                            /* truncate */
    CHECK(C4(88,AT,"/ln",0,0x100)==0 && lstat_of("/ln")==0 && (ST_MODE&0170000)==0120000 && C4(78,AT,"/ln",buffer,64)==1 && buffer[0]=='f');
    /* readlink of what is no link, of missing paths, and opens of missing files: the same
       answers again (from the lookup cache, when the run has one), until the file is made */
    for(int i=0;i<3;i++) {
        CHECK(C4(78,AT,"/d",buffer,64)==-22 && C4(78,AT,"/d/a",buffer,64)==-22 && C4(78,AT,"/f",buffer,64)==-22 && C4(78,AT,"/d/a",buffer,0)==-22);
        CHECK(C4(78,AT,"/d/none",buffer,64)==-2 && C4(78,AT,"/nodir/x",buffer,64)==-2 && C4(78,AT,"/d/a/x",buffer,64)==-20);
        CHECK(C4(78,AT,"/abs",buffer,64)==2 && same(buffer,"/d",2) && open3("/e/later",0,0)==-2 && open3("/nodir/x",0,0)==-2 && open3("/d/a/x",0,0)==-20);
    }
    CHECK(put("/e/later","l")==1 && has("/e/later","l") && C4(78,AT,"/e/later",buffer,64)==-22);
    CHECK(C3(34,AT,"/nodir",0755)==0 && put("/nodir/x","x")==1 && has("/nodir/x","x") && C4(78,AT,"/nodir/x",buffer,64)==-22);
    /* whiteouts: a lower file, then its directory; made again, the directory is empty */
    CHECK(C3(35,AT,"/gone/x",0)==0 && stat_of("/gone/x")==-2 && names("/gone",0)==0);
    CHECK(C3(35,AT,"/gone",0x200)==0 && stat_of("/gone")==-2 && names("/","gone")==0);
    CHECK(C3(34,AT,"/gone",0755)==0 && names("/gone",0)==0 && stat_of("/gone/x")==-2 && put("/gone/new","n")==1 && names("/gone",0)==1);
    CHECK(C3(35,AT,"/d",0x200)==-39 && C3(35,AT,"/d/b",0x200)==-20 && C3(35,AT,"/d",0)==-21);
    CHECK(C3(35,AT,"/x/y",0)==0 && open3("/x/y",0,0)==-2 && put("/x/y","new")==3 && has("/x/y","new") && names("/x",0)==1);
    /* merged listings */
    CHECK(put("/d/n","n")==1 && C3(35,AT,"/d/a",0)==0 && names("/d",0)==3 && names("/d","a")==0 && names("/d","n")==1 && names("/d","b")==1);
    /* rename: a lower file; a lower directory refuses (EXDEV, mv copies); onto lower names */
    CHECK(C4(38,AT,"/mv",AT,"/mv2")==0 && has("/mv2","mv") && open3("/mv",0,0)==-2 && names("/","mv")==0);
    CHECK(C4(38,AT,"/tdir",AT,"/tdir2")==-18 && has("/tdir/t","t"));
    CHECK(C3(34,AT,"/newdir",0755)==0 && put("/newdir/k","k")==1 && C4(38,AT,"/newdir",AT,"/edir2")==0 && names("/edir2",0)==1 && has("/edir2/k","k"));
    CHECK(put("/nf","nf")==2 && C4(38,AT,"/nf",AT,"/ro")==0 && has("/ro","nf") && open3("/nf",0,0)==-2);
    CHECK(C3(34,AT,"/nd2",0755)==0 && C4(38,AT,"/nd2",AT,"/tdir")==-39 && C4(38,AT,"/nd2",AT,"/d/b")==-20);
    CHECK(C4(38,AT,"/d/b",AT,"/d/b")==0 && has("/d/b","b"));
    /* hard links: the lower file is copied up, then linked */
    CHECK(C5(37,AT,"/h1",AT,"/h3",0)==0 && stat_of("/h3")==0 && ST_NLINK==2);
    CHECK(put("/h3","new")==3 && has("/h1","new") && has("/h2","hard"));
    /* unlink while open */
    fd=open3("/open",0,0);CHECK(fd>=0 && C3(35,AT,"/open",0)==0 && stat_of("/open")==-2 && C4(67,fd,buffer,4,0)==4 && same(buffer,"open",4) && C1(57,fd)==0);
    /* lower names exist for create calls */
    CHECK(open3("/d/b",0301,0644)==-17 && C3(34,AT,"/e",0755)==-17 && C3(36,"x",AT,"/d/b")==-17 && C4(33,AT,"/d/b",0100644,0)==-17);
    CHECK(C5(37,AT,"/ro",AT,"/d/b",0)==-17);
    /* reserved names */
    CHECK(put("/.wh.x","x")==-2 && C3(34,AT,"/.wh.d",0755)==-2 && C4(38,AT,"/ro",AT,"/.wh.ro")==-2 && names("/",".wh.mv")==0 && stat_of("/.wh.mv")==-2 && stat_of("/d/.wh.a")==-2);
    /* mmap: shared writes to a copied-up file; none through a lower file opened read-only */
    fd=open3("/map",02,0);CHECK(fd>=0);
    unsigned char *m=(unsigned char *)call(222,0,8192,3,1,(U)fd,0);CHECK((long)m>0 && m[0]=='L');
    m[0]='S';CHECK(C3(227,(U)m,8192,4)==0 && C2(215,(U)m,8192)==0 && C4(67,fd,buffer,2,0)==2 && buffer[0]=='S' && buffer[1]=='L' && C1(57,fd)==0);
    fd=open3("/d/b",0,0);CHECK(fd>=0 && call(222,0,4096,3,1,(U)fd,0)==-13 && C3(64,fd,"x",1)<0 && C2(46,fd,0)<0 && C2(52,fd,0777)==-30);
    CHECK(C1(57,fd)==0 && has("/d/b","b"));
    /* a lower directory's descriptor: fchmod copies it up */
    fd=open3("/ddir",040000,0);CHECK(fd>=0 && C2(52,fd,0700)==0 && C1(57,fd)==0 && stat_of("/ddir")==0 && (ST_MODE&07777)==0700);
    /* a lower directory as working directory and as dirfd, after files appear in it */
    CHECK(C1(49,"/cwd")==0 && put("rel","r")==1 && has("/cwd/rel","r") && C2(17,buffer,sizeof buffer)==5 && same(buffer,"/cwd",5) && C1(49,"/")==0);
    fd=open3("/deep/1/2",040000,0);CHECK(fd>=0 && put("/deep/1/2/new","n")==1);
    { long f=C4(56,fd,"new",0,0);CHECK(f>=0 && C1(57,f)==0);f=C4(56,fd,"3/f",0,0);CHECK(f>=0 && C1(57,f)==0 && C1(57,fd)==0); }
    CHECK(C3(36,"/deep/1/2/3",AT,"/sl")==0 && has("/sl/f","deep"));
    CHECK(C3(35,AT,"/deep/1/2/3/f",0)==0 && C3(35,AT,"/deep/1/2/3",0x200)==0 && names("/deep/1/2",0)==1 && names("/deep/1/2","new")==1);
    /* busybox find's walk: every directory once, nothing reserved */
    CHECK(names("/",".wh..wh..opq")==0 && names("/gone",".wh..wh..opq")==0 && names("/edir2",0)==1);
    say("overlay PASS\n");C1(94,0);for(;;){}
}
