/* Files past 4 GiB under the engine's per-file limit (--file-limit-mib 1048576): a
   sparse write at 5 GiB, its size, reading it back, ftruncate beyond it
   and a reopen. Needs a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"bigfile FAIL\n",13,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define GIB (1UL<<30)
void _start(void) {
    long fd=call(56,(U)-100,(U)"/big",64|2,0600,0,0);check(fd>=0);
    check(call(68,(U)fd,(U)"five",4,5*GIB,0,0)==4);                      /* pwrite64 at 5 GiB */
    unsigned char st[128];check(call(80,(U)fd,(U)st,0,0,0,0)==0 && *(unsigned long *)(st+48)==5*GIB+4);
    char buf[4];check(call(67,(U)fd,(U)buf,4,5*GIB,0,0)==4 && buf[0]=='f' && buf[3]=='e');
    check(call(62,(U)fd,0,2,0,0,0)==(long)(5*GIB+4));                    /* lseek SEEK_END */
    check(call(64,(U)fd,(U)"!",1,0,0,0)==1);                             /* write at the end */
    check(call(46,(U)fd,6*GIB,0,0,0,0)==0);                               /* ftruncate */
    check(call(80,(U)fd,(U)st,0,0,0,0)==0 && *(unsigned long *)(st+48)==6*GIB);
    call(57,(U)fd,0,0,0,0,0);
    fd=call(56,(U)-100,(U)"/big",0,0,0,0);check(fd>=0);                  /* reopens past 4 GiB */
    check(call(67,(U)fd,(U)buf,1,5*GIB+4,0,0)==1 && buf[0]=='!');
    call(64,1,(U)"bigfile PASS\n",13,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
