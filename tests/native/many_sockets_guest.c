/* RLIMIT_NOFILE is 4096; 100 sockets take the lowest free fds 3..102; one epoll watches
   99 connections and reports all of them writable; closing frees the low fds. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"many-sockets FAIL\n",18,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static long fds[100];
static U events[128*2];
void _start(void) {
    U limit[2]={0,0};check(call(261,0,7,0,(U)limit,0,0)==0 && limit[0]==4096 && limit[1]==4096);
    unsigned char addr[16]={2,0,0,0,127,0,0,1};U size=16;
    for(int i=0;i<100;i++) { fds[i]=call(198,2,1,0,0,0,0);check(fds[i]==3+i); }
    check(call(200,fds[0],(U)addr,16,0,0,0)==0 && call(201,fds[0],128,0,0,0,0)==0);
    check(call(204,fds[0],(U)addr,(U)&size,0,0,0)==0);                       /* the listener's port */
    long ep=call(20,0,0,0,0,0,0);check(ep>=0);
    for(int i=1;i<100;i++) {
        check(call(203,fds[i],(U)addr,16,0,0,0)==0);
        U event[2]={4,(U)i};check(call(21,ep,1,fds[i],(U)event,0,0)==0);    /* EPOLLOUT */
    }
    check(call(22,ep,(U)events,128,1000,0,0)==99);
    for(int i=0;i<100;i++)check(call(57,fds[i],0,0,0,0,0)==0);
    check(call(198,2,1,0,0,0,0)==3);
    call(64,1,(U)"many-sockets PASS\n",18,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
