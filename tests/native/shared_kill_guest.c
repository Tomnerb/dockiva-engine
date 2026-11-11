/* Two processes that share a writable MAP_SHARED file mapping die together: P maps
   /kill-data, writes 'P', forks C, which writes 'C'; both block on a pipe, and the
   first process kills their process group. Both bytes must reach the file, as on
   Linux. A dead image's references to its memory go later (image_destroy_later,
   after the runtime lock), so the second exit's check counted the first's still,
   skipped the write-back, and no later check came: the table kept the file's
   mapping, unwritten. Needs a writable root; --cpus 1 finishes both in one pass. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(unsigned code) {
    char m[]="shared-kill FAIL x\n";m[17]=(char)('0'+code);
    call(64,1,(U)m,19,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
static char zero[4096];
void _start(void) {
    long fd=call(56,(U)-100,(U)"/kill-data",0102|01000,0600,0,0);if(fd<0)fail(1);   /* O_RDWR|O_CREAT|O_TRUNC */
    if(call(64,(U)fd,(U)zero,4096,0,0,0)!=4096)fail(1);
    int ready[2],block[2];
    if(call(59,(U)ready,0,0,0,0,0) || call(59,(U)block,0,0,0,0,0))fail(2);
    long p=call(220,17,0,0,0,0,0);if(p<0)fail(3);
    if(!p) {                                                     /* P */
        call(154,0,0,0,0,0,0);                                  /* setpgid(0, 0) */
        volatile char *map=(volatile char *)call(222,0,4096,3,1,(U)fd,0);
        if((long)map<0)call(93,10,0,0,0,0,0);
        map[0]='P';
        long c=call(220,17,0,0,0,0,0);
        if(!c) { map[1]='C';call(64,(U)ready[1],(U)"c",1,0,0,0); }
        else if(c<0)call(93,11,0,0,0,0,0);
        else call(64,(U)ready[1],(U)"p",1,0,0,0);
        char x;call(63,(U)block[0],(U)&x,1,0,0,0);              /* blocks: nobody writes */
        call(93,12,0,0,0,0,0);
    }
    char got[2];
    if(call(63,(U)ready[0],(U)got,1,0,0,0)!=1 || call(63,(U)ready[0],(U)got+1,1,0,0,0)!=1)fail(4);
    if(call(129,(U)-p,9,0,0,0,0))fail(5);                       /* kill(-P, SIGKILL): P and C */
    int status;
    while(call(260,(U)-1,(U)&status,0,0,0,0)>0) {}              /* wait4 every child (C is ours once P died) */
    char bytes[2]={0,0};
    if(call(67,(U)fd,(U)bytes,2,0,0,0)!=2)fail(6);              /* pread64 */
    if(bytes[0]!='P' || bytes[1]!='C')fail(7);
    call(64,1,(U)"shared-kill PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
