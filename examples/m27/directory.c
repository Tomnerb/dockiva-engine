#include "syscall.h"
#define C(n,a,b,c,d,e) call5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e))
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int guest_main(void) {
    CHECK(C(34,-100,"/dir",0700,0,0)==0);
    long fd=C(56,-100,"/dir/item",66,0600,0);CHECK(fd>=0);CHECK(C(57,fd,0,0,0,0)==0);
    long directory=C(56,-100,"/dir",16384,0,0);CHECK(directory>=0);
    char entries[512],cwd[1025];CHECK(C(61,directory,entries,24,0,0)==24);
    CHECK(C(50,directory,0,0,0,0)==0);CHECK(C(17,cwd,sizeof(cwd),0,0,0)==5 && equal(cwd,"/dir"));
    long child=C(220,17,0,0,0,0);CHECK(child>=0);
    if(!child) {
        CHECK(C(61,directory,entries,sizeof(entries),0,0)>0);
        CHECK(C(61,directory,entries,sizeof(entries),0,0)==0);
        CHECK(C(57,directory,0,0,0,0)==0);CHECK(C(49,"/",0,0,0,0)==0);return 0;
    }
    int status=-1;CHECK(C(260,child,&status,0,0,0)==child && status==0);
    CHECK(C(61,directory,entries,sizeof(entries),0,0)==0);
    CHECK(C(62,directory,0,0,0,0)==0);CHECK(C(61,directory,entries,sizeof(entries),0,0)>0);
    CHECK(C(57,directory,0,0,0,0)==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==5 && equal(cwd,"/dir"));
    fd=C(56,-100,"item",0,0,0);CHECK(fd>=0);CHECK(C(50,fd,0,0,0,0)==-20);CHECK(C(57,fd,0,0,0,0)==0);
    CHECK(C(50,999,0,0,0,0)==-9);
    output(1,"fchdir, shared directory cursor and descriptor lifetime passed\n");return 0;
}
