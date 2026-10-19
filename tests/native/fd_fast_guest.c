/* The hypervisor backend answers fcntl F_GETFD, F_SETFD, F_GETFL, F_SETFL and
   epoll_ctl of files in its EL1 vector, from a per-process descriptor page the host
   keeps (fd_mirror). Its answers must be the host's: a forked child under an
   allow-all seccomp filter (which turns the vector's answers off) replays the same
   queries on its copies of the descriptors as the reference. Then the answers stay
   right through dup, dup2, dup3, F_DUPFD_CLOEXEC, close, close_range, ioctl FIOCLEX,
   fork, a vfork child running on its threaded parent's page tables, SCM_RIGHTS,
   threads racing dup3 and close over the queried number, many threads opening,
   querying, reading and closing, exec (close-on-exec), and O_APPEND changed on a
   description another descriptor shares. The vector must also really answer: an
   F_GETFD costs a fraction of getppid, which always exits. Needs /fd_fast (this
   program) in a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
__asm__(".global _start\n_start:\n    mov x0, sp\n    bl start\n");
#define AT (U)-100
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
#define RDONLY 0x20000UL                 /* F_GETFL of a file opened O_RDONLY: O_LARGEFILE */
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void fail(unsigned line) {
    char m[40]="fd-fast FAIL line ";unsigned n=18;char d[8];unsigned k=0;
    do { d[k++]=(char)('0'+line%10);line/=10; } while(line);
    while(k)m[n++]=d[--k];
    m[n++]='\n';call(64,1,(U)m,n,0,0,0);call(94,1,0,0,0,0,0);for(;;){}
}
#define CHECK(x) do { if(!(x))fail(__LINE__); } while(0)
static long fcntl(U fd,U cmd,U arg) { return call(25,fd,cmd,arg,0,0,0); }
static long epoll_ctl(U ep,U op,U fd,void *event) { return call(21,ep,op,fd,(U)event,0,0); }
static U ticks(void) { U t;__asm__ volatile("isb\nmrs %0, cntvct_el0":"=r"(t));return t; }
static void join_thread(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,128,(U)v,0,0,0); }
static unsigned char stacks[8][65536] __attribute__((aligned(16)));
static int done_word[8];
static long a,b,c,d,ep,pipes[2];        /* RDONLY|CLOEXEC, RDWR, WRONLY|CLOEXEC files; a directory; an epoll; a pipe */

/* Every query on every kind of number; only calls that change nothing (F_SETFD and
   F_SETFL to what they are, epoll_ctl that fails). */
