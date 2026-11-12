/* The top of the address space, as FEX's allocator sees it. FEX finds the VA size by
   mapping the last page below 1 << 48 with MAP_FIXED_NOREPLACE (success or EEXIST:
   48-bit), then reserves (PROT_NONE) every gap above 47 bits that /proc/self/maps
   shows, up to 1 << 48, and allocates from those reservations without checking that
   they were made (its release build drops the check). Guest addresses end at
   0xffffffff0000 (the hypervisor's page is above): the last gap's reservation
   failed, and FEX later used memory it never got (SIGSEGV in its allocator, reported
   to the x86 program in clone: "go build" of amd64 code crashed or hung).
   Checks: the 64 KiB at the top are taken (EEXIST for MAP_FIXED_NOREPLACE, ENOMEM
   for MAP_FIXED, EINVAL for mremap there), /proc/self/maps lists them, and a gap
   that ends where they start can be reserved. Exit 0 and "va-top PASS". */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="va-top FAIL x\n";m[12]=(char)('0'+code);
    call(64,1,(U)m,14,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define TOP 0x1000000000000UL            /* 1 << 48 */
#define END 0xffffffff0000UL             /* the end of guest addresses */
#define ANON 0x22UL                      /* MAP_PRIVATE|MAP_ANONYMOUS */
#define FIXED 0x10UL
#define NOREPLACE 0x100000UL
static char maps[65536];
static int contains(const char *s,long n,const char *line) {
    for(long i=0;i<n;i++) { long j=0;while(line[j] && i+j<n && s[i+j]==line[j])j++;if(!line[j])return 1; }
    return 0;
}
void _start(void) {
    if(call(222,TOP-4096,4096,0,ANON|NOREPLACE,(U)-1,0)!=-17)fail(1);         /* FEX's VA probe: taken */
    if(call(222,END-65536,131072,0,ANON|NOREPLACE,(U)-1,0)!=-17)fail(2);      /* across its start */
    if(call(222,TOP-4096,4096,3,ANON|FIXED,(U)-1,0)!=-12)fail(3);             /* never replaced */
    if(call(222,END-0x8000000,0x8000000+65536,0,ANON|0x4000|NOREPLACE,(U)-1,0)!=-17)fail(4); /* a gap up to 1 << 48 */
    long fd=call(56,(U)-100,(U)"/proc/self/maps",0,0,0,0);if(fd<0)fail(5);
    long n=call(63,(U)fd,(U)maps,sizeof maps,0,0,0);call(57,(U)fd,0,0,0,0,0);
    if(n<=0 || !contains(maps,n,"ffffffff0000-1000000000000 ---p"))fail(5);
    long gap=call(222,END-0x8000000,0x8000000,0,ANON|0x4000|NOREPLACE,(U)-1,0); /* the gap that ends at it */
    if(gap!=(long)(END-0x8000000))fail(6);
    long page=call(222,0,4096,3,ANON,(U)-1,0);if(page<0)fail(7);
    if(call(216,(U)page,4096,4096,3,TOP-4096,0)!=-22)fail(8);                 /* mremap MAYMOVE|FIXED onto it */
    if(call(215,(U)gap,0x8000000,0,0,0,0))fail(9);
    call(64,1,(U)"va-top PASS\n",12,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
