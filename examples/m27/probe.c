#include "syscall.h"
#define C(n,a,b,c,d,e) call5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e))
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int guest_main(void) {
    char cwd[1025],buf[8];int status=0;
    CHECK(C(34,-100,"/a",0700,0,0)==0);
    CHECK(C(34,-100,"/a/b",0700,0,0)==0);
    CHECK(C(49,"/a",0,0,0,0)==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==3 && equal(cwd,"/a"));
    CHECK(C(17,cwd,2,0,0,0)==-34);
    CHECK(C(17,1,sizeof(cwd),0,0,0)==-14);
    long fd=C(56,-100,"data",66,0600,0);CHECK(fd>=0);
    CHECK(C(64,fd,"abc",3,0,0)==3);CHECK(C(62,fd,0,0,0,0)==0);
    long child=C(220,17,0,0,0,0);CHECK(child>=0);
    if(!child) {
        CHECK(C(49,"b",0,0,0,0)==0);
        CHECK(C(17,cwd,sizeof(cwd),0,0,0)==5 && equal(cwd,"/a/b"));
        CHECK(C(63,fd,buf,1,0,0)==1 && buf[0]=='a');return 0;
    }
    CHECK(C(260,child,&status,0,0,0)==child && status==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==3 && equal(cwd,"/a"));
    CHECK(C(63,fd,buf,2,0,0)==2 && buf[0]=='b' && buf[1]=='c');
    CHECK(C(57,fd,0,0,0,0)==0);
    CHECK(C(49,"..",0,0,0,0)==-13);
    CHECK(C(49,"/escape",0,0,0,0)<0);
    CHECK(C(49,"data",0,0,0,0)==-20);
    CHECK(C(49,"missing",0,0,0,0)==-2);
    CHECK(C(38,-100,"/a",-100,"/renamed",0)==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==-2);
    fd=C(56,-100,"data",0,0,0);CHECK(fd>=0);
    CHECK(C(63,fd,buf,3,0,0)==3 && buf[0]=='a');CHECK(C(57,fd,0,0,0,0)==0);
    CHECK(C(49,"/renamed/./b//",0,0,0,0)==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==11 && equal(cwd,"/renamed/b"));
    CHECK(C(49,"/",0,0,0,0)==0);
    CHECK(C(17,cwd,sizeof(cwd),0,0,0)==2 && equal(cwd,"/"));
    output(1,"cwd inheritance, isolation, shared offsets and boundaries passed\n");return 0;
}
