/* Set-user-ID and set-group-ID exec, run as uid/gid 1000 (--uid 1000 --gid 1000):
   /suid (04755) and /sgid (02755) run with effective and saved ID 0 (setuid_leaf
   checks the details and exits 10 or 30), /plain keeps 1000 (20), and with
   no_new_privs /suid keeps it too; uid 1000 itself cannot become root. Needs --process-tree and a root holding them. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"setuid FAIL\n",12,0,0,0);call(93,(U)checks,0,0,0,0,0);for(;;){} } }
static int run(const char *path) {                                    /* the leaf's exit code */
    long pid=call(220,17,0,0,0,0,0);
    if(!pid) { const char *argv[]={path,0},*envp[]={0};call(221,(U)path,(U)argv,(U)envp,0,0,0);call(93,127,0,0,0,0,0); }
    int status=-1;if(pid<=0 || call(260,(U)pid,(U)&status,0,0,0,0)!=pid)return -1;
    return (status>>8)&255;
}
void _start(void) {
    check(call(174,0,0,0,0,0,0)==1000 && call(175,0,0,0,0,0,0)==1000 && call(177,0,0,0,0,0,0)==1000);
    check(call(146,0,0,0,0,0,0)==-1 && call(144,0,0,0,0,0,0)==-1);       /* a non-root run holds no capabilities */
    check(run("/suid")==10);
    check(run("/sgid")==30);
    check(run("/plain")==20);
    check(call(174,0,0,0,0,0,0)==1000 && call(175,0,0,0,0,0,0)==1000);  /* the caller is unchanged */
    long pid=call(220,17,0,0,0,0,0);                                    /* a child's no_new_privs (apt's sandbox sets it) is the child's */
    if(!pid)call(93,call(167,38,1,0,0,0,0)==0 && call(167,39,0,0,0,0,0)==1 && run("/suid")==20?0:1,0,0,0,0,0);
    int status=-1;check(pid>0 && call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    check(call(167,39,0,0,0,0,0)==0 && run("/suid")==10);
    check(call(167,38,1,0,0,0,0)==0);                                   /* PR_SET_NO_NEW_PRIVS */
    check(run("/suid")==20);
    call(64,1,(U)"setuid PASS\n",12,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
