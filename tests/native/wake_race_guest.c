/* Wake-ups under load, checked for lost and late ones (run with --signals):
   ring: 8 threads pass 3 tokens around a ring whose links are a futex count, an
     eventfd under epoll_pwait, a pipe under ppoll, a blocking pipe read and a blocking
     eventfd read; a watchdog fails the run if the tokens stop moving (a lost wakeup).
   procs: 4 processes pass tokens around rings of shared futexes (MAP_SHARED across
     fork) and of pipes, watched the same way.
   bits: FUTEX_WAKE_BITSET wakes only matching waiters; CMP_REQUEUE moves (or wakes)
     a waiter and checks its value; a timed wait times out on time.
   signals: a thread waiting in a futex, epoll_pwait or ppoll returns EINTR at once
     when a handler's signal comes, early in its wait (in place) and late (parked);
     a blocking read with SA_RESTART resumes.
   exit: a process whose threads all wait (futex, epoll, ppoll, read) forks, its
     child exits, then it exits from its main thread, at once. */
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
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void num(U v) { char b[24];int i=23;b[i]=0;do { b[--i]=(char)('0'+v%10);v/=10; } while(v);out(b+i); }
static void fail(const char *s) { out("wake-race FAIL ");out(s);out("\n");call(94,1,0,0,0,0,0);for(;;){} }
static U ms(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000+ts[1]/1000000; }
static void sleep_ms(U n) { U t[2]={n/1000,(n%1000)*1000000};call(101,(U)t,0,0,0,0,0); }
static void add(volatile int *p,int v) { int value;unsigned failed;
    __asm__ volatile("1: ldaxr %w0,[%2]\n add %w0,%w0,%w3\n stlxr %w1,%w0,[%2]\n cbnz %w1,1b":"=&r"(value),"=&r"(failed):"r"(p),"r"(v):"memory"); }
