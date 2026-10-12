#include "syscall.h"
static volatile unsigned counter;
int guest_main(unsigned long argc,char **argv,char **env) {
    (void)env;if(argc!=2)return 2;
    if(equal(argv[1],"mask")) {
        long pid=call5(220,17,0,123,456,789);if(pid<0)return 3;
        if(!pid) {
            unsigned long mask=1ul<<14,old=0;
            if(call5(135,0,(long)&mask,(long)&old,8,0) || old)return 4;
            if(call5(129,call5(172,0,0,0,0,0),15,0,0,0))return 5;
            if(call5(135,1,(long)&mask,0,8,0))return 6;
            return 7;
        }
        unsigned status=0;if(call5(260,pid,(long)&status,0,0,0)!=pid || status!=15)return 8;
        output(1,"masked signal delivered after unblock\n");return 0;
    }
    if(equal(argv[1],"ignored")) {
        unsigned long action[4]={1,0,0,0},old[4],mask=1ul<<14;
        if(call5(135,0,(long)&mask,0,8,0) || call5(129,call5(172,0,0,0,0,0),15,0,0,0))return 9;
        if(call5(134,15,(long)action,(long)old,8,0) || old[0])return 10;
        if(call5(135,1,(long)&mask,0,8,0))return 11;
        long pid=call5(220,17,0,0,0,0);if(pid<0)return 12;
        if(!pid) { if(call5(129,call5(172,0,0,0,0,0),15,0,0,0))return 13;return 37; }
        unsigned status;if(call5(260,pid,(long)&status,0,0,0)!=pid || status!=(37<<8))return 14;
        output(1,"ignored pending signal cleared and disposition inherited\n");return 0;
    }
    if(equal(argv[1],"signal-errors")) {
        unsigned long action[4]={12345,0,0,0},old=0,mask=0;
        if(call5(134,15,(long)action,0,8,0)!=-95)return 15;
        if(call5(134,9,0,0,8,0)!=-22 || call5(134,0,0,0,8,0)!=-22)return 16;
        if(call5(135,0,1,0,8,0)!=-14 || call5(135,0,0,1,8,0)!=-14)return 17;
        if(call5(135,0,0,0,16,0)!=-22 || call5(135,3,(long)&mask,0,8,0)!=-22)return 18;
        if(call5(135,0,0,(long)&old,8,0) || old)return 19;
        output(1,"signal errors preserved state\n");return 0;
    }
    if(equal(argv[1],"nested") || equal(argv[1],"orphan")) {
        int orphan=equal(argv[1],"orphan");long root=call5(172,0,0,0,0,0);
        long child=call5(220,17,0,0,0,0);if(child<0)return 20;
        if(!child) {
            long parent=call5(172,0,0,0,0,0),grand=call5(220,17,0,0,0,0);
            if(grand<0)return 21;
            if(!grand) {
                if(orphan)while(call5(173,0,0,0,0,0)!=root)counter++;
                else if(call5(173,0,0,0,0,0)!=parent)return 22;
                return 43;
            }
            if(orphan)return 27;
            unsigned status;if(call5(260,grand,(long)&status,0,0,0)!=grand || status!=(43<<8))return 23;
            return 27;
        }
        unsigned status;if(call5(260,child,(long)&status,0,0,0)!=child || status!=(27<<8))return 24;
        if(orphan && (call5(260,-1,(long)&status,0,0,0)<0 || status!=(43<<8)))return 25;
        if(call5(260,-1,0,0,0,0)!=-10)return 26;
        output(1,"process tree reaped correctly\n");return 0;
    }
    return 2;
}
