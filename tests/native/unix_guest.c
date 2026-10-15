/* AF_UNIX sockets on guest paths: bind/listen/connect/accept, names, a socket file
   that stat reports, EADDRINUSE, unlink, a host path beyond macOS's 104-byte
   sun_path limit, socketpair (stream and datagram), SO_TYPE/SO_PEERCRED, and
   abstract names (Linux's leading NUL): bind/listen/connect from a child process,
   names that differ only in length or bytes, getsockname/getpeername, EADDRINUSE,
   ECONNREFUSED, the name freed on close, autobind and datagrams. Needs a writable
   root and --process-tree. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"unix FAIL\n",10,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static unsigned set(unsigned char *a,const char *path) {             /* sockaddr_un; returns its length */
    unsigned n=0;a[0]=1;a[1]=0;while(path[n]) { a[2+n]=(unsigned char)path[n];n++; }
    a[2+n]=0;return n+3;
}
static int same(const unsigned char *a,const char *b) { while(*b && *a==(unsigned char)*b) { a++;b++; }return !*b && !*a; }
static void roundtrip(const char *path) {
    unsigned char addr[110],name[110];unsigned length=set(addr,path),size=110;char buf[8];
    long server=call(198,1,1,0,0,0,0),client=call(198,1,1,0,0,0,0);check(server>=0 && client>=0);
    check(call(200,server,(U)addr,length,0,0,0)==0 && call(201,server,8,0,0,0,0)==0);
    check(call(203,client,(U)addr,length,0,0,0)==0);
    long peer=call(202,server,(U)name,(U)&size,0,0,0);check(peer>=0 && size==2 && name[0]==1);
    check(call(64,client,(U)"hi",2,0,0,0)==2 && call(63,peer,(U)buf,8,0,0,0)==2 && buf[0]=='h');
    size=110;check(call(204,server,(U)name,(U)&size,0,0,0)==0 && same(name+2,path));
    size=110;check(call(205,client,(U)name,(U)&size,0,0,0)==0 && same(name+2,path));
    long again=call(198,1,1,0,0,0,0);check(call(200,again,(U)addr,length,0,0,0)==-98);   /* EADDRINUSE */
    check(call(53,(U)-100,(U)path,0700,0,0,0)==0);                                    /* chmod */
    unsigned char st[128];check(call(79,(U)-100,(U)path,(U)st,0,0,0)==0 && *(unsigned *)(st+16)==0140700);
    check(call(35,(U)-100,(U)path,0,0,0,0)==0);                                      /* unlink */
    check(call(203,again,(U)addr,length,0,0,0)==-2);                                 /* ENOENT */
    for(long fd=server;fd<=again;fd++)call(57,(U)fd,0,0,0,0,0);
}
static int equal(const unsigned char *a,const unsigned char *b,unsigned n) { for(unsigned i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1; }
static void abstract_names(void) {
    unsigned char name[16]={1,0,0,'d','b','u','s',0,'x'},other[16]={1,0,0,'d','b','u','s'},out[110];
    unsigned size;char buf[8];                     /* name: "\0dbus\0x" (9 bytes); other: "\0dbus" (7) */
    long server=call(198,1,1,0,0,0,0);check(server>=0);
    check(call(200,server,(U)name,9,0,0,0)==0 && call(201,server,8,0,0,0,0)==0);
    size=110;check(call(204,server,(U)out,(U)&size,0,0,0)==0 && size==9 && equal(out,name,9));
    long dup=call(198,1,1,0,0,0,0);check(call(200,dup,(U)name,9,0,0,0)==-98);       /* EADDRINUSE */
    long miss=call(198,1,1,0,0,0,0);check(call(203,miss,(U)other,7,0,0,0)==-111);  /* the shorter name is another one */
    long pid=call(220,17,0,0,0,0,0);
    if(!pid) {                                                                     /* another process connects */
        long client=call(198,1,1,0,0,0,0);
        check(call(203,client,(U)name,9,0,0,0)==0 && call(64,client,(U)"ab",2,0,0,0)==2);
        size=110;check(call(205,client,(U)out,(U)&size,0,0,0)==0 && size==9 && equal(out,name,9));
        call(94,0,0,0,0,0,0);
    }
    long peer=call(202,server,0,0,0,0,0);check(peer>=0 && call(63,peer,(U)buf,8,0,0,0)==2 && buf[1]=='b');
    int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    call(57,(U)peer,0,0,0,0,0);call(57,(U)server,0,0,0,0,0);
    check(call(200,dup,(U)name,9,0,0,0)==0);                                       /* closing freed the name */
    long auto_=call(198,1,2,0,0,0,0);check(call(200,auto_,(U)name,2,0,0,0)==0);    /* autobind: "\0" and five hex digits */
    size=110;check(call(204,auto_,(U)out,(U)&size,0,0,0)==0 && size==8 && out[2]==0 && out[3]>='0');
    long sender=call(198,1,2,0,0,0,0);
    check(call(203,sender,(U)out,8,0,0,0)==0 && call(64,sender,(U)"dg",2,0,0,0)==2 && call(63,auto_,(U)buf,8,0,0,0)==2 && buf[0]=='d');
    for(long fd=dup;fd<=sender;fd++)call(57,(U)fd,0,0,0,0,0);
    /* FEXServer's pattern: "\00.FEXServer.Socket", a client process that sends a
       descriptor (SCM_RIGHTS) over it, and the server reading through that descriptor. */
    static const char fex[]="0.FEXServer.Socket";unsigned char addr[40]={1,0,0};unsigned n=0;
    while(fex[n]) { addr[3+n]=(unsigned char)fex[n];n++; }
    long fs=call(198,1,0x80001,0,0,0,0);check(fs>=0);                             /* SOCK_STREAM|SOCK_CLOEXEC */
    check(call(200,(U)fs,(U)addr,3+n,0,0,0)==0 && call(201,(U)fs,16,0,0,0,0)==0);
    size=110;check(call(204,(U)fs,(U)out,(U)&size,0,0,0)==0 && size==3+n && equal(out,addr,3+n));
    pid=call(220,17,0,0,0,0,0);
    if(!pid) {
        int pair[2];check(call(199,1,1,0,(U)pair,0,0)==0 && call(64,(U)pair[1],(U)"fd",2,0,0,0)==2);
        long c=call(198,1,1,0,0,0,0);check(call(203,(U)c,(U)addr,3+n,0,0,0)==0);
        U cmsg[3]={20,1|((U)1<<32),(U)pair[0]};                                   /* len 20, SOL_SOCKET, SCM_RIGHTS, the fd */
        U io[2]={(U)"x",1},msg[7]={0,0,(U)io,1,(U)cmsg,24,0};
        check(call(211,(U)c,(U)msg,0,0,0,0)==1);
        call(94,0,0,0,0,0,0);
    }
    long peer2=call(202,(U)fs,0,0,0,0,0);check(peer2>=0);
    U cmsg[3]={0,0,0};char x=0;U io[2]={(U)&x,1},msg[7]={0,0,(U)io,1,(U)cmsg,24,0};
    check(call(212,(U)peer2,(U)msg,0,0,0,0)==1 && x=='x' && (unsigned)cmsg[1]==1 && (unsigned)(cmsg[1]>>32)==1);
    char got[4]={0};check(call(63,(U)(unsigned)cmsg[2],(U)got,4,0,0,0)==2 && got[0]=='f');   /* the passed descriptor */
    status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    call(57,(U)peer2,0,0,0,0,0);call(57,(U)fs,0,0,0,0,0);
}
void _start(void) {
    roundtrip("/test.sock");
    static const char deep[]="/deep/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    check(call(34,(U)-100,(U)"/deep",0755,0,0,0)==0 && call(34,(U)-100,(U)deep,0755,0,0,0)==0);
    char path[100];unsigned n=0;while(deep[n]) { path[n]=deep[n];n++; }
    path[n++]='/';path[n++]='s';path[n]=0;
    roundtrip(path);                                                                 /* host path > 104 bytes */
    int pair[2];char buf[8];
    check(call(199,1,1,0,(U)pair,0,0)==0 && call(64,(U)pair[0],(U)"ok",2,0,0,0)==2 && call(63,(U)pair[1],(U)buf,8,0,0,0)==2 && buf[1]=='k');
    int type=0;unsigned size=4;check(call(209,(U)pair[0],1,3,(U)&type,(U)&size,0)==0 && type==1);
    unsigned cred[3];size=12;check(call(209,(U)pair[0],1,17,(U)cred,(U)&size,0)==0 && size==12 && cred[0]>0);
    int dgram[2];check(call(199,1,2,0,(U)dgram,0,0)==0 && call(64,(U)dgram[0],(U)"d",1,0,0,0)==1 && call(63,(U)dgram[1],(U)buf,8,0,0,0)==1);
    abstract_names();
    check(call(198,1,5,0,0,0,0)==-94);                                               /* no SOCK_SEQPACKET */
    call(64,1,(U)"unix PASS\n",10,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
