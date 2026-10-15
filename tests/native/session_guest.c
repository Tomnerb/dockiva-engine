/* setsid: the initial process (already a group leader) gets EPERM; a forked child
   becomes a session leader whose getsid is its own pid, and cannot do it twice.
   A child that only starts its own process group stays in its parent's session
   (MSBuild matches its nodes by session id). */
typedef unsigned long U;
static long call(U n,U a,U b,U c) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return (long)x0;
}
static void check(int ok,int code) { if(!ok) { call(64,1,(U)"session FAIL\n",13);call(94,(U)code,0,0);for(;;){} } }
void _start(void) {
    check(call(157,0,0,0)==-1,1);
    long child=call(220,17,0,0);check(child>=0,2);
    if(!child) { long pid=call(172,0,0,0);call(94,call(157,0,0,0)==pid && call(156,0,0,0)==pid && call(157,0,0,0)==-1?0:9,0,0);for(;;){} }
    int status=-1;check(call(260,(U)child,(U)&status,0)==child && status==0,3);
    long session=call(156,0,0,0);check(session>0,4);
    child=call(220,17,0,0);check(child>=0,5);
    if(!child) { long pid=call(172,0,0,0);call(94,call(154,0,0,0)==0 && call(155,0,0,0)==pid && call(156,0,0,0)==session?0:9,0,0);for(;;){} }
    status=-1;check(call(260,(U)child,(U)&status,0)==child && status==0,6);
    call(64,1,(U)"session PASS\n",13);call(94,0,0,0);for(;;){}
}
