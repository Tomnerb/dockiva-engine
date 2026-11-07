/* A fork's child that forks again (perl's open "-|" whose child opens "|-": the
   dictionaries-common and emacsen-common maintainer scripts):
   - the parent fills a private mapping and forks A; A writes more than the fork copy
     bound (fork_copy_pages) of fresh memory, so its own fork takes an overlay too;
   - A forks B, which exits at once; A then rewrites the parent's mapping and checks it;
   - the parent must still see only its own bytes, and A only its own.
   Before backing_materialize remapped what it copied, A's tables still named the
   parent's memory for every page it had not written, and its writes landed there.
   Exit 0 and "fork-nested PASS" when both sides kept their own memory. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="fork-nested FAIL xx\n";m[17]=(char)('0'+code/10);m[18]=(char)('0'+code%10);
    call(64,1,(U)m,20,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
static U *map(U bytes) { long p=call(222,0,bytes,3,0x22,(U)-1,0);if(p<0)fail(1);return (U *)p; }
static int child_status(long pid) { int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid)fail(2);return status; }
static void fill(volatile U *p,U words,U tag) { for(U i=0;i<words;i++)p[i]=tag^i; }
static int same(volatile const U *p,U words,U tag) { for(U i=0;i<words;i++)if(p[i]!=(tag^i))return 0;return 1; }
#define PRIVATE (1u<<18)                 /* 2 MiB the parent fills */
#define FRESH (1u<<17)                   /* 1 MiB the child writes: 64 host pages, over the copy bound */
void _start(void) {
    for(U round=0;round<8;round++) {
        volatile U *priv=map(PRIVATE*8);
        fill(priv,PRIVATE,0x1111+round);
        long a=call(220,17,0,0,0,0,0);                 /* fork */
        if(a<0)fail(3);
        if(!a) {
            volatile U *fresh=map(FRESH*8);
            fill(fresh,FRESH,0x2222);
            long b=call(220,17,0,0,0,0,0);
            if(b<0)call(94,4,0,0,0,0,0);
            if(!b)call(94,0,0,0,0,0,0);                /* B: gone at once */
            if(child_status(b))call(94,5,0,0,0,0,0);
            if(!same(priv,PRIVATE,0x1111+round))call(94,6,0,0,0,0,0);
            fill(priv,PRIVATE,0x3333);                 /* A's own copy, never the parent's */
            if(!same(priv,PRIVATE,0x3333) || !same(fresh,FRESH,0x2222))call(94,7,0,0,0,0,0);
            call(94,0,0,0,0,0,0);
        }
        int status=child_status(a);
        if(status)fail(10+(unsigned)((status>>8)&7));
        if(!same(priv,PRIVATE,0x1111+round))fail(20);
        if(call(215,(U)priv,PRIVATE*8,0,0,0,0))fail(21);
    }
    call(64,1,(U)"fork-nested PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
