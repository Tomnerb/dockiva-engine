/* Wake latency between two threads of one process: ping-pong round trips through
   a futex (a condition variable's wait and wake), an eventfd watched by epoll_pwait
   (libuv's uv_async), and a pipe watched by ppoll. Arguments: [budget-us [spinners]].
   spinners: threads that only burn CPU in guest code meanwhile (busy CPUs). Prints
   each kind's median and 90th percentile round trip (one wake each way) in
   microseconds; fails when a median exceeds budget-us (0: report only). */
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
void start_c(U *sp);
__asm__(".global _start\n_start:\n    mov x0, sp\n    b start_c\n");
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
#define ROUNDS 2000
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void num(U v) { char b[24];int i=23;b[i]=0;do { b[--i]=(char)('0'+v%10);v/=10; } while(v);out(b+i); }
static void fail(const char *s) { out("wake-latency FAIL ");out(s);out("\n");call(94,1,0,0,0,0,0);for(;;){} }
static U ticks(void) { U v;__asm__ volatile("isb\nmrs %0, cntvct_el0":"=r"(v));return v; }
static U freq(void) { U v;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(v));return v; }
static U parse(const char *s) { U v=0;while(*s>='0' && *s<='9')v=v*10+(U)(*s++-'0');return v; }
static unsigned char stacks[16][65536] __attribute__((aligned(16)));
static int tids[16];
static volatile int turn,stop;
static int efd[2],epfd[2],pfd[2][2];
static U samples[ROUNDS];
static void futex_wait(volatile int *w,int v) { while(*w==v)call(98,(U)w,128,(U)v,0,0,0); }
static void futex_wake(volatile int *w) { call(98,(U)w,129,1,0,0,0); }
static void ev_wait(int side) {
    unsigned char ev[16];U v;
    for(;;) { long n=call(22,(U)epfd[side],(U)ev,1,(U)-1,0,8);if(n==1)break;if(n!=-4)fail("epoll_pwait"); }
    if(call(63,(U)efd[side],(U)&v,8,0,0,0)!=8)fail("eventfd read");
}
static void ev_post(int side) { U one=1;if(call(64,(U)efd[side],(U)&one,8,0,0,0)!=8)fail("eventfd write"); }
static void pipe_wait(int side) {
    struct { int fd;short events,revents; } p={pfd[side][0],1,0};char c;
    for(;;) { long n=call(73,(U)&p,1,0,0,0,0);if(n==1)break;if(n!=-4)fail("ppoll"); }
    if(call(63,(U)pfd[side][0],(U)&c,1,0,0,0)!=1)fail("pipe read");
}
static void pipe_post(int side) { if(call(64,(U)pfd[side][1],(U)"x",1,0,0,0)!=1)fail("pipe write"); }
static volatile int kind;
/* The partner: answers every ping of the current kind until stop. */
static void partner(void *arg) {
    (void)arg;
    for(int k=0;k<3;k++)for(int r=0;r<ROUNDS;r++) {
        if(k==0) { futex_wait(&turn,0);turn=0;futex_wake(&turn); }
        else if(k==1) { ev_wait(0);ev_post(1); }
        else { pipe_wait(0);pipe_post(1); }
    }
}
static void burn(void *arg) { (void)arg;while(!stop)__asm__ volatile("":::"memory"); }
static void sort(U *a,int n) { for(int i=1;i<n;i++) { U v=a[i];int j=i;while(j && a[j-1]>v) { a[j]=a[j-1];j--; } a[j]=v; } }
void start_c(U *sp) {
    U argc=sp[0];char **argv=(char **)(sp+1);
    U budget=argc>1?parse(argv[1]):0,spinners=argc>2?parse(argv[2]):0;if(spinners>14)spinners=14;
    for(int s=0;s<2;s++) {
        efd[s]=(int)call(19,0,0,0,0,0,0);epfd[s]=(int)call(20,0,0,0,0,0,0);
        if(efd[s]<0 || epfd[s]<0 || call(59,(U)pfd[s],0,0,0,0,0))fail("setup");
        U ev[2]={1,(U)s};if(call(21,(U)epfd[s],1,(U)efd[s],(U)ev,0,0))fail("epoll_ctl");
    }
    for(U i=0;i<spinners;i++)if(spawn(FLAGS,stacks[2+i]+sizeof(stacks[0]),&tids[2+i],0,&tids[2+i],burn,0)<=0)fail("spawn");
    if(spawn(FLAGS,stacks[0]+sizeof(stacks[0]),&tids[0],0,&tids[0],partner,0)<=0)fail("spawn");
    static const char *names[3]={"futex","eventfd-epoll","pipe-ppoll"};
    U hz=freq(),worst=0;
    for(int k=0;k<3;k++) {
        for(int r=0;r<ROUNDS;r++) {
            U t0=ticks();
            if(k==0) { turn=1;futex_wake(&turn);futex_wait(&turn,1); }
            else if(k==1) { ev_post(0);ev_wait(1); }
            else { pipe_post(0);pipe_wait(1); }
            samples[r]=ticks()-t0;
        }
        sort(samples,ROUNDS);
        U med=samples[ROUNDS/2]*1000000/hz,p90=samples[ROUNDS*9/10]*1000000/hz;
        out(names[k]);out(" median-us=");num(med);out(" p90-us=");num(p90);out("\n");
        if(med>worst)worst=med;
    }
    stop=1;
    if(budget && worst>budget)fail("median over budget");
    out("wake-latency PASS\n");call(94,0,0,0,0,0,0);for(;;){}
}
