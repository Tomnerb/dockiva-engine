/* A 1 GiB anonymous mapping under a 64 MiB memory budget: pages are charged on
   first write, MADV_DONTNEED and munmap give them back (DONTNEED pages read as
   zero), so writing 3 x 40 MiB in turn fits. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"lazy-memory FAIL\n",17,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define MIB (1UL<<20)
static void fill(volatile unsigned char *p,U size,unsigned char value) { for(U i=0;i<size;i+=4096)p[i]=value; }
void _start(void) {
    long base=call(222,0,1024*MIB,3,0x22,(U)-1,0);check(base>0);
    volatile unsigned char *p=(volatile unsigned char *)base;
    check(p[0]==0 && p[1023*MIB]==0);                          /* reading costs nothing */
    fill(p,40*MIB,1);check(p[0]==1 && p[40*MIB-4096]==1);
    check(call(233,(U)base,40*MIB,4,0,0,0)==0);                 /* MADV_DONTNEED */
    check(p[0]==0 && p[40*MIB-4096]==0);
    check(call(233,(U)base,4096,8,0,0,0)==-22);                 /* MADV_FREE stays EINVAL */
    fill(p+512*MIB,40*MIB,2);check(p[512*MIB]==2);
    check(call(215,(U)base+512*MIB,40*MIB,0,0,0,0)==0);         /* munmap */
    fill(p+256*MIB,40*MIB,3);check(p[256*MIB]==3 && p[296*MIB-4096]==3);
    call(64,1,(U)"lazy-memory PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