static int take(volatile int *p) {        /* decrement if positive; 1 if taken */
    int value;unsigned failed;
    for(;;) {
        __asm__ volatile("ldaxr %w0,[%1]":"=&r"(value):"r"(p):"memory");
        if(value<=0) { __asm__ volatile("clrex":::"memory");return 0; }
        __asm__ volatile("stlxr %w0,%w2,[%1]":"=&r"(failed):"r"(p),"r"(value-1):"memory");
        if(!failed)return 1;
    }
}
static unsigned char stacks[16][32768] __attribute__((aligned(16)));
static int tids[16];
/* ---- ring of threads ---- */
#define RING 8
#define TOKENS 3
#define HOPS 24000
static volatile int fword[RING],progress,done;
static int efd[RING],epfd[RING],pfd[RING][2];
static void link_wait(int i) {
    int kind=i%5;U v;char c;
    if(kind==0) { while(!take(&fword[i]))call(98,(U)&fword[i],128,0,0,0,0);return; }
    if(kind==1) { unsigned char ev[16];for(;;) { if(call(63,(U)efd[i],(U)&v,8,0,0,0)==8) { if(v>1) { U back=v-1;call(64,(U)efd[i],(U)&back,8,0,0,0); } return; }
                  long n=call(22,(U)epfd[i],(U)ev,1,(U)-1,0,8);if(n<0 && n!=-4)fail("epoll_pwait"); } }
    if(kind==2) { struct { int fd;short events,revents; } p={pfd[i][0],1,0};
                  for(;;) { if(call(63,(U)pfd[i][0],(U)&c,1,0,0,0)==1)return;long n=call(73,(U)&p,1,0,0,0,0);if(n<0 && n!=-4)fail("ppoll"); } }
    if(kind==3) { long n;while((n=call(63,(U)pfd[i][0],(U)&c,1,0,0,0))==-4) {} if(n!=1)fail("pipe read");return; }
    long n;while((n=call(63,(U)efd[i],(U)&v,8,0,0,0))==-4) {} if(n!=8)fail("eventfd read");
    if(v>1) { U back=v-1;call(64,(U)efd[i],(U)&back,8,0,0,0); }
}
static void link_post(int i) {
    int kind=i%5;U one=1;
    if(kind==0) { add(&fword[i],1);call(98,(U)&fword[i],129,1,0,0,0); }
    else if(kind==1 || kind==4) { if(call(64,(U)efd[i],(U)&one,8,0,0,0)!=8)fail("eventfd write"); }
    else if(call(64,(U)pfd[i][1],(U)"t",1,0,0,0)!=1)fail("pipe write");
}
static void ring_member(void *arg) {
    int i=(int)(U)arg;
    for(;;) { link_wait(i);if(done)return;add(&progress,1);link_post((i+1)%RING); }
}
static void watch(volatile int *count,int target,const char *what) {
    int last=-1;U since=ms();
    while(*count<target) {
        sleep_ms(20);
        if(*count!=last) { last=*count;since=ms(); }
        else if(ms()-since>5000) { out("stalled at ");num((U)last);out(" of ");num((U)target);out(": ");fail(what); }
    }
}
static void ring(void) {
    for(int i=0;i<RING;i++) {
        int kind=i%5;
        if(kind==1 || kind==4) {
            efd[i]=(int)call(19,0,kind==1?2048:0,0,0,0,0);if(efd[i]<0)fail("eventfd");   /* EFD_NONBLOCK under epoll */
            if(kind==1) { epfd[i]=(int)call(20,0,0,0,0,0,0);U ev[2]={1,(U)i};if(epfd[i]<0 || call(21,(U)epfd[i],1,(U)efd[i],(U)ev,0,0))fail("epoll"); }
        }
        if(kind==2 || kind==3)if(call(59,(U)pfd[i],kind==2?2048:0,0,0,0,0))fail("pipe2");
    }
    for(int i=0;i<RING;i++)if(spawn(FLAGS,stacks[i]+sizeof(stacks[0]),&tids[i],0,&tids[i],ring_member,(void *)(U)i)<=0)fail("spawn");
    U start=ms();
    for(int t=0;t<TOKENS;t++)link_post(t*3%RING);
    watch(&progress,HOPS,"ring of threads lost a wakeup");
    done=1;for(int i=0;i<RING;i++)link_post(i);
    out("ring hops=");num((U)progress);out(" ms=");num(ms()-start);out("\n");
}
/* ---- ring of processes ---- */
#define PROCS 4
#define PROC_HOPS 8000
struct shared { volatile int word[PROCS],progress,stop; };
static void proc_member(struct shared *s,int i,int pipes[PROCS][2]) {
    for(;;) {
        /* the futex ring first, then the pipe ring: each process has one inbound link of each */
        while(!take(&s->word[i])) { if(s->stop)call(93,0,0,0,0,0,0);call(98,(U)&s->word[i],0,0,0,0,0); }  /* shared futex (no PRIVATE flag) */
        if(s->stop)call(93,0,0,0,0,0,0);
        char c;long n;while((n=call(63,(U)pipes[i][0],(U)&c,1,0,0,0))==-4) {}
        if(n!=1)call(93,2,0,0,0,0,0);
        add(&s->progress,1);
        int next=(i+1)%PROCS;add(&s->word[next],1);call(98,(U)&s->word[next],1,1,0,0,0);
        if(call(64,(U)pipes[next][1],(U)"p",1,0,0,0)!=1)call(93,3,0,0,0,0,0);
    }
}
static void procs(void) {
    struct shared *s=(struct shared *)call(222,0,4096,3,0x21,(U)-1,0);if((long)s<0)fail("mmap shared");
    static int pipes[PROCS][2];
    for(int i=0;i<PROCS;i++)if(call(59,(U)pipes[i],0,0,0,0,0))fail("pipe2");
    long pids[PROCS];
    for(int i=0;i<PROCS;i++) {
        pids[i]=call(220,17,0,0,0,0,0);if(pids[i]<0)fail("fork");
        if(!pids[i])proc_member(s,i,pipes);
    }
    U start=ms();
    for(int t=0;t<2;t++) { add(&s->word[t*2],1);call(98,(U)&s->word[t*2],1,1,0,0,0);call(64,(U)pipes[t*2][1],(U)"p",1,0,0,0); }
    watch(&s->progress,PROC_HOPS,"ring of processes lost a wakeup");
    s->stop=1;
    for(int i=0;i<PROCS;i++) { add(&s->word[i],1);call(98,(U)&s->word[i],1,1,0,0,0);call(64,(U)pipes[i][1],(U)"p",1,0,0,0); }
    for(int i=0;i<PROCS;i++) { int status=-1;if(call(260,(U)pids[i],(U)&status,0,0,0,0)!=pids[i] || status)fail("ring process exit"); }
    out("procs hops=");num((U)s->progress);out(" ms=");num(ms()-start);out("\n");
}
/* ---- bitsets, requeue, timeouts ---- */
static volatile int bword=0,bdone=-1,qa=0,qb=0,qdone=-1;
static void bit_waiter(void *arg) { (void)arg;bdone=(int)call(98,(U)&bword,128|9,0,0,0,1); }   /* WAIT_BITSET, mask 1 */
static void requeue_waiter(void *arg) { (void)arg;qdone=(int)call(98,(U)&qa,128,0,0,0,0); }
static void bits(void) {
    if(spawn(FLAGS,stacks[9]+sizeof(stacks[0]),&tids[9],0,&tids[9],bit_waiter,0)<=0)fail("spawn");
    sleep_ms(30);
    if(call(98,(U)&bword,128|10,1,0,0,2)!=0 || bdone!=-1)fail("WAKE_BITSET woke a waiter of another bit");
    if(call(98,(U)&bword,128|10,1,0,0,3)!=1)fail("WAKE_BITSET missed its waiter");
    for(int i=0;i<200 && bdone==-1;i++)sleep_ms(5);
    if(bdone!=0)fail("bitset waiter result");
    if(spawn(FLAGS,stacks[10]+sizeof(stacks[0]),&tids[10],0,&tids[10],requeue_waiter,0)<=0)fail("spawn");
    sleep_ms(30);
    if(call(98,(U)&qa,128|4,0,1,(U)&qb,5)!=-11)fail("CMP_REQUEUE ignored a changed value");
    if(call(98,(U)&qa,128|4,0,1,(U)&qb,0)!=1)fail("CMP_REQUEUE did not count its waiter");
    for(int i=0;i<10 && qdone==-1;i++) { call(98,(U)&qb,129,1,0,0,0);sleep_ms(5); }   /* moved: B's wake; woken: already back */
    for(int i=0;i<200 && qdone==-1;i++)sleep_ms(5);
    if(qdone!=0)fail("requeued waiter result");
    U t0=ms(),timeout[2]={0,5000000};int w=0;
    if(call(98,(U)&w,128,0,(U)timeout,0,0)!=-110)fail("timed wait result");
    U took=ms()-t0;if(took<5 || took>60)fail("timed wait duration");
    out("bits ok\n");
}
/* ---- signals mid-wait ---- */
static volatile int sig_hits,sig_stage,sig_result[4],sword;
static int sfd,sep,spipe[2];
static void on_signal(int n) { (void)n;sig_hits++; }
static void sig_waiter(void *arg) {
    (void)arg;unsigned char ev[16];struct { int fd;short events,revents; } p={spipe[0],1,0};char c;
    sig_stage=1;sig_result[0]=(int)call(98,(U)&sword,128,0,0,0,0);
    sig_stage=2;sig_result[1]=(int)call(22,(U)sep,(U)ev,1,(U)-1,0,8);
    sig_stage=3;sig_result[2]=(int)call(73,(U)&p,1,0,0,0,0);
    sig_stage=4;sig_result[3]=(int)call(63,(U)spipe[0],(U)&c,1,0,0,0);    /* SIGUSR2 has SA_RESTART */
    sig_stage=5;
}
static U ticks(void) { U v;__asm__ volatile("isb\nmrs %0, cntvct_el0":"=r"(v));return v; }
static U freq(void) { U v;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(v));return v; }
static void signals_round(int brief,U *worst) {
    sig_stage=0;
    if(spawn(FLAGS,stacks[11]+sizeof(stacks[0]),&tids[11],0,&tids[11],sig_waiter,0)<=0)fail("spawn");
    long pid=call(172,0,0,0,0,0,0);
    for(int stage=1;stage<=4;stage++) {
        while(sig_stage!=stage) { if(brief)call(124,0,0,0,0,0,0);else sleep_ms(1); }
        /* brief: about 300 us into its wait (waiting in place); else deep in it (parked) */
        if(brief) { U until=ticks()+freq()*300/1000000;while(ticks()<until) {} } else sleep_ms(stage==4?30:10);
        int hits=sig_hits;U t0=ms();
        if(call(131,(U)pid,(U)tids[11],stage==4?12:10,0,0,0))fail("tgkill");
        while(sig_hits==hits && ms()-t0<2000)sleep_ms(1);
        if(sig_hits==hits)fail("a signal did not reach a waiting thread");
        if(stage<4) { while(sig_stage==stage && ms()-t0<2000)sleep_ms(1);if(sig_stage==stage)fail("an interrupted wait did not end"); }
        if(ms()-t0>*worst)*worst=ms()-t0;
    }
    sleep_ms(20);
    if(sig_stage!=4)fail("a SA_RESTART read did not resume");
    call(64,(U)spipe[1],(U)"r",1,0,0,0);
    for(int i=0;i<400 && sig_stage!=5;i++)sleep_ms(5);
    if(sig_stage!=5)fail("a resumed read missed its data");
    if(sig_result[0]!=-4 || sig_result[1]!=-4 || sig_result[2]!=-4 || sig_result[3]!=1)fail("interrupted wait results");
    while(tids[11])sleep_ms(1);                                    /* CLONE_CHILD_CLEARTID: it is gone */
}
static void signals(void) {
    U act1[4]={(U)on_signal,0,0,0},act2[4]={(U)on_signal,0x10000000,0,0};
    if(call(134,10,(U)act1,0,8,0,0) || call(134,12,(U)act2,0,8,0,0))fail("sigaction");
    sfd=(int)call(19,0,2048,0,0,0,0);sep=(int)call(20,0,0,0,0,0,0);U ev[2]={1,0};
    if(sfd<0 || sep<0 || call(21,(U)sep,1,(U)sfd,(U)ev,0,0) || call(59,(U)spipe,0,0,0,0,0))fail("signal setup");
    U worst=0;signals_round(1,&worst);signals_round(0,&worst);
    if(worst>500)fail("a signal reached a waiting thread late");
    out("signals ok worst-ms=");num(worst);out("\n");
}
/* ---- exit while threads wait ---- */
static int xword,xpipe[2],xefd,xep;
static void x_futex(void *a) { (void)a;for(;;)call(98,(U)&xword,128,0,0,0,0); }
static void x_epoll(void *a) { (void)a;unsigned char ev[16];for(;;)call(22,(U)xep,(U)ev,1,(U)-1,0,8); }
static void x_poll(void *a) { (void)a;struct { int fd;short events,revents; } p={xpipe[0],1,0};for(;;)call(73,(U)&p,1,0,0,0,0); }
static void x_read(void *a) { (void)a;char c;for(;;)call(63,(U)xpipe[0],(U)&c,1,0,0,0); }
static void exiting(void) {
    U t0=ms();long pid=call(220,17,0,0,0,0,0);
    if(pid<0)fail("fork");
    if(!pid) {
        xefd=(int)call(19,0,2048,0,0,0,0);xep=(int)call(20,0,0,0,0,0,0);U ev[2]={1,0};
        call(21,(U)xep,1,(U)xefd,(U)ev,0,0);call(59,(U)xpipe,0,0,0,0,0);
        void (*fns[4])(void *)={x_futex,x_epoll,x_poll,x_read};
        for(int i=0;i<4;i++)if(spawn(FLAGS,stacks[12+i]+sizeof(stacks[0]),&tids[12+i],0,&tids[12+i],fns[i],0)<=0)call(94,20,0,0,0,0,0);
        sleep_ms(30);
        long child=call(220,17,0,0,0,0,0);                         /* fork while its threads wait */
        if(!child)call(94,3,0,0,0,0,0);
        int status=-1;if(child<0 || call(260,(U)child,(U)&status,0,0,0,0)!=child || status!=(3<<8))call(94,21,0,0,0,0,0);
        call(94,7,0,0,0,0,0);
    }
    int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid)fail("wait");
    if(status!=(7<<8))fail("exit status with waiting threads");
    U took=ms()-t0;if(took>1000)fail("exit with waiting threads was slow");
    out("exit ok ms=");num(took);out("\n");
}
void _start(void) {
    ring();procs();bits();signals();exiting();
    out("wake-race PASS\n");call(94,0,0,0,0,0,0);for(;;){}
}
