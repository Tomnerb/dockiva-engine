#include "syscall.h"
static volatile unsigned marker=42;
int guest_main(unsigned long argc,char **argv,char **env) {
    if(argc<2)return 2;
    if(equal(argv[1],"dup-syscalls")) {
        unsigned pair[2];if(call5(59,(long)pair,2048|524288,0,0,0))return 40;
        long copy=call5(23,pair[0],0,0,0,0);if(copy<0)return 41;
        if(call5(25,copy,1,0,0,0)!=0)return 42;
        long clo=call5(25,pair[0],1030,110,0,0);if(clo<110)return 43;
        if(call5(25,clo,1,0,0,0)!=1)return 44;
        if(call5(24,pair[0],0,524288,0,0)!=0)return 45;
        if(call5(25,0,1,0,0,0)!=1)return 46;
        if(call5(57,pair[1],0,0,0,0)!=0)return 47;
        char c;if(call5(63,0,(long)&c,1,0,0)!=0)return 48;
        return 0;
    }
    if(equal(argv[1],"both")) { output(1,"stdout\n");output(2,"stderr\n");return 0; }
    if(equal(argv[1],"emit") || equal(argv[1],"full") || equal(argv[1],"overflow")) {
        static char block[4096];for(unsigned i=0;i<sizeof(block);i++)block[i]='Z';
        unsigned count=equal(argv[1],"emit")?16:equal(argv[1],"full")?256:257;
        for(unsigned i=0;i<count;i++)if(call5(64,1,(long)block,sizeof(block),0,0)!=(long)sizeof(block)) { output(2,"probe: output failed\n");return 1; }
        return 0;
    }
    if(equal(argv[1],"exit"))return argc==3?number(argv[2]):2;
    if(equal(argv[1],"env")) { for(unsigned i=0;env[i];i++) { output(1,env[i]);output(1,"\n"); }return 0; }
    if(equal(argv[1],"spin")) { while(1)marker++; }
    if(equal(argv[1],"exec-loop"))return (int)-call5(221,(long)"/bin/probe",(long)argv,(long)env,0,0);
    if(equal(argv[1],"exec-error")) {
        if(argc!=4)return 2;
        long result=call5(221,(long)argv[2],(long)argv,(long)env,0,0);
        if(result!=-number(argv[3]) || marker!=42)return 3;
        output(1,"exec preserved caller\n");return 0;
    }
    if(equal(argv[1],"fd-check")) {
        char byte=0;
        if(call5(63,64,(long)&byte,1,0,0)!=1 || byte!='a')return 31;
        if(call5(63,65,(long)&byte,1,0,0)!=-9)return 32;
        return 0;
    }
    if(equal(argv[1],"fds")) {
        if(call5(56,-100,(long)"/letters",0,0,0)!=64)return 20;
        if(call5(56,-100,(long)"/letters",524288,0,0)!=65)return 21;
        if(call5(25,64,1,0,0,0)!=0 || call5(25,65,1,0,0,0)!=1)return 22;
        char *next[]={"probe","fd-check",0};
        return (int)-call5(221,(long)"/bin/probe",(long)next,(long)env,0,0);
    }
    if(equal(argv[1],"fd-fork")) {
        if(call5(56,-100,(long)"/letters",0,0,0)!=64)return 20;
        long child=call5(220,17,0,0,0,0);char byte=0;
        if(!child) {
            if(call5(63,64,(long)&byte,1,0,0)!=1 || byte!='a')return 21;
            if(call5(57,64,0,0,0,0)!=0)return 22;
            return 0;
        }
        unsigned status=99;
        if(child<0 || call5(260,child,(long)&status,0,0,0)!=child || status)return 23;
        if(call5(63,64,(long)&byte,1,0,0)!=1 || byte!='b')return 24;
        return 0;
    }
    if(equal(argv[1],"fork")) {
        unsigned local=12,waitstatus=0;
        long base=call5(214,0,0,0,0,0);
        if(call5(214,base+4096,0,0,0,0)!=base+4096)return 40;
        volatile unsigned *heap=(void *)base;*heap=17;
        volatile unsigned *map=(void *)call5(222,0,4096,3,0x22,-1);
        /* The sixth mmap argument (offset) is zero in this startup fixture. */
        if((long)map<0)return 41;*map=19;
        if(call5(220,0x111,0,0,0,0)!=-95)return 3;
        long child=call5(220,17,0,0,0,0);
        if(child<0)return 4;
        if(!child) {
            if(call5(172,0,0,0,0,0)!=2 || call5(173,0,0,0,0,0)!=1)return 5;
            if(call5(220,17,0,0,0,0)!=-11)return 6;
            marker=99;local=33;*heap=21;*map=23;return 37;
        }
        if(marker!=42 || local!=12 || child!=2 || *heap!=17 || *map!=19)return 7;
        if(call5(260,child,1,0,0,0)!=-14)return 8;
        if(call5(260,child,(long)&waitstatus,2,0,0)!=-95)return 9;
        if(call5(260,-1,(long)&waitstatus,1,0,0)!=child || waitstatus!=(37<<8))return 10;
        if(call5(260,child,0,0,0,0)!=-10)return 11;
        output(1,"fork isolation and wait status OK\n");return 0;
    }
    return 2;
}
