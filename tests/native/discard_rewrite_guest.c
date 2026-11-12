/* Pages of a reservation above the mmap arena given back and written again (FEX's
   allocator reserves its memory PROT_NONE, then maps pieces MAP_FIXED; it empties its
   return-address stack and lookup caches with MADV_DONTNEED). Three ways to give them
   back, each followed by writes and a MADV_DONTNEED that must read back zeros:
   MADV_DONTNEED itself (three rounds), mremap MREMAP_DONTUNMAP to elsewhere in the
   reservation (the old pages stay, fresh) and mmap MAP_FIXED over them again. The
   hypervisor's tables kept given-back pages writable, so later writes went unseen and
   the next MADV_DONTNEED left them in place: stale code addresses then sent FEX's
   threads into a code buffer it had freed. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
#define PIECE (1UL<<20)
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"discard-rewrite FAIL\n",21,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define RESERVE 0x600000000000UL
static void fill(U at,unsigned round) { for(U p=0;p<PIECE;p+=4096)*(volatile U *)(at+p)=0x5eed0000UL+round+p; }
static int zeros(U at) { for(U p=0;p<PIECE;p+=8)if(*(volatile U *)(at+p))return 0;return 1; }
#define SIZE (64UL<<20)
void _start(void) {
    /* PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE */
    check(call(222,RESERVE,SIZE,0,0x2|0x20|0x4000|0x100000,(U)-1,0)==(long)RESERVE);
    U piece=RESERVE+0x1000;                                 /* not 16 KiB aligned, as FEX's pieces are not */
    check(call(222,piece,PIECE,3,0x2|0x20|0x10,(U)-1,0)==(long)piece);   /* RW, MAP_FIXED */
    for(unsigned round=0;round<3;round++) {
        fill(piece,round);
        check(call(233,piece,PIECE,4,0,0,0)==0);           /* MADV_DONTNEED */
        check(zeros(piece));
    }
    /* MREMAP_MAYMOVE|MREMAP_FIXED|MREMAP_DONTUNMAP to a piece 8 MiB on (a new segment) */
    fill(piece,3);
    U moved=RESERVE+(8UL<<20);
    check(call(216,piece,PIECE,PIECE,1|2|4,moved,0)==(long)moved);
    check(*(volatile U *)(moved+4096)==0x5eed0003UL+4096 && zeros(piece));
    fill(piece,4);
    check(call(233,piece,PIECE,4,0,0,0)==0);
    check(zeros(piece));
    /* MAP_FIXED over the written piece again, with the same protection */
    fill(piece,5);
    check(call(222,piece,PIECE,3,0x2|0x20|0x10,(U)-1,0)==(long)piece);
    check(zeros(piece));
    fill(piece,6);
    check(call(233,piece,PIECE,4,0,0,0)==0);
    check(zeros(piece));
    call(64,1,(U)"discard-rewrite PASS\n",21,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
