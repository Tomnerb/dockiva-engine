/* setuid_guest's leaf: exits 10 as set-user-ID root, 30 as set-group-ID root, 20
   as plain uid/gid 1000, after checking getres[ug]id, /proc/self/status, AT_SECURE
   and Linux's rules for switching between real, effective and saved IDs. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"setuid-leaf FAIL\n",17,0,0,0);call(93,50+(U)checks,0,0,0,0,0);for(;;){} } }
static int contains(const char *text,long n,const char *want) {
    for(long i=0;i<n;i++) { long j=0;while(want[j] && i+j<n && text[i+j]==want[j])j++;if(!want[j])return 1; }
    return 0;
}
static char status[4096];
static long read_status(void) {
    long fd=call(56,(U)-100,(U)"/proc/self/status",0,0,0,0);if(fd<0)return -1;
    long n=call(63,(U)fd,(U)status,sizeof(status),0,0,0);call(57,(U)fd,0,0,0,0,0);return n;
}
static long secure(void) {                                            /* AT_SECURE from /proc/self/auxv */
    static U auxv[96];long fd=call(56,(U)-100,(U)"/proc/self/auxv",0,0,0,0);if(fd<0)return -1;
    long n=call(63,(U)fd,(U)auxv,sizeof(auxv),0,0,0);call(57,(U)fd,0,0,0,0,0);
    for(long i=0;i+1<n/8;i+=2)if(auxv[i]==23)return (long)auxv[i+1];
    return -1;
}
void _start(void) {
    unsigned r=0,e=0,s=0;long n;
    long uid=call(174,0,0,0,0,0,0),euid=call(175,0,0,0,0,0,0),gid=call(176,0,0,0,0,0,0),egid=call(177,0,0,0,0,0,0);
    if(euid==0) {
        check(uid==1000 && gid==1000 && egid==1000 && secure()==1);
        check(call(148,(U)&r,(U)&e,(U)&s,0,0,0)==0 && r==1000 && e==0 && s==0);
        n=read_status();check(n>0 && contains(status,n,"Uid:\t1000\t0\t0\t0\n") && contains(status,n,"Gid:\t1000\t1000\t1000\t1000\n"));
        check(call(147,(U)-1,1000,(U)-1,0,0,0)==0 && call(175,0,0,0,0,0,0)==1000);   /* seteuid(1000) */
        check(call(147,(U)-1,0,(U)-1,0,0,0)==0 && call(175,0,0,0,0,0,0)==0);         /* back: 0 is the saved ID */
        check(call(146,1000,0,0,0,0,0)==0);                                          /* setuid as root: all three */
        check(call(148,(U)&r,(U)&e,(U)&s,0,0,0)==0 && r==1000 && e==1000 && s==1000);
        check(call(146,0,0,0,0,0,0)==-1 && call(147,(U)-1,0,(U)-1,0,0,0)==-1);         /* root is gone */
        call(93,10,0,0,0,0,0);
    }
    if(egid==0) {
        check(uid==1000 && euid==1000 && gid==1000 && secure()==1);
        check(call(150,(U)&r,(U)&e,(U)&s,0,0,0)==0 && r==1000 && e==0 && s==0);
        n=read_status();check(n>0 && contains(status,n,"Gid:\t1000\t0\t0\t0\n") && contains(status,n,"Uid:\t1000\t1000\t1000\t1000\n"));
        check(call(144,1000,0,0,0,0,0)==0 && call(177,0,0,0,0,0,0)==1000);           /* setgid without CAP_SETGID: effective only */
        check(call(150,(U)&r,(U)&e,(U)&s,0,0,0)==0 && r==1000 && e==1000 && s==0);
        check(call(144,0,0,0,0,0,0)==0 && call(177,0,0,0,0,0,0)==0);                 /* 0 is still the saved ID */
        check(call(144,5,0,0,0,0,0)==-1);
        call(93,30,0,0,0,0,0);
    }
    check(uid==1000 && euid==1000 && gid==1000 && egid==1000 && secure()==0);
    call(93,20,0,0,0,0,0);for(;;){}
}
