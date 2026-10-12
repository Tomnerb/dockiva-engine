#include "syscall.h"
#define C(n,a,b,c,d,e) call5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e))
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int guest_main(int argc,char **argv) {
    (void)argv;long memory=C(222,0,16*1024*1024,3,34,-1);CHECK(memory>0);
    long base=C(214,0,0,0,0,0);CHECK(C(214,base+8*1024*1024,0,0,0,0)==base+8*1024*1024);
    *(volatile char *)memory=42;*(volatile char *)base=17;
    int fds[2];CHECK(C(59,fds,0,0,0,0)==0);
    for(unsigned i=0;i<4;i++) {
        long child=C(220,17,0,0,0,0);
        if(argc>1) {
            CHECK(child>=0);if(!child)return 0;
            int status=-1;CHECK(C(260,child,&status,0,0,0)==child && status==0);
        } else CHECK(child==-12);
        CHECK(*(volatile char *)memory==42 && *(volatile char *)base==17);
    }
    CHECK(C(57,fds[1],0,0,0,0)==0);
    char byte;CHECK(C(63,fds[0],&byte,1,0,0)==0);CHECK(C(57,fds[0],0,0,0,0)==0);
    CHECK(C(215,memory,16*1024*1024,0,0,0)==0);
    output(1,argc>1?"bounded child allocation succeeded and cleaned up\n":"aggregate allocation denied; parent and pipe state preserved\n");return 0;
}
