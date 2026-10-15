/* A static position-independent program loads where Linux (arm64, no randomization)
   puts a loader run directly: just below mmap_base (0xfffff8000000), with its brk heap
   right after it. Built -fPIE, linked -static -pie; uses PC-relative addresses only.
   Writes the number of passed checks (4 bytes); on a failure, exit status 99. */
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
    U here;__asm__ volatile("adr %0, ." :"=r"(here));
    check(here>=0xfffff0000000UL && here<0xfffff8000000UL);
    U brk=(U)call(214,0,0,0,0,0,0);
    check(brk>here && brk<0xfffff8100000UL);
    check((U)call(214,brk+65536,0,0,0,0,0)==brk+65536);
    *(volatile U *)(brk+4096)=1;check(*(volatile U *)(brk+4096)==1);
    /* The 4 MiB a non-PIE x86 or arm64 program wants is free for MAP_FIXED_NOREPLACE. */
    check(call(222,0x400000,4096,3,0x100022,(U)-1,0)==0x400000);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
