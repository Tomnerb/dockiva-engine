/* statfs as a container sees it: run by scripts/verify_overlay.py with its root on a
   small case-sensitive disk image, as the engine keeps its store (dockivad/store.go),
   with and without --statfs-from and the memory layer (--upper-memory). Writes three
   Linux struct statfs (120 bytes each) to stdout: statfs of /, statfs of a file it
   creates (in memory with --upper-memory), and fstatfs of that file. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");return (long)x0;
}
static unsigned char out[360];
void _start(void) {
    long fd=call(56,(U)-100,(U)"/m",0101,0644);                       /* openat(AT_FDCWD, "/m", O_WRONLY|O_CREAT) */
    int ok=fd>=0 && !call(43,(U)"/",(U)out,0,0) && !call(43,(U)"/m",(U)(out+120),0,0) && !call(44,(U)fd,(U)(out+240),0,0);
    if(ok)call(64,1,(U)out,sizeof out,0);
    call(94,ok?0:1,0,0,0);                                             /* exit_group */
    for(;;){}
}
