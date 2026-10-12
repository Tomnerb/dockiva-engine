#include "syscall.h"
static volatile unsigned counter;
static long socket_new(void) { return call5(198,2,1,0,0,0); }
static long transfer(unsigned call,long fd,char *bytes,unsigned count) {
    unsigned done=0;
    while(done<count) {
        long n=call5(call,fd,(long)(bytes+done),count-done,0,0);
        if(n<=0)return n;done+=(unsigned)n;
    }
    return done;
}
static long listener(unsigned char address[16]) {
    long fd=socket_new();if(fd<0)return fd;
    if(call5(200,fd,(long)address,16,0,0) || call5(201,fd,4,0,0,0))return -1;
    unsigned size=16;if(call5(204,fd,(long)address,(long)&size,0,0) || size!=16)return -1;
    return fd;
}
int guest_main(unsigned long argc,char **argv,char **env) {
    (void)env;if(argc!=2)return 2;
    unsigned char address[16]={2,0,0,0,127,0,0,1};
    if(equal(argv[1],"poll-timeout")) {
        unsigned long timeout[2]={0,100000000};
        if(call5(73,0,0,(long)timeout,0,0))return 3;
        output(1,"poll timeout completed\n");return 0;
    }
    if(equal(argv[1],"accept-timeout")) {
        long fd=listener(address);if(fd<0)return 4;
        unsigned long timeout[2]={0,100000};
        if(call5(208,fd,1,20,(long)timeout,16))return 5;
        if(call5(202,fd,0,0,0,0)!=-11)return 6;
        if(call5(25,fd,3,0,0,0)!=2)return 7;
        if(call5(25,fd,4,2048,0,0) || call5(25,fd,3,0,0,0)!=2050)return 8;
        if(call5(202,fd,0,0,0,0)!=-11)return 9;
        output(1,"accept timeout and guest nonblocking flags preserved\n");return 0;
    }
    if(equal(argv[1],"socket-boundaries")) {
        long fd=socket_new();if(fd<0)return 10;
        if(call5(220,17,0,0,0,0)!=-95)return 11;
        address[4]=8;address[7]=8;if(call5(203,fd,(long)address,16,0,0)!=-13)return 12;
        if(call5(203,fd,1,16,0,0)!=-14)return 13;
        if(call5(25,fd,2,1,0,0) || call5(25,fd,1,0,0,0)!=1)return 14;
        if(call5(57,fd,0,0,0,0) || call5(63,fd,0,0,0,0)!=-9)return 15;
        output(1,"socket isolation boundaries preserved\n");return 0;
    }
    int cancel=equal(argv[1],"cancel-accept"),roundtrip=equal(argv[1],"roundtrip"),readtimeout=equal(argv[1],"read-timeout");
    if(!cancel && !roundtrip && !readtimeout)return 2;
    unsigned ready[2];if(call5(59,(long)ready,0,0,0,0))return 16;
    long server=call5(220,17,0,0,0,0);if(server<0)return 17;
    if(!server) {
        long fd=listener(address);if(fd<0)return 18;
        if(transfer(64,ready[1],(char *)address+2,2)!=2)return 19;
        struct { int fd;unsigned short events,revents; } wait={(int)fd,1,0};
        unsigned long timeout[2]={2,0};
        if(!cancel && (call5(73,(long)&wait,1,(long)timeout,0,0)!=1 || !(wait.revents&1)))return 20;
        long peer=call5(202,fd,0,0,0,0);if(peer<0)return 21;
        char bytes[4];
        if(readtimeout) {
            unsigned long duration[2]={0,100000};
            if(call5(208,peer,1,20,(long)duration,16) || call5(63,peer,(long)bytes,4,0,0)!=-11)return 22;
        } else {
            if(transfer(63,peer,bytes,4)!=4 || bytes[0]!='p' || bytes[3]!='g')return 23;
            if(transfer(64,peer,"pong",4)!=4)return 24;
        }
        return 0;
    }
    if(transfer(63,ready[0],(char *)address+2,2)!=2)return 25;
    long cpu=call5(220,17,0,0,0,0);if(cpu<0)return 26;
    if(!cpu) { for(unsigned i=0;i<100000;i++)counter++;return 31; }
    unsigned status;
    if(call5(260,cpu,(long)&status,0,0,0)!=cpu || status!=(31<<8))return 27;
    if(cancel) {
        if(call5(129,server,15,0,0,0) || call5(260,server,(long)&status,0,0,0)!=server || status!=15)return 28;
        output(1,"blocked socket cancelled; CPU and pipe siblings survived\n");return 0;
    }
    long client=call5(220,17,0,0,0,0);if(client<0)return 29;
    if(!client) {
        long fd=socket_new();if(fd<0 || call5(203,fd,(long)address,16,0,0))return 30;
        if(readtimeout) { char byte;return call5(63,fd,(long)&byte,1,0,0)==0?37:32; }
        char bytes[4];
        if(transfer(64,fd,"ping",4)!=4 || transfer(63,fd,bytes,4)!=4 || bytes[0]!='p' || bytes[3]!='g')return 33;
        return 37;
    }
    if(call5(260,server,(long)&status,0,0,0)!=server || status)return 34;
    if(call5(260,client,(long)&status,0,0,0)!=client || status!=(37<<8))return 35;
    output(1,"network, CPU and pipe siblings completed\n");return 0;
}
