#include "syscall.h"
extern void restore_signal(void),clobber_vector(void);
extern unsigned long check_signal_registers(void);
typedef struct {
    unsigned long fault,regs[31],sp,pc,pstate;
    unsigned char reserved[4096] __attribute__((aligned(16)));
} Context;
typedef struct {
    unsigned long flags,link,stack;
    unsigned stackflags,pad;
    unsigned long stacksize,mask;
    unsigned char unused[120];
    Context machine;
} UContext;
_Static_assert(__builtin_offsetof(UContext,machine)==176,"ARM64 mcontext offset");
_Static_assert(__builtin_offsetof(Context,reserved)==288,"ARM64 FPSIMD offset");
static volatile unsigned caught,bad,editcontext;
static long ackfd=-1;
static void handler(unsigned signal,void *info,UContext *context) {
    if(signal!=2 || *(unsigned *)info!=2)bad=1;
    caught++;if(editcontext)context->machine.regs[19]=99;
    if(ackfd>=0) { char byte='k';call5(64,ackfd,(long)&byte,1,0,0); }
    clobber_vector();
}
static void bad_handler(unsigned signal,void *info,UContext *context) {
    (void)signal;(void)info;context->machine.reserved[0]=0;
}
static long install(void (*fn)(unsigned,void *,UContext *),unsigned long flags) {
    unsigned long action[4]={(unsigned long)fn,0x04000004|flags,(unsigned long)restore_signal,0};
    return call5(134,2,(long)action,0,8,0);
}
int guest_main(unsigned long argc,char **argv,char **env) {
    (void)env;if(argc!=2)return 2;
    if(equal(argv[1],"handler")) {
        editcontext=1;if(install(handler,0))return 3;
        if(check_signal_registers()!=0x1111111111111111ul || caught!=1 || bad)return 4;
        output(1,"signal frame, registers and SIMD restored\n");return 0;
    }
    if(equal(argv[1],"bad-frame")) {
        long pid=call5(220,17,0,0,0,0);if(pid<0)return 5;
        if(!pid) { if(install(bad_handler,0))return 6;call5(129,call5(172,0,0,0,0,0),2,0,0,0);return 7; }
        unsigned status;if(call5(260,pid,(long)&status,0,0,0)!=pid || (status&127)!=11)return 8;
        output(1,"bad signal frame terminated only its guest\n");return 0;
    }
    if(equal(argv[1],"stop-continue")) {
        unsigned pipe[2];char byte=0;if(call5(59,(long)pipe,0,0,0,0))return 9;
        long pid=call5(220,17,0,0,0,0);if(pid<0)return 9;
        if(!pid) { call5(129,call5(172,0,0,0,0,0),19,0,0,0);if(call5(63,pipe[0],(long)&byte,1,0,0)!=1)return 15;return 37; }
        unsigned status;
        if(call5(260,pid,(long)&status,2,0,0)!=pid || status!=((19<<8)|127))return 10;
        if(call5(260,pid,(long)&status,3,0,0)!=0)return 11;
        if(call5(129,pid,18,0,0,0))return 12;
        if(call5(260,pid,(long)&status,8,0,0)!=pid || status!=65535)return 13;
        if(call5(64,pipe[1],(long)&byte,1,0,0)!=1)return 14;
        if(call5(260,pid,(long)&status,0,0,0)!=pid || status!=(37<<8))return 14;
        output(1,"stop, continue and exit statuses preserved\n");return 0;
    }
    if(equal(argv[1],"interrupt-read") || equal(argv[1],"restart-read")) {
        int restart=equal(argv[1],"restart-read");unsigned pair[2],ack[2];char byte=0;
        if(call5(59,(long)pair,0,0,0,0) || call5(59,(long)ack,0,0,0,0))return 15;
        long pid=call5(220,17,0,0,0,0);if(pid<0)return 16;
        if(!pid) {
            ackfd=ack[1];if(install(handler,restart?0x10000000:0))return 17;
            byte='r';if(call5(64,ack[1],(long)&byte,1,0,0)!=1)return 17;
            long result=call5(63,pair[0],(long)&byte,1,0,0);
            if(caught!=1 || bad || result!=(restart?1:-4) || (restart && byte!='x'))return 18;
            return 0;
        }
        if(call5(63,ack[0],(long)&byte,1,0,0)!=1 || byte!='r')return 19;
        for(volatile unsigned i=0;i<100000;i++) { }
        if(call5(129,pid,2,0,0,0))return 19;
        if(call5(63,ack[0],(long)&byte,1,0,0)!=1 || byte!='k')return 19;
        byte='x';if(call5(64,pair[1],(long)&byte,1,0,0)!=1)return 20;
        unsigned status;if(call5(260,pid,(long)&status,0,0,0)!=pid || status)return 21;
        output(1,"blocked read signal policy preserved\n");return 0;
    }
    return 2;
}
