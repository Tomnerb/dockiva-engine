/* lazy_charge_guest's leaf: grows its brk heap by 64 KiB and maps 64 KiB, writes
   both, then waits on descriptor 3 (a pipe) until its writer closes. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
void _start(void) {
    U base=(U)call(214,0,0,0,0,0,0);
    if((U)call(214,base+65536,0,0,0,0,0)!=base+65536)call(93,1,0,0,0,0,0);
    for(U k=0;k<65536;k+=4096)((volatile char *)base)[k]=1;
    char *m=(char *)call(222,0,65536,3,0x22,(U)-1,0);if((long)m<0)call(93,2,0,0,0,0,0);
    for(U k=0;k<65536;k+=4096)m[k]=1;
    char c;call(63,3,(U)&c,1,0,0,0);
    call(93,0,0,0,0,0,0);for(;;){}
}
