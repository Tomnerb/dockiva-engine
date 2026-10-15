/* fork_exec_stress_guest's exec target: /leaf DIR UID. Checks what an exec'd
   image inherits and what it must not: the working directory /DIR, identity UID,
   descriptor 7 (its offset shared with the parent) open and 8 (close-on-exec)
   closed, SIGUSR1 still ignored, SIGUSR2's handler reset, SIGHUP still blocked, and
   fresh memory (static, stack, brk heap, mmap) reading zero. Then it dirties all of
   that memory, so a later image reusing it must see zeros again. Exit 0 or the
   number of the failed check. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void leave(U code) { call(93,code,0,0,0,0,0);for(;;){} }
#define STATICS 8192u
static volatile U statics[STATICS];
static int zero(volatile U *p,U words) { for(U i=0;i<words;i++)if(p[i])return 0;return 1; }
static void dirty(volatile U *p,U words) { for(U i=0;i<words;i++)p[i]=~i; }
void leaf_main(U *sp);
__asm__(".globl _start\n_start:\n mov x0, sp\n b leaf_main\n");
void leaf_main(U *sp) {
    char **argv=(char **)(sp+1);
    if(*sp!=3)leave(2);
    volatile U *below=(volatile U *)((U)sp-49152);           /* stack pages never touched yet */
    if(!zero(below,32768/8))leave(3);
    if(!zero(statics,STATICS))leave(4);
    U base=(U)call(214,0,0,0,0,0,0);
    if((U)call(214,base+65536,0,0,0,0,0)!=base+65536)leave(5);
    if(!zero((volatile U *)base,65536/8))leave(6);
    volatile U *m=(volatile U *)call(222,0,262144,3,0x22,(U)-1,0);
    if((long)m<0 || !zero(m,262144/8))leave(7);
    char cwd[64];long n=call(17,(U)cwd,sizeof(cwd),0,0,0,0);
    if(n!=4 || cwd[0]!='/' || cwd[1]!='d' || cwd[2]!=argv[1][1] || cwd[3])leave(8);
    U uid=0;for(const char *s=argv[2];*s;s++)uid=uid*10+(U)(*s-'0');
    if((U)call(174,0,0,0,0,0,0)!=uid || (U)call(175,0,0,0,0,0,0)!=uid)leave(9);
    char c=0;if(call(63,7,(U)&c,1,0,0,0)!=1 || c!='x')leave(10);  /* moves the offset the parent shares */
    if(call(25,8,1,0,0,0,0)!=-9)leave(11);                    /* F_GETFD: closed on exec */
    U act[4]={0};
    if(call(134,10,0,(U)act,8,0,0) || act[0]!=1)leave(12);    /* SIGUSR1: SIG_IGN survives exec */
    if(call(134,12,0,(U)act,8,0,0) || act[0]!=0)leave(13);    /* SIGUSR2: a handler is reset to SIG_DFL */
    U mask=0;if(call(135,0,0,(U)&mask,8,0,0) || !(mask&1))leave(14); /* SIGHUP stays blocked */
    dirty(below,32768/8);dirty(statics,STATICS);dirty((volatile U *)base,65536/8);dirty(m,262144/8);
    leave(0);
}
