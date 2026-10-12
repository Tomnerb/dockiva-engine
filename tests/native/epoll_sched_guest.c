/* epoll under the interpreter's scheduler: a parent blocked in epoll_wait must
   let other guest processes run. A child writes a pipe, then connects to the
   parent's listener; both arrive through an epoll instance inherited by fork. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"epoll-sched FAIL\n",17,0,0,0);call(93,(U)checks,0,0,0,0,0);for(;;){} } }
void _start(void) {
    unsigned char addr[16]={2,0,0,0,0,0,0,0};unsigned size=16;unsigned pipe[2];
    long server=call(198,2,1|2048,0,0,0,0);check(server>=0);        /* nonblocking listener */
    check(call(200,server,(U)addr,16,0,0,0)==0 && call(204,server,(U)addr,(U)&size,0,0,0)==0);
    check(call(201,server,8,0,0,0,0)==0);
    check(call(59,(U)pipe,0,0,0,0,0)==0);
    long ep=call(20,0,0,0,0,0,0);check(ep>=0);
    U event[2]={1,1};check(call(21,ep,1,(U)server,(U)event,0,0)==0);
    event[1]=2;check(call(21,ep,1,pipe[0],(U)event,0,0)==0);
    U out[2]={0,0};
    check(call(22,ep,(U)out,1,50,0,0)==0);                           /* timeout with nothing ready */
    long pid=call(220,17,0,0,0,0,0);check(pid>=0);
    if(!pid) {
        char b='p';check(call(64,pipe[1],(U)&b,1,0,0,0)==1);
        long c=call(198,2,1,0,0,0,0);check(c>=0 && call(203,c,(U)addr,16,0,0,0)==0);
        char r=0;check(call(63,c,(U)&r,1,0,0,0)==1 && r=='s');
        call(93,0,0,0,0,0,0);
    }
    check(call(22,ep,(U)out,1,-1,0,0)==1 && out[1]==2);              /* blocks until the child writes */
    char b=0;check(call(63,pipe[0],(U)&b,1,0,0,0)==1 && b=='p');
    check(call(22,ep,(U)out,1,-1,0,0)==1 && out[1]==1);              /* then until it connects */
    long conn=call(202,server,0,0,0,0,0);check(conn>=0);
    char s='s';check(call(64,conn,(U)&s,1,0,0,0)==1);
    int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    call(64,1,(U)"epoll-sched PASS\n",17,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
