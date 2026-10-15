/* Write faults that map neighbouring pages ahead (fault-around) and futex waits
   served in place, under parallel workers:
   - eight threads fill interleaved 16 KiB stripes of one mapping in order, so one
     thread's fault-around takes pages a sibling is about to write; every word must
     hold what its writer put there;
   - a read-only page in the middle of a sequentially written mapping stays
     read-only (a write faults: SIGSEGV to a handler, and a child without one
     dies of it), and so does a PROT_NONE one;
   - after fork, parent and child rewrite halves of a filled mapping (copy-on-write
     with fault-around), and a MAP_SHARED mapping stays shared: each side sees only
     its own private writes and both see the shared ones;
   - two threads ping-pong 20000 times on a futex while a forked child does the same
     at the same addresses (wakes never cross processes), and a timed wait nobody
     wakes returns ETIMEDOUT, a wait on a changed word EAGAIN.
   Exit 0 and "fault-around PASS" when everything held. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="fault-around FAIL xx\n";m[18]=(char)('0'+code/10);m[19]=(char)('0'+code%10);
    call(64,1,(U)m,21,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
static U *map(U bytes,U prot,U flags) { long p=call(222,0,bytes,prot,flags,(U)-1,0);if(p<0)fail(1);return (U *)p; }
static int child_status(long pid) { int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid)fail(2);return status; }
/* clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|PARENT_SETTID|CHILD_CLEARTID): the
   new thread finds its function and argument at the top of its stack. */
