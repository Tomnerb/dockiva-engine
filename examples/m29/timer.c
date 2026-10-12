#include "syscall.h"
#define C(n,a,b,c,d,e) call5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e))
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
extern void restore_signal(void);
static volatile unsigned caught;
static void handler(unsigned signal) { if(signal==14)caught++; }
int guest_main(int argc,char **argv) {
    if(argc!=2)return 2;
    unsigned long timer[4]={0,20000,0,20000},old[4]={0},zero[4]={0};
    if(equal(argv[1],"validation")) {
        CHECK(C(103,3,timer,0,0,0)==-22);CHECK(C(103,0,1,0,0,0)==-14);
        timer[2]=2;CHECK(C(103,0,timer,1,0,0)==-14);
        CHECK(C(103,0,zero,old,0,0)==0 && old[1]==20000 && old[2]>0);
        timer[3]=1000000;CHECK(C(103,0,timer,0,0,0)==-22);
        timer[3]=0;timer[2]=(unsigned long)-1;CHECK(C(103,0,timer,0,0,0)==-22);
        CHECK(C(103,0,zero,old,0,0)==0 && old[0]==0 && old[1]==0 && old[2]==0 && old[3]==0);
        output(1,"timer validation and error side effects match Linux\n");return 0;
    }
    if(equal(argv[1],"inherit")) {
        timer[0]=timer[1]=timer[3]=0;timer[2]=10;CHECK(C(103,0,timer,0,0,0)==0);
        long child=C(220,17,0,0,0,0);CHECK(child>=0);
        if(!child) { CHECK(C(103,0,zero,old,0,0)==0 && old[0]==0 && old[1]==0 && old[2]==0 && old[3]==0);return 0; }
        int status=-1;CHECK(C(260,child,&status,0,0,0)==child && status==0);
        CHECK(C(103,0,zero,old,0,0)==0 && old[2]>0);output(1,"fork clears child timer and preserves parent timer\n");return 0;
    }
    if(equal(argv[1],"periodic")) {
        unsigned long action[4]={(unsigned long)handler,0x04000000,(unsigned long)restore_signal,0};
        CHECK(C(134,14,action,0,8,0)==0);
    } else { timer[0]=timer[1]=0;timer[2]=equal(argv[1],"cancel")?86400:0;timer[3]=timer[2]?0:50000; }
    int fds[2];CHECK(C(59,fds,0,0,0,0)==0);CHECK(C(103,0,timer,0,0,0)==0);
    if(equal(argv[1],"cancel"))output(1,"ready\n");
    char byte;
    do { CHECK(C(63,fds[0],&byte,1,0,0)==-4); } while(caught<2);
    CHECK(C(103,0,zero,old,0,0)==0 && old[1]==20000);
    CHECK(C(57,fds[0],0,0,0,0)==0);CHECK(C(57,fds[1],0,0,0,0)==0);
    output(1,"periodic alarm interrupts blocked guest reads\n");return 0;
}
