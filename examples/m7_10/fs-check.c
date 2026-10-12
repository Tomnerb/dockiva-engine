#include "../m5/syscall.h"
static const U cwd=(U)-100;
static U op(const char *p,U flags) {return sc(56,cwd,(U)p,flags,0600,0,0);}
static U rd(U fd,void *p,U n) {return sc(63,fd,(U)p,n,0,0,0);}
static U wr(U fd,const void *p,U n) {return sc(64,fd,(U)p,n,0,0,0);}
static U cl(U fd) {return sc(57,fd,0,0,0,0,0);}
int guest_main(U argc,char **argv,char **envp) {
    (void)envp;char mode=argc>1?argv[1][0]:'r';char b[128];U f;
    if(mode=='r') {
        f=op("/hello.txt",0);if(bad(f))return 10;
        if(rd(f,b,5)!=5 || b[0]!='h' || b[4]!='o')return 11;
        if(sc(62,f,1,0,0,0,0)!=1 || rd(f,b,1)!=1 || b[0]!='e')return 12;
        if(sc(80,f,(U)b,0,0,0,0)!=0 || b[48]!=6)return 13;
        if(cl(f) || cl(f)!=(U)-9)return 14;
        return 0;
    }
    if(mode=='w') {
        f=op("/counter",2|64);if(bad(f))return 20;
        U n=rd(f,b,1);if(bad(n))return 21;
        b[0]=n?b[0]+1:'1';
        if(sc(62,f,0,0,0,0,0)!=0 || wr(f,b,1)!=1 || cl(f))return 22;
        wr(1,b,1);wr(1,"\n",1);return 0;
    }
    if(mode=='d') {
        if(sc(34,cwd,(U)"/dir",0700,0,0,0))return 30;
        U dir=op("/dir",16384);if(bad(dir))return 31;
        f=sc(56,dir,(U)"one",2|64|128,0600,0,0);if(bad(f))return 32;
        if(wr(f,"abc",3)!=3 || sc(46,f,2,0,0,0,0))return 33;
        if(sc(38,dir,(U)"one",dir,(U)"two",0,0))return 34;
        if(sc(35,dir,(U)"two",0,0,0,0) || rd(f,b,1)!=0 || cl(f) || cl(dir))return 35;
        if(sc(35,cwd,(U)"/dir",512,0,0,0))return 36;
        return 0;
    }
    if(mode=='o') return op("/new",64|1)==(U)-30?0:40;
    if(mode=='t') return op("/../outside",0)==(U)-13?0:41;
    if(mode=='l') return bad(op("/link",0)) && bad(op("/linked/file",0))?0:42;
    if(mode=='p') return sc(56,cwd,1,0,0,0,0)==(U)-14?0:43;
    if(mode=='b') {f=op("/hello.txt",0);if(bad(f))return 44;return rd(f,0,1)==(U)-14 && sc(80,f,1,0,0,0,0)==(U)-14?0:45;}
    if(mode=='n') return op("/missing",0)==(U)-2?0:46;
    if(mode=='x') {f=op("/hello.txt",0);return bad(f)?47:wr(f,"x",1)==(U)-9?0:48;}
    if(mode=='e') {f=op("/hello.txt",0);return bad(f)?49:rd(f,0,0)==0?0:50;}
    if(mode=='q') {f=op("/counter",2|64);if(bad(f))return 51;return sc(46,f,16777217,0,0,0,0)==(U)-27?0:52;}
    if(mode=='f') { for(U i=0;i<32;i++)if(bad(op("/hello.txt",0)))return 53;return op("/hello.txt",0)==(U)-24?0:54; }
    if(mode=='a') {f=op("/counter",1|64|1024);if(bad(f))return 55;return wr(f,"A",1)==1?0:56;}
    return 99;
}
