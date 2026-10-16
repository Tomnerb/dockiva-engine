/* Go's heap start-up probes: every Go process asks for 64 MiB of PROT_NONE at a
   hint outside the mmap arena about 64 times and unmaps each answer. Those calls
   must stay cheap (no per-page work for pages never used) and exact: first fit
   still finds every hole, fresh mappings over recycled pages read zero, and a page
   made accessible inside a large untouched PROT_NONE range is mapped. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { out("mmap-probe FAIL\n");call(93,100+checks,0,0,0,0,0);for(;;){} } }
static U now_ns(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000000000UL+ts[1]; }
#define ANON 0x22UL                                          /* MAP_PRIVATE|MAP_ANONYMOUS */
static long map(U addr,U bytes,U prot) { return call(222,addr,bytes,prot,ANON,(U)-1,0); }
void _start(void) {
    /* 1. Probes: hinted 64 MiB PROT_NONE reservations, answered and given back. */
    U start=now_ns();
    for(unsigned i=0;i<256;i++) {
        long p=map(0x7f4000000000UL-((U)i<<40>>8),64UL<<20,0);check(p>0 && !(p&4095));
        check(call(215,(U)p,64UL<<20,0,0,0,0)==0);
    }
    U per_probe=(now_ns()-start)/256;
    check(per_probe<200000);                                 /* was ~24 us per pair; generous for loaded machines */
    /* 2. First fit finds holes of every size and alignment between used pages. */
    long base=map(0,4096UL*4096,3);check(base>0);
    static const unsigned hole[][2]={{100,300},{1024,64},{2000,65},{3000,1},{3500,129}};
    for(unsigned h=0;h<5;h++) {
        check(call(215,(U)base+hole[h][0]*4096UL,hole[h][1]*4096UL,0,0,0,0)==0);
        long p=map(0,hole[h][1]*4096UL,3);
        check(p==base+(long)(hole[h][0]*4096UL));            /* the lowest hole that fits: this one */
    }
    /* 3. Pages written, unmapped and mapped again read zero. */
    for(U i=0;i<4096;i++)((volatile char *)base)[i*4096]=(char)(i|1);
    check(call(215,(U)base,4096UL*4096,0,0,0,0)==0);
    long again=map((U)base,4096UL*4096,3);check(again>0);
    for(U i=0;i<4096;i++)check(((volatile char *)again)[i*4096]==0);
    check(call(215,(U)again,4096UL*4096,0,0,0,0)==0);
    /* 4. One page made accessible in a large untouched PROT_NONE range works. */
    long none=map(0,512UL<<20,0);check(none>0);
    volatile U *page=(volatile U *)(none+(300UL<<20)+4096*7);
    check(call(226,(U)page&~4095UL,4096,3,0,0,0)==0);
    *page=0x5a5a;check(*page==0x5a5a);
    check(call(215,(U)none,512UL<<20,0,0,0,0)==0);
    out("mmap-probe PASS\n");call(93,0,0,0,0,0,0);for(;;){}
}
