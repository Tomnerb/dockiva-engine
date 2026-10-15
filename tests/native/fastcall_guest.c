/* The hypervisor backend answers getpid, gettid and the uid/gid getters in its EL1
   vector, from values the host publishes in the vCPU's frame, and every other
   call through the frame the vector saves. Those answers must be exactly the
   host's: after fork and clone, after setresuid (also for a sibling thread already
   running guest code on another vCPU), and once a seccomp filter takes over. Every
   register but x0, the flags, SP and TPIDR_EL0 survive each kind of call. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static long call0(U n) { return call(n,0,0,0,0,0,0); }
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
/* regs(n, a0, a1): x2..x30 (x8 = n), NZCV and TPIDR_EL0 set to patterns, svc with
   x0 = a0 and x1 = a1, then the number of them that changed (SP too). */
long regs(U n,U a0,U a1);
__asm__(".global regs\nregs:\n"
        "    stp x29, x30, [sp, #-112]!\n    stp x19, x20, [sp, #16]\n    stp x21, x22, [sp, #32]\n    stp x23, x24, [sp, #48]\n    stp x25, x26, [sp, #64]\n"
        "    stp x27, x28, [sp, #80]\n    mrs x9, tpidr_el0\n    stp x9, x0, [sp, #96]\n    mov x8, x0\n    mov x0, x1\n"
        "    mov x1, x2\n    mov x9, sp\n    msr tpidr_el0, x9\n    mov x2, #0x1222\n    mov x3, #0x1333\n"
        "    mov x4, #0x1444\n    mov x5, #0x1555\n    mov x6, #0x1666\n    mov x7, #0x1777\n    mov x9, #0x1999\n"
        "    mov x10, #0x1aaa\n    mov x11, #0x1bbb\n    mov x12, #0x1ccc\n    mov x13, #0x1ddd\n    mov x14, #0x1eee\n"
        "    mov x15, #0x1fff\n    mov x16, #0x2110\n    mov x17, #0x2221\n    mov x18, #0x2332\n    mov x19, #0x2443\n"
        "    mov x20, #0x2554\n    mov x21, #0x2665\n    mov x22, #0x2776\n    mov x23, #0x2887\n    mov x24, #0x2998\n"
        "    mov x25, #0x2aa9\n    mov x26, #0x2bba\n    mov x27, #0x2ccb\n    mov x28, #0x2ddc\n    mov x29, #0x2eed\n"
        "    mov x30, #0x2ffe\n    movk x22, #0x22, lsl #48\n    mov x15, #0xa0000000\n    msr nzcv, x15\n    mov x15, #0x1fff\n"
        "    svc #0\n    mrs x0, nzcv\n    mov x1, #0xa0000000\n    cmp x0, x1\n    cset x0, ne\n"
        "    mov x1, #0x1222\n    cmp x2, x1\n    cinc x0, x0, ne\n    mov x1, #0x1333\n    cmp x3, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x1444\n    cmp x4, x1\n    cinc x0, x0, ne\n    mov x1, #0x1555\n"
        "    cmp x5, x1\n    cinc x0, x0, ne\n    mov x1, #0x1666\n    cmp x6, x1\n    cinc x0, x0, ne\n"
        "    mov x1, #0x1777\n    cmp x7, x1\n    cinc x0, x0, ne\n    mov x1, #0x1999\n    cmp x9, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x1aaa\n    cmp x10, x1\n    cinc x0, x0, ne\n    mov x1, #0x1bbb\n"
        "    cmp x11, x1\n    cinc x0, x0, ne\n    mov x1, #0x1ccc\n    cmp x12, x1\n    cinc x0, x0, ne\n"
        "    mov x1, #0x1ddd\n    cmp x13, x1\n    cinc x0, x0, ne\n    mov x1, #0x1eee\n    cmp x14, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x1fff\n    cmp x15, x1\n    cinc x0, x0, ne\n    mov x1, #0x2110\n"
        "    cmp x16, x1\n    cinc x0, x0, ne\n    mov x1, #0x2221\n    cmp x17, x1\n    cinc x0, x0, ne\n"
        "    mov x1, #0x2332\n    cmp x18, x1\n    cinc x0, x0, ne\n    mov x1, #0x2443\n    cmp x19, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x2554\n    cmp x20, x1\n    cinc x0, x0, ne\n    mov x1, #0x2665\n"
        "    cmp x21, x1\n    cinc x0, x0, ne\n    mov x1, #0x2776\n    movk x1, #0x22, lsl #48\n    cmp x22, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x2887\n    cmp x23, x1\n    cinc x0, x0, ne\n    mov x1, #0x2998\n"
        "    cmp x24, x1\n    cinc x0, x0, ne\n    mov x1, #0x2aa9\n    cmp x25, x1\n    cinc x0, x0, ne\n"
        "    mov x1, #0x2bba\n    cmp x26, x1\n    cinc x0, x0, ne\n    mov x1, #0x2ccb\n    cmp x27, x1\n"
        "    cinc x0, x0, ne\n    mov x1, #0x2ddc\n    cmp x28, x1\n    cinc x0, x0, ne\n    mov x1, #0x2eed\n"
        "    cmp x29, x1\n    cinc x0, x0, ne\n    mov x1, #0x2ffe\n    cmp x30, x1\n    cinc x0, x0, ne\n"
        "    ldr x1, [sp, #104]\n    cmp x8, x1\n    cinc x0, x0, ne\n    mrs x1, tpidr_el0\n    mov x9, sp\n"
        "    cmp x1, x9\n    cinc x0, x0, ne\n    ldr x9, [sp, #96]\n    msr tpidr_el0, x9\n    ldp x19, x20, [sp, #16]\n"
        "    ldp x21, x22, [sp, #32]\n    ldp x23, x24, [sp, #48]\n    ldp x25, x26, [sp, #64]\n    ldp x27, x28, [sp, #80]\n    ldp x29, x30, [sp], #112\n"
        "    ret\n");
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"fastcall FAIL\n",14,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000) /* VM FS FILES SIGHAND THREAD SYSVSEM PARENT_SETTID CHILD_CLEARTID */
static unsigned char stacks[2][32768] __attribute__((aligned(16)));
static int join_word[2];
static volatile long seen_tid,seen_pid,seen_set_tid,ready,go,seen_euid,seen_euid_before;
static void join(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,128,(U)v,0,0,0); }
static void identify(void *arg) {
    (void)arg;seen_tid=call0(178);seen_pid=call0(172);
    seen_set_tid=call(96,(U)&join_word[0],0,0,0,0,0);         /* the host's answer: the caller's tid */
}
static void watcher(void *arg) {                               /* spins in guest code while the IDs change */
    (void)arg;seen_euid_before=call0(175);
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&go,__ATOMIC_ACQUIRE)) {}
    seen_euid=call0(175);
}
static void ids(void) {                                        /* the getters agree with getresuid/getresgid */
    unsigned r[3],g[3];
    check(call(148,(U)&r[0],(U)&r[1],(U)&r[2],0,0,0)==0 && call(150,(U)&g[0],(U)&g[1],(U)&g[2],0,0,0)==0);
    check(call0(174)==r[0] && call0(175)==r[1] && call0(176)==g[0] && call0(177)==g[1]);
}
void _start(void) {
    long pid=call0(172);
    check(pid>0 && call0(178)==pid && call(96,0,0,0,0,0,0)==pid); /* set_tid_address answers on the host */
    ids();
    /* Registers survive the vector's answer, an unlocked call, a locked one and a sleep. */
    check(regs(172,0,0)==0 && regs(174,0,0)==0 && regs(173,0,0)==0 && regs(62,1,0)==0 && regs(113,1,(U)stacks[1])==0);
    U nap[2]={0,1000};check(regs(101,(U)nap,0)==0 && regs(124,0,0)==0 && regs(160,(U)stacks[1],0)==0);
    /* fork: the child's own answers match the parent's view of it */
    int pipe[2];check(call(59,(U)pipe,0,0,0,0,0)==0);
    long child=call(220,17,0,0,0,0,0);check(child>=0);
    if(!child) {
        long me[4]={call0(172),call0(178),call0(173),call(96,0,0,0,0,0,0)};
        call(64,(U)pipe[1],(U)me,sizeof(me),0,0,0);call(93,0,0,0,0,0,0);
    }
    long got[4]={0,0,0,0};check(call(63,(U)pipe[0],(U)got,sizeof(got),0,0,0)==sizeof(got));
    check(got[0]==child && got[1]==child && got[2]==pid && got[3]==child);
    int status=-1;check(call(260,(U)child,(U)&status,0,0,0,0)==child && status==0);
    check(call0(172)==pid && call0(178)==pid);
    /* clone: a thread's tid and pid */
    long tid=spawn(FLAGS,stacks[0]+sizeof(stacks[0]),&join_word[0],0,&join_word[0],identify,0);
    check(tid>pid);join(&join_word[0]);
    check(seen_tid==tid && seen_set_tid==tid && seen_pid==pid);
    /* setresuid while a sibling runs guest code: it sees the new effective uid at once */
    check(spawn(FLAGS,stacks[1]+sizeof(stacks[1]),&join_word[1],0,&join_word[1],watcher,0)>0);
    while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE)) {}
    check(call(147,(U)-1,500,(U)-1,0,0,0)==0 && call0(175)==500);
    __atomic_store_n(&go,1,__ATOMIC_RELEASE);join(&join_word[1]);
    check(seen_euid_before==0 && seen_euid==500);
    ids();
    check(call(147,(U)-1,0,(U)-1,0,0,0)==0 && call0(175)==0 && call(149,(U)-1,600,(U)-1,0,0,0)==0 && call0(177)==600);
    ids();
    check(call(149,(U)-1,0,(U)-1,0,0,0)==0 && call0(177)==0);
    /* A seccomp filter sees every call: getuid now fails with EPERM, getpid still answers. */
    static const U filter[4]={0x0000000000000020UL,0x000000ae00010015UL,0x7fff000000000006UL,0x0005000100000006UL};
    /* ld [nr]; jeq #174, 1, 0; ret ALLOW; ret ERRNO|1 */
    U program[2]={4,(U)filter};
    check(call(167,38,1,0,0,0,0)==0 && call(277,1,0,(U)program,0,0,0)==0);
    check(call0(174)==-1 && call0(172)==pid && call0(175)==0);
    call(64,1,(U)"fastcall PASS\n",14,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
