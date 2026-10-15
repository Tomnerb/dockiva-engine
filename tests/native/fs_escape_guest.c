/* A swapper process renames a directory and a symlink to ../../outside (which the host
   kernel, were it to follow it, would resolve beside the root) in and out of /race/d,
   while three attackers create, change, rename, remove and read files beneath /race/d.
   The verifier checks the host side afterwards; here a read must never return the
   host's secret. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define AT (U)-100
#define ROUNDS 100000
static void finish(unsigned status) { call(94,status,0,0,0,0,0);for(;;){} }
static void put(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void number(U v) { char b[24];int i=23;b[i]=0;do { b[--i]=(char)('0'+v%10);v/=10; } while(v);put(b+i); }
static int leaked(const char *path) {
    long fd=call(56,AT,(U)path,0,0,0,0);if(fd<0)return 0;
    char b[4]={0};call(63,fd,(U)b,4,0,0,0);call(57,fd,0,0,0,0,0);
    return b[0]=='H' && b[1]=='O' && b[2]=='S' && b[3]=='T';
}
static U attack(void) {
    U created=0;
    for(unsigned i=0;i<ROUNDS;i++) {
        long fd=call(56,AT,(U)"/race/d/pwned",64|1,0644,0,0);if(fd>=0) { created++;call(57,fd,0,0,0,0,0); }
        call(53,AT,(U)"/race/d/secret",0777,0,0,0);
        call(88,AT,(U)"/race/d/secret",0,0,0,0);
        call(35,AT,(U)"/race/d/pwned",0,0,0,0);
        if(i%8)continue;                                   /* the rest less often */
        call(34,AT,(U)"/race/d/pd",0755,0,0,0);call(35,AT,(U)"/race/d/pd",0x200,0,0,0);
        call(54,AT,(U)"/race/d/secret",0,0,0,0);
        call(45,(U)"/race/d/secret",0,0,0,0,0);
        call(38,AT,(U)"/race/d/secret",AT,(U)"/race/stolen",0,0);
        call(37,AT,(U)"/race/d/secret",AT,(U)"/race/hard",0,0);
        call(36,(U)"x",AT,(U)"/race/d/sl",0,0,0);
        call(33,AT,(U)"/race/d/fifo",0010644,0,0,0);
        call(35,AT,(U)"/race/d/sub/inner",0,0,0,0);
        if(leaked("/race/d/secret") || leaked("/race/d/sub/inner") || leaked("/race/stolen") || leaked("/race/hard")) { put("LEAK\n");finish(1); }
    }
    return created;
}
void _start(void) {
    call(34,AT,(U)"/race",0755,0,0,0);call(34,AT,(U)"/race/dir",0755,0,0,0);
    call(36,(U)"../../outside",AT,(U)"/race/link",0,0,0);
    long swapper=call(220,17,0,0,0,0,0);if(swapper<0)finish(2);
    if(!swapper) {                                      /* renames are atomic: d is the directory, the link or absent */
        unsigned char st[128];
        while(call(79,AT,(U)"/race/stop",(U)st,0,0,0)<0) {
            call(38,AT,(U)"/race/dir",AT,(U)"/race/d",0,0);call(38,AT,(U)"/race/d",AT,(U)"/race/dir",0,0);
            call(38,AT,(U)"/race/link",AT,(U)"/race/d",0,0);call(38,AT,(U)"/race/d",AT,(U)"/race/link",0,0);
        }
        finish(0);
    }
    long others[2];
    for(int i=0;i<2;i++) { others[i]=call(220,17,0,0,0,0,0);if(others[i]<0)finish(2);if(!others[i]) { attack();finish(0); } }
    U created=attack();unsigned status=0,failed=0;
    for(int i=0;i<2;i++) { call(260,others[i],(U)&status,0,0,0,0);failed|=status; }
    if(call(56,AT,(U)"/race/stop",64|1,0644,0,0)<0)finish(4);
    call(260,swapper,(U)&status,0,0,0,0);
    if(failed || status)finish(3);
    put("race rounds ");number(3*ROUNDS);put(" created ");number(created);put("\n");
    finish(0);
}
