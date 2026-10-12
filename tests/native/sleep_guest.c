/* nanosleep and clock_nanosleep (relative and TIMER_ABSTIME) wake on time, first
   single-threaded, then while a sibling thread is parked on a futex forever (the
   Redis background-thread layout). */
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
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"sleep FAIL\n",11,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static U now_ms(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000+ts[1]/1000000; }
/* Each sleep must last at least 100 ms and finish well within 2 s. */
static void sleeps(void) {
    U ts[2]={0,100000000},start=now_ms();
    check(call(101,(U)ts,0,0,0,0,0)==0);
    U mid=now_ms();check(mid-start>=100 && mid-start<2000);
    check(call(115,1,0,(U)ts,0,0,0)==0);
    U end=now_ms();check(end-mid>=100 && end-mid<2000);
    U abs[2];call(113,1,(U)abs,0,0,0,0);abs[1]+=100000000;if(abs[1]>=1000000000) { abs[0]++;abs[1]-=1000000000; }
    check(call(115,1,1,(U)abs,0,0,0)==0);
    check(now_ms()-end>=100);
    U bad[2]={0,1000000000};check(call(101,(U)bad,0,0,0,0,0)==-22);
}
static int park_word;
static void parked(void *arg) { (void)arg;for(;;)call(98,(U)&park_word,128,0,0,0,0); }
static unsigned char stack[32768] __attribute__((aligned(16)));
static int tid_word;
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
void _start(void) {
    sleeps();
    check(spawn(FLAGS,stack+sizeof(stack),&tid_word,0,&tid_word,parked,0)>0);
    sleeps();
    call(64,1,(U)"sleep PASS\n",11,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
