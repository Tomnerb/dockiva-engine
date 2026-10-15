/* Raw IPv4 ICMP sockets for root (Docker gives it CAP_NET_RAW): echo requests the
   guest checksums itself, replies with a Linux IP header (total length in network
   order, a valid checksum). Other raw protocols, raw IPv6, AF_PACKET and non-root
   raw sockets fail with EPERM. Ping sockets (SOCK_DGRAM, IPPROTO_ICMP) work as on
   Linux: echo requests to 127.0.0.1 come back as replies without an IP header,
   carrying the socket's identifier (the one the first request had, or the bound
   port), with sendto/recvfrom, sendmsg/recvmsg and getsockname; anything but an echo
   request is EINVAL. Needs --process-tree. */
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
    check(call(198,2,3,255,0,0,0)==-1 && call(198,10,3,58,0,0,0)==-1);  /* EPERM */
    check(call(198,17,3,0x0300,0,0,0)==-1 && call(198,17,2,0x0300,0,0,0)==-1); /* AF_PACKET */
    long r=call(198,2,3,1,0,0,0);check(r>=0);                          /* root: SOCK_RAW, IPPROTO_ICMP */
    unsigned char raw[24],rx[64];request(raw,0x4242,7);raw[2]=raw[3]=0;
    unsigned sum=0;for(unsigned i=0;i<24;i+=2)sum+=raw[i]<<8|raw[i+1];
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    raw[2]=(unsigned char)(~sum>>8);raw[3]=(unsigned char)~sum;
    check(call(206,(U)r,(U)raw,24,0,(U)loopback,16)==24);
    long got=call(207,(U)r,(U)rx,64,0,0,0);
    check(got==44 && rx[0]==0x45 && rx[2]==0 && rx[3]==44 && rx[9]==1 && rx[12]==127 && rx[15]==1);
    sum=0;for(unsigned i=0;i<20;i+=2)sum+=rx[i]<<8|rx[i+1];
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    check(sum==0xffff && rx[20]==0 && rx[24]==0x42 && rx[25]==0x42 && rx[27]==7); /* header checksum, the reply */
    long pid=call(220,17,0,0,0,0,0);
    if(!pid) { call(146,1000,0,0,0,0,0);call(94,call(198,2,3,1,0,0,0)==-1?0:1,0,0,0,0,0); }   /* non-root: EPERM */
    int status=-1;check(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
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
    long z=call(198,2,2,1,0,0,0);unsigned char any[16]={2,0};size=16;  /* binding port 0 picks an identifier */
    check(call(200,(U)z,(U)any,16,0,0,0)==0 && call(204,(U)z,(U)from,(U)&size,0,0,0)==0 && (from[2] || from[3]));
    unsigned char ident[2]={from[2],from[3]};request(out,0x2222,3);
    check(call(206,(U)z,(U)out,24,0,(U)loopback,16)==24 && call(207,(U)z,(U)in,64,0,0,0)==24 && in[4]==ident[0] && in[5]==ident[1]);
    request(out,0x1111,2);                                            /* the bound port wins */
    U io[2]={(U)out,24},msg[7]={0,0,(U)io,1,0,0,0};
    check(call(211,(U)b,(U)msg,0,0,0,0)==24);
    U back[2]={(U)in,64},reply[7]={(U)from,16,(U)back,1,0,0,0};
    check(call(212,(U)b,(U)reply,0,0,0,0)==24 && in[0]==0 && in[4]==0x43 && in[5]==0x21 && in[7]==2);
    call(64,1,(U)"ping PASS\n",10,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
