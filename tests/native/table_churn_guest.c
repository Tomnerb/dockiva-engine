/* Page tables reused while the pool runs short, on a runtime built with a 4 MiB pool
   (1020 tables): a word written in each of 600 spans of 2 MiB above the arena keeps
   about 610 tables in use, more than a quarter of the pool, so every layout change
   rebuilds them all (hvf_sync_partial). 2000 mprotects of one page (read-only, then
   read-write again) each rebuild, after the never-used tables ran out: a rebuild can
   only take tables freed by the one before, once a TLB invalidation has passed. Taken
   at once, an EL1 routine walked a reused table ("Hypervisor EL1 routine faulted").
   The words read back after each round. Exit 0 and "table-churn PASS". */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="table-churn FAIL x\n";m[17]=(char)('0'+code);
    call(64,1,(U)m,19,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define BASE 0x100000000000UL                         /* 16 TiB, above the arena */
#define SPAN (2UL<<20)
#define SPANS 600UL
void _start(void) {
    if(call(222,BASE,SPANS*SPAN,0,0x22|0x4000|0x100000,(U)-1,0)!=(long)BASE)fail(1);
    if(call(226,BASE,SPANS*SPAN,3,0,0,0))fail(2);
    for(U i=0;i<SPANS;i++)*(volatile U *)(BASE+i*SPAN)=i;
    for(U round=0;round<2000;round++) {
        U page=BASE+(round%SPANS)*SPAN+4096;
        if(call(226,page,4096,1,0,0,0) || call(226,page,4096,3,0,0,0))fail(3);
        for(U i=0;i<SPANS;i++)if(*(volatile U *)(BASE+i*SPAN)!=i)fail(4);
    }
    call(64,1,(U)"table-churn PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