static long spawn(U *top,volatile unsigned *tid,void (*fn)(U),U arg) {
    top-=2;top[0]=(U)fn;top[1]=arg;
    register U x8 __asm__("x8")=220,x0 __asm__("x0")=0x10f00|0x40000|0x100000|0x200000,x1 __asm__("x1")=(U)top;
    register U x2 __asm__("x2")=(U)tid,x3 __asm__("x3")=0,x4 __asm__("x4")=(U)tid;
    __asm__ volatile("svc #0\n cbnz x0,1f\n ldp x9,x0,[sp]\n blr x9\n mov x0,#0\n mov x8,#93\n svc #0\n1:"
                     :"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory","cc","x9","x30");
    return (long)x0;
}
static void join(volatile unsigned *tid) { for(unsigned t;(t=__atomic_load_n(tid,__ATOMIC_ACQUIRE));)call(98,(U)tid,0,t,0,0,0); }
#define THREADS 8
static U stacks[THREADS][2048] __attribute__((aligned(16)));
static volatile unsigned tids[THREADS];
/* Stripes: 16 KiB (2048 words) each, thread t writes stripes t, t+8, t+16, ... */
#define STRIPE 2048u
#define STRIPES 1024u                     /* 16 MiB */
static U *striped;
static void stripe_writer(U t) {
    for(U s=t;s<STRIPES;s+=THREADS)for(U w=0;w<STRIPE;w++)striped[s*STRIPE+w]=(t<<56)^(s*STRIPE+w);
}
/* SIGSEGV handler: count the fault and skip the faulting store (the ucontext's pc). */
static volatile U faults;
static void on_segv(U sig,U info,U uc) { (void)sig;(void)info;faults++;*(volatile U *)(uc+440)+=4; }
__asm__(".text\n.p2align 2\nsegv_restorer: mov x8,#139\n svc #0\n");
extern void segv_restorer(void);
static void store(U *p,U value) { __asm__ volatile("str %0,[%1]"::"r"(value),"r"(p):"memory"); }
static void fill(U *p,U words,U tag) { for(U i=0;i<words;i++)p[i]=tag^i; }
static int same(const U *p,U words,U tag) { for(U i=0;i<words;i++)if(p[i]!=(tag^i))return 0;return 1; }
/* Futex ping-pong: turn 0 is ping's, 1 pong's. */
static volatile unsigned turn,rounds_done;
#define ROUNDS 20000u
static void pong(U unused) {
    (void)unused;
    for(unsigned i=0;i<ROUNDS;i++) {
        while(__atomic_load_n(&turn,__ATOMIC_ACQUIRE)!=1)call(98,(U)&turn,128,0,0,0,0);   /* FUTEX_WAIT_PRIVATE while 0 */
        __atomic_store_n(&turn,0,__ATOMIC_RELEASE);call(98,(U)&turn,129,1,0,0,0);          /* FUTEX_WAKE_PRIVATE */
    }
}
static void ping_pong(void) {
    static volatile unsigned pong_tid;
    turn=0;rounds_done=0;
    if(spawn(&stacks[0][2048],&pong_tid,pong,0)<=0)fail(30);
    for(unsigned i=0;i<ROUNDS;i++) {
        while(__atomic_load_n(&turn,__ATOMIC_ACQUIRE)!=0)call(98,(U)&turn,128,1,0,0,0);
        rounds_done++;
        __atomic_store_n(&turn,1,__ATOMIC_RELEASE);call(98,(U)&turn,129,1,0,0,0);
    }
    join(&pong_tid);
    if(rounds_done!=ROUNDS)fail(31);
    /* A timed wait nobody wakes, and one on a word that changed. */
    static volatile unsigned quiet=7;U ts[2]={0,2000000};                                      /* 2 ms */
    if(call(98,(U)&quiet,128,7,(U)ts,0,0)!=-110)fail(32);
    if(call(98,(U)&quiet,128,8,0,0,0)!=-11)fail(33);
}
void _start(void) {
    /* Interleaved stripes from parallel threads. */
    striped=map((U)STRIPES*STRIPE*8,3,0x22);
    for(unsigned t=0;t<THREADS;t++)if(spawn(&stacks[t][2048],&tids[t],stripe_writer,t)<=0)fail(3);
    for(unsigned t=0;t<THREADS;t++)join(&tids[t]);
    for(U s=0;s<STRIPES;s++)for(U w=0;w<STRIPE;w++)if(striped[s*STRIPE+w]!=(((s%THREADS)<<56)^(s*STRIPE+w)))fail(4);
    /* Read-only and PROT_NONE pages inside a mapping written in order. */
    U action[4]={(U)on_segv,4|0x04000000|0x40000000,(U)segv_restorer,0};   /* SA_SIGINFO|SA_RESTORER|SA_NODEFER */
    if(call(134,11,(U)action,0,8,0,0))fail(5);
    for(U guard=0;guard<2;guard++) {
        U *p=map(64*4096,3,0x22);U *hole=p+32*512;
        if(call(226,(U)hole,4096,guard?0:1,0,0,0))fail(5);
        fill(p,32*512,0x1234);                      /* everything before the hole, in order */
        if(!guard && hole[0])fail(6);
        U before=faults;store(hole+1,1);
        if(faults!=before+1 || (!guard && hole[1]))fail(29);
        U done[4]={0,0,0,0};if(call(134,11,(U)done,0,8,0,0))fail(5); /* the child dies of it */
        long pid=call(220,17,0,0,0,0,0);
        if(pid<0)fail(7);
        if(!pid) { store(hole+1,1);call(94,0,0,0,0,0,0); } /* must fault */
        int status=child_status(pid);
        if((status&0x7f)!=11)fail(8);
        if(call(134,11,(U)action,0,8,0,0))fail(5);
        fill(hole+512,31*512,0x5678);               /* and after it */
        if(!same(p,32*512,0x1234) || !same(hole+512,31*512,0x5678))fail(9);
    }
    /* Copy-on-write halves after fork, next to shared memory. */
    for(unsigned round=0;round<4;round++) {
        U words=(U)(1u<<19);                        /* 4 MiB */
        U *priv=map(words*8,3,0x22),*shared=map(words*8,3,0x21);
        fill(priv,words,0xaaaa+round);fill(shared,words,0xbbbb+round);
        long pid=call(220,17,0,0,0,0,0);
        if(pid<0)fail(10);
        if(!pid) {
            if(!same(priv,words,0xaaaa+round))call(94,11,0,0,0,0,0);
            fill(priv,words/2,0xcccc);              /* first half, in order: fault-around runs ahead */
            fill(shared,words/2,0xdddd);
            call(124,0,0,0,0,0,0);
            if(!same(priv,words/2,0xcccc))call(94,12,0,0,0,0,0);
            for(U i=words/2;i<words;i++)if(priv[i]!=((0xaaaa+round)^i))call(94,13,0,0,0,0,0); /* never the parent's writes */
            call(94,0,0,0,0,0,0);
        }
        for(U i=words/2;i<words;i++)priv[i]=0xeeee^i; /* second half while the child runs */
        int status=child_status(pid);
        if(status)fail(14+(unsigned)((status>>8)&7));
        if(!same(priv,words/2,0xaaaa+round))fail(22);
        for(U i=words/2;i<words;i++)if(priv[i]!=(0xeeee^i))fail(23);
        if(!same(shared,words/2,0xdddd))fail(24);
        if(call(215,(U)priv,words*8,0,0,0,0) || call(215,(U)shared,words*8,0,0,0,0))fail(25);
    }
    /* Futex handoffs in two processes at the same addresses. */
    long pid=call(220,17,0,0,0,0,0);
    if(pid<0)fail(26);
    ping_pong();
    if(!pid)call(94,0,0,0,0,0,0);
    if(child_status(pid))fail(27);
    call(64,1,(U)"fault-around PASS\n",18,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
