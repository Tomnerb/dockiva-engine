/* writev accepts up to UIO_MAXIOV (1024) buffers: 64 one-byte buffers write 64 bytes. */
typedef unsigned long U;
static long call(U n,U a,U b,U c) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return (long)x0;
}
static U iov[64][2];
void _start(void) {
    for(int i=0;i<64;i++) { iov[i][0]=(U)"x";iov[i][1]=1; }
    long n=call(66,1,(U)iov,64);
    call(64,1,(U)"\n",1);
    call(94,n==64?0:1,0,0);for(;;){}
}
