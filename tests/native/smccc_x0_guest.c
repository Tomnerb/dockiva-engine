/* x0 values shaped like Arm SMCCC function IDs (bit 31 set: a fast call; 0xC1000000
   is owner 1, SMC64) must reach the runtime and survive guest exceptions intact. The
   hypervisor backend leaves EL0 through an EL1 vector that executes HVC, and
   Hypervisor.framework answers some such IDs itself (x0 = 0xffffffff, NOT_SUPPORTED)
   when x0 still holds the guest's value: V8 then saw munmap(0xffffffff) for an mmap
   arena address 0x3c1xxxxxx. Run with --mmap-limit-mib 4096 (arena 0x300000000 up). */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }
/* A first write to a fresh page faults (lazily charged) with x0 live. */
static U store_keeping(U page,U value) {
    register U x0 __asm__("x0")=value,x1 __asm__("x1")=page;
    __asm__ volatile("str xzr,[x1]":"+r"(x0):"r"(x1):"memory");
    return x0;
}
void _start(void) {
    for(U id=0x80;id<0x100;id++) {                      /* fast calls, SMC32 and SMC64, every owner */
        U at=0x300000000UL|id<<24;
        check((U)call(222,at,0x10000,3,0x22,(U)-1,0)==at);     /* the hint arrives: free, so it is used */
        check(store_keeping(at+0x8000,at)==at);
        check(store_keeping(at+0xc000,(U)(unsigned)(id<<24))==(U)(unsigned)(id<<24));
        check((U)call(222,at,0x4000,3,0x32,(U)-1,0)==at);      /* MAP_FIXED over a mapping: TLB maintenance */
        check(call(215,at,0x10000,0,0,0,0)==0);
    }
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
