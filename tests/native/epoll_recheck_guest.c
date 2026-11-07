/* An edge-triggered epoll_wait whose retry finds the socket's edge already taken
   (Go's netpoller: one thread blocks in epoll_wait while others poll with timeout 0):
   - thread T blocks in epoll_wait(3000 ms) on an empty socket with EPOLLIN|EPOLLET;
   - the main thread makes it readable and takes that edge itself (epoll_wait 0), so T,
     woken, finds nothing new and waits again without the socket's POLLIN, rechecking
     within 10 ms;
   - 50 ms later more data arrives (a new edge): T must report it within a second.
   Before net_suspend let that retry's 10 ms bound replace the call's first deadline, T
   slept out the whole 3 s (Go: minutes, while a connection's data piled up unread).
   Under parallel workers T may take the first edge itself; the round then proves
   nothing but must still pass. Exit 0 and "epoll-recheck PASS" when every round did. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="epoll-recheck FAIL xx\n";m[19]=(char)('0'+code/10);m[20]=(char)('0'+code%10);
    call(64,1,(U)m,22,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
static U now_ms(void) { U ts[2];if(call(113,1,(U)ts,0,0,0,0))fail(1);return ts[0]*1000+ts[1]/1000000; }   /* CLOCK_MONOTONIC */
static void sleep_ms(U ms) { U ts[2]={ms/1000,(ms%1000)*1000000};call(101,(U)ts,0,0,0,0,0); }
/* clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|PARENT_SETTID|CHILD_CLEARTID): the
   new thread finds its function and argument at the top of its stack. */
static long spawn(U *top,volatile unsigned *tid,void (*fn)(U),U arg) {
    top-=2;top[0]=(U)fn;top[1]=arg;
    register U x8 __asm__("x8")=220,x0 __asm__("x0")=0x10f00|0x40000|0x100000|0x200000,x1 __asm__("x1")=(U)top;
    register U x2 __asm__("x2")=(U)tid,x3 __asm__("x3")=0,x4 __asm__("x4")=(U)tid;
    __asm__ volatile("svc #0\n cbnz x0,1f\n ldp x9,x0,[sp]\n blr x9\n mov x0,#0\n mov x8,#93\n svc #0\n1:"
                     :"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory","cc","x9","x30");
    return (long)x0;
}
static void join(volatile unsigned *tid) { for(unsigned t;(t=__atomic_load_n(tid,__ATOMIC_ACQUIRE));)call(98,(U)tid,0,t,0,0,0); }
static U stack[4096] __attribute__((aligned(16)));
static volatile unsigned waiter_tid;
static volatile U returned_at;
static int ep;
static void waiter(U unused) {
    (void)unused;U events[8];
    for(;;) {                                        /* 0 is allowed early (the recheck); an event ends it */
        long n=call(22,(U)ep,(U)events,4,3000,0,0);
        if(n<0)call(93,20,0,0,0,0,0);
        if(n>0)break;
    }
    returned_at=now_ms();
}
void _start(void) {
    for(unsigned round=0;round<10;round++) {
        int pair[2];
        if(call(199,1,1|2048,0,(U)pair,0,0))fail(2);              /* socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK) */
        ep=(int)call(20,0,0,0,0,0,0);if(ep<0)fail(3);
        U event[2]={1u|(1u<<31),7};                              /* EPOLLIN|EPOLLET, data 7 */
        if(call(21,(U)ep,1,(U)pair[0],(U)event,0,0))fail(4);
        returned_at=0;
        if(spawn(&stack[4096],&waiter_tid,waiter,0)<=0)fail(5);
        sleep_ms(20);                                            /* T blocks: nothing to read */
        if(call(64,(U)pair[1],(U)"a",1,0,0,0)!=1)fail(6);
        U events[8];call(22,(U)ep,(U)events,4,0,0,0);            /* take the edge before T looks again */
        sleep_ms(50);
        U sent=now_ms();
        if(call(64,(U)pair[1],(U)"b",1,0,0,0)!=1)fail(7);        /* a new edge */
        join(&waiter_tid);
        if(!returned_at)fail(8);
        if(returned_at>sent+1000)fail(10+round);                 /* slept to its 3 s deadline */
        call(57,(U)pair[0],0,0,0,0,0);call(57,(U)pair[1],0,0,0,0,0);call(57,(U)ep,0,0,0,0,0);
    }
    call(64,1,(U)"epoll-recheck PASS\n",19,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
