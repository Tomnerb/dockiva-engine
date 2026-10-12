#include <stddef.h>
typedef unsigned long U;
struct event { unsigned events;U data; };
_Static_assert(sizeof(struct event)==16 && offsetof(struct event,data)==8,"ARM64 epoll ABI");
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}

static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }
static long ctl(long e,long op,long fd,struct event *v) { return call(21,e,op,fd,(U)v,0,0); }
static long wait(long e,struct event *v,long max,long ms) { return call(22,e,(U)v,max,ms,0,8); }
void _start(void) {
    int pipe[2];struct event v={1,0x12345678abcdef01UL},out[2];char c='x';
    check(call(20,1,0,0,0,0,0)==-22);
    long e=call(20,524288,0,0,0,0,0);check(e>=0);
    check(call(25,e,1,0,0,0,0)==1);
    check(call(25,e,3,0,0,0,0)==2);
    long alias=call(23,e,0,0,0,0,0);check(alias>=0);
    check(call(57,e,0,0,0,0,0)==0);e=alias;
    check(call(59,(U)pipe,2048,0,0,0,0)==0);
    check(ctl(e,1,pipe[0],&v)==0);
    check(ctl(e,1,pipe[0],&v)==-17);
    check(wait(e,out,0,0)==-22);
    check(wait(e,out,2,5)==0);
    check(call(64,pipe[1],(U)&c,1,0,0,0)==1);
    check(wait(e,out,2,0)==1 && out[0].events==1 && out[0].data==v.data);
    check(wait(e,out,2,0)==1);
    check(wait(e,0,1,0)==-14);
    v.data=99;check(ctl(e,3,pipe[0],&v)==0);
    check(wait(e,out,1,0)==1 && out[0].data==99);
    long r=call(23,pipe[0],0,0,0,0,0);check(r>=0);
    v.data=100;check(ctl(e,1,r,&v)==0);
    check(wait(e,out,1,0)==1);U first=out[0].data;
    check(wait(e,out,1,0)==1 && out[0].data!=first);
    check(call(57,pipe[0],0,0,0,0,0)==0);
    check(wait(e,out,2,0)==2);
    check(call(63,r,(U)&c,1,0,0,0)==1);
    check(wait(e,out,2,0)==0);
    check(call(57,pipe[1],0,0,0,0,0)==0);
    check(wait(e,out,2,0)==2 && out[0].events==16 && out[1].events==16);
    check(ctl(e,2,r,0)==0);
    check(ctl(e,2,r,0)==-2);
    check(call(57,r,0,0,0,0,0)==0);
    check(wait(e,out,2,0)==0);
    check(call(59,(U)pipe,2048,0,0,0,0)==0);
    v.events=4;check(ctl(e,1,pipe[1],&v)==0);
    check(wait(e,out,2,0)==1 && out[0].events==4);
    check(call(57,pipe[0],0,0,0,0,0)==0);
    check(wait(e,out,2,0)==1 && (out[0].events&8));
    check(call(57,pipe[1],0,0,0,0,0)==0);
    check(wait(e,out,2,0)==0);
    check(call(57,e,0,0,0,0,0)==0);
    check(wait(e,out,2,0)==-9);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
