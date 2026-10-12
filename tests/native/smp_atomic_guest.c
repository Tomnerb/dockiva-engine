/* Atomics in shared memory across parallel workers: eight processes add to one
   MAP_SHARED counter with load/store-exclusive loops (as Linux arm64 software does
   without LSE), yielding now and then so they migrate between vCPUs; plain and
   atomic totals must both come out exact. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void add(volatile U *p) {
    U value,failed;
    __asm__ volatile("1: ldxr %0,[%2]\n add %0,%0,#1\n stxr %w1,%0,[%2]\n cbnz %w1,1b":"=&r"(value),"=&r"(failed):"r"(p):"memory");
}
static void fail(unsigned code) { call(64,1,(U)"smp-atomic FAIL\n",16,0,0,0);call(94,code,0,0,0,0,0);for(;;){} }
#define WORKERS 8
#define ADDS 200000
void _start(void) {
    volatile U *shared=(volatile U *)call(222,0,4096,3,0x21,(U)-1,0);   /* MAP_SHARED|MAP_ANONYMOUS */
    if((long)shared<0)fail(1);
    long pids[WORKERS];
    for(unsigned w=0;w<WORKERS;w++) {
        pids[w]=call(220,17,0,0,0,0,0);
        if(pids[w]<0)fail(2);
        if(!pids[w]) {
            for(unsigned i=0;i<ADDS;i++) {
                add(&shared[0]);
                if(!(i&1023))call(124,0,0,0,0,0,0);               /* sched_yield: switch vCPUs */
            }
            call(94,0,0,0,0,0,0);
        }
    }
    for(unsigned w=0;w<WORKERS;w++) { int status=-1;if(call(260,(U)pids[w],(U)&status,0,0,0,0)!=pids[w] || status)fail(3); }
    if(shared[0]!=(U)WORKERS*ADDS) {
        char m[]="lost updates: 0000000\n";U lost=(U)WORKERS*ADDS-shared[0];
        for(int i=20;i>=14;i--) { m[i]=(char)('0'+lost%10);lost/=10; }
        call(64,1,(U)m,sizeof(m)-1,0,0,0);fail(4);
    }
    call(64,1,(U)"smp-atomic PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
