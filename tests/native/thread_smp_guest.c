/* Threads of one process on parallel workers: eight threads add to one counter
   with load/store-exclusive loops, update a second counter under a futex mutex,
   and map, fill and unmap memory now and then (changing the shared page tables
   while their siblings run). The main thread joins them through
   CLONE_CHILD_CLEARTID futex wakes; both totals must come out exact. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) { call(64,1,(U)"thread-smp FAIL\n",16,0,0,0);call(94,code,0,0,0,0,0);for(;;){} }
static void add(volatile U *p) {
    U value,failed;
    __asm__ volatile("1: ldxr %0,[%2]\n add %0,%0,#1\n stxr %w1,%0,[%2]\n cbnz %w1,1b":"=&r"(value),"=&r"(failed):"r"(p):"memory");
}
static unsigned exchange(volatile unsigned *p,unsigned next) {      /* acquire-release swap */
    unsigned old,failed;
    __asm__ volatile("1: ldaxr %w0,[%2]\n stlxr %w1,%w3,[%2]\n cbnz %w1,1b":"=&r"(old),"=&r"(failed):"r"(p),"r"(next):"memory");
    return old;
}
static unsigned compare_exchange(volatile unsigned *p,unsigned expected,unsigned next) {
    unsigned old,failed;
    __asm__ volatile("1: ldaxr %w0,[%2]\n cmp %w0,%w3\n b.ne 2f\n stlxr %w1,%w4,[%2]\n cbnz %w1,1b\n2: clrex":"=&r"(old),"=&r"(failed):"r"(p),"r"(expected),"r"(next):"memory","cc");
    return old;
}
/* A futex mutex: 0 free, 1 locked, 2 locked with waiters. */
static volatile unsigned mutex;
static void lock(void) {
    unsigned c=compare_exchange(&mutex,0,1);
    if(!c)return;
    if(c!=2)c=exchange(&mutex,2);
    while(c) { call(98,(U)&mutex,128,2,0,0,0);c=exchange(&mutex,2); }   /* FUTEX_WAIT_PRIVATE */
}
static void unlock(void) {
    if(exchange(&mutex,0)==2)call(98,(U)&mutex,129,1,0,0,0);           /* FUTEX_WAKE_PRIVATE */
}
#define THREADS 8
#define ADDS 2000000
#define LOCKED 100000
static volatile U counter,guarded;
static volatile unsigned tids[THREADS];
static void worker(U index) {
    for(unsigned i=0;i<ADDS;i++) {
        add(&counter);
        if(i<LOCKED) { lock();U v=guarded;for(volatile unsigned k=0;k<(index&3);k++) {}guarded=v+1;unlock(); }
        if(!(i&4095)) {
            U pages=1+((i>>12)+index)%8,bytes=pages*4096;
            volatile U *p=(volatile U *)call(222,0,bytes,3,0x22,(U)-1,0);
            if((long)p<0)fail(10);
            for(U j=0;j<bytes/8;j+=512) { if(p[j])fail(11);p[j]=index^j; }
            for(U j=0;j<bytes/8;j+=512)if(p[j]!=(index^j))fail(12);
            if(call(215,(U)p,bytes,0,0,0,0))fail(13);
        }
        if(!(i&16383))call(124,0,0,0,0,0,0);
    }
}
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
static U stacks[THREADS][2048] __attribute__((aligned(16)));
void _start(void) {
    for(unsigned t=0;t<THREADS;t++)if(spawn(&stacks[t][2048],&tids[t],worker,t)<=0)fail(1);
    for(unsigned t=0;t<THREADS;t++)
        for(unsigned tid;(tid=tids[t]);)call(98,(U)&tids[t],0,tid,0,0,0);   /* FUTEX_WAIT until CLEARTID */
    if(counter!=(U)THREADS*ADDS)fail(2);
    if(guarded!=(U)THREADS*LOCKED)fail(3);
    call(64,1,(U)"thread-smp PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
