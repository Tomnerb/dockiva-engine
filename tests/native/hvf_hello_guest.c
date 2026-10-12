/* Hypervisor backend milestone 1: native EL0 execution with system calls served
   by the shared Linux layer. Exercises write, floating point (FDIV, which the
   interpreter lacks), stack and data writes, getpid, and exit. */
typedef unsigned long U;
static long call(U n,U a,U b,U c) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return (long)x0;
}
static volatile double numerator=10.0,denominator=4.0;
static char buffer[64];
void _start(void) {
    double q=numerator/denominator;                    /* 2.5 via FDIV */
    long whole=(long)(q*2.0);                          /* 5 */
    const char text[]="hello from native arm64, 10/4*2=";
    unsigned n=0;
    for(;text[n];n++)buffer[n]=text[n];                /* stack -> data writes */
    buffer[n++]=(char)('0'+whole);buffer[n++]='\n';
    long written=call(64,1,(U)buffer,n);
    long pid=call(172,0,0,0);
    call(93,(written==(long)n && pid==1 && whole==5)?0:1,0,0);
    for(;;){}
}
