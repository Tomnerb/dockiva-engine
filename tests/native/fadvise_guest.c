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
    const char path[]="/sample",dir[]="/";char data[6]={0};int pipes[2];
    long fd=call(56,(U)-100,(U)path,0,0,0,0);check(fd>=0);
    check(call(62,fd,2,0,0,0,0)==2);
    for(U advice=0;advice<6;advice++) {
        check(call(223,fd,0,0,advice,0,0)==0);
        check(call(223,fd,1,3,advice,0,0)==0);
        check(call(223,fd,8192,4096,advice,0,0)==0);
    }
    check(call(62,fd,0,1,0,0,0)==2); /* Hints do not move the offset. */
    check(call(223,fd,(U)-1,0,0,0,0)==0);
    check(call(223,fd,0,(U)-1,0,0,0)==-22);
    check(call(223,fd,0,0,6,0,0)==-22);
    check(call(223,fd,0,0,(U)-1,0,0)==-22);
    check(call(223,fd+4294967296UL,0,0,4294967296UL,0,0)==0);
    check(call(223,fd,9223372036854775807UL,9223372036854775807UL,0,0,0)==0);
    long copy=call(23,fd,0,0,0,0,0);check(copy>=0);
    check(call(223,copy,0,0,4,0,0)==0);
    check(call(57,fd,0,0,0,0,0)==0);
    check(call(223,fd,0,0,4,0,0)==-9);
    check(call(62,copy,0,0,0,0,0)==0);
    check(call(63,copy,(U)data,5,0,0,0)==5);
    check(data[0]=='h' && data[1]=='e' && data[2]=='l' && data[3]=='l' && data[4]=='o');
    check(call(57,copy,0,0,0,0,0)==0);
    check(call(59,(U)pipes,2048,0,0,0,0)==0);
    check(call(223,pipes[0],0,0,4,0,0)==-29);
    check(call(223,pipes[1],0,(U)-1,99,0,0)==-29);
    check(call(57,pipes[0],0,0,0,0,0)==0);
    check(call(57,pipes[1],0,0,0,0,0)==0);
    fd=call(56,(U)-100,(U)dir,16384,0,0,0);check(fd>=0);
    check(call(223,fd,0,0,0,0,0)==0);
    check(call(57,fd,0,0,0,0,0)==0);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
