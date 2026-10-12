/* A forking TCP server: the parent accepts and forks one handler per connection
   while holding the listening socket; a client child connects three times.
   Checks fork with open sockets, per-process close, shared descriptions and reaping. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"fork-server FAIL\n",17,0,0,0);call(93,(U)checks,0,0,0,0,0);for(;;){} } }
#define ROUNDS 3
void _start(void) {
    unsigned char addr[16]={2,0,0,0,0,0,0,0};unsigned size=16;  /* wildcard bind, ephemeral port */
    long server=call(198,2,1,0,0,0,0);check(server>=0);
    check(call(200,server,(U)addr,16,0,0,0)==0);
    check(call(204,server,(U)addr,(U)&size,0,0,0)==0);
    check(call(201,server,8,0,0,0,0)==0);
    long client_pid=call(220,17,0,0,0,0,0);check(client_pid>=0);
    if(!client_pid) { /* client: inherits the listener, closes it, connects ROUNDS times */
        check(call(57,server,0,0,0,0,0)==0);
        for(unsigned i=0;i<ROUNDS;i++) {
            long c=call(198,2,1,0,0,0,0);check(c>=0);
            check(call(203,c,(U)addr,16,0,0,0)==0);
            char reply[4]={0};long n=0,got;
            while(n<3 && (got=call(63,c,(U)(reply+n),(U)(3-n),0,0,0))>0)n+=got;
            check(n==3 && reply[0]=='h' && reply[1]=='i' && reply[2]=='0'+(char)i);
            check(call(63,c,(U)reply,1,0,0,0)==0);              /* handler closed: EOF */
            check(call(57,c,0,0,0,0,0)==0);
        }
        call(93,0,0,0,0,0,0);
    }
    for(unsigned i=0;i<ROUNDS;i++) {
        long conn=call(202,server,0,0,0,0,0);check(conn>=0);
        long handler=call(220,17,0,0,0,0,0);check(handler>=0);
        if(!handler) { /* handler: owns its copies of both sockets */
            check(call(57,server,0,0,0,0,0)==0);
            char msg[3]={'h','i',(char)('0'+i)};
            check(call(64,conn,(U)msg,3,0,0,0)==3);
            check(call(57,conn,0,0,0,0,0)==0);
            call(93,0,0,0,0,0,0);
        }
        check(call(57,conn,0,0,0,0,0)==0); /* parent's copy; the handler's stays open */
        int status=-1;check(call(260,(U)handler,(U)&status,0,0,0,0)==handler && status==0);
    }
    int status=-1;check(call(260,(U)client_pid,(U)&status,0,0,0,0)==client_pid && status==0);
    check(call(57,server,0,0,0,0,0)==0);
    call(64,1,(U)"fork-server PASS\n",17,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
