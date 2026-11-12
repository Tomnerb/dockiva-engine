/* Page tables for memory committed but never touched: a reservation above the arena
   (PROT_NONE) made read-write 1 GiB at a time, 272 GiB in all, as FEX's allocator
   commits its slabs (about 32 GiB per x86 process). Mapped at once, one last-level
   table per 2 MiB, that was 139264 tables, more than the 131072 the table pool holds,
   and a go build under FEX (a dozen compilers at once) ran out ("Guest memory exceeds
   the hypervisor's page tables"). Mapped at first use, it is one per GiB written:
   each GiB gets one write, then everything goes. Exit 0 and "table-pool PASS". */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="table-pool FAIL x\n";m[16]=(char)('0'+code);
    call(64,1,(U)m,18,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define BASE 0x100000000000UL            /* 16 TiB, above the arena */
#define GIB (1UL<<30)
#define COUNT 272UL
void _start(void) {
    if(call(222,BASE,COUNT*GIB,0,0x22|0x4000|0x100000,(U)-1,0)!=(long)BASE)fail(1);   /* PROT_NONE reservation */
    for(U i=0;i<COUNT;i++)if(call(226,BASE+i*GIB,GIB,3,0,0,0))fail(2);                 /* commit, untouched */
    for(U i=0;i<COUNT;i++)*(volatile U *)(BASE+i*GIB+4096*i)=i;
    for(U i=0;i<COUNT;i++)if(*(volatile U *)(BASE+i*GIB+4096*i)!=i)fail(3);
    if(call(215,BASE,COUNT*GIB,0,0,0,0))fail(4);
    call(64,1,(U)"table-pool PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
