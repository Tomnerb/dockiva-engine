/* Read-only MAP_SHARED mappings of a file on a writable filesystem each take a
   segment (Go's linker maps every package archive so); they stop with ENOMEM before
   the last SEGMENT_SPARE, which stay for other memory. Under FEX the link took every
   segment: FEX's allocator then committed a piece of its reservation (mprotect, its
   failure unchecked) and faulted writing it.
   - maps /data read-only and shared until ENOMEM (and not before 1000 maps: 1024
     less the program's own segments);
   - unmaps one of them: another then works again;
   - commits ten pieces of a PROT_NONE reservation above the arena and writes them,
     maps /data writable and shared, and maps shared anonymous memory: all must work.
   Exit 0 and "segment-spare PASS". Run with a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="segment-spare FAIL x\n";m[19]=(char)('0'+code);
    call(64,1,(U)m,21,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define SIZE 16384UL
#define RESERVE 0x600000000000UL
static char page[SIZE];
static long maps[1024];
void _start(void) {
    long fd=call(56,(U)-100,(U)"/data",0102|01000,0644,0,0);            /* O_RDWR|O_CREAT|O_TRUNC */
    if(fd<0)fail(1);
    for(U i=0;i<SIZE;i++)page[i]=(char)i;
    if(call(64,(U)fd,(U)page,SIZE,0,0,0)!=(long)SIZE)fail(1);
    long ro=call(56,(U)-100,(U)"/data",0,0,0,0);if(ro<0)fail(1);         /* O_RDONLY */
    unsigned n=0;long last=0;
    while(n<1024) {
        last=call(222,0,SIZE,1,1,(U)ro,0);                               /* PROT_READ, MAP_SHARED */
        if(last<0)break;
        maps[n++]=last;
    }
    if(last!=-12 || n<1000)fail(2);                                      /* ENOMEM, after enough */
    if(call(215,(U)maps[0],SIZE,0,0,0,0))fail(7);
    if(call(222,0,SIZE,1,1,(U)ro,0)<0)fail(8);                           /* one freed: one more */
    if(call(222,RESERVE,64UL<<20,0,0x22|0x4000|0x100000,(U)-1,0)!=(long)RESERVE)fail(3);
    for(U i=0;i<10;i++) {                                                /* separate commits */
        U at=RESERVE+i*(2UL<<20);
        if(call(226,at,SIZE,3,0,0,0))fail(4);
        *(volatile U *)at=i;
    }
    if(call(222,0,SIZE,3,1,(U)fd,0)<0)fail(5);                           /* writable, shared */
    if(call(222,0,SIZE,3,0x21,(U)-1,0)<0)fail(6);                        /* MAP_SHARED|MAP_ANONYMOUS */
    call(64,1,(U)"segment-spare PASS\n",19,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
