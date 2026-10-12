/* One descriptor table: pipes, sockets and files take the lowest free numbers;
   dup2/dup3 place any kind on any number (closing the old one), F_DUPFD honours its
   minimum, and repeated dup(2) yields consecutive numbers until EMFILE (the probe
   PostgreSQL uses to size its file cache). Needs a writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"fd-table FAIL\n",14,0);call(94,checks,0,0,0);for(;;){} } }
void _start(void) {
    int p[2];check(call(59,(U)p,0,0,0)==0 && p[0]==3 && p[1]==4);                  /* pipe2 */
    long s=call(198,2,1,0,0);check(s==5);                                         /* socket */
    long f=call(56,(U)-100,(U)"/table.txt",0102,0644);check(f==6);                 /* openat O_CREAT|O_RDWR */
    check(call(57,4,0,0,0)==0 && call(23,(U)f,0,0,0)==4);                         /* close, dup: lowest */
    check(call(64,4,(U)"hi",2,0)==2);                                             /* the dup writes the file */
    check(call(24,(U)s,10,0,0)==10 && call(25,10,1,0,0)==0);                      /* dup2 a socket to 10 */
    check(call(24,(U)p[0],(U)f,524288,0)==(long)f && call(25,(U)f,1,0,0)==1);      /* dup3 a pipe over the file */
    check(call(25,(U)s,1030,20,0)==20);                                           /* F_DUPFD_CLOEXEC from 20 */
    check(call(24,(U)s,(U)s,0,0)==-22);                                           /* dup3 onto itself */
    long first=call(23,2,0,0,0),last=first,n=1;
    for(;;) { long next=call(23,2,0,0,0);if(next<0) { check(next==-24);break; }
              if(next!=last+1 && next!=last+2 && !(next==10 || next==20))check(next>last);last=next;n++; }
    check(first==7 && n>400);                                                     /* dense, and plenty */
    call(64,1,(U)"fd-table PASS\n",14,0);call(94,0,0,0,0);for(;;){}
}
