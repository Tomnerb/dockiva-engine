/* Independent guest-side LP64 layout, compiled once and run on both backends
   and Linux. Numeric resource values intentionally differ between systems. */
#include <stddef.h>
typedef unsigned long U;
struct info {
    long uptime;U loads[3],totalram,freeram,sharedram,bufferram,totalswap,freeswap;
    unsigned short procs,pad;U totalhigh,freehigh;unsigned int mem_unit;
};
_Static_assert(sizeof(struct info)==112,"AArch64 sysinfo size");
_Static_assert(offsetof(struct info,totalram)==32,"totalram ABI");
_Static_assert(offsetof(struct info,procs)==80,"procs ABI");
_Static_assert(offsetof(struct info,mem_unit)==104,"mem_unit ABI");
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
void _start(void) {
    struct info first,second;unsigned char raw[114],errors[4];
    if(call(179,(U)&first,0,0,0,0,0))goto bad;
    U base=(U)call(222,0,8192,3,34,(U)-1,0);if((long)base<0)goto bad;
    if(call(179,(U)&second,0,0,0,0,0))goto bad;
    raw[0]=171;raw[113]=205;
    if(call(179,(U)(raw+1),0,0,0,0,0))goto bad;
    if(raw[0]!=171 || raw[113]!=205)goto bad;
    errors[0]=(unsigned char)-call(179,0,0,0,0,0,0);
    if(call(226,base+4096,4096,1,0,0,0))goto bad;
    errors[1]=(unsigned char)-call(179,base+4096,0,0,0,0,0);
    errors[2]=(unsigned char)-call(179,base+4096-64,0,0,0,0,0);
    if(call(215,base,8192,0,0,0,0))goto bad;
    errors[3]=(unsigned char)-call(179,base,0,0,0,0,0);
    if(call(64,1,(U)&first,112,0,0,0)!=112)goto bad;
    if(call(64,1,(U)&second,112,0,0,0)!=112)goto bad;
    if(call(64,1,(U)errors,4,0,0,0)!=4)goto bad;
    call(93,0,0,0,0,0,0);
bad:
    call(93,99,0,0,0,0,0);for(;;){}
}
