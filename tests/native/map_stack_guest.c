typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}

static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }


void _start(void) {
    U base=(U)call(222,0,12288,0,0x20022,(U)-1,0);check((long)base>0);
    check(call(226,base+4096,8192,3,0,0,0)==0);
    volatile unsigned char *p=(volatile unsigned char *)(base+4096);
    check(p[0]==0 && p[8191]==0);p[0]=17;p[8191]=29;
    check(p[0]==17 && p[8191]==29);
    check(call(64,1,base,1,0,0,0)==-14); /* Guard page is inaccessible. */
    check(call(226,base+4096,8192,0,0,0,0)==0);
    check(call(64,1,base+4096,1,0,0,0)==-14);
    check(call(226,base+4096,8192,3,0,0,0)==0);
    check(p[0]==17 && p[8191]==29);
    check(call(215,base,12288,0,0,0,0)==0);
    check(call(64,1,base+4096,1,0,0,0)==-14);
    check(call(222,0,0,3,0x20022,(U)-1,0)==-22);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
