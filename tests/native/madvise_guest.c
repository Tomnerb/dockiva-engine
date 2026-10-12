/* Freestanding Linux ARM64 ABI probe, shared unchanged with the Linux reference. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned char advice(U address,U size,U kind) { return (unsigned char)-call(233,address,size,kind,0,0,0); }
void _start(void) {
    unsigned char result[13];
    U p=(U)call(222,0,12288,3,34,(U)-1,0);
    if((long)p<0)goto bad;
    *(volatile unsigned char *)p=90;
    result[0]=advice(p,12288,0);
    result[1]=advice(p,12288,1);
    result[2]=advice(p,12288,2);
    result[3]=*(volatile unsigned char *)p==90?0:1;
    result[4]=advice(p,4096,65535);
    result[5]=advice(p+1,4096,0);
    result[6]=advice(4096,0,0);
    result[7]=advice(p,(U)-1,0);
    result[8]=advice(4096,4096,0);
    if(call(226,p+4096,4096,0,0,0,0))goto bad;
    result[9]=advice(p,12288,0);
    if(call(215,p+4096,4096,0,0,0,0))goto bad;
    result[10]=advice(p,12288,0);
    result[12]=advice(p,4096,((U)1<<32)|1);
    result[11]=advice(p,4096,8); /* Declared capability gap: Linux supports FREE. */
    if(call(64,1,(U)result,sizeof(result),0,0,0)!=(long)sizeof(result))goto bad;
    call(93,0,0,0,0,0,0);
bad:
    call(93,99,0,0,0,0,0);
    for(;;){}
}
