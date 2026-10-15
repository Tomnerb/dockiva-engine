/* The memory budget counts what processes use, not what they could: 100 children
   exec /leaf (lazy_charge_leaf: a little brk heap and mmap memory, then it waits) in
   a 12 GiB mmap arena under --memory-limit-mib 256, all alive at once (oom_kill_guest
   checks that writing past the budget still kills). Needs --process-tree and a root
   holding /leaf. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"lazy-charge FAIL\n",17,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define CHILDREN 100
void _start(void) {
    int pipe[2];check(call(59,(U)pipe,0,0,0,0,0)==0 && pipe[0]==3);
    long pids[CHILDREN];
    for(unsigned i=0;i<CHILDREN;i++) {
        long pid=call(220,17,0,0,0,0,0);check(pid>=0);
        if(!pid) { const char *argv[]={"/leaf",0},*envp[]={0};call(57,(U)pipe[1],0,0,0,0,0);call(221,(U)"/leaf",(U)argv,(U)envp,0,0,0);call(93,127,0,0,0,0,0); }
        pids[i]=pid;
    }
    char text[128];long fd=call(56,(U)-100,(U)"/proc/loadavg",0,0,0,0);check(fd>=0);
    long n=call(63,(U)fd,(U)text,sizeof(text)-1,0,0,0);check(n>0);call(57,(U)fd,0,0,0,0,0);
    unsigned total=0;long at=0;while(at<n && text[at]!='/')at++;
    for(at++;at<n && text[at]>='0' && text[at]<='9';at++)total=total*10+(unsigned)(text[at]-'0');
    check(total>=CHILDREN+1);                                           /* all alive at once */
    call(57,(U)pipe[1],0,0,0,0,0);
    for(unsigned i=0;i<CHILDREN;i++) { int status=-1;check(call(260,(U)pids[i],(U)&status,0,0,0,0)==pids[i] && status==0); }
    call(64,1,(U)"lazy-charge PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