#define QUERIES 12
#define TARGETS 10
static void snapshot(long r[TARGETS][QUERIES]) {
    const U targets[TARGETS]={(U)a,(U)b,(U)c,(U)d,(U)ep,(U)pipes[0],1000,4095,4096,(U)1<<40};
    unsigned char event[16]={1};
    for(unsigned t=0;t<TARGETS;t++) {
        U fd=targets[t];int file=t<4,other=t==4 || t==5;
        for(unsigned q=0;q<QUERIES;q++)r[t][q]=0x7777;
        r[t][0]=fcntl(fd,1,0);r[t][1]=fcntl(fd,3,0);
        if(!other) {
            r[t][2]=fcntl(fd,2,file?(U)r[t][0]:0);r[t][3]=fcntl(fd,4,file?(U)r[t][1]|0x800:0x800);r[t][4]=fcntl(fd,3,0);
            r[t][5]=fcntl(fd,2,2);r[t][6]=fcntl(fd,1|(U)1<<32,0);
            r[t][7]=epoll_ctl((U)ep,1,fd,event);r[t][9]=epoll_ctl((U)ep,0x100000002UL,fd,event);r[t][11]=epoll_ctl((U)ep,1,fd,0);
            r[t][10]=epoll_ctl((U)a,1,fd,event);                       /* a is no epoll */
        }
        r[t][8]=epoll_ctl((U)ep,0,fd,event);
    }
}
static void compare_with_host(void) {
    static long fast[TARGETS][QUERIES],host[TARGETS][QUERIES];
    snapshot(fast);
    int p[2];CHECK(call(59,(U)p,0,0,0,0,0)==0);
    long pid=call(220,17,0,0,0,0,0);CHECK(pid>=0);
    if(!pid) {
        static const U filter[1]={0x7fff000000000006UL};                  /* ret ALLOW */
        U program[2]={1,(U)filter};
        if(call(167,38,1,0,0,0,0) || call(277,1,0,(U)program,0,0,0))call(94,5,0,0,0,0,0);
        snapshot(host);
        call(64,(U)p[1],(U)host,sizeof(host),0,0,0);call(94,0,0,0,0,0,0);
    }
    unsigned char *to=(unsigned char *)host;U got=0;
    while(got<sizeof(host)) { long n=call(63,(U)p[0],(U)(to+got),sizeof(host)-got,0,0,0);CHECK(n>0);got+=(U)n; }
    int status=-1;CHECK(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    call(57,(U)p[0],0,0,0,0,0);call(57,(U)p[1],0,0,0,0,0);
    for(unsigned t=0;t<TARGETS;t++)for(unsigned q=0;q<QUERIES;q++)if(fast[t][q]!=host[t][q])fail(1000+t*100+q);
    /* and what they are */
    CHECK(fast[0][0]==1 && fast[0][1]==(long)RDONLY && fast[1][0]==0 && fast[1][1]==(long)(RDONLY|2) && fast[2][1]==(long)(RDONLY|1));
    CHECK(fast[0][2]==0 && fast[0][3]==0 && fast[0][4]==(long)RDONLY && fast[0][5]==-22 && fast[0][7]==-1 && fast[0][8]==-22 && fast[0][9]==-1);
    CHECK(fast[3][0]==0 && fast[3][1]==(long)RDONLY && fast[3][7]==-1 && fast[6][0]==-9 && fast[6][7]==-9 && fast[9][1]==-9);
}
/* The vector really answers: F_GETFD at a fraction of getppid's cost. */
static void answered_in_guest(void) {
    U best_fast=~0UL,best_exit=~0UL;
    for(int round=0;round<5;round++) {
        U t0=ticks();for(int i=0;i<2000;i++)fcntl((U)a,1,0);
        U t1=ticks();for(int i=0;i<2000;i++)call(173,0,0,0,0,0,0);
        U t2=ticks();
        if(t1-t0<best_fast)best_fast=t1-t0;
        if(t2-t1<best_exit)best_exit=t2-t1;
    }
    CHECK(best_fast*3<best_exit);
}
static void tables(void) {
    long x=call(23,(U)a,0,0,0,0,0);CHECK(x>=0 && fcntl((U)x,1,0)==0 && fcntl((U)x,3,0)==(long)RDONLY);   /* dup: no close-on-exec */
    CHECK(call(24,(U)b,50,0x80000,0,0,0)==50 && fcntl(50,1,0)==1 && fcntl(50,3,0)==(long)(RDONLY|2));     /* dup3 O_CLOEXEC */
    CHECK(call(24,(U)c,50,0,0,0,0)==50 && fcntl(50,1,0)==0 && fcntl(50,3,0)==(long)(RDONLY|1));            /* over it */
    CHECK(fcntl(50,2,1)==0 && fcntl(50,1,0)==1 && fcntl(50,2,0)==0 && fcntl(50,1,0)==0);                     /* the host's F_SETFD */
    CHECK(call(29,50,0x5451,0,0,0,0)==0 && fcntl(50,1,0)==1 && call(29,50,0x5450,0,0,0,0)==0 && fcntl(50,1,0)==0); /* FIOCLEX, FIONCLEX */
    long y=fcntl(50,1030,60);CHECK(y==60 && fcntl(60,1,0)==1 && fcntl(60,3,0)==(long)(RDONLY|1));          /* F_DUPFD_CLOEXEC */
    CHECK(call(436,50,60,4,0,0,0)==0 && fcntl(50,1,0)==1 && fcntl(60,1,0)==1);                              /* close_range CLOEXEC */
    unsigned char event[16]={1};
    CHECK(call(57,50,0,0,0,0,0)==0 && fcntl(50,1,0)==-9 && fcntl(50,3,0)==-9 && fcntl(50,2,0)==-9 && fcntl(50,4,0)==-9 && epoll_ctl((U)ep,1,50,event)==-9);
    CHECK(call(436,60,60,0,0,0,0)==0 && fcntl(60,1,0)==-9 && call(57,(U)x,0,0,0,0,0)==0 && fcntl((U)x,3,0)==-9);
    long sock[2];int pair[2];CHECK(call(199,1,1,0,(U)pair,0,0)==0);sock[0]=pair[0];sock[1]=pair[1];
    CHECK(epoll_ctl((U)ep,1,(U)sock[0],event)==0 && epoll_ctl((U)ep,2,(U)sock[0],event)==0);                /* a socket: the host's */
    /* SCM_RIGHTS: received with and without MSG_CMSG_CLOEXEC */
    for(int cloexec=0;cloexec<2;cloexec++) {
        U cmsg[3]={20,1|((U)1<<32),(U)b};U io[2]={(U)"x",1},msg[7]={0,0,(U)io,1,(U)cmsg,24,0};
        CHECK(call(211,(U)sock[0],(U)msg,0,0,0,0)==1);
        U back[3]={0,0,0};char ch=0;U io2[2]={(U)&ch,1},msg2[7]={0,0,(U)io2,1,(U)back,24,0};
        CHECK(call(212,(U)sock[1],(U)msg2,cloexec?0x40000000UL:0,0,0,0)==1 && ch=='x');
        long r=(long)(unsigned)back[2];
        CHECK(fcntl((U)r,1,0)==cloexec && fcntl((U)r,3,0)==(long)(RDONLY|2) && epoll_ctl((U)ep,1,(U)r,event)==-1 && call(57,(U)r,0,0,0,0,0)==0);
    }
    call(57,(U)sock[0],0,0,0,0,0);call(57,(U)sock[1],0,0,0,0,0);
    /* fork: the child's own table, a copy */
    long pid=call(220,17,0,0,0,0,0);CHECK(pid>=0);
    if(!pid) {
        if(fcntl((U)a,1,0)!=1 || fcntl((U)b,3,0)!=(long)(RDONLY|2) || epoll_ctl((U)ep,1,(U)c,event)!=-1)call(94,2,0,0,0,0,0);
        if(fcntl((U)a,2,0) || fcntl((U)a,1,0)!=0 || call(57,(U)b,0,0,0,0,0) || fcntl((U)b,1,0)!=-9)call(94,3,0,0,0,0,0);
        if(call(24,(U)c,(U)a,0,0,0,0)!=a || fcntl((U)a,3,0)!=(long)(RDONLY|1))call(94,4,0,0,0,0,0);
        call(94,0,0,0,0,0,0);
    }
    int status=-1;CHECK(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    CHECK(fcntl((U)a,1,0)==1 && fcntl((U)a,3,0)==(long)RDONLY && fcntl((U)b,1,0)==0 && fcntl((U)b,3,0)==(long)(RDONLY|2));
}
/* A vfork child of a threaded process runs on its parent's page tables (and so its
   descriptor page): its answers must still be its own table's. */
static volatile int parked=1;
static void parker(void *arg) { (void)arg;while(__atomic_load_n(&parked,__ATOMIC_ACQUIRE))call(98,(U)&parked,128,1,0,0,0); }
static void vfork_child(void) {
    CHECK(spawn(FLAGS,stacks[7]+sizeof(stacks[7]),&done_word[7],0,&done_word[7],parker,0)>0);
    long pid=call(220,0x4111,0,0,0,0,0);                                  /* CLONE_VM|CLONE_VFORK|SIGCHLD */
    if(!pid) {
        int bad=fcntl((U)a,1,0)!=1 || fcntl((U)a,2,0)!=0 || fcntl((U)a,1,0)!=0 || call(24,(U)c,(U)b,0,0,0,0)!=b || fcntl((U)b,3,0)!=(long)(RDONLY|1);
        call(93,bad?6:0,0,0,0,0,0);
    }
    int status=-1;CHECK(pid>0 && call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    CHECK(fcntl((U)a,1,0)==1 && fcntl((U)b,3,0)==(long)(RDONLY|2));
    __atomic_store_n(&parked,0,__ATOMIC_RELEASE);call(98,(U)&parked,129,1,0,0,0);join_thread(&done_word[7]);
}
/* Threads query one number while another dup3s two different files over it (never
   EBADF: dup3 replaces atomically), then while it opens and closes it. */
#define X 77
static volatile int racing;
static long bad_answers;
static void flipper(void *arg) {
    (void)arg;
    for(int i=0;i<20000;i++)CHECK(call(24,(U)(i&1?c:a),X,i&1?0:0x80000,0,0,0)==X);  /* a with, c without close-on-exec */
    __atomic_store_n(&racing,0,__ATOMIC_RELEASE);
}
static void opener(void *arg) {
    (void)arg;
    for(int i=0;i<4000;i++) {
        long fd=call(56,AT,(U)"/f",0x80000,0,0,0);CHECK(fd>=0);             /* O_CLOEXEC */
        if(fd!=X) { CHECK(call(24,(U)fd,X,0x80000,0,0,0)==X);call(57,(U)fd,0,0,0,0,0); }
        CHECK(call(57,X,0,0,0,0,0)==0);
    }
    __atomic_store_n(&racing,0,__ATOMIC_RELEASE);
}
static void querier(void *arg) {
    U open_close=(U)arg;unsigned char event[16]={1};
    while(__atomic_load_n(&racing,__ATOMIC_ACQUIRE)) {
        long fd=fcntl(X,1,0),fl=fcntl(X,3,0),e=epoll_ctl((U)ep,1,X,event);
        int ok=open_close?(fd==1 || fd==-9) && (fl==(long)RDONLY || fl==-9) && (e==-1 || e==-9)
                         :(fd==0 || fd==1) && (fl==(long)RDONLY || fl==(long)(RDONLY|1)) && e==-1;
        if(!ok)__atomic_add_fetch(&bad_answers,1,__ATOMIC_RELAXED);
    }
}
static void race(void (*changer)(void *),U open_close) {
    racing=1;
    if(!open_close)CHECK(call(24,(U)a,X,0x80000,0,0,0)==X);              /* there from the start */
    for(int q=0;q<3;q++)CHECK(spawn(FLAGS,stacks[q]+sizeof(stacks[q]),&done_word[q],0,&done_word[q],querier,(void *)open_close)>0);
    CHECK(spawn(FLAGS,stacks[3]+sizeof(stacks[3]),&done_word[3],0,&done_word[3],changer,0)>0);
    for(int q=0;q<4;q++)join_thread(&done_word[q]);
    CHECK(bad_answers==0);
    call(57,X,0,0,0,0,0);
}
/* Many threads: open, query, read, change close-on-exec, close. */
static void stress_worker(void *arg) {
    U id=(U)arg;unsigned char event[16]={1};char buf[8];
    for(int i=0;i<600;i++) {
        int rw=(i+(int)id)&1;
        long fd=call(56,AT,(U)"/f",rw?2:0x80000,0,0,0);CHECK(fd>=0);
        CHECK(fcntl((U)fd,1,0)==!rw && fcntl((U)fd,3,0)==(long)(RDONLY|(rw?2:0)));
        CHECK(fcntl((U)fd,4,(U)fcntl((U)fd,3,0)|0x800)==0 && fcntl((U)fd,3,0)==(long)(RDONLY|(rw?2:0)));  /* Go's SetNonblock */
        CHECK(epoll_ctl((U)ep,1,(U)fd,event)==-1);
        CHECK(call(67,(U)fd,(U)buf,5,0,0,0)==5 && buf[0]=='h' && buf[4]=='o');
        CHECK(fcntl((U)fd,2,(U)rw)==0 && fcntl((U)fd,1,0)==rw && fcntl((U)fd,2,(U)rw)==0);
        CHECK(call(57,(U)fd,0,0,0,0,0)==0);
    }
}
static void stress(void) {
    for(U w=0;w<6;w++)CHECK(spawn(FLAGS,stacks[w]+sizeof(stacks[w]),&done_word[w],0,&done_word[w],stress_worker,(void *)w)>0);
    for(int w=0;w<6;w++)join_thread(&done_word[w]);
}
void start(U *sp) {
    char **argv=(char **)(sp+1);unsigned char event[16]={1};
    if(sp[0]>=4 && argv[1][0]=='l') {                                     /* exec'd: a closed, b kept */
        long ea=0,eb=0;for(char *p=argv[2];*p;p++)ea=ea*10+(*p-'0');for(char *p=argv[3];*p;p++)eb=eb*10+(*p-'0');
        CHECK(fcntl((U)ea,1,0)==-9 && fcntl((U)ea,3,0)==-9 && fcntl((U)eb,1,0)==0 && fcntl((U)eb,3,0)==(long)(RDONLY|2));
        long e=call(20,0,0,0,0,0,0);CHECK(e>=0 && epoll_ctl((U)e,1,(U)eb,event)==-1);
        /* O_APPEND set on b's description: its dup sees it, also in a child (no stale answer) */
        long bb=call(23,(U)eb,0,0,0,0,0);CHECK(bb>=0 && fcntl((U)bb,3,0)==(long)(RDONLY|2));
        CHECK(fcntl((U)eb,4,(RDONLY|2|0x400))==0 && fcntl((U)bb,3,0)==(long)(RDONLY|2|0x400) && fcntl((U)eb,3,0)==(long)(RDONLY|2|0x400));
        long pid=call(220,17,0,0,0,0,0);
        if(!pid) { if(fcntl((U)bb,3,0)!=(long)(RDONLY|2|0x400) || fcntl((U)bb,4,RDONLY|2) || fcntl((U)eb,3,0)!=(long)(RDONLY|2))call(94,7,0,0,0,0,0);call(94,0,0,0,0,0,0); }
        int status=-1;CHECK(pid>0 && call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
        CHECK(fcntl((U)bb,3,0)==(long)(RDONLY|2) && fcntl((U)eb,4,RDONLY|2)==0 && fcntl((U)eb,1,0)==0);
        out("fd-fast PASS\n");call(94,0,0,0,0,0,0);
    }
    long f=call(56,AT,(U)"/f",0102|01000,0644,0,0);CHECK(f>=0 && call(64,(U)f,(U)"hello",5,0,0,0)==5 && call(57,(U)f,0,0,0,0,0)==0);
    a=call(56,AT,(U)"/f",0x80000,0,0,0);b=call(56,AT,(U)"/f",2,0,0,0);c=call(56,AT,(U)"/f",1|0x80000,0,0,0);
    d=call(56,AT,(U)"/",0x10000,0,0,0);ep=call(20,0,0,0,0,0,0);
    CHECK(a>=0 && b>=0 && c>=0 && d>=0 && ep>=0);
    int p2[2];CHECK(call(59,(U)p2,0,0,0,0,0)==0);pipes[0]=p2[0];pipes[1]=p2[1];
    compare_with_host();
    answered_in_guest();
    tables();
    vfork_child();
    race(flipper,0);
    race(opener,1);
    stress();
    compare_with_host();
    char na[8],nb[8];char *p=na;long v=a;char t[8];int k=0;do { t[k++]=(char)('0'+v%10);v/=10; } while(v);while(k)*p++=t[--k];*p=0;
    p=nb;v=b;do { t[k++]=(char)('0'+v%10);v/=10; } while(v);while(k)*p++=t[--k];*p=0;
    const char *args[]={"/fd_fast","leaf",na,nb,0};
    call(221,(U)"/fd_fast",(U)args,0,0,0,0);
    fail(__LINE__);
}
