typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,2,(U)&checks,4,0,0,0);call(94,99,0,0,0,0,0);for(;;){} } }
static void finish(unsigned status) { call(94,status,0,0,0,0,0);for(;;){} }
static volatile unsigned value=11;
void _start(void) {
    unsigned pipe[2],status=0,ptid=0,ctid=0;
    long root=call(172,0,0,0,0,0,0);
    check(call(59,(U)pipe,0,0,0,0,0)==0);
    long epoll=call(20,0,0,0,0,0,0);check(epoll>=0);
    U event[2]={1,123};check(call(21,epoll,1,pipe[0],(U)event,0,0)==0);
    /* Socket description + epoll are inherited across fork. */
    long sock=call(198,2,2049,0,0,0,0);check(sock>=0);
    static const char path[]="/shared";
    long file=call(56,(U)-100,(U)path,66,0600,0,0);check(file>=0);
    check(call(64,file,(U)"AB",2,0,0,0)==2);check(call(62,file,0,0,0,0,0)==0);
    long pid=call(220,0x1300011,0,(U)&ptid,0,(U)&ctid,0);check(pid>=0);
    if(!pid) {
        check(call(172,0,0,0,0,0,0)==ctid);check(call(178,0,0,0,0,0,0)==ctid);
        check(call(173,0,0,0,0,0,0)==root);check(value==11 && ptid==0);
        value=22;
        char from_file=0;check(call(63,file,(U)&from_file,1,0,0,0)==1 && from_file=='A');
        check(call(57,file,0,0,0,0,0)==0);
        check(call(25,sock,4,2,0,0,0)==0); /* Shared O_NONBLOCK status. */
        check(call(25,sock,2,1,0,0,0)==0); /* Private CLOEXEC flag. */
        check(call(57,sock,0,0,0,0,0)==0);
        check(call(57,pipe[0],0,0,0,0,0)==0);
        check(call(57,epoll,0,0,0,0,0)==0);
        char byte='x';check(call(64,pipe[1],(U)&byte,1,0,0,0)==1);
        long nested=call(220,17,0,0,0,0,0);check(nested>=0);
        if(!nested) { check(value==22);value=33;finish(9); }
        check(call(260,nested,(U)&status,0,0,0,0)==nested && status==(9<<8));
        check(value==22);finish(7);
    }
    check(ptid==pid && ctid==0 && value==11);
    check(call(57,pipe[1],0,0,0,0,0)==0);
    check(call(22,epoll,(U)event,1,1000,0,8)==1 && event[1]==123);
    char byte=0;check(call(63,pipe[0],(U)&byte,1,0,0,0)==1 && byte=='x');
    check(call(260,pid,(U)&status,0,0,0,0)==pid && status==(7<<8));
    check(value==11 && ctid==0);
    char from_file=0;check(call(63,file,(U)&from_file,1,0,0,0)==1 && from_file=='B');
    check(call(57,file,0,0,0,0,0)==0);
    check(call(25,sock,3,0,0,0,0)==2);check(call(25,sock,1,0,0,0,0)==0);
    check(call(57,sock,0,0,0,0,0)==0);
    check(call(63,pipe[0],(U)&byte,1,0,0,0)==0);
    check(call(260,pid,(U)&status,0,0,0,0)==-10);
    pid=call(220,17,0,0,0,0,0);check(pid>=0);
    if(!pid) { for(;;)call(124,0,0,0,0,0,0); }
    check(call(260,pid,(U)&status,1,0,0,0)==0);
    check(call(129,pid,9,0,0,0,0)==0);
    check(call(260,pid,(U)&status,0,0,0,0)==pid && status==9);
    /* vfork: the parent waits until the child exits or execs. */
    pid=call(220,0x4111,0,0,0,0,0);check(pid>=0);
    if(!pid)finish(5);
    check(call(260,pid,(U)&status,0,0,0,0)==pid && status==(5<<8));
    /* At most 63 live children (PROCESS_LIMIT); the bound reports EAGAIN.
       Reaped slots are reused. */
    static long children[64];unsigned live=0;
    while(live<64) {
        children[live]=call(220,17,0,0,0,0,0);
        if(children[live]==-11)break;
        check(children[live]>=0);
        if(!children[live])for(;;)call(73,0,0,0,0,0,0);   /* ppoll(NULL,0,NULL): block until killed */
        live++;
    }
    check(live==63);
    for(unsigned i=0;i<live;i++) {
        check(call(129,children[i],9,0,0,0,0)==0);
        check(call(260,children[i],(U)&status,0,0,0,0)==children[i] && status==9);
    }
    /* No lifetime cap: forty more fork/reap cycles all succeed. */
    for(unsigned i=0;i<40;i++) {
        pid=call(220,17,0,0,0,0,0);check(pid>=0);if(!pid)finish(0);
        check(call(260,pid,(U)&status,0,0,0,0)==pid && status==0);
    }
    static const char ok[]="fork PASS\n";call(64,1,(U)ok,sizeof(ok)-1,0,0,0);finish(0);
}
