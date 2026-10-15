/* Hardware TSO for x86 translators (FEX asks with prctl PR_SET_MEM_MODEL). A
   message-passing litmus test across two processes on two vCPUs: the writer stores
   x then y (plain STRs, separate cache lines), the reader loads y then x (plain
   LDRs). Seeing a newer y than x is allowed on Arm's weak model and impossible
   under TSO. Prints "weak N tso M" (reorderings seen in each model) and fails if
   TSO is offered but one is seen under it, or if the prctl contract is broken. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
#define PR_SET_MEM_MODEL 0x4d4d444cUL
#define PR_GET_MEM_MODEL 0x6d4d444cUL
#define ROUNDS 2000000UL
static void say(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void fail(const char *why) { say("tso FAIL: ");say(why);say("\n");call(94,1,0,0,0,0,0);for(;;){} }
static void number(U v) { char b[24];int i=23;b[i]=0;do { b[--i]=(char)('0'+v%10);v/=10; } while(v);say(b+i); }
static U litmus(volatile U *shared) {
    volatile U *x=shared,*y=shared+32;                     /* 256 bytes apart: different cache lines */
    *x=*y=0;
    long pid=call(220,17,0,0,0,0,0);                         /* fork: the child writes */
    if(pid<0)fail("fork");
    if(!pid) {
        if(call(167,PR_GET_MEM_MODEL,0,0,0,0,0)!=(long)shared[64])call(94,3,0,0,0,0,0); /* inherited across fork */
        for(U i=1;i<=ROUNDS;i++)__asm__ volatile("str %0,[%1]\n str %0,[%2]"::"r"(i),"r"(x),"r"(y):"memory");
        call(94,0,0,0,0,0,0);
    }
    U seen=0,r1,r2;
    do {
        __asm__ volatile("ldr %0,[%2]\n ldr %1,[%3]":"=&r"(r1),"=&r"(r2):"r"(y),"r"(x):"memory");
        if(r2<r1)seen++;
    } while(r1<ROUNDS);
    int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid || status)fail("child (model not inherited?)");
    return seen;
}
void _start(void) {
    volatile U *shared=(volatile U *)call(222,0,4096,3,0x21,(U)-1,0); /* MAP_SHARED|MAP_ANONYMOUS */
    if((long)shared<0)fail("mmap");
    long model=call(167,PR_GET_MEM_MODEL,0,0,0,0,0);
    if(model==-22) { shared[64]=(U)model;say("weak ");number(litmus(shared));say(" tso unsupported\n");call(94,0,0,0,0,0,0); }
    if(model!=0)fail("default model is not 0");
    if(call(167,PR_SET_MEM_MODEL,2,0,0,0,0)!=-22)fail("model 2 accepted");
    shared[64]=0;U weak=litmus(shared);
    if(call(167,PR_SET_MEM_MODEL,1,0,0,0,0)!=0 || call(167,PR_GET_MEM_MODEL,0,0,0,0,0)!=1)fail("cannot select TSO");
    shared[64]=1;U tso=litmus(shared);
    if(call(167,PR_SET_MEM_MODEL,0,0,0,0,0)!=0 || call(167,PR_GET_MEM_MODEL,0,0,0,0,0)!=0)fail("cannot return to the default");
    say("weak ");number(weak);say(" tso ");number(tso);say("\n");
    call(94,tso?2:0,0,0,0,0,0);for(;;){}
}
