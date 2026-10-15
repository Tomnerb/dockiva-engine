/* Under --memory-limit-mib 256 a child that writes 512 MiB is killed with SIGKILL
   (the budget still counts every page written); its parent goes on. Needs --process-tree. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
void _start(void) {
    long hog=call(220,17,0,0,0,0,0);
    if(!hog) {
        unsigned char *p=(unsigned char *)call(222,0,512UL<<20,3,0x22,(U)-1,0);
        for(unsigned long k=0;k<(512UL<<20);k+=4096)p[k]=1;
        call(93,0,0,0,0,0,0);
    }
    int status=-1;
    if(hog<0 || call(260,(U)hog,(U)&status,0,0,0,0)!=hog || (status&0x7f)!=9) { call(64,1,(U)"oom-kill FAIL\n",14,0,0,0);call(94,1,0,0,0,0,0); }
    call(64,1,(U)"oom-kill PASS\n",14,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
