/* Raw IP and packet sockets fail with EPERM (no CAP_NET_RAW), and ping sockets
   (SOCK_DGRAM, IPPROTO_ICMP) work as on Linux: echo requests to 127.0.0.1 come back
   as replies without an IP header, carrying the socket's identifier (the one the
   first request had, or the bound port), with sendto/recvfrom, sendmsg/recvmsg and
   getsockname; anything but an echo request is EINVAL. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"ping FAIL\n",10,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static unsigned char loopback[16]={2,0,0,0,127,0,0,1};
static void request(unsigned char *p,unsigned id,unsigned seq) {    /* type 8, code 0, checksum left wrong on purpose */
    for(unsigned i=0;i<24;i++)p[i]=(unsigned char)i;
    p[0]=8;p[1]=0;p[2]=p[3]=0xee;p[4]=(unsigned char)(id>>8);p[5]=(unsigned char)id;p[6]=(unsigned char)(seq>>8);p[7]=(unsigned char)seq;
}
void _start(void) {
    check(call(198,2,3,1,0,0,0)==-1);                                 /* SOCK_RAW, IPPROTO_ICMP: EPERM */
    check(call(198,2,3,255,0,0,0)==-1 && call(198,10,3,58,0,0,0)==-1);
    check(call(198,17,3,0x0300,0,0,0)==-1 && call(198,17,2,0x0300,0,0,0)==-1); /* AF_PACKET */
    long s=call(198,2,2,1,0,0,0);check(s>=0);
    unsigned char out[24],in[64],from[16];unsigned size=16;
    request(out,0x1234,1);
    check(call(206,(U)s,(U)out,24,0,(U)loopback,16)==24);
    check(call(207,(U)s,(U)in,64,0,(U)from,(U)&size)==24);           /* no IP header */
    check(in[0]==0 && in[1]==0 && in[4]==0x12 && in[5]==0x34 && in[7]==1 && in[23]==23);
    check(size==16 && from[0]==2 && from[4]==127 && from[7]==1);
    size=16;check(call(204,(U)s,(U)from,(U)&size,0,0,0)==0 && from[2]==0x12 && from[3]==0x34);
    out[0]=13;check(call(206,(U)s,(U)out,24,0,(U)loopback,16)==-22);   /* only echo requests */
    long b=call(198,2,2,1,0,0,0);unsigned char bound[16]={2,0,0x43,0x21,127,0,0,1};
    check(call(200,(U)b,(U)bound,16,0,0,0)==0 && call(203,(U)b,(U)loopback,16,0,0,0)==0);
    request(out,0x1111,2);                                            /* the bound port wins */
    U io[2]={(U)out,24},msg[7]={0,0,(U)io,1,0,0,0};
    check(call(211,(U)b,(U)msg,0,0,0,0)==24);
    U back[2]={(U)in,64},reply[7]={(U)from,16,(U)back,1,0,0,0};
    check(call(212,(U)b,(U)reply,0,0,0,0)==24 && in[0]==0 && in[4]==0x43 && in[5]==0x21 && in[7]==2);
    call(64,1,(U)"ping PASS\n",10,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
