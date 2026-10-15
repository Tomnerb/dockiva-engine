/* An edge-triggered EPOLLOUT edge while a thread already waits: as .NET's socket
   engine does (MSBuild node packets), a thread sleeps in epoll_wait on a Unix
   socket whose EPOLLOUT it has reported; another thread fills the socket buffer
   (EAGAIN) and the peer drains it. Linux reports a new EPOLLOUT edge, which the
   writer waits for; a missed edge left MSBuild's nodes and host waiting forever. */
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
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"epoll-out-edge FAIL\n",20,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
#define WAIT 128
#define WAKE 129
static unsigned char stack[65536] __attribute__((aligned(16)));
static int join_word,outs;
static long ep,s[2];
static unsigned char block[4096];
/* The engine thread: report each EPOLLOUT edge. */
static void engine(void *arg) {
    (void)arg;
    for(;;) {
        U ev[2]={0,0};long n=call(22,(U)ep,(U)ev,1,(U)-1,0,0);
        if(n==1 && (ev[0]&4)) { __atomic_fetch_add(&outs,1,__ATOMIC_SEQ_CST);call(98,(U)&outs,WAKE,1,0,0,0); }
        if(n==1 && ev[1]==99)return;                                 /* told to stop */
    }
}
/* Wait up to about two seconds for outs to pass seen. */
static int edge_after(int seen) {
    for(int i=0;i<40;i++) {
        int now=__atomic_load_n(&outs,__ATOMIC_SEQ_CST);if(now>seen)return 1;
        U timeout[2]={0,50000000};call(98,(U)&outs,WAIT,(U)now,(U)timeout,0,0);
    }
    return 0;
}
void _start(void) {
    int pair[2];check(call(199,1,1,0,(U)pair,0,0)==0);                     /* socketpair(AF_UNIX, SOCK_STREAM) */
    s[0]=pair[0];s[1]=pair[1];
    check(call(25,(U)s[0],4,04000,0,0,0)==0 && call(25,(U)s[1],4,04000,0,0,0)==0); /* O_NONBLOCK */
    ep=call(20,0,0,0,0,0,0);check(ep>=0);
    U ev[2]={4u|1u|(1u<<31),7};check(call(21,(U)ep,1,(U)s[0],(U)ev,0,0)==0);  /* IN|OUT|ET */
    check(spawn(FLAGS,stack+sizeof(stack),&join_word,0,&join_word,engine,0)>0);
    check(edge_after(0));                                                  /* the first edge: writable */
    int seen=__atomic_load_n(&outs,__ATOMIC_SEQ_CST);
    U pause[2]={0,100000000};call(101,(U)pause,0,0,0,0,0);                 /* the engine sleeps in epoll_wait */
    long total=0,n;
    while((n=call(64,(U)s[0],(U)block,sizeof(block),0,0,0))>0)total+=n;    /* fill the buffer */
    check(n==-11 && total>0);
    seen=__atomic_load_n(&outs,__ATOMIC_SEQ_CST);
    while(total>0) { n=call(63,(U)s[1],(U)block,sizeof(block),0,0,0);if(n<=0)break;total-=n; } /* the peer drains it */
    check(total==0);
    check(edge_after(seen));                                               /* writable again: a new edge */
    U stop[2]={1,99};check(call(21,(U)ep,1,(U)s[1],(U)stop,0,0)==0);
    check(call(64,(U)s[0],(U)"x",1,0,0,0)==1);                             /* s[1] readable: the engine stops */
    int v;while((v=__atomic_load_n(&join_word,__ATOMIC_ACQUIRE))!=0)call(98,(U)&join_word,WAIT,(U)v,0,0,0);
    call(64,1,(U)"epoll-out-edge PASS\n",20,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
