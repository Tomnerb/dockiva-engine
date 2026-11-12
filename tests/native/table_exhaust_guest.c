/* More page tables than the hypervisor's pool holds, from memory in use: four threads
   write a word in every 2 MiB of 4 GiB committed above the arena (2048 last-level
   tables), run on a runtime built with a 4 MiB pool (1020 tables). The run must end
   with exit 125 and "Guest memory exceeds the hypervisor's page tables", not an EL1
   routine's fault: tables run out while siblings run guest code, in first writes
   resumed through EL1 routines and in the rebuilds after them. Exit 0 and
   "table-exhaust DONE" would mean the pool never ran out. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="table-exhaust FAIL x\n";m[19]=(char)('0'+code);
    call(64,1,(U)m,21,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define BASE 0x100000000000UL                         /* 16 TiB, above the arena */
#define GIB (1UL<<30)
#define SPAN (2UL<<20)
#define THREADS 4u
static long spawn(U *top,volatile unsigned *tid,void (*fn)(U),U arg) {
    top-=2;top[0]=(U)fn;top[1]=arg;
    register U x8 __asm__("x8")=220,x0 __asm__("x0")=0x10f00|0x40000|0x100000|0x200000,x1 __asm__("x1")=(U)top;
    register U x2 __asm__("x2")=(U)tid,x3 __asm__("x3")=0,x4 __asm__("x4")=(U)tid;
    __asm__ volatile("svc #0\n cbnz x0,1f\n ldp x9,x0,[sp]\n blr x9\n mov x0,#0\n mov x8,#93\n svc #0\n1:"
                     :"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory","cc","x9","x30");
    return (long)x0;
}
static U stacks[THREADS][2048] __attribute__((aligned(16)));
static volatile unsigned tids[THREADS];
static void writer(U t) { for(U at=BASE+t*GIB;at<BASE+(t+1)*GIB;at+=SPAN)*(volatile U *)at=at; }
void _start(void) {
    if(call(222,BASE,THREADS*GIB,0,0x22|0x4000|0x100000,(U)-1,0)!=(long)BASE)fail(1);
    if(call(226,BASE,THREADS*GIB,3,0,0,0))fail(2);
    for(unsigned t=0;t<THREADS;t++)if(spawn(&stacks[t][2048],&tids[t],writer,t)<=0)fail(3);
    for(unsigned t=0;t<THREADS;t++)for(unsigned v;(v=__atomic_load_n(&tids[t],__ATOMIC_ACQUIRE));)call(98,(U)&tids[t],0,v,0,0,0);
    call(64,1,(U)"table-exhaust DONE\n",19,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
