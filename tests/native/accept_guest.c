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
    unsigned char addr[16]={2,0,0,0,127,0,0,1};unsigned size=16;
    long server=call(198,2,1,0,0,0,0);check(server>=0);
    check(call(200,server,(U)addr,16,0,0,0)==0);
    check(call(204,server,(U)addr,(U)&size,0,0,0)==0);
    check(call(201,server,8,0,0,0,0)==0);
    check(call(205,server,(U)addr,(U)&size,0,0,0)==-107);
    for(unsigned test=0;test<7;test++) {
        unsigned char peer[20],local[16];for(unsigned j=0;j<20;j++)peer[j]=171;
        unsigned capacity=test==1?4:test==2?0:16,local_size=16;
        long client=call(198,2,1,0,0,0,0);check(client>=0);
        check(call(203,client,(U)addr,16,0,0,0)==0);
        check(call(204,client,(U)local,(U)&local_size,0,0,0)==0);
        U output=test==3?0:test==4?1:(U)(peer+1);
        U length=test==3?1:test==5?0:(U)&capacity;
        if(test==6)capacity=0xffffffff;
        long accepted=call(test==0?202:242,server,output,length,test==0?0:526336,0,0);
        if(test>=4)check(accepted==(test==6?-22:-14));
        else {
            check(accepted>=0);
            unsigned char remote[18];unsigned len=16;int same=1;
            check(call(205,accepted,(U)(remote+1),(U)&len,0,0,0)==0 && len==16);
            for(unsigned j=0;j<16;j++)if(remote[j+1]!=local[j])same=0;
            check(same);
            for(unsigned j=0;j<18;j++)remote[j]=171;
            len=4;check(call(205,accepted,(U)(remote+1),(U)&len,0,0,0)==0 && len==16);
            same=remote[0]==171 && remote[5]==171;
            for(unsigned j=0;j<4;j++)if(remote[j+1]!=local[j])same=0;
            check(same);
            len=0;check(call(205,accepted,0,(U)&len,0,0,0)==0 && len==16);
            check(call(205,accepted,(U)remote,0,0,0,0)==-14);
            len=16;check(call(205,accepted,0,(U)&len,0,0,0)==-14);
            len=0xffffffff;check(call(205,accepted,(U)remote,(U)&len,0,0,0)==-22);
            len=16;check(call(205,client,(U)remote,(U)&len,0,0,0)==0 && len==16);
            same=1;for(unsigned j=0;j<16;j++)if(remote[j]!=addr[j])same=0;
            check(same);
            if(test!=3) {
                check(capacity==16);
                unsigned bytes=test==1?4:test==2?0:16;
                int match=1;for(unsigned j=0;j<bytes;j++)if(peer[j+1]!=local[j])match=0;
                check(match);check(peer[0]==171 && peer[bytes+1]==171);
            }
            check((call(25,accepted,3,0,0,0,0)&2048)==(test==0?0:2048));
            char c='k',got=0;check(call(64,client,(U)&c,1,0,0,0)==1);
            /* The data is local; poll before reading a nonblocking accepted socket. */
            struct { int fd;short events,revents; } pollfd={(int)accepted,1,0};
            U timeout[2]={1,0};check(call(73,(U)&pollfd,1,(U)timeout,0,0,0)==1);
            check(call(63,accepted,(U)&got,1,0,0,0)==1 && got=='k');
            check(call(57,accepted,0,0,0,0,0)==0);
        }
        check(call(57,client,0,0,0,0,0)==0);
    }
    check(call(57,server,0,0,0,0,0)==0);
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
