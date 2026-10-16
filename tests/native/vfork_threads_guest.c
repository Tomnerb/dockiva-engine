/* vfork (CLONE_VM|CLONE_VFORK) from a threaded process, as Go's os/exec and
   posix_spawn do it: the child shares the parent's memory in place (its writes are
   the parent's, it sees a sibling thread's writes as they happen), only the calling
   thread waits for the child's exec or exit, and the parent's other threads run on
   meanwhile, mapping and faulting memory. The child outlives a parent that exits
   while it waits, and a sibling's exec in the parent waits until the child left
   the memory. Run as /vfork_threads in a writable root: it execs itself. */
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
__asm__(".global _start\n_start:\n    mov x0, sp\n    bl start\n");
#define THREAD (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000) /* VM FS FILES SIGHAND THREAD SYSVSEM PARENT_SETTID CHILD_CLEARTID */
#define VFORK (0x4000|0x100|17)                                              /* CLONE_VFORK|CLONE_VM|SIGCHLD */
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { out("vfork-threads FAIL\n");call(94,100+checks,0,0,0,0,0);for(;;){} } }
static U now_ns(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000000000UL+ts[1]; }
static unsigned char stacks[16][16384] __attribute__((aligned(16)));
static int join_word[8];
static volatile long ticks,stop,shared_word,child_done,sibling_maps,leaf_status;
static void join(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,128,(U)v,0,0,0); }
static void ticker(void *arg) { (void)arg;while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) { __atomic_add_fetch(&ticks,1,__ATOMIC_RELEASE);call(124,0,0,0,0,0,0); } }
/* Maps, writes (fresh pages fault) and unmaps memory while vfork children run. */
static void mapper(void *arg) {
    (void)arg;
    while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        long p=call(222,0,1<<20,3,0x22,(U)-1,0);if(p<0)continue;
        for(U i=0;i<(1<<20);i+=4096)((volatile char *)p)[i]=(char)i;
        call(215,(U)p,1<<20,0,0,0,0);__atomic_add_fetch(&sibling_maps,1,__ATOMIC_RELEASE);
    }
}
/* The child: it must see the sibling thread's ticks advance (a private copy never
   would), and leaves a word in the parent's memory. */
