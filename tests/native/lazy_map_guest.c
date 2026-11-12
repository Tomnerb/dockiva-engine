/* Committed memory a process has not touched gets no page tables under the hypervisor
   until its first use: a read, a write or an instruction fetch there maps its 2 MiB
   span (hvf_touched, hvf_first_use). Each check has its own exit code:
   1x  reads of untouched committed memory are zero (a reservation above the arena, the
       mmap arena, the brk heap); a write there reads back, the span's other pages zero;
       cache maintenance by address works there, and the memory stays zero;
   2x  code read() put in an untouched RWX span runs (an instruction fetch's first use);
   3x  after fork the child reads the parent's bytes and zero where the parent wrote
       nothing, and writes its own; while the child lives the parent writes the same
       pages (memory held for the fork); neither sees the other's writes;
   4x  eight threads first-touch the same untouched spans at once, reading and writing:
       a read sees zero or a sibling's word, and every word holds at the end.
   Exit 0 and "lazy-map PASS". */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="lazy-map FAIL xx\n";m[14]=(char)('0'+code/10);m[15]=(char)('0'+code%10);
    call(64,1,(U)m,17,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define RES 0x100000000000UL                          /* 16 TiB: reservations above the arena */
#define GIB (1UL<<30)
#define MIB (1UL<<20)
static volatile U *map(U bytes,U prot) { long p=call(222,0,bytes,prot,0x22,(U)-1,0);if(p<0)fail(1);return (volatile U *)p; }
static void commit(U at,U bytes) { if(call(226,at,bytes,3,0,0,0))fail(2); }
static void reads_and_writes(void) {
    volatile U *r=(volatile U *)RES;
    commit(RES,2*GIB);
    for(U at=0;at<2*GIB;at+=2*MIB+20480)if(r[at/8])fail(10);              /* a read in every span */
    r[5*MIB/8]=0x1234;                                                      /* a span the reads mapped */
    if(r[5*MIB/8]!=0x1234 || r[5*MIB/8+1] || r[(5*MIB+4096)/8] || r[(5*MIB+16384)/8])fail(11);
    volatile U *w=(volatile U *)(RES+2*GIB);
    commit(RES+2*GIB,GIB);
    w[3*MIB/8]=0xabc;                                                       /* a write first */
    if(w[3*MIB/8]!=0xabc || w[(3*MIB-8)/8] || w[(3*MIB+16384)/8] || w[2*MIB/8] || w[(4*MIB-8)/8] || w[(4*MIB)/8])fail(12);
    volatile U *a=map(64*MIB,3);                                            /* the mmap arena */
    for(U at=0;at<64*MIB;at+=MIB+4096)if(a[at/8])fail(13);
    a[40*MIB/8]=5;a[(40*MIB+8192)/8]=6;
    if(a[40*MIB/8]!=5 || a[(40*MIB+8192)/8]!=6 || a[(40*MIB+4096)/8] || a[(41*MIB)/8])fail(14);
    U top=(U)call(214,0,0,0,0,0,0);                                         /* the brk heap */
    if((U)call(214,top+8*MIB,0,0,0,0,0)!=top+8*MIB)fail(15);
    volatile U *h=(volatile U *)((top+4095)&~4095UL);
    if(h[0] || h[(6*MIB)/8])fail(16);
    h[6*MIB/8]=9;if(h[6*MIB/8]!=9 || h[(6*MIB+4096)/8])fail(17);
    volatile U *c=w+64*MIB/8;                                               /* cache maintenance first: a write fault that is not a store */
    __asm__ volatile("dc civac, %0\n\tdc cvau, %0\n\tic ivau, %0\n\tdsb ish"::"r"(c):"memory");
    if(c[0] || c[512])fail(18);
}
static void fetch(void) {
    volatile U *code=map(4*MIB,7);                                          /* RWX, untouched */
    static const unsigned insn[2]={0xd2800540,0xd65f03c0};                 /* mov x0, #42; ret */
    int p[2];
    if(call(59,(U)p,0,0,0,0,0) || call(64,(U)p[1],(U)insn,8,0,0,0)!=8)fail(20);
    if(call(63,(U)p[0],(U)code+3*MIB,8,0,0,0)!=8)fail(21);                 /* the runtime writes it: no mapping changes */
    if(((U (*)(void))((U)code+3*MIB))()!=42)fail(22);
    call(57,(U)p[0],0,0,0,0,0);call(57,(U)p[1],0,0,0,0,0);
}
static long wait_child(long pid) { int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid)fail(30);return status; }
static void forks(void) {
    volatile U *f=(volatile U *)(RES+3*GIB),*a=map(256*MIB,3);
    commit(RES+3*GIB,GIB);
    f[MIB/8]=111;a[MIB/8]=112;                                              /* one touched span each; 9 MiB on untouched */
    int go[2],back[2];char c=0;
    if(call(59,(U)go,0,0,0,0,0) || call(59,(U)back,0,0,0,0,0))fail(31);
    long pid=call(220,17,0,0,0,0,0);
    if(pid<0)fail(32);
    if(!pid) {
        unsigned bad=0;
        if(f[MIB/8]!=111 || f[(MIB+4096)/8] || f[9*MIB/8] || a[MIB/8]!=112 || a[(MIB+4096)/8] || a[9*MIB/8])bad|=1;
        f[9*MIB/8]=221;f[MIB/8]=222;a[9*MIB/8]=223;a[MIB/8]=224;            /* its own copies */
        if(f[9*MIB/8]!=221 || f[MIB/8]!=222 || a[9*MIB/8]!=223 || a[MIB/8]!=224)bad|=2;
        call(64,(U)back[1],(U)&c,1,0,0,0);                                  /* the parent writes now */
        if(call(63,(U)go[0],(U)&c,1,0,0,0)!=1)bad|=4;
        if(f[9*MIB/8]!=221 || f[MIB/8]!=222 || a[9*MIB/8]!=223 || a[MIB/8]!=224 || f[17*MIB/8] || a[17*MIB/8])bad|=8;
        call(93,bad,0,0,0,0,0);for(;;){}
    }
    if(call(63,(U)back[0],(U)&c,1,0,0,0)!=1)fail(33);
    if(f[MIB/8]!=111 || f[9*MIB/8] || a[MIB/8]!=112 || a[9*MIB/8])fail(34);
    f[MIB/8]=331;f[9*MIB/8]=332;f[17*MIB/8]=333;a[MIB/8]=334;a[9*MIB/8]=335;a[17*MIB/8]=336;   /* held for the child */
    call(64,(U)go[1],(U)&c,1,0,0,0);
    long status=wait_child(pid);
    if(status)fail(50+(unsigned)((status>>8)&15));                         /* the child's bits */
    if(f[MIB/8]!=331 || f[9*MIB/8]!=332 || f[17*MIB/8]!=333 || a[MIB/8]!=334 || a[9*MIB/8]!=335 || a[17*MIB/8]!=336)fail(39);
}
/* Threads: clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|PARENT_SETTID|CHILD_CLEARTID); the
   new thread finds its function and argument at the top of its stack. */
