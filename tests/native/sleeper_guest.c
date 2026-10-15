/* A thread that sleeps gets the CPU back when its timer expires, though sibling
   threads never block: three threads loop on system calls (getppid on the host's
   locked path, fstat, openat and close) while the main thread sleeps 3 ms 100
   times. Each sleep after the first (which waits for the new threads' first
   slices) must end within SLACK_MS of its deadline: a woken thread runs before
   the preempted ones, at the next 10 ms tick at the latest. The whole run must take
   under BUDGET_MS (about 1.3 s; starved, it took minutes). Prints the worst lateness. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
#define SLEEPS 100
#define SLACK_MS 40
#define BUDGET_MS 3000
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void num(U v) { char b[24];int i=23;b[i]=0;do { b[--i]=(char)('0'+v%10);v/=10; } while(v);out(b+i); }
static U ms(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000+ts[1]/1000000; }
static unsigned char stacks[3][32768] __attribute__((aligned(16)));
static int words[3];
static void spin(void *arg) {
    U kind=(U)arg;U st[16];long fd=call(56,(U)-100,(U)"/sleeper.elf",0,0,0,0);
    for(;;) {
        if(kind==0)call(173,0,0,0,0,0,0);
        else if(kind==1)call(80,(U)fd,(U)st,0,0,0,0);
        else call(57,(U)call(56,(U)-100,(U)"/sleeper.elf",0,0,0,0),0,0,0,0,0);
    }
}
void _start(void) {
    for(U k=0;k<3;k++)if(spawn(FLAGS,stacks[k]+sizeof(stacks[k]),&words[k],0,&words[k],spin,(void *)k)<=0) { out("sleeper FAIL spawn\n");call(94,1,0,0,0,0,0); }
    U start=ms(),worst=0;
    for(U i=0;i<SLEEPS;i++) {
        U before=ms(),t[2]={0,3000000};call(101,(U)t,0,0,0,0,0);
        U late=ms()-before-3;if(i && late>worst && late<(U)1<<62)worst=late;
    }
    U total=ms()-start;
    out(worst<=SLACK_MS && total<=BUDGET_MS?"sleeper PASS":"sleeper FAIL");
    out(" worst-late-ms=");num(worst);out(" total-ms=");num(total);out("\n");
    call(94,worst<=SLACK_MS && total<=BUDGET_MS?0:1,0,0,0,0,0);for(;;){}   /* exit_group ends the spinners */
}
