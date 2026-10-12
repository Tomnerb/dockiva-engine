#include "../m5/syscall.h"
struct pollfd {int fd;unsigned short events,revents;};
int guest_main(U argc,char **argv,char **envp) {
 (void)argc;(void)argv;(void)envp;
 U fd=sc(198,2,2049,0,0,0,0);if(bad(fd))return 1;
 if(sc(25,fd,3,0,0,0,0)!=2050)return 2;
 if(sc(25,fd,4,2,0,0,0)!=0 || sc(25,fd,3,0,0,0,0)!=2)return 3;
 if(sc(25,fd,4,2050,0,0,0)!=0)return 4;
 U time[2]={0,0};struct pollfd p[2];p[0].fd=9999;p[0].events=1;p[0].revents=0;p[1].fd=-1;p[1].events=1;p[1].revents=99;
 if(sc(73,(U)p,2,(U)time,0,0,0)!=1 || p[0].revents!=32 || p[1].revents!=0)return 5;
 if(sc(73,1,1,(U)time,0,0,0)!=(U)-14)return 6;
 time[1]=1000000000;if(sc(73,(U)p,2,(U)time,0,0,0)!=(U)-22)return 7;
 if(sc(57,fd,0,0,0,0,0))return 8;
 return 0;
}
