/* fexecve of a script in a memfd, as apk 3 runs package scripts: execveat(fd, "",
   AT_EMPTY_PATH) and execve("/proc/self/fd/N") (musl's fexecve without execveat).
   The script's interpreter is this program ("#!/fexec child"): run so, it checks
   the script's name and reads the script through it from the start. Writes the
   number of checks passed (4 bytes) and exits 0. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }
static int prefix(const char *s,const char *p) { while(*p)if(*s++!=*p++)return 0;return 1; }

/* The script's interpreter: argv is /fexec, child, the script's name. */
static void child(long argc,char **argv,const char *want) {
    if(argc!=3 || !prefix(argv[2],want)) { for(long i=0;i<argc;i++) { long n=0;while(argv[i][n])n++;call(64,2,(U)argv[i],n,0,0,0);call(64,2,(U)"|",1,0,0,0); }call(93,3,0,0,0,0,0); }
    long fd=call(56,(U)-100,(U)argv[2],0,0,0,0);if(fd<0)call(93,4,0,0,0,0,0);
    char head[2]={0};if(call(63,fd,(U)head,2,0,0,0)!=2 || head[0]!='#' || head[1]!='!')call(93,5,0,0,0,0,0);
    call(93,0,0,0,0,0,0);for(;;){}
}
void start(U *sp) {
    long argc=(long)sp[0];char **argv=(char **)(sp+1);
    if(argc>=2 && argv[1][0]=='c')child(argc,argv,argv[1][1]=='p'?"/proc/self/fd/":"/dev/fd/");
    for(int mode=0;mode<2;mode++) {
        long pid=call(220,17,0,0,0,0,0);check(pid>=0);                      /* fork */
        if(!pid) {
            const char name[]="script",body[]="#!/fexec c \n",bodyp[]="#!/fexec cp\n",empty[]="";
            long fd=call(279,(U)name,0,0,0,0,0);if(fd<0)call(93,10,0,0,0,0,0);    /* memfd_create, not close-on-exec */
            if(call(64,fd,(U)(mode?bodyp:body),sizeof(body)-1,0,0,0)!=(long)sizeof(body)-1)call(93,11,0,0,0,0,0);
            char *args[]={(char *)"script",0},*env[]={0};
            if(!mode)call(281,fd,(U)empty,(U)args,(U)env,0x1000,0);             /* execveat AT_EMPTY_PATH */
            else { char path[]="/proc/self/fd/0";path[14]=(char)('0'+fd);call(221,(U)path,(U)args,(U)env,0,0,0); }
            call(93,12+mode,0,0,0,0,0);for(;;){}
        }
        int status=-1;check(call(260,pid,(U)&status,0,0,0,0)==pid);
        if(status) { call(64,1,(U)&status,4,0,0,0);call(93,98,0,0,0,0,0); }   /* exited 0 */
        check(status==0);
    }
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
__asm__(".globl _start\n_start:\n mov x0, sp\n b start\n");
