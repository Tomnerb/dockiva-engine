typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
static void finish(unsigned code) { call(94,code,0,0,0,0,0);for(;;){} }
static volatile unsigned word=123;
void _start(void) {
    long child=call(220,17,0,0,0,0,0);if(child<0)finish(91);
    if(!child) {
        const char *argv[]={"/exec.elf",0};word=456;
        if(call(221,(U)"/bad.elf",(U)argv,0,0,0,0)!=-8 || word!=456)finish(92);
        long mapped=call(222,0,4096,3,0x22,(U)-1,0);if(mapped<0)finish(95);
        __asm__ volatile("ldxr x9,[%0]"::"r"((U)mapped):"x9","memory");
        call(221,(U)argv[0],(U)argv,0,0,0,0);finish(93);
    }
    unsigned status=0;
    if(call(260,child,(U)&status,0,0,0,0)!=child || status!=(23<<8) || word!=123)finish(94);
    finish(0);
}
