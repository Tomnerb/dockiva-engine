/* Freestanding allocation probes. All addresses are obtained from Linux syscalls. */
typedef unsigned long U;
static U sc(U n,U a,U b,U c,U d,U e,U f) {
    register U r0 __asm__("x0")=a,r1 __asm__("x1")=b,r2 __asm__("x2")=c;
    register U r3 __asm__("x3")=d,r4 __asm__("x4")=e,r5 __asm__("x5")=f;
    register U r8 __asm__("x8")=n;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r5),"r"(r8) : "memory","cc");
    return r0;
}
static U brk(U addr) { return sc(214,addr,0,0,0,0,0); }
static U map(U len,U prot,U flags) { return sc(222,0,len,prot,flags,(U)-1,0); }
static U unmap(U addr,U len) { return sc(215,addr,len,0,0,0,0); }
static int bad(U v) { return v >= (U)-4095; }
int guest_main(U argc,char **argv,char **envp) {
    (void)argc;(void)argv;(void)envp;
#if CASE == 0
    U base=brk(0);
    if(brk(base+8192)!=base+8192) return 91;
    volatile unsigned char *h=(volatile unsigned char *)base;
    if(h[0] || h[8191]) return 92;
    h[0]=17;h[8191]=34;
    if(h[0]+h[8191]!=51) return 93;
    if(brk(base)!=base) return 94;
    U addr=map(8192,3,0x22);if(bad(addr)) return 95;
    unsigned char *p=(unsigned char *)addr;
    static const char msg[]="heap allocation OK\n";
    for(U i=0;i<sizeof(msg)-1;i++) p[4090+i]=(unsigned char)msg[i];
    if(sc(64,1,addr+4090,sizeof(msg)-1,0,0,0)!=sizeof(msg)-1) return 96;
    if(unmap(addr,8192)) return 97;
#elif CASE == 1
    U base=brk(0);if(brk(base+8192)!=base+8192) return 91;
    volatile unsigned char *p=(volatile unsigned char *)base;p[0]=7;p[4096]=9;
    if(brk(base+4096)!=base+4096 || brk(base+8192)!=base+8192) return 92;
    if(p[0]!=7 || p[4096]!=0) return 93;
    if(brk(base)!=base) return 94;
#elif CASE == 2
    U a=map(12288,3,0x22);if(bad(a)) return 91;
    volatile unsigned char *p=(volatile unsigned char *)a;p[0]=11;p[8192]=22;
    if(unmap(a+4096,4096)) return 92;
    if(p[0]!=11 || p[8192]!=22) return 93;
    if(unmap(a,12288)) return 94;
#elif CASE == 3
    U a=map(4096,3,0x22);if(bad(a)) return 91;
    *(volatile unsigned char *)a=99;
    if(unmap(a,4096)) return 92;
    U b=map(4096,3,0x22);if(bad(b)) return 93;
    if(*(volatile unsigned char *)b) return 94;
    if(unmap(b,4096)) return 95;
#elif CASE == 4
    if(map(0,3,0x22)!=(U)-22) return 91;
#elif CASE == 5
    U a=map(4096,3,0x22);if(bad(a)) return 91;
    if(unmap(a+1,4096)!=(U)-22 || unmap(a,0)!=(U)-22) return 92;
    *(volatile unsigned char *)a=7;
    if(*(volatile unsigned char *)a!=7 || unmap(a,1)) return 93;
#elif CASE == 6
    U a=map(1,1,0x22);if(bad(a)) return 91;
    if(*(volatile unsigned char *)a || unmap(a,1)) return 92;
#elif CASE == 7
    U a=map(4096,0,0x22);if(bad(a) || unmap(a,4096)) return 91;
#elif CASE == 8
    U a=map(4096,3,0x22);if(bad(a)) return 91;
    if(unmap(a,4096) || unmap(a,4096)) return 92;
#elif CASE == 9
    U base=brk(0);if(brk(base-1)!=base || brk(0)!=base) return 91;
#elif CASE == 10
    U a=map(4096,1,0x22);if(bad(a)) return 91;
    *(volatile unsigned char *)a=1;
#elif CASE == 11
    U a=map(4096,3,0x22);if(bad(a) || unmap(a,4096)) return 91;
    return *(volatile unsigned char *)a;
#elif CASE == 12
    U a=map(4096,0,0x22);if(bad(a)) return 91;
    return *(volatile unsigned char *)a;
#elif CASE == 13
    U a=map(16*1024*1024,3,0x22);if(bad(a)) return 91;
    *(volatile unsigned char *)a=55;
    if(map(1,3,0x22)!=(U)-12 || *(volatile unsigned char *)a!=55) return 92;
    if(unmap(a,16*1024*1024)) return 93;
    U b=map(4096,3,0x22);if(bad(b) || unmap(b,4096)) return 94;
#elif CASE == 14
    U base=brk(0);if(brk(base+8*1024*1024)!=base+8*1024*1024) return 91;
    if(brk(base+8*1024*1024+1)!=base+8*1024*1024) return 92;
    if(brk(base)!=base) return 93;
#elif CASE == 15
    if(map(4096,3,0x32)!=(U)-95 || map(4096,4,0x22)!=(U)-22) return 91;
#elif CASE == 16
    U base=brk(0);if(brk(base+8192)!=base+8192 || brk(base+4096)!=base+4096) return 91;
    return *(volatile unsigned char *)(base+4096);
#endif
    return 0;
}
