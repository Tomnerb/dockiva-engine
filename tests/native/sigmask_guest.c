/* Signal masks are per thread: a helper thread blocks every signal (as Redis's
   background threads do), yet SIGUSR1 sent to the process still reaches the main
   thread's handler, which returns through the vDSO trampoline (no SA_RESTORER). */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return (long)x0;
}
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
static void check(int ok,int code) { if(!ok) { call(64,1,(U)"sigmask FAIL\n",13,0);call(94,(U)code,0,0,0);for(;;){} } }
static volatile int caught,ready;
static int park;
static void handler(int signal) { if(signal==10)caught++; }
static void helper(void *arg) {
    (void)arg;U all=~0UL;call(135,0,(U)&all,0,8);ready=1;                  /* SIG_BLOCK everything */
    for(;;)call(98,(U)&park,128,0,0);
}
static unsigned char stack[32768] __attribute__((aligned(16)));
static int tid;
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
void _start(void) {
    U action[4]={(U)handler,0,0,0};check(call(134,10,(U)action,0,8)==0,1);  /* SIGUSR1, no SA_RESTORER */
    check(spawn(FLAGS,stack+sizeof(stack),&tid,0,&tid,helper,0)>0,2);
    while(!ready)call(124,0,0,0,0);                                          /* let it block everything */
    check(call(129,(U)call(172,0,0,0,0),10,0,0)==0,3);                       /* kill(getpid(), SIGUSR1) */
    for(int i=0;i<100 && !caught;i++)call(124,0,0,0,0);
    check(caught==1,4);
    call(64,1,(U)"sigmask PASS\n",13,0);call(94,0,0,0,0);for(;;){}
}
