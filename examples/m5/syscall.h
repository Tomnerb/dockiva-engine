typedef unsigned long U;
static U sc(U n,U a,U b,U c,U d,U e,U f) {
    register U r0 __asm__("x0")=a,r1 __asm__("x1")=b,r2 __asm__("x2")=c;
    register U r3 __asm__("x3")=d,r4 __asm__("x4")=e,r5 __asm__("x5")=f;
    register U r8 __asm__("x8")=n;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r5),"r"(r8) : "memory","cc");
    return r0;
}

static int bad(U v) { return v >= (U)-4095; }
