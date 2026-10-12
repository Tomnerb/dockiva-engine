/* UDP over loopback: sendto/recvfrom carry addresses, a reply reaches the sender,
   connected sockets use read/write. Connecting a UDP socket to 192.0.2.1 (TEST-NET,
   no packet is sent) needs --outbound: it prints which policy is in force. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"udp FAIL\n",9,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
void _start(void) {
    unsigned char self[16]={2,0,0,0,127,0,0,1},from[16];unsigned size=16,from_size=16;char buf[8];
    long a=call(198,2,2,0,0,0,0),b=call(198,2,2,17,0,0,0);check(a>=0 && b>=0);
    check(call(200,a,(U)self,16,0,0,0)==0 && call(204,a,(U)self,(U)&size,0,0,0)==0 && (self[2] || self[3]));
    check(call(206,b,(U)"ping",4,0,(U)self,16)==4);
    check(call(207,a,(U)buf,8,0,(U)from,(U)&from_size)==4 && buf[0]=='p' && buf[3]=='g');
    check(from_size==16 && from[0]==2 && from[4]==127 && from[7]==1 && (from[2] || from[3]));
    check(call(206,a,(U)"pong",4,0,(U)from,16)==4);                       /* reply to the sender */
    check(call(63,b,(U)buf,8,0,0,0)==4 && buf[1]=='o');
    check(call(203,b,(U)self,16,0,0,0)==0 && call(64,b,(U)"x",1,0,0,0)==1 && call(63,a,(U)buf,8,0,0,0)==1 && buf[0]=='x');
    check(call(206,b,(U)"y",1,0,(U)self,8)==-22);                          /* short address */
    long d=call(198,2,2,0,0,0,0);check(d>=0);                               /* sendmsg/recvmsg, as musl's resolver */
    U out[4]={(U)"ab",2,(U)"cd",2},in[2]={(U)buf,8},send[7]={(U)self,16,(U)out,2,0,0,0};
    check(call(211,d,(U)send,0,0,0,0)==4);
    from_size=0;U recv[7]={(U)from,16,(U)in,1,0,99,7};
    check(call(212,a,(U)recv,0,0,0,0)==4 && buf[0]=='a' && buf[3]=='d' && (unsigned)recv[1]==16 && from[7]==1 && recv[5]==0 && (unsigned)recv[6]==0);
    U one[2]={(U)"A",1},two[2]={(U)"BB",2},batch[16]={(U)self,16,(U)one,1,0,0,0,0,(U)self,16,(U)two,1,0,0,0,0};
    check(call(269,d,(U)batch,2,16384,0,0)==2 && (unsigned)batch[7]==1 && (unsigned)batch[15]==2);  /* sendmmsg, as glibc's resolver */
    check(call(63,a,(U)buf,8,0,0,0)==1 && buf[0]=='A' && call(63,a,(U)buf,8,0,0,0)==2 && buf[1]=='B');
    long c=call(198,2,2,0,0,0,0);unsigned char remote[16]={2,0,0,9,192,0,2,1};
    long r=call(203,c,(U)remote,16,0,0,0);
    if(r==0)call(64,1,(U)"udp outbound PASS\n",18,0,0,0);
    else { check(r==-13);call(64,1,(U)"udp loopback PASS\n",18,0,0,0); }
    call(94,0,0,0,0,0,0);for(;;){}
}
