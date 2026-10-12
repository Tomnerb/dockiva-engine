/* Linux ARM64 syscall probe, independent of the runtime implementation. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}

static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }

void _start(void) {
    const U backlogs[]={0,1,128,129,511,2147483647UL,(U)-1,2147483648UL,4294967296UL,4294967807UL};
    for(unsigned i=0;i<sizeof(backlogs)/sizeof(backlogs[0]);i++) {
        unsigned char addr[16]={2,0,0,0,127,0,0,1};unsigned size=16;char value='k',out=0;
        long server=call(198,2,1,0,0,0,0);check(server>=0);
        check(call(200,server,(U)addr,16,0,0,0)==0);
        check(call(204,server,(U)addr,(U)&size,0,0,0)==0 && size==16);
        check(call(201,server,backlogs[i],0,0,0,0)==0);
        long client=call(198,2,1,0,0,0,0);check(client>=0);
        check(call(203,client,(U)addr,16,0,0,0)==0);
        long peer=call(202,server,0,0,0,0,0);check(peer>=0);
        check(call(64,client,(U)&value,1,0,0,0)==1);
        check(call(63,peer,(U)&out,1,0,0,0)==1 && out=='k');
        check(call(57,peer,0,0,0,0,0)==0);
        check(call(57,client,0,0,0,0,0)==0);
        check(call(201,server,backlogs[i],0,0,0,0)==0); /* Repeat on listening socket. */
        check(call(57,server,0,0,0,0,0)==0);
        check(call(201,server,backlogs[i],0,0,0,0)==-9);
    }
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
