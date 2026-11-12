/* Many files mapped MAP_SHARED and read-only at once, as Go's linker maps a large
   module's archives (over a thousand). Each mapped file is an entry in the runtime's
   table of shared files, one table for every process of the container, and it held
   1024: then mmap failed with ENFILE ("mapping output file failed: too many open
   files in system"). A child maps 650 files of 16 KiB and waits while this process maps 650
   others (each process stays under its own 1024 mappings); a write() to one of them
   shows in its read-only mapping, and a new file mapped read-write and written
   reaches the file on munmap. Needs a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"shared-many FAIL\n",17,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define FILES 1300
#define HALF 650
#define SIZE (16UL<<10)
static char page[SIZE];
static volatile unsigned char *maps[FILES];
static void name(char *path,unsigned n) {                 /* "/fN" */
    unsigned i=0;path[i++]='/';path[i++]='f';
    char d[8];unsigned k=0;do { d[k++]=(char)('0'+n%10);n/=10; } while(n);
    while(k)path[i++]=d[--k];path[i]=0;
}
static void map_files(unsigned from,unsigned to) {        /* read-only, MAP_SHARED, kept */
    for(unsigned n=from;n<to;n++) {
        char path[16];name(path,n);
        long fd=call(56,(U)-100,(U)path,0,0,0,0);check(fd>=0);             /* O_RDONLY */
        long m=call(222,0,SIZE,1,1,(U)fd,0);check(m>0 || m<-4096);           /* PROT_READ, MAP_SHARED */
        call(57,(U)fd,0,0,0,0,0);
        maps[n]=(volatile unsigned char *)m;check(maps[n][4096]==n%251);
    }
}
void _start(void) {
    for(unsigned n=0;n<FILES;n++) {
        char path[16];name(path,n);
        for(U i=0;i<SIZE;i+=4096)page[i]=(char)(n%251);
        long fd=call(56,(U)-100,(U)path,64|2,0644,0,0);check(fd>=0);      /* O_CREAT|O_RDWR */
        check(call(64,(U)fd,(U)page,SIZE,0,0,0)==(long)SIZE);call(57,(U)fd,0,0,0,0,0);
    }
    int ready[2],go[2];check(call(59,(U)ready,0,0,0,0,0)==0 && call(59,(U)go,0,0,0,0,0)==0);   /* pipe2 */
    long pid=call(220,17,0,0,0,0,0);check(pid>=0);                        /* fork */
    if(!pid) {
        map_files(0,HALF);
        char c='r';call(64,(U)ready[1],(U)&c,1,0,0,0);
        call(63,(U)go[0],(U)&c,1,0,0,0);                                   /* keeps its mappings until told */
        call(94,0,0,0,0,0,0);for(;;){}
    }
    char c=0;check(call(63,(U)ready[0],(U)&c,1,0,0,0)==1);
    map_files(HALF,FILES);
    long fd=call(56,(U)-100,(U)"/f700",2,0,0,0);check(fd>=0);               /* O_RDWR */
    check(call(68,(U)fd,(U)"Z",1,100,0,0)==1);call(57,(U)fd,0,0,0,0,0);     /* pwrite64 */
    check(maps[700][100]=='Z');
    fd=call(56,(U)-100,(U)"/out",64|2,0644,0,0);check(fd>=0);
    check(call(46,(U)fd,SIZE,0,0,0,0)==0);                                  /* ftruncate */
    long m=call(222,0,SIZE,3,1,(U)fd,0);check(m>0 || m<-4096);              /* RW, MAP_SHARED */
    ((volatile char *)m)[200]='Q';
    check(call(215,(U)m,SIZE,0,0,0,0)==0);
    c=0;check(call(67,(U)fd,(U)&c,1,200,0,0)==1 && c=='Q');                 /* pread64 */
    check(call(64,(U)go[1],(U)"g",1,0,0,0)==1);
    int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    call(64,1,(U)"shared-many PASS\n",17,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
