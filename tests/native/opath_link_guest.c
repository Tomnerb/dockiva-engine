/* open(O_PATH|O_NOFOLLOW) of a symlink opens the link itself, as Linux: glibc's
   fchmodat(AT_SYMLINK_NOFOLLOW) (lchmod) opens so, fstats the descriptor and answers
   EOPNOTSUPP for a link, which tar -p takes as done for each link it makes. Before, the
   open failed with ELOOP and tar exited 2 ("Cannot change mode ... Too many levels of
   symbolic links"). Run with --root <dir> --writable. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;static char where[]="opath-link FAIL\n";
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)where,sizeof(where)-1,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define AT (U)-100
static unsigned char st[128];
static U mode(void) { return (U)(st[16]|st[17]<<8|st[18]<<16|st[19]<<24)&0170000; }
void _start(void) {
    long r=call(34,AT,(U)"/opath-link",0755,0,0,0);check(r==0 || r==-17);
    call(35,AT,(U)"/opath-link/dangling",0,0,0,0);call(35,AT,(U)"/opath-link/file",0,0,0,0);call(35,AT,(U)"/opath-link/to-file",0,0,0,0);
    long f=call(56,AT,(U)"/opath-link/file",0101|01000000,0755,0,0);check(f>=0);call(57,(U)f,0,0,0,0,0);
    check(call(36,(U)"../missing/x",AT,(U)"/opath-link/dangling",0,0,0)==0);    /* a placeholder tar repoints */
    check(call(36,(U)"file",AT,(U)"/opath-link/to-file",0,0,0)==0);
    const char *links[]={"/opath-link/dangling","/opath-link/to-file"};
    for(int i=0;i<2;i++) {
        long fd=call(56,AT,(U)links[i],0x200000|0100000|02000000,0,0,0);check(fd>=0);   /* O_PATH|O_NOFOLLOW|O_CLOEXEC */
        check(call(79,(U)fd,(U)"",(U)st,0x1000,0,0)==0 && mode()==0120000);        /* fstatat(fd, "", AT_EMPTY_PATH): the link */
        check(call(80,(U)fd,(U)st,0,0,0,0)==0 && mode()==0120000);
        check(call(57,(U)fd,0,0,0,0,0)==0);
        check(call(56,AT,(U)links[i],0100000,0,0,0)==-40);                         /* without O_PATH: ELOOP still */
        check(call(452,AT,(U)links[i],0777,0x100,0,0)==-95);                       /* fchmodat2 NOFOLLOW: EOPNOTSUPP */
    }
    long fd=call(56,AT,(U)"/opath-link/to-file",0x200000,0,0,0);check(fd>=0);      /* O_PATH, followed: the file */
    check(call(80,(U)fd,(U)st,0,0,0,0)==0 && mode()==0100000);call(57,(U)fd,0,0,0,0,0);
    call(64,1,(U)"opath-link PASS\n",16,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
