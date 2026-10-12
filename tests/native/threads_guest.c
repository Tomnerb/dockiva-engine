/* CLONE_THREAD threads as pthreads use them: shared memory, TIDs, futex join via
   CLONE_CHILD_CLEARTID, futex hand-off and timeout, a thread blocked in a pipe
   read while siblings run, and fork from a process that had threads. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
/* spawn(flags, stack, ptid, tls, ctid, fn, arg): the child runs fn(arg) on its stack, then exits. */
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"threads FAIL\n",13,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000) /* VM FS FILES SIGHAND THREAD SYSVSEM PARENT_SETTID CHILD_CLEARTID */
#define WAIT 128   /* FUTEX_WAIT|PRIVATE */
#define WAKE 129
static unsigned char stacks[3][32768] __attribute__((aligned(16)));
static int join_word[3];
static long tids[3];
static int counter,turn,pipefd[2];
static char got;
static void join(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,WAIT,(U)v,0,0,0); }
static void add(void *arg) {
    tids[(long)arg]=call(178,0,0,0,0,0,0);
    for(int i=0;i<1000;i++)__atomic_fetch_add(&counter,1,__ATOMIC_SEQ_CST);
}
static void waiter(void *arg) {
    (void)arg;
    while(__atomic_load_n(&turn,__ATOMIC_ACQUIRE)!=1)call(98,(U)&turn,WAIT,0,0,0,0);
    __atomic_store_n(&turn,2,__ATOMIC_RELEASE);call(98,(U)&turn,WAKE,1,0,0,0);
}
static void reader(void *arg) { (void)arg;check(call(63,(U)pipefd[0],(U)&got,1,0,0,0)==1); }
void _start(void) {
    long pid=call(172,0,0,0,0,0,0);
    check(call(178,0,0,0,0,0,0)==pid);                                     /* main tid == pid */
    for(long i=0;i<3;i++) {
        long tid=spawn(FLAGS,stacks[i]+sizeof(stacks[i]),&join_word[i],0,&join_word[i],add,(void *)i);
        check(tid>=65536);
    }
    for(int i=0;i<3;i++)join(&join_word[i]);
    check(counter==3000);
    check(tids[0]>=65536 && tids[0]!=tids[1] && tids[1]!=tids[2]);
    U timeout[2]={0,20000000};int still=5;
    check(call(98,(U)&still,WAIT,5,(U)timeout,0,0)==-110);                 /* futex timeout */
    check(spawn(FLAGS,stacks[0]+sizeof(stacks[0]),&join_word[0],0,&join_word[0],waiter,0)>0);
    __atomic_store_n(&turn,1,__ATOMIC_RELEASE);call(98,(U)&turn,WAKE,1,0,0,0);
    while(__atomic_load_n(&turn,__ATOMIC_ACQUIRE)!=2)call(98,(U)&turn,WAIT,1,0,0,0);
    join(&join_word[0]);
    check(call(59,(U)pipefd,0,0,0,0,0)==0);                               /* blocking pipe */
    check(spawn(FLAGS,stacks[1]+sizeof(stacks[1]),&join_word[1],0,&join_word[1],reader,0)>0);
    check(call(98,(U)&still,WAIT,5,(U)timeout,0,0)==-110);                 /* reader is blocked; main runs */
    check(got==0);
    check(call(64,(U)pipefd[1],(U)"z",1,0,0,0)==1);
    join(&join_word[1]);
    check(got=='z');
    long child=call(220,17,0,0,0,0,0);check(child>=0);
    if(!child) { call(94,call(178,0,0,0,0,0,0)==call(172,0,0,0,0,0,0)?7:8,0,0,0,0,0);for(;;){} }
    int status=-1;check(call(260,(U)child,(U)&status,0,0,0,0)==child && status==(7<<8));
    call(64,1,(U)"threads PASS\n",13,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
