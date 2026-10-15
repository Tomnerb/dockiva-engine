/* IPv6 sockets: a dual-stack TCP listener on [::] (IPV6_V6ONLY 0, Linux's default)
   reports [::] and takes IPv6 clients from [::1] and IPv4 ones from 127.0.0.1 and
   ::ffff:127.0.0.1; IPv4 listeners on 0.0.0.0 report 0.0.0.0; UDP between [::] and
   [::1] carries [::1] source addresses; an IPV6_V6ONLY listener on [::] shares its
   port with an IPv4 one on 0.0.0.0, each reached by its own family. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)"inet6 FAIL\n",11,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
static void v6(unsigned char a[28],unsigned char p0,unsigned char p1,int last,int mapped) {   /* [::], [::1] or ::ffff:127.0.0.1 */
    for(unsigned i=0;i<28;i++)a[i]=0;
    a[0]=10;a[2]=p0;a[3]=p1;a[23]=(unsigned char)last;
    if(mapped) { a[18]=a[19]=255;a[20]=127;a[23]=1; }
}
static int zero(const unsigned char *p,unsigned n) { for(unsigned i=0;i<n;i++)if(p[i])return 0;return 1; }
static long serve(long listener,long client,const unsigned char *to,unsigned length,unsigned char peer[28]) {
    unsigned size=28;check(call(203,(U)client,(U)to,length,0,0,0)==0);
    long s=call(202,(U)listener,(U)peer,(U)&size,0,0,0);check(s>=0 && size==28 && peer[0]==10);
    char c=0;check(call(64,(U)client,(U)"z",1,0,0,0)==1 && call(63,(U)s,(U)&c,1,0,0,0)==1 && c=='z');
    call(57,(U)s,0,0,0,0,0);call(57,(U)client,0,0,0,0,0);return 0;
}
void _start(void) {
    unsigned char any[28],name[28],peer[28],to[28];unsigned size=28,value=1,vsize=4;
    long l=call(198,10,1,0,0,0,0);check(l>=0);
    check(call(209,(U)l,41,26,(U)&value,(U)&vsize,0)==0 && value==0);       /* IPV6_V6ONLY reads 0 */
    v6(any,0,0,0,0);check(call(200,(U)l,(U)any,28,0,0,0)==0 && call(201,(U)l,16,0,0,0,0)==0);
    check(call(204,(U)l,(U)name,(U)&size,0,0,0)==0 && size==28 && name[0]==10 && (name[2] || name[3]) && zero(name+8,16));
    v6(to,name[2],name[3],1,0);serve(l,call(198,10,1,0,0,0,0),to,28,peer);         /* from [::1] */
    check(peer[23]==1 && zero(peer+8,15));
    unsigned char four[16]={2,0,name[2],name[3],127,0,0,1};serve(l,call(198,2,1,0,0,0,0),four,16,peer); /* from 127.0.0.1 */
    v6(to,name[2],name[3],0,1);serve(l,call(198,10,1,0,0,0,0),to,28,peer);         /* to ::ffff:127.0.0.1 */
    call(57,(U)l,0,0,0,0,0);
    long w=call(198,2,1,0,0,0,0);unsigned char wild[16]={2,0},wname[16];size=16; /* IPv4 0.0.0.0 */
    check(call(200,(U)w,(U)wild,16,0,0,0)==0 && call(201,(U)w,4,0,0,0,0)==0);
    check(call(204,(U)w,(U)wname,(U)&size,0,0,0)==0 && size==16 && (wname[2] || wname[3]) && zero(wname+4,4));
    call(57,(U)w,0,0,0,0,0);
    long u=call(198,10,2,0,0,0,0),v=call(198,10,2,0,0,0,0);check(u>=0 && v>=0);   /* UDP */
    v6(any,0,0,0,0);size=28;check(call(200,(U)u,(U)any,28,0,0,0)==0 && call(204,(U)u,(U)name,(U)&size,0,0,0)==0);
    v6(to,name[2],name[3],1,0);check(call(206,(U)v,(U)"u6",2,0,(U)to,28)==2);
    char buf[4];size=28;check(call(207,(U)u,(U)buf,4,0,(U)peer,(U)&size)==2 && size==28 && peer[0]==10 && peer[23]==1 && zero(peer+8,15));
    long only=call(198,10,1,0,0,0,0),four_l=call(198,2,1,0,0,0,0);value=1;     /* V6ONLY [::]:p beside 0.0.0.0:p */
    check(call(208,(U)only,41,26,(U)&value,4,0)==0);
    v6(any,0,0,0,0);check(call(200,(U)only,(U)any,28,0,0,0)==0 && call(201,(U)only,4,0,0,0,0)==0);
    size=28;check(call(204,(U)only,(U)name,(U)&size,0,0,0)==0 && (name[2] || name[3]));
    unsigned char same[16]={2,0,name[2],name[3]};check(call(200,(U)four_l,(U)same,16,0,0,0)==0 && call(201,(U)four_l,4,0,0,0,0)==0);
    v6(to,name[2],name[3],1,0);serve(only,call(198,10,1,0,0,0,0),to,28,peer);
    unsigned char back[16]={2,0,name[2],name[3],127,0,0,1};size=16;
    long c4=call(198,2,1,0,0,0,0);check(call(203,(U)c4,(U)back,16,0,0,0)==0 && call(202,(U)four_l,(U)peer,(U)&size,0,0,0)>=0 && size==16 && peer[4]==127);
    call(64,1,(U)"inet6 PASS\n",11,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
