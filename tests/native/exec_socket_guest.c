/* exec keeps sockets unless they are close-on-exec. The parent opens a plain
   socket (fd 3) and a SOCK_CLOEXEC socket (fd 4), then a child execs /leaf
   (exec_socket_leaf.c), which checks that only fd 3 survived. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(void) { call(64,1,(U)"exec-socket FAIL\n",17,0,0,0);call(93,1,0,0,0,0,0);for(;;){} }
void _start(void) {
    if(call(198,2,1,0,0,0,0)!=3 || call(198,2,1|524288,0,0,0,0)!=4)fail();
    long pid=call(220,17,0,0,0,0,0);if(pid<0)fail();
    if(!pid) { const char *argv[]={"/leaf",0},*envp[]={0};call(221,(U)"/leaf",(U)argv,(U)envp,0,0,0);fail(); }
    int status=-1;if(call(260,(U)pid,(U)&status,0,0,0,0)!=pid || status!=0)fail();
    if(call(25,3,1,0,0,0,0)<0 || call(25,4,1,0,0,0,0)<0)fail(); /* the parent keeps both */
    call(64,1,(U)"exec-socket PASS\n",17,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
