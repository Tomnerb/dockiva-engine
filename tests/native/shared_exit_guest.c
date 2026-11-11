/* A process that exits with a writable MAP_SHARED file mapping in place (Go's
   tools never munmap): 1100 children each map /shared-N, write a byte through the
   mapping and exit. Each byte reaches its file, as on Linux, and no mapping fails:
   the runtime's table of shared file mappings holds 1024, and exits kept their
   entries until mmap answered ENFILE. Needs a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"shared-exit FAIL\n",17,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static void name(char *path,unsigned n) {                /* "/shared-N" */
    const char prefix[]="/shared-";unsigned i=0;for(;prefix[i];i++)path[i]=prefix[i];
    char digits[8];unsigned d=0;do { digits[d++]=(char)('0'+n%10);n/=10; } while(n);
    while(d)path[i++]=digits[--d];path[i]=0;
}
void _start(void) {
    for(unsigned n=0;n<1100;n++) {
        char path[32];name(path,n);
        long pid=call(220,17,0,0,0,0,0);check(pid>=0);                    /* fork */
        if(!pid) {
            long fd=call(56,(U)-100,(U)path,64|2,0600,0,0);if(fd<0)call(94,10,0,0,0,0,0);
            if(call(46,(U)fd,4096,0,0,0,0))call(94,11,0,0,0,0,0);          /* ftruncate */
            long map=call(222,0,4096,3,1,(U)fd,0);                          /* PROT_READ|WRITE, MAP_SHARED */
            if(map<0 && map>-4096)call(94,12,0,0,0,0,0);
            *(volatile char *)map='x';
            call(94,0,0,0,0,0,0);for(;;){}                                  /* exit_group, mapped */
        }
        int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
        long fd=call(56,(U)-100,(U)path,0,0,0,0);check(fd>=0);
        char c=0;check(call(63,(U)fd,(U)&c,1,0,0,0)==1 && c=='x');
        call(57,(U)fd,0,0,0,0,0);call(35,(U)-100,(U)path,0,0,0,0);           /* unlinkat */
    }
    call(64,1,(U)"shared-exit PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
