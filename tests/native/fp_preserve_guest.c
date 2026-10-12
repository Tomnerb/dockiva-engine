/* FP/SIMD state survives system calls, thread switches and fork: each thread and
   a forked child keep their own v16 contents and FPCR rounding mode while they
   repeatedly make syscalls and yield to each other. */
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
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"fp-preserve FAIL\n",17,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
/* Load v16 with seed:~seed and FPCR.RMode=mode, then for rounds: getpid, sched_yield,
   verify both. Returns 1 when everything survived. All inside one asm block, so the
   compiler never touches v16 or FPCR in between. */
static U survives(U seed,U mode,U rounds) {
    U ok;
    __asm__ volatile(
        "    mvn x9, %[seed]\n    fmov d16, %[seed]\n    mov v16.d[1], x9\n"
        "    lsl x12, %[mode], #22\n    msr fpcr, x12\n"
        "    mov x13, %[rounds]\n"
        "2:  mov x8, #172\n    svc #0\n"                      /* getpid */
        "    mov x8, #124\n    svc #0\n"                      /* sched_yield: siblings run */
        "    fmov x10, d16\n    cmp x10, %[seed]\n    b.ne 3f\n"
        "    mov x10, v16.d[1]\n    mvn x9, %[seed]\n    cmp x10, x9\n    b.ne 3f\n"
        "    mrs x10, fpcr\n    cmp x10, x12\n    b.ne 3f\n"
        "    subs x13, x13, #1\n    b.ne 2b\n"
        "    mov %[ok], #1\n    b 4f\n"
        "3:  mov %[ok], #0\n"
        "4:  msr fpcr, xzr\n"
        :[ok]"=r"(ok):[seed]"r"(seed),[mode]"r"(mode),[rounds]"r"(rounds)
        :"x0","x8","x9","x10","x12","x13","v16","memory","cc");
    return ok;
}
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
static unsigned char stacks[2][32768] __attribute__((aligned(16)));
static int join_word[2];
static U results[2];
static void worker(void *arg) { U i=(U)arg;results[i]=survives(0x1111111111111111UL*(i+3),i+1,200); }
static void join(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,128,(U)v,0,0,0); }
void _start(void) {
    for(U i=0;i<2;i++)check(spawn(FLAGS,stacks[i]+sizeof(stacks[i]),&join_word[i],0,&join_word[i],worker,(void *)i)>0);
    check(survives(0x0123456789abcdefUL,3,200));               /* main, interleaved with two threads */
    for(int i=0;i<2;i++)join(&join_word[i]);
    check(results[0]==1 && results[1]==1);
    long child=call(220,17,0,0,0,0,0);check(child>=0);
    if(!child)call(94,survives(0xfedcba9876543210UL,2,100)?0:9,0,0,0,0,0);
    check(survives(0x5555aaaa5555aaaaUL,1,100));               /* parent, interleaved with the child */
    int status=-1;check(call(260,(U)child,(U)&status,0,0,0,0)==child && status==0);
    call(64,1,(U)"fp-preserve PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
