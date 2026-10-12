/* Exec target for exec_socket_guest.c: fd 3 survived exec, fd 4 (CLOEXEC) did not. */
typedef unsigned long U;
static long call(U n,U a,U b,U c) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return (long)x0;
}
void _start(void) {
    call(93,(call(25,3,1,0)>=0 && call(25,4,1,0)==-9)?0:7,0,0);for(;;){}
}
