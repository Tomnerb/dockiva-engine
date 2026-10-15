/* Guest identity switching as su-exec/gosu do it: root may drop to another
   uid/gid, may not regain root afterwards, and a child's switch stays in the
   child. chown succeeds for root and is refused to others giving files away. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"identity FAIL\n",14,0,0,0);call(93,(U)checks,0,0,0,0,0);for(;;){} } }
void _start(void) {
    long f=call(56,(U)-100,(U)"/data.txt",64|1,0600,0,0);check(f>=0);
    check(call(174,0,0,0,0,0,0)==0);                                   /* starts as root */
    check(call(54,(U)-100,(U)"/data.txt",999,1000,0,0)==0);            /* root chowns */
    check(call(55,(U)f,999,1000,0,0,0)==0);
    check(call(54,(U)-100,(U)"/missing",999,1000,0,0)==-2);
    long pid=call(220,17,0,0,0,0,0);check(pid>=0);
    if(!pid) {                                                         /* child drops privileges */
        check(call(159,0,0,0,0,0,0)==0 && call(144,1000,0,0,0,0,0)==0 && call(146,999,0,0,0,0,0)==0);
        check(call(174,0,0,0,0,0,0)==999 && call(176,0,0,0,0,0,0)==1000);
        check(call(146,0,0,0,0,0,0)==-1);                              /* cannot regain root */
        check(call(159,0,0,0,0,0,0)==-1);
        check(call(54,(U)-100,(U)"/data.txt",0,(U)-1,0,0)==-1);        /* cannot give files away */
        check(call(54,(U)-100,(U)"/data.txt",999,(U)-1,0,0)==0);       /* own identity is fine */
        call(93,0,0,0,0,0,0);
    }
    int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    check(call(174,0,0,0,0,0,0)==0);                                   /* parent is still root */
    check(call(147,(U)-1,500,(U)-1,0,0,0)==0 && call(175,0,0,0,0,0,0)==500 && call(174,0,0,0,0,0,0)==0); /* setresuid: effective only */
    call(64,1,(U)"identity PASS\n",14,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
