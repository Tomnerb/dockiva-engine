/* A server-style fork pattern under parallel workers: the parent keeps a checksummed
   table in its mmap arena and churns small mappings (as malloc does) while it forks
   short-lived children without waiting for each; every child checks the parent's
   table as of its fork, churns its own mappings, and exits. Children are reaped
   later with WNOHANG. Exit 0 when nobody saw another process's writes. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) { call(64,1,(U)"fork-churn FAIL\n",16,0,0,0);call(94,code,0,0,0,0,0);for(;;){} }
static void hex(const char *label,U v) {             /* a diagnostic line on stderr */
    char m[40];unsigned n=0;while(label[n]) { m[n]=label[n];n++; }
    for(int i=15;i>=0;i--)m[n++]="0123456789abcdef"[(v>>(i*4))&15];
    m[n++]='\n';call(64,2,(U)m,n,0,0,0);
}
#define TABLE 8192u
static volatile U *table;
static U stamp;
static void write_table(U tag) { stamp=tag;for(unsigned i=0;i<TABLE;i++)table[i]=tag*0x9e3779b97f4a7c15u+i; }
static int check_table(U tag) { for(unsigned i=0;i<TABLE;i++)if(table[i]!=tag*0x9e3779b97f4a7c15u+i)return 0;return 1; }
static U me;
static void churn(U seed) {                     /* map, fill, check and unmap a few regions */
    for(unsigned k=0;k<4;k++) {
        U pages=1+((seed+k)%37),bytes=pages*4096;
        volatile U *p=(volatile U *)call(222,0,bytes,3,0x22,(U)-1,0);
        if((long)p<0)fail(10);
        for(U i=0;i<bytes/8;i+=64) {                    /* fresh mappings read zero */
            if(p[i]) { hex("pid=",(U)call(172,0,0,0,0,0,0));hex("addr=",(U)&p[i]);hex("value=",p[i]);hex("seed=",seed);hex("pages=",pages);hex("index=",i);fail(11); }
            p[i]=(me<<40)^(seed+i);
        }
        for(U i=0;i<bytes/8;i+=64)if(p[i]!=((me<<40)^(seed+i)))fail(12);
        if(call(215,(U)p,bytes,0,0,0,0))fail(13);
    }
}
void _start(void) {
    table=(volatile U *)call(222,0,TABLE*8,3,0x22,(U)-1,0);
    if((long)table<0)fail(1);
    unsigned live=0;me=1;
    for(U round=1;round<=3000;round++) {
        write_table(round);churn(round);
        long pid=call(220,17,0,0,0,0,0);
        if(pid<0) { if(pid!=-11)fail(2);round--; }
        else if(!pid) {
            U mine=stamp;me=(U)call(172,0,0,0,0,0,0);
            if(!check_table(mine))call(94,30,0,0,0,0,0);
            churn(mine*7);
            if(!check_table(mine))call(94,31,0,0,0,0,0);
            write_table(mine^0xfeed);if(!check_table(mine^0xfeed))call(94,32,0,0,0,0,0);
            call(94,0,0,0,0,0,0);
        } else live++;
        for(;;) {                                   /* reap whatever finished (WNOHANG) */
            int status=-1;long done=call(260,(U)-1,(U)&status,1,0,0,0);
            if(done<=0)break;
            if(status)fail(40+((status>>8)&15));
            live--;
        }
        if(live>40) { int status=-1;if(call(260,(U)-1,(U)&status,0,0,0,0)<=0 || status)fail(50);live--; }
        if(!check_table(round))fail(5);             /* the parent's own table survived its children */
    }
    while(live) { int status=-1;if(call(260,(U)-1,(U)&status,0,0,0,0)<=0 || status)fail(60);live--; }
    call(64,1,(U)"fork-churn PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
