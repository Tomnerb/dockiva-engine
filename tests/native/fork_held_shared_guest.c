/* Memory the hypervisor held for one fork, kept as is (shared) by a later fork that
   ran out of its eight copy and overlay slots (address_space_fork_backing):
   - the parent fills 2 MiB of the mmap arena (over the fork copy bound) and forks A,
     which exits at once: the arena was held for A's overlay and is thawed now, still
     marked held, since the parent writes nothing to it meanwhile;
   - the parent maps eight pieces outside the arena, each in its own 1 GiB window and
     so its own backing, ahead of the arena in fork order, and forks B: the pieces take
     the eight slots and B shares the arena;
   - B writes the arena and exits; the parent must still read its own bytes.
   The first write to the held backing made all of it writable in place
   (hvf_held_restore) although B shared it, so B's writes landed in the parent's
   memory. Exit 0 and "fork-held-shared PASS" when the parent kept its bytes. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="fork-held-shared FAIL x\n";m[22]=(char)('0'+code);
    call(64,1,(U)m,24,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
static int child_status(long pid) { int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid)fail(2);return status; }
static void fill(volatile U *p,U words,U tag) { for(U i=0;i<words;i++)p[i]=tag^i; }
static int same(volatile const U *p,U words,U tag) { for(U i=0;i<words;i++)if(p[i]!=(tag^i))return 0;return 1; }
#define WORDS (1u<<18)                   /* 2 MiB: 128 host pages, over the copy bound */
#define OUTSIDE 0x600000000000UL         /* above the arena */
void _start(void) {
    long p=call(222,0,WORDS*8,3,0x22,(U)-1,0);         /* RW, MAP_PRIVATE|MAP_ANONYMOUS: the arena */
    if(p<0)fail(1);
    volatile U *arena=(U *)p;
    fill(arena,WORDS,0x1111);
    long a=call(220,17,0,0,0,0,0);                     /* fork */
    if(a<0)fail(3);
    if(!a)call(94,0,0,0,0,0,0);                        /* A: gone at once */
    if(child_status(a))fail(4);
    for(U i=0;i<8;i++) {                               /* MAP_FIXED_NOREPLACE, one window each */
        U at=OUTSIDE+(i<<30);
        if(call(222,at,1UL<<20,3,0x22|0x100000,(U)-1,0)!=(long)at)fail(5);
        *(volatile U *)at=i;
    }
    long b=call(220,17,0,0,0,0,0);
    if(b<0)fail(6);
    if(!b) { fill(arena,WORDS,0x2222);call(94,0,0,0,0,0,0); }
    if(child_status(b))fail(7);
    if(!same(arena,WORDS,0x1111))fail(8);
    call(64,1,(U)"fork-held-shared PASS\n",22,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
