/* EPOLLET, EPOLLRDHUP and EPOLLONESHOT on a loopback TCP pair and a pipe.
   Positive waits use a short timeout so loopback delivery has settled. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"epoll-et FAIL\n",14,0,0,0);call(93,(U)checks,0,0,0,0,0);for(;;){} } }
#define IN 1u
#define OUT 4u
#define RDHUP 0x2000u
#define ONESHOT (1u<<30)
#define ET (1u<<31)
static long ep;
static U out[2];
static void ctl(long op,long fd,unsigned events,U data) { U ev[2]={events,data};check(call(21,(U)ep,(U)op,(U)fd,(U)ev,0,0)==0); }
static long wait_ms(long ms) { out[0]=out[1]=0;return call(22,(U)ep,(U)out,1,(U)ms,0,0); }
/* Loopback delivery is not instantaneous: wait up to a second to read. */
static long read_some(long fd,char *buf) {
    struct { int fd;short events,revents; } p={(int)fd,1,0};U ts[2]={1,0};
    call(73,(U)&p,1,(U)ts,0,0,0);
    return call(63,(U)fd,(U)buf,16,0,0,0);
}
static void pair(long *client,long *server) {
    unsigned char addr[16]={2,0,0,0,127,0,0,1};unsigned size=16;
    long l=call(198,2,1,0,0,0,0);check(l>=0);
    check(call(200,l,(U)addr,16,0,0,0)==0 && call(204,l,(U)addr,(U)&size,0,0,0)==0 && call(201,l,1,0,0,0,0)==0);
    *client=call(198,2,1|2048,0,0,0,0);check(*client>=0);
    long r=call(203,*client,(U)addr,16,0,0,0);check(r==0 || r==-115);
    *server=call(242,l,0,0,2048,0,0);check(*server>=0);check(call(57,l,0,0,0,0,0)==0);
}
void _start(void) {
    char buf[16];long c,a;
    ep=call(20,0,0,0,0,0,0);check(ep>=0);
    pair(&c,&a);
    ctl(1,a,IN|ET|RDHUP,1);
    check(wait_ms(0)==0);
    check(call(64,c,(U)"abc",3,0,0,0)==3);
    check(wait_ms(200)==1 && out[0]==IN && out[1]==1);               /* new data: edge */
    check(wait_ms(0)==0);                                            /* unread, no new data: no edge */
    check(call(64,c,(U)"de",2,0,0,0)==2);
    check(wait_ms(200)==1 && (out[0]&IN));                           /* more data: edge */
    check(call(63,a,(U)buf,16,0,0,0)==5 && call(63,a,(U)buf,16,0,0,0)==-11);
    check(wait_ms(0)==0);                                            /* drained */
    check(call(64,c,(U)"f",1,0,0,0)==1);
    check(wait_ms(200)==1 && (out[0]&IN));                           /* data after drain: edge */
    check(call(63,a,(U)buf,16,0,0,0)==1);

    ctl(3,a,OUT|ET,2);                                               /* MOD: fresh edge state */
    check(wait_ms(0)==1 && out[0]==OUT && out[1]==2);
    check(wait_ms(0)==0);                                            /* still writable: no edge */
    check(call(64,a,(U)"g",1,0,0,0)==1);
    check(wait_ms(0)==1 && out[0]==OUT);                             /* after a write: edge */
    check(read_some(c,buf)==1);

    ctl(3,a,IN|ONESHOT,3);                                           /* level-triggered oneshot */
    check(call(64,c,(U)"h",1,0,0,0)==1);
    check(wait_ms(200)==1 && out[1]==3);
    check(wait_ms(0)==0);                                            /* disarmed though readable */
    ctl(3,a,IN|ONESHOT,4);
    check(wait_ms(0)==1 && out[1]==4);                               /* MOD re-arms */
    check(call(63,a,(U)buf,16,0,0,0)==1);

    ctl(3,a,IN|ET|RDHUP,5);
    check(call(57,c,0,0,0,0,0)==0);                                  /* peer closes */
    check(wait_ms(200)==1 && (out[0]&IN) && (out[0]&RDHUP) && out[1]==5);
    check(read_some(a,buf)==0);                                       /* EOF */
    ctl(2,a,0,0);check(call(57,a,0,0,0,0,0)==0);

    unsigned p[2];check(call(59,(U)p,2048,0,0,0,0)==0);
    ctl(1,p[0],IN|ET,6);
    check(wait_ms(0)==0);
    check(call(64,p[1],(U)"x",1,0,0,0)==1);
    check(wait_ms(0)==1 && out[1]==6);
    check(wait_ms(0)==0);
    check(call(64,p[1],(U)"y",1,0,0,0)==1);
    check(wait_ms(0)==1 && out[1]==6);                               /* more pipe data: edge */

    call(64,1,(U)"epoll-et PASS\n",14,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
