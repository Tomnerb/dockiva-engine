/* x86-64 Linux, freestanding: writes "vdso" when it was given a vDSO (AT_SYSINFO_EHDR)
   whose __vdso_clock_gettime(CLOCK_MONOTONIC) works and agrees with the system call,
   "none" without one, or what failed; then a newline. With "exec" as its argument it
   first execs itself with only A=1 in its environment, so the exec path (an x86
   program a guest program runs) is checked too; with "script" it execs /s, a #!
   script whose interpreter is itself, so FEX running itself again is.
   scripts/verify_x86_vdso.py runs it through FEX. */
typedef unsigned long U;
struct timespec { long sec,nsec; };
static long sys(U n,U a,U b,U c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static U length(const char *s) { U n=0;while(s[n])n++;return n; }
static int same(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a==*b; }
static void finish(const char *s) { sys(1,1,(U)s,length(s));sys(1,1,(U)"\n",1);sys(60,0,0,0); }
static long ns(struct timespec t) { return t.sec*1000000000L+t.nsec; }
void start_c(U *sp) {
    U argc=sp[0];char **argv=(char **)(sp+1),**envp=argv+argc+1;
    if(argc>1 && (argv[1][0]=='e' || argv[1][0]=='s')) {
        static char *args[]={(char *)"/vdso",0},*script[]={(char *)"/s",0},*env[]={(char *)"A=1",0};
        if(argv[1][0]=='s')sys(59,(U)"/s",(U)script,(U)env);  /* execve; its interpreter gets "/s" */
        else sys(59,(U)"/vdso",(U)args,(U)env);
        finish("exec failed");
    }
    char **e=envp;while(*e)e++;
    U base=0;
    for(U *a=(U *)(e+1);a[0];a+=2)if(a[0]==33)base=a[1];      /* AT_SYSINFO_EHDR */
    if(!base)finish("none");
    const unsigned char *h=(const unsigned char *)base;
    if(h[0]!=0x7f || h[1]!='E' || h[2]!='L' || h[3]!='F')finish("no ELF header");
    /* The load bias from the segment at file offset 0; symbols by the section headers
       (the whole file is mapped at base). */
    U phoff=*(const U *)(h+0x20),shoff=*(const U *)(h+0x28);
    unsigned phsize=*(const unsigned short *)(h+0x36),phnum=*(const unsigned short *)(h+0x38);
    unsigned shsize=*(const unsigned short *)(h+0x3a),shnum=*(const unsigned short *)(h+0x3c);
    U bias=base;
    for(unsigned i=0;i<phnum;i++) {
        const unsigned char *p=h+phoff+i*phsize;
        if(*(const unsigned *)p==1 && *(const U *)(p+8)==0) { bias=base-*(const U *)(p+16);break; }  /* PT_LOAD at offset 0 */
    }
    long (*clock)(long,struct timespec *)=0;
    for(unsigned i=0;i<shnum && !clock;i++) {
        const unsigned char *s=h+shoff+i*shsize;
        if(*(const unsigned *)(s+4)!=11)continue;              /* SHT_DYNSYM */
        const unsigned char *strtab=h+*(const U *)(h+shoff+*(const unsigned *)(s+40)*shsize+24);  /* sh_link's sh_offset */
        U offset=*(const U *)(s+24),size=*(const U *)(s+32),entry=*(const U *)(s+56);
        for(U k=0;entry && k<size/entry;k++) {
            const unsigned char *sym=h+offset+k*entry;
            if(same((const char *)strtab+*(const unsigned *)sym,"__vdso_clock_gettime") && *(const U *)(sym+8)) {
                clock=(long (*)(long,struct timespec *))(bias+*(const U *)(sym+8));break;
            }
        }
    }
    if(!clock)finish("no __vdso_clock_gettime");
    struct timespec a={0,0},b={0,0},c={0,0};
    if(clock(1,&a) || sys(228,1,(U)&b,0) || clock(1,&c))finish("clock_gettime failed");   /* CLOCK_MONOTONIC */
    if(!ns(a) || ns(a)>ns(b) || ns(b)>ns(c))finish("clock out of order");
    finish("vdso");
}
__asm__(".globl _start\n_start:\n mov %rsp,%rdi\n and $-16,%rsp\n call start_c\n hlt\n");
