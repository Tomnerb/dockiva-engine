#include "syscall.h"
int guest_main(U argc,char **argv,char **envp) {
    (void)argc;(void)argv;(void)envp;
#if CASE == 1
    char byte=0;
    if(sc(57,65535,0,0,0,0,0)!=(U)-9 || sc(63,65535,(U)&byte,1,0,0,0)!=(U)-9) return 91;
#elif CASE == 2
    U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 91;
    if(sc(57,fd,0,0,0,0,0) || sc(57,fd,0,0,0,0,0)!=(U)-9)return 92;
#elif CASE == 3
    U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 91;
    if(sc(200,fd,0,16,0,0,0)!=(U)-14)return 92;
#elif CASE == 4
    U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 91;
    if(sc(63,fd,0,1,0,0,0)!=(U)-14)return 92;
#elif CASE == 5
    U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 91;
    unsigned char addr[16];for(U i=0;i<16;i++)addr[i]=0;addr[0]=2;
    if(sc(200,fd,(U)addr,16,0,0,0)!=(U)-13)return 92;
#elif CASE == 6
    if(sc(198,2,2,0,0,0,0)!=(U)-95)return 91;
#elif CASE == 7
    for(U i=0;i<32;i++)if(bad(sc(198,2,1,0,0,0,0)))return 91;
    if(sc(198,2,1,0,0,0,0)!=(U)-24)return 92;
#elif CASE == 8
    U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 91;
    if(sc(201,fd,8,0,0,0,0)!=(U)-13)return 92;
#endif
    return 0;
}
