/* Copy-on-write under parallel workers: a parent forks children and immediately
   rewrites its memory (mmap arena, a static array, the stack) while each child checks
   it still sees the values from before the fork, then writes and rechecks its own.
   Exit 0 when every child and the parent saw only their own data. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
#define WORDS (1u<<17)           /* 1 MiB of arena memory */
#define STATICS 4096u
static volatile U statics[STATICS];
static void fill(volatile U *p,unsigned n,U tag) { for(unsigned i=0;i<n;i+=8)p[i]=tag^i; }
static int same(volatile U *p,unsigned n,U tag) { for(unsigned i=0;i<n;i+=8)if(p[i]!=(tag^i))return 0;return 1; }
static void fail(unsigned code) { call(64,1,(U)"cow-stress FAIL\n",16,0,0,0);call(94,code,0,0,0,0,0);for(;;){} }
void _start(void) {
    volatile U *arena=(volatile U *)call(222,0,WORDS*8,3,0x22,(U)-1,0);
    if((long)arena<0)fail(1);
    volatile U stack[512];
    for(U round=1;round<=40;round++) {
        U before=round*0x1000003u;
        fill(arena,WORDS,before);fill(statics,STATICS,before);fill(stack,512,before);
        long pids[6];
        for(unsigned c=0;c<6;c++) {
            pids[c]=call(220,17,0,0,0,0,0);
            if(pids[c]<0)fail(2);
            if(!pids[c]) {
                U mine=before^(0xabcdefu+c);
                for(unsigned pass=0;pass<3;pass++)
                    if(!same(arena,WORDS,before) || !same(statics,STATICS,before) || !same(stack,512,before))call(94,10+pass,0,0,0,0,0);
                fill(arena,WORDS,mine);fill(statics,STATICS,mine);fill(stack,512,mine);
                call(124,0,0,0,0,0,0);
                if(!same(arena,WORDS,mine) || !same(statics,STATICS,mine) || !same(stack,512,mine))call(94,20,0,0,0,0,0);
                call(94,0,0,0,0,0,0);
            }
        }
        U after=before^0x5555u;                       /* the parent writes while children check */
        fill(arena,WORDS,after);fill(statics,STATICS,after);fill(stack,512,after);
        for(unsigned c=0;c<6;c++) {
            int status=-1;
            if(call(260,(U)pids[c],(U)&status,0,0,0,0)!=pids[c])fail(3);
            if(status) { char m[]="child failed: x\n";m[14]=(char)('A'+((status>>8)&31));call(64,1,(U)m,16,0,0,0);fail(4); }
        }
        if(!same(arena,WORDS,after) || !same(statics,STATICS,after) || !same(stack,512,after))fail(5);
    }
    call(64,1,(U)"cow-stress PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
