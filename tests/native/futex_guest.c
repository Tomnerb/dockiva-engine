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
    unsigned word=7;U expired[2]={0,0},brief[2]={0,2000000},bad[2]={0,1000000000};
    check(call(98,(U)&word,128,6,0,0,0)==-11);
    check(call(98,(U)&word,128,7,(U)expired,0,0)==-110);
    check(call(98,(U)&word,128,7,(U)brief,0,0)==-110);
    check(call(98,(U)&word,137,7,(U)expired,0,1)==-110);
    check(call(98,(U)&word,393,7,(U)expired,0,1)==-110);
    check(call(98,(U)&word,128,7,(U)bad,0,0)==-22);
    check(call(98,(U)&word,137,7,0,0,0)==-22);
    check(call(98,(U)&word+1,128,7,0,0,0)==-22);
    check(call(98,0,128,7,0,0,0)==-14);
    check(call(98,(U)&word,129,1,0,0,0)==0);
    check(call(98,(U)&word,138,1,0,0,1)==0);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
