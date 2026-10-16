/* POSIX semantics of a tmpfs at /m (and a 64 KiB one at /s), as Linux's tmpfs has
   them: run with --tmpfs /m --tmpfs /s:size=64k on a root holding /hostfile ("host\n")
   and /leaf (fork_exec_leaf: exits 23). Also runs on Linux itself (OrbStack) with
   --tmpfs, which is how its expectations were checked. Ends as uid 1000. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define C0(n) call(n,0,0,0,0,0,0)
#define C1(n,a) call(n,(U)(a),0,0,0,0,0)
#define C2(n,a,b) call(n,(U)(a),(U)(b),0,0,0,0)
#define C3(n,a,b,c) call(n,(U)(a),(U)(b),(U)(c),0,0,0)
#define C4(n,a,b,c,d) call(n,(U)(a),(U)(b),(U)(c),(U)(d),0,0)
#define C5(n,a,b,c,d,e) call(n,(U)(a),(U)(b),(U)(c),(U)(d),(U)(e),0)
#define AT (-100)
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void say(const char *s) { C3(64,1,s,slen(s)); }
static void fail_at(int line) {
    char text[32]="memfs FAIL line ";unsigned n=16;char digits[8];unsigned k=0;
    while(line && k<8) { digits[k++]=(char)('0'+line%10);line/=10; }
    while(k)text[n++]=digits[--k];
    text[n++]='\n';C3(64,1,text,n);C1(94,1);for(;;){}
}
#define CHECK(x) do { if(!(x))fail_at(__LINE__); } while(0)
static int scmp(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a!=*b; }
static int same(const void *a,const void *b,unsigned n) { const unsigned char *p=a,*q=b;for(unsigned i=0;i<n;i++)if(p[i]!=q[i])return 0;return 1; }
static void fill(void *a,int v,unsigned n) { unsigned char *p=a;for(unsigned i=0;i<n;i++)p[i]=(unsigned char)v; }
static unsigned long u64(const unsigned char *p) { unsigned long v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned u32(const unsigned char *p) { return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static unsigned char st[128];
static long stat_of(const char *path) { return C4(79,AT,path,st,0); }
static long lstat_of(const char *path) { return C4(79,AT,path,st,0x100); }
#define ST_MODE u32(st+16)
#define ST_NLINK u32(st+20)
#define ST_UID u32(st+24)
#define ST_SIZE u64(st+48)
#define ST_BLOCKS u64(st+64)
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
/* Names in a directory (getdents64), counting how many there are besides . and .. */
static int names(const char *dir,const char *want) {
    unsigned char buffer[4096];long fd=open3(dir,040000,0);if(fd<0)return -1;   /* O_DIRECTORY */
    int count=0,found=0;
    for(;;) {
        long n=C3(61,fd,buffer,sizeof buffer);if(n<=0)break;
        for(long at=0;at<n;) {
            const char *name=(const char *)buffer+at+19;unsigned size=buffer[at+16]|buffer[at+17]<<8;
            if(!(name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2])))) { count++;if(want && !scmp(name,want))found=1; }
            at+=size;
        }
    }
    C1(57,fd);return want?found:count;
}
static unsigned char big[3*4096];
void _start(void) {
    char buffer[8192];
    /* files: create, O_EXCL, O_TRUNC, O_APPEND, positioned I/O, holes */
    CHECK(put("/m/a","hello\n")==6 && has("/m/a","hello\n"));
    CHECK(open3("/m/a",0301,0644)==-17);                                      /* O_CREAT|O_EXCL: EEXIST */
    long fd=open3("/m/a",02002,0);CHECK(fd>=0);                               /* O_RDWR|O_APPEND */
    CHECK(C3(64,fd,"more\n",5)==5 && C4(68,fd,"X",1,0)==1);                  /* pwrite with O_APPEND appends (Linux) */
    CHECK(C1(57,fd)==0 && has("/m/a","hello\nmore\nX"));
    CHECK(put("/m/a","t")==1 && has("/m/a","t"));                            /* O_TRUNC */
    fd=open3("/m/sparse",0102,0600);CHECK(fd>=0);
    CHECK(C4(68,fd,"end",3,1048576)==3 && C2(80,fd,st)==0 && ST_SIZE==1048579 && ST_BLOCKS==8);
    fill(buffer,7,16);CHECK(C4(67,fd,buffer,16,4096)==16);for(int i=0;i<16;i++)CHECK(!buffer[i]);
    CHECK(C3(62,fd,0,3)==1048576 && C3(62,fd,0,4)==0 && C3(62,fd,5,0)==5);   /* SEEK_DATA, SEEK_HOLE */
    CHECK(C3(62,fd,2000000,3)==-6);                                          /* past the end: ENXIO */
    CHECK(C2(46,fd,2)==0 && C2(80,fd,st)==0 && ST_SIZE==2 && ST_BLOCKS==0);   /* ftruncate frees */
    CHECK(C4(47,fd,0,0,10000)==0 && C2(80,fd,st)==0 && ST_SIZE==10000 && ST_BLOCKS==24); /* fallocate */
    CHECK(C4(47,fd,3,4096,4096)==0 && C2(80,fd,st)==0 && ST_SIZE==10000 && ST_BLOCKS==16); /* punch a hole */
    CHECK(C1(82,fd)==0 && C1(83,fd)==0 && C1(57,fd)==0);
    CHECK(C2(45,"/m/sparse",5)==0 && stat_of("/m/sparse")==0 && ST_SIZE==5);  /* truncate */
    CHECK(stat_of("/m/a/")==-20 && open3("/m/nope",0,0)==-2 && open3("/m/a/x",0,0)==-20);
    /* directories, getdents stable while entries go */
    CHECK(C3(34,AT,"/m/d",0755)==0 && C3(34,AT,"/m/d",0755)==-17 && C3(34,AT,"/m/d/",0755)==-17);
    CHECK(C3(34,AT,"/m/d/e/f",0755)==-2 && C3(34,AT,"/m/d/e",0700)==0 && stat_of("/m/d")==0 && ST_NLINK==3);
    CHECK(C3(35,AT,"/m/d",0x200)==-39 && C3(35,AT,"/m/d",0)==-21 && C3(35,AT,"/m/a",0x200)==-20);
    CHECK(C3(35,AT,"/m",0x200)==-16 && C3(35,AT,"/m/d/.",0x200)==-22);       /* a mount point; "." */
    char name[]="/m/many/f000";CHECK(C3(34,AT,"/m/many",0755)==0);
    for(int i=0;i<300;i++) { name[9]=(char)('0'+i/100);name[10]=(char)('0'+i/10%10);name[11]=(char)('0'+i%10);CHECK(put(name,"x")==1); }
    CHECK(names("/m/many",0)==300 && names("/m/many","f123")==1);
    {   /* busybox rm -r: read a little, unlink what was read, read on */
        long d=open3("/m/many",040000,0);CHECK(d>=0);int removed=0;unsigned char small[200];
        for(;;) {
            long n=C3(61,d,small,sizeof small);if(n<=0) { CHECK(!n);break; }
            for(long at=0;at<n;) { const char *f=(const char *)small+at+19;
                if(f[0]!='.') { CHECK(C3(35,d,f,0)==0);removed++; }
                at+=small[at+16]|small[at+17]<<8; }
        }
        C1(57,d);CHECK(removed==300 && names("/m/many",0)==0 && C3(35,AT,"/m/many",0x200)==0);
    }
    /* symlinks */
    CHECK(C3(36,"a",AT,"/m/l")==0 && has("/m/l","t") && lstat_of("/m/l")==0 && (ST_MODE&0170000)==0120000 && ST_SIZE==1);
    CHECK(C4(78,AT,"/m/l",buffer,64)==1 && buffer[0]=='a' && open3("/m/l",0100000,0)==-40); /* readlink; O_NOFOLLOW */
    CHECK(C3(36,"loop",AT,"/m/loop")==0 && open3("/m/loop",0,0)==-40);
    CHECK(C3(36,"/hostfile",AT,"/m/h")==0 && has("/m/h","host\n"));         /* out to the host */
    CHECK(has("/m/../hostfile","host\n") && has("/m/d/../../hostfile","host\n"));
    CHECK(C3(36,"/m/d",AT,"/m/dl")==0 && C3(34,AT,"/m/dl/x",0755)==0 && stat_of("/m/d/x")==0);  /* through a link, back in */
    /* hard links */
    CHECK(C5(37,AT,"/m/a",AT,"/m/a2",0)==0 && stat_of("/m/a")==0 && ST_NLINK==2);
    CHECK(C5(37,AT,"/m/d",AT,"/m/d2",0)==-1 && C5(37,AT,"/m/a",AT,"/hostlink",0)==-18);
    CHECK(C3(35,AT,"/m/a",0)==0 && has("/m/a2","t") && stat_of("/m/a2")==0 && ST_NLINK==1);
    /* rename */
    CHECK(put("/m/r1","one")==3 && put("/m/r2","two")==3);
    CHECK(C5(276,AT,"/m/r1",AT,"/m/r2",1)==-17);                              /* RENAME_NOREPLACE */
    CHECK(C5(276,AT,"/m/r1",AT,"/m/r2",2)==0 && has("/m/r1","two") && has("/m/r2","one")); /* RENAME_EXCHANGE */
    CHECK(C4(38,AT,"/m/r1",AT,"/m/r2")==0 && has("/m/r2","two") && open3("/m/r1",0,0)==-2);
    CHECK(put("/m/d/x/in","deep")==4 && C4(38,AT,"/m/d",AT,"/m/moved")==0 && has("/m/moved/x/in","deep"));
    CHECK(C4(38,AT,"/m/moved",AT,"/m/moved/x/sub")==-22 && C4(38,AT,"/m/moved",AT,"/m/r2")==-20);
    CHECK(C4(38,AT,"/m/r2",AT,"/m/moved")==-21 && C3(34,AT,"/m/full",0755)==0 && put("/m/full/f","f")==1);
    CHECK(C4(38,AT,"/m/moved",AT,"/m/full")==-39 && C4(38,AT,"/m/r2",AT,"/renamed")==-18);
    CHECK(stat_of("/m/moved")==0 && ST_NLINK==4 && stat_of("/m")==0);
    /* unlink while open */
    fd=open3("/m/gone",0102,0644);CHECK(fd>=0 && C3(64,fd,"still",5)==5 && C3(35,AT,"/m/gone",0)==0);
    CHECK(C2(80,fd,st)==0 && ST_NLINK==0 && C4(67,fd,buffer,5,0)==5 && same(buffer,"still",5) && C1(57,fd)==0);
    /* owners, modes, times */
    CHECK(C3(53,AT,"/m/a2",0640)==0 && stat_of("/m/a2")==0 && (ST_MODE&07777)==0640);
    CHECK(C5(54,AT,"/m/a2",1000,1000,0)==0 && stat_of("/m/a2")==0 && ST_UID==1000 && u32(st+28)==1000);
    { unsigned char times[32]={0};times[0]=100;times[16]=200;                 /* atime 100 s, mtime 200 s */
      CHECK(C4(88,AT,"/m/a2",times,0)==0 && stat_of("/m/a2")==0 && u64(st+72)==100 && u64(st+88)==200); }
    /* extended attributes */
    CHECK(C5(5,"/m/a2","user.k","value",5,0)==0 && C4(8,"/m/a2","user.k",buffer,64)==5 && same(buffer,"value",5));
    CHECK(C5(5,"/m/a2","user.k","v",1,1)==-17 && C3(11,"/m/a2",buffer,64)==7 && same(buffer,"user.k",7));
    CHECK(C2(14,"/m/a2","user.k")==0 && C4(8,"/m/a2","user.k",buffer,64)==-61);
    /* statfs, access */
    unsigned char sfs[120];CHECK(C2(43,"/m",sfs)==0 && u64(sfs)==0x01021994);
    CHECK(C3(48,AT,"/m/a2",4)==0 && C3(48,AT,"/m/nope",0)==-2);
    /* mmap: shared writes reach the file; private ones do not */
    fd=open3("/m/map",0102,0644);CHECK(fd>=0 && C2(46,fd,8192)==0 && C4(68,fd,"file",4,0)==4);
    unsigned char *m=(unsigned char *)call(222,0,8192,3,1,(U)fd,0);CHECK((long)m>0 && same(m,"file",4));
    m[4096]='S';CHECK(C3(227,(U)m,8192,4)==0 && C2(215,(U)m,8192)==0);       /* msync, munmap */
    CHECK(C4(67,fd,buffer,1,4096)==1 && buffer[0]=='S');
    m=(unsigned char *)call(222,0,4096,3,2,(U)fd,0);CHECK((long)m>0 && m[0]=='f');
    m[0]='P';CHECK(C2(215,(U)m,4096)==0 && C4(67,fd,buffer,1,0)==1 && buffer[0]=='f' && C1(57,fd)==0);
    /* O_TMPFILE, then a name through /proc/self/fd */
    fd=open3("/m",020040002,0600);CHECK(fd>=0 && C3(64,fd,"tmp",3)==3);       /* O_TMPFILE|O_RDWR */
    { char link[]="/proc/self/fd/  ";link[14]=(char)('0'+fd/10);link[15]=(char)('0'+fd%10);if(fd<10) { link[14]=(char)('0'+fd);link[15]=0; }
      CHECK(C5(37,AT,link,AT,"/m/named",0x400)==0 && has("/m/named","tmp") && C1(57,fd)==0); }
    /* FIFO */
    CHECK(C4(33,AT,"/m/fifo",0010644,0)==0 && stat_of("/m/fifo")==0 && (ST_MODE&0170000)==0010000);
    fd=open3("/m/fifo",04000,0);long w=open3("/m/fifo",01,0);                 /* the read end (O_NONBLOCK), then the write end */
    CHECK(fd>=0 && w>=0 && C3(64,w,"p",1)==1 && C3(63,fd,buffer,1)==1 && buffer[0]=='p' && C1(57,fd)==0 && C1(57,w)==0);
    /* poll: a regular file is always ready */
    fd=open3("/m/a2",02,0);CHECK(fd>=0);
    { struct { int fd;short events,revents; } p={(int)fd,1|4,0};unsigned char zero[16]={0};
      CHECK(C5(73,&p,1,zero,0,8)==1 && p.revents==(1|4)); }
    /* dup shares the offset; fork shares the description and the file system */
    long d2=C1(23,fd);CHECK(d2>=0 && C3(63,fd,buffer,1)==1 && C3(62,d2,0,1)==1);
    long child=C5(220,17,0,0,0,0);CHECK(child>=0);
    if(!child) { if(C3(64,fd,"c",1)!=1 || put("/m/child","made")!=4)C1(94,9);C1(94,0); }
    unsigned status=1;CHECK(C4(260,child,&status,0,0)==child && status==0 && C3(62,fd,0,1)==2 && has("/m/child","made"));
    C1(57,d2);
    /* fd passing: SCM_RIGHTS */
    { int pair[2];CHECK(C4(199,1,1,0,pair)==0);
      unsigned char control[24]={0};control[0]=20;control[8]=1;control[12]=1;*(int *)(control+16)=(int)fd; /* len 20, SOL_SOCKET, SCM_RIGHTS */
      struct { U base,length; } io={(U)"x",1};
      struct { U name;unsigned namelen,pad;U iov,iovlen,control,controllen;int flags,pad2; } msg={0,0,0,(U)&io,1,(U)control,24,0,0};
      CHECK(C3(211,pair[0],&msg,0)==1);
      unsigned char in[24]={0};char byte;struct { U base,length; } io2={(U)&byte,1};
      struct { U name;unsigned namelen,pad;U iov,iovlen,control,controllen;int flags,pad2; } msg2={0,0,0,(U)&io2,1,(U)in,24,0,0};
      CHECK(C3(212,pair[1],&msg2,0)==1 && in[12]==1);
      long got=*(int *)(in+16);CHECK(got>=0 && C4(67,got,buffer,1,0)==1 && C3(62,got,0,1)==2); /* the same description */
      C1(57,got);C1(57,pair[0]);C1(57,pair[1]); }
    C1(57,fd);
    /* record locks and flock across processes */
    fd=open3("/m/lock",0102,0644);CHECK(fd>=0);
    { unsigned char lk[32]={0};lk[0]=1;                                         /* F_WRLCK, whole file */
      CHECK(C3(25,fd,6,lk)==0);
      child=C5(220,17,0,0,0,0);CHECK(child>=0);
      if(!child) { unsigned char q[32]={0};q[0]=1;long f2=open3("/m/lock",02,0);
                   if(C3(25,f2,6,q)!=-11 || C3(25,f2,5,q)!=0 || q[0]!=1)C1(94,8);C1(94,0); }
      status=1;CHECK(C4(260,child,&status,0,0)==child && status==0); }
    CHECK(C2(32,fd,2|4)==0 && C1(57,fd)==0);
    /* exec of a memfs file */
    { long in=open3("/leaf",0,0),out=open3("/m/leaf",01101,0755);CHECK(in>=0 && out>=0);
      for(;;) { long n=C3(63,in,big,sizeof big);CHECK(n>=0);if(!n)break;CHECK(C3(64,out,big,n)==n); }
      C1(57,in);C1(57,out);
      child=C5(220,17,0,0,0,0);CHECK(child>=0);
      if(!child) { const char *argv[]={"/m/leaf",0};C3(221,"/m/leaf",argv,0);C1(94,77); }
      status=0;CHECK(C4(260,child,&status,0,0)==child && status==(23<<8)); }
    /* copy between the host and memfs */
    { long in=open3("/hostfile",0,0),out=open3("/m/copy",0101,0644);CHECK(in>=0 && out>=0);
      CHECK(C4(71,out,in,0,100)==5 && C1(57,out)==0 && has("/m/copy","host\n"));
      long back=open3("/m/copy",0,0);out=open3("/m/copy2",0101,0644);
      CHECK(C5(285,back,0,out,0,100)==5 && has("/m/copy2","host\n"));C1(57,in);C1(57,out);C1(57,back); }
    /* working directory */
    CHECK(C1(49,"/m/moved/x")==0 && has("in","deep") && C2(17,buffer,sizeof buffer)==11 && same(buffer,"/m/moved/x",11));
    CHECK(C4(38,AT,"/m/moved",AT,"/m/renamed")==0 && C2(17,buffer,sizeof buffer)==13 && same(buffer,"/m/renamed/x",13));
    CHECK(C1(49,"../../..")==0 && C2(17,buffer,sizeof buffer)==2 && buffer[0]=='/' && has("hostfile","host\n"));
    { long dir=open3("/m/renamed",040000,0);CHECK(dir>=0 && has("/m/renamed/x/in","deep") && C1(50,dir)==0 && has("x/in","deep"));
      CHECK(C4(56,dir,"x/in",0,0)>=0);C1(49,"/");C1(57,dir); }
    /* a size limit: short write, then ENOSPC */
    fd=open3("/s/f",0101,0644);CHECK(fd>=0);fill(big,1,sizeof big);
    long total=0;for(;;) { long n=C3(64,fd,big,sizeof big);if(n<0) { CHECK(n==-28);break; }CHECK(n>0);total+=n; }
    CHECK(total==65536 && C1(57,fd)==0 && C3(35,AT,"/s/f",0)==0 && put("/s/g","ok")==2);
    /* inotify on a memfs directory */
    { CHECK(C3(34,AT,"/m/w",0755)==0);long in=C1(26,04000);CHECK(in>=0);
      long wd=C3(27,in,"/m/w",0x100|0x200|0x2|0x8|0x40|0x80);CHECK(wd>=0);   /* CREATE DELETE MODIFY CLOSE_WRITE MOVED_FROM MOVED_TO */
      CHECK(put("/m/w/f","data")==4 && C4(38,AT,"/m/w/f",AT,"/m/w/g")==0 && C3(35,AT,"/m/w/g",0)==0);
      unsigned char ev[1024];long n=C3(63,in,ev,sizeof ev);CHECK(n>0);
      static const unsigned masks[]={0x100,0x2,0x8,0x40,0x80,0x200};static const char names[]="ffffgg";unsigned k=0,cookie=0;
      for(long at=0;at<n;k++) { CHECK(k<6 && u32(ev+at)==(unsigned)wd && u32(ev+at+4)==masks[k] && ev[at+16]==names[k] && !ev[at+17]);
          if(k==3)cookie=u32(ev+at+8);if(k==4)CHECK(cookie && u32(ev+at+8)==cookie);
          at+=16+u32(ev+at+12); }
      CHECK(k==6 && C3(63,in,ev,sizeof ev)==-11 && C2(28,in,wd)==0 && C1(57,in)==0); }
    /* AF_UNIX sockets bound in memfs */
    { struct { unsigned short family;char path[108]; } addr={1,"/m/sock"};
      long s=C3(198,1,1,0),c=C3(198,1,1,0);CHECK(s>=0 && c>=0 && C3(200,s,&addr,10)==0 && C2(201,s,4)==0);
      CHECK(stat_of("/m/sock")==0 && (ST_MODE&0170000)==0140000);
      long t=C3(198,1,1,0);CHECK(C3(200,t,&addr,10)==-98 && C1(57,t)==0);   /* EADDRINUSE */
      CHECK(C3(203,c,&addr,10)==0);long a=C3(202,s,0,0);CHECK(a>=0 && C3(64,c,"u",1)==1 && C3(63,a,buffer,1)==1 && buffer[0]=='u');
      CHECK(C3(35,AT,"/m/sock",0)==0);t=C3(198,1,1,0);CHECK(C3(203,t,&addr,10)==-2);
      C1(57,t);C1(57,a);C1(57,c);C1(57,s); }
    /* permissions as uid 1000: modes, the sticky bit */
    CHECK(put("/m/root-only","secret")==6 && C3(53,AT,"/m/root-only",0600)==0 && C3(34,AT,"/m/mine",0700)==0);
    CHECK(C3(149,1000,1000,1000)==0 && C3(147,1000,1000,1000)==0);           /* setresgid, setresuid */
    CHECK(open3("/m/root-only",0,0)==-13 && C3(35,AT,"/m/root-only",0)==-1);  /* not ours, sticky /m */
    CHECK(put("/m/user","u")==1 && stat_of("/m/user")==0 && ST_UID==1000 && C3(35,AT,"/m/user",0)==0);
    CHECK(open3("/m/mine/f",0101,0644)==-13 && C1(49,"/m/mine")==-13 && C3(53,AT,"/m/a2",0777)==0); /* a2 is uid 1000's */
    say("memfs PASS\n");C1(94,0);for(;;){}
}