static long spawn(U *top,volatile unsigned *tid,void (*fn)(U),U arg) {
    top-=2;top[0]=(U)fn;top[1]=arg;
    register U x8 __asm__("x8")=220,x0 __asm__("x0")=0x10f00|0x40000|0x100000|0x200000,x1 __asm__("x1")=(U)top;
    register U x2 __asm__("x2")=(U)tid,x3 __asm__("x3")=0,x4 __asm__("x4")=(U)tid;
    __asm__ volatile("svc #0\n cbnz x0,1f\n ldp x9,x0,[sp]\n blr x9\n mov x0,#0\n mov x8,#93\n svc #0\n1:"
                     :"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory","cc","x9","x30");
    return (long)x0;
}
#define THREADS 8u
#define ROUNDS 64u
static U stacks[THREADS][2048] __attribute__((aligned(16)));
static volatile unsigned tids[THREADS],turn,arrived,bad;
static void toucher(U t) {
    volatile U *base=(volatile U *)(RES+4*GIB);
    for(unsigned k=0;k<ROUNDS;k++) {
        while(__atomic_load_n(&turn,__ATOMIC_ACQUIRE)<=k)call(98,(U)&turn,128,k,0,0,0);      /* FUTEX_WAIT_PRIVATE */
        volatile U *span=base+(U)k*(2*MIB/8);
        for(unsigned s=0;s<THREADS;s++) { U v=span[s*1536];if(v && v!=(((U)k<<8)|s))bad=1; }   /* 12 KiB apart: shared host pages */
        span[t*1536]=((U)k<<8)|t;
        if(span[t*1536]!=(((U)k<<8)|t))bad=2;
        if(__atomic_add_fetch(&arrived,1,__ATOMIC_ACQ_REL)==(k+1)*THREADS)call(98,(U)&arrived,129,1,0,0,0);
    }
}
static void threads(void) {
    commit(RES+4*GIB,GIB);
    for(unsigned t=0;t<THREADS;t++)if(spawn(&stacks[t][2048],&tids[t],toucher,t)<=0)fail(40);
    for(unsigned k=0;k<ROUNDS;k++) {
        __atomic_store_n(&turn,k+1,__ATOMIC_RELEASE);call(98,(U)&turn,129,THREADS,0,0,0);    /* FUTEX_WAKE_PRIVATE */
        for(unsigned n;(n=__atomic_load_n(&arrived,__ATOMIC_ACQUIRE))<(k+1)*THREADS;)call(98,(U)&arrived,128,n,0,0,0);
    }
    for(unsigned t=0;t<THREADS;t++)for(unsigned v;(v=__atomic_load_n(&tids[t],__ATOMIC_ACQUIRE));)call(98,(U)&tids[t],0,v,0,0,0);
    if(bad)fail(41+bad);
    volatile U *base=(volatile U *)(RES+4*GIB);
    for(unsigned k=0;k<ROUNDS;k++)for(unsigned t=0;t<THREADS;t++)if(base[(U)k*(2*MIB/8)+t*1536]!=(((U)k<<8)|t))fail(44);
}
void _start(void) {
    if(call(222,RES,8*GIB,0,0x22|0x4000|0x100000,(U)-1,0)!=(long)RES)fail(3);   /* PROT_NONE, MAP_FIXED_NOREPLACE */
    reads_and_writes();
    fetch();
    forks();
    threads();
    call(64,1,(U)"lazy-map PASS\n",14,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
