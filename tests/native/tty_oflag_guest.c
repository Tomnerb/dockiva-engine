/* verify_tty's terminal writer: sets the c_oflag bits SET and clears CLEAR (Linux
   values) on its terminal (fd 1), then copies /data/payload to it, or with TEXT
   writes that instead. Built once per variant (-DSET=... -DCLEAR=... [-DTEXT=...]). */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void put(const char *p,long n) { while(n>0) { long w=call(64,1,(U)p,(U)n,0,0,0);if(w<=0)call(93,3,0,0,0,0,0);p+=w;n-=w; } }
static char buffer[65536];
void _start(void) {
    unsigned termios[15];                                              /* struct termios: 60 bytes */
    if(call(29,1,0x5401,(U)termios,0,0,0))call(93,1,0,0,0,0,0);        /* TCGETS */
    termios[1]=(termios[1]|(SET))&~(unsigned)(CLEAR);
    if(call(29,1,0x5402,(U)termios,0,0,0))call(93,2,0,0,0,0,0);        /* TCSETS */
#ifdef TEXT
    static const char text[]=TEXT;put(text,sizeof(text)-1);
#else
    long fd=call(56,(U)-100,(U)"/data/payload",0,0,0,0),n;if(fd<0)call(93,4,0,0,0,0,0);
    while((n=call(63,(U)fd,(U)buffer,sizeof(buffer),0,0,0))>0)put(buffer,n);
#endif
    call(93,0,0,0,0,0,0);for(;;){}
}
