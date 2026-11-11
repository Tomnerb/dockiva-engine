/* x86-64 Linux, freestanding: writes the GODEBUG it was given ("unset" without one)
   and a newline. With "exec" as its argument it first execs itself with only A=1
   in its environment, so the exec path (an x86 program a guest program runs) is
   checked too. scripts/verify_x86_godebug.py runs it through FEX. */
typedef unsigned long U;
static long sys(U n,U a,U b,U c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static U length(const char *s) { U n=0;while(s[n])n++;return n; }
void start_c(U *sp) {
    U argc=sp[0];char **argv=(char **)(sp+1),**envp=argv+argc+1;
    if(argc>1 && argv[1][0]=='e') {
        static char *args[]={(char *)"/godebug",0},*env[]={(char *)"A=1",0};
        sys(59,(U)"/godebug",(U)args,(U)env);                 /* execve */
        sys(60,3,0,0);
    }
    const char *value="unset";
    for(char **e=envp;*e;e++) {
        const char *s=*e;
        if(s[0]=='G' && s[1]=='O' && s[2]=='D' && s[3]=='E' && s[4]=='B' && s[5]=='U' && s[6]=='G' && s[7]=='=') { value=s+8;break; }
    }
    sys(1,1,(U)value,length(value));sys(1,1,(U)"\n",1);
    sys(60,0,0,0);                                             /* exit */
}
__asm__(".globl _start\n_start:\n mov %rsp,%rdi\n and $-16,%rsp\n call start_c\n hlt\n");