static void watch(void *arg) {
    (void)arg;long seen=__atomic_load_n(&ticks,__ATOMIC_ACQUIRE);U until=now_ns()+5000000000UL;
    while(__atomic_load_n(&ticks,__ATOMIC_ACQUIRE)<seen+3 && now_ns()<until)call(124,0,0,0,0,0,0);
    shared_word=__atomic_load_n(&ticks,__ATOMIC_ACQUIRE)>=seen+3?0x1234:0x666;
    U nap[2]={0,20000000};call(101,(U)nap,0,0,0,0,0);   /* the caller still waits meanwhile */
    child_done=1;call(93,0,0,0,0,0,0);
}
static const char *self="/vfork_threads";
static void exec_leaf(void *arg) { const char *argv[]={self,(const char *)arg,0};call(221,(U)self,(U)argv,0,0,0,0);call(93,9,0,0,0,0,0); }
/* Several threads vfork and exec at once while siblings map and fault. */
static void spawner(void *arg) {
    unsigned index=(unsigned)(U)arg;
    for(unsigned i=0;i<40;i++) {
        long pid=spawn(VFORK,stacks[8+index]+sizeof(stacks[0]),0,0,0,exec_leaf,(void *)"leaf");
        int status=-1;
        if(pid<=0 || call(260,(U)pid,(U)&status,0,0,0,0)!=pid || status!=(7<<8)) { __atomic_store_n(&leaf_status,1,__ATOMIC_RELEASE);return; }
    }
}
static int pipe_fds[2];
static volatile long borrower_started;
static void slow_child(void *arg) {
    (void)arg;__atomic_store_n(&borrower_started,1,__ATOMIC_RELEASE);
    U nap[2]={0,50000000};call(101,(U)nap,0,0,0,0,0);
    shared_word=0x77;call(64,(U)pipe_fds[1],(U)"A",1,0,0,0);call(93,0,0,0,0,0,0);
}
static void exiter(void *arg) {                     /* the whole process exits while its vfork child runs */
    (void)arg;while(!__atomic_load_n(&borrower_started,__ATOMIC_ACQUIRE))call(124,0,0,0,0,0,0);
    call(94,3,0,0,0,0,0);
}
static void execer(void *arg) {                     /* exec while a vfork child still uses the memory */
    (void)arg;while(!__atomic_load_n(&borrower_started,__ATOMIC_ACQUIRE))call(124,0,0,0,0,0,0);
    char fd[2]={(char)('0'+pipe_fds[0]),0};const char *argv[]={self,"final",fd,0};
    call(221,(U)self,(U)argv,0,0,0,0);call(94,8,0,0,0,0,0);
}
void start(U *sp) {
    long argc=(long)sp[0];const char **argv=(const char **)(sp+1);
    if(argc>1 && argv[1][0]=='l')call(94,7,0,0,0,0,0);                         /* leaf */
    if(argc>2 && argv[1][0]=='f') {                                            /* final: after the waiting exec */
        char c=0;check(call(63,(U)(argv[2][0]-'0'),(U)&c,1,0,0,0)==1 && c=='A');
        int status=-1;check(call(260,(U)-1,(U)&status,0,0,0,0)>0 && status==0); /* the vfork child, ours now */
        out("vfork-threads PASS\n");call(94,0,0,0,0,0,0);
    }
    /* 1. The child shares memory with the running siblings; only the caller waits. */
    check(spawn(THREAD,stacks[0]+sizeof(stacks[0]),&join_word[0],0,&join_word[0],ticker,0)>0);
    check(spawn(THREAD,stacks[1]+sizeof(stacks[1]),&join_word[1],0,&join_word[1],mapper,0)>0);
    while(__atomic_load_n(&ticks,__ATOMIC_ACQUIRE)<3)call(124,0,0,0,0,0,0);
    for(unsigned round=0;round<20;round++) {
        shared_word=0;child_done=0;
        long pid=spawn(VFORK,stacks[2]+sizeof(stacks[2]),0,0,0,watch,0);
        check(pid>0 && child_done==1 && shared_word==0x1234);                 /* returned after the child, with its write */
        int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    }
    /* 2. vfork and exec from several threads at once, siblings faulting meanwhile. */
    for(unsigned i=0;i<4;i++)check(spawn(THREAD,stacks[3+i]+sizeof(stacks[0]),&join_word[2+i],0,&join_word[2+i],spawner,(void *)(U)i)>0);
    for(unsigned i=0;i<4;i++)join(&join_word[2+i]);
    check(!leaf_status && sibling_maps>0);
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);join(&join_word[0]);join(&join_word[1]);
    /* 3. The process exits while its vfork child runs: the child lives on. */
    check(call(59,(U)pipe_fds,0,0,0,0,0)==0);
    long q=call(220,17,0,0,0,0,0);check(q>=0);
    if(!q) {
        check(spawn(THREAD,stacks[0]+sizeof(stacks[0]),&join_word[0],0,&join_word[0],exiter,0)>0);
        spawn(VFORK,stacks[2]+sizeof(stacks[2]),0,0,0,slow_child,0);
        call(94,4,0,0,0,0,0);                                                  /* not reached: the sibling exits first */
    }
    int status=-1;check(call(260,(U)q,(U)&status,0,0,0,0)==q && status==(3<<8));
    char c=0;check(call(63,(U)pipe_fds[0],(U)&c,1,0,0,0)==1 && c=='A');
    check(call(260,(U)-1,(U)&status,0,0,0,0)>0 && status==0);                  /* the orphan, reparented to us */
    /* 4. A sibling execs while the vfork child runs; the new image reads its mark. */
    borrower_started=0;
    check(spawn(THREAD,stacks[0]+sizeof(stacks[0]),&join_word[0],0,&join_word[0],execer,0)>0);
    spawn(VFORK,stacks[2]+sizeof(stacks[2]),0,0,0,slow_child,0);
    for(;;)call(124,0,0,0,0,0,0);                                               /* the exec ends this thread */
}
