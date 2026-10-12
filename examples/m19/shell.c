/* Deliberately small noninteractive shell, not a POSIX shell or BusyBox ash. */
#include "syscall.h"
#define COMMANDS 32
#define ARGS 32
static struct { char *argv[ARGS+1];unsigned argc,condition; } commands[COMMANDS];
static char words[4097];
static int parse(const char *s) {
    unsigned ci=0,used=0;
    while(*s) {
        while(*s==' '||*s=='\t')s++;
        if(!*s)break;
        if(*s==';'||*s=='&'||*s=='|') {
            if(!commands[ci].argc)return -1;
            unsigned condition=0;
            if(*s==';')s++;
            else { char op=*s++;if(*s++!=op)return -1;condition=op=='&'?1:2; }
            if(++ci==COMMANDS)return -1;
            commands[ci].condition=condition;continue;
        }
        if(commands[ci].argc==ARGS)return -1;
        commands[ci].argv[commands[ci].argc++]=words+used;
        char quote=0;
        while(*s && (quote || (*s!=' '&&*s!='\t'&&*s!=';'&&*s!='&'&&*s!='|'))) {
            char c=*s++;
            if(c=='\''||c=='"') { if(!quote) { quote=c;continue; }if(quote==c) { quote=0;continue; } }
            if(c=='\\' && quote!='\'') { if(!*s)return -1;c=*s++; }
            else if(quote!='\'' && (c=='$'||c=='`'||c=='<'||c=='>'||c=='\n'||c=='('||c==')'||c=='*'||c=='?'||c=='['||c=='#'))return -1;
            if(used==4096)return -1;words[used++]=c;
        }
        if(quote || used==4096)return -1;words[used++]=0;
    }
    if(!commands[ci].argc) { if(ci && commands[ci].condition)return -1;return (int)ci; }
    return (int)ci+1;
}
int guest_main(unsigned long argc,char **argv,char **env) {
    if(argc!=3 || !equal(argv[1],"-c") || length(argv[2])>4096) { output(2,"usage: sh -c COMMAND (max 4096 bytes)\n");return 2; }
    int count=parse(argv[2]),status=0;
    if(count<0) { output(2,"sh: unsupported syntax or command limit\n");return 2; }
    for(int i=0;i<count;i++) {
        unsigned condition=commands[i].condition;
        if((condition==1 && status) || (condition==2 && !status))continue;
        char **args=commands[i].argv;unsigned n=commands[i].argc;
        if(equal(args[0],"exit")) { if(n>2)return 2;int value=n==1?status:number(args[1]);return value<0?2:value; }
        if(equal(args[0],"true")||equal(args[0],"false")) { status=equal(args[0],"false");continue; }
        if(equal(args[0],"status")) { char out[5];out[0]='0'+status/100;out[1]='0'+status/10%10;out[2]='0'+status%10;out[3]='\n';out[4]=0;output(1,out+(status<10?2:status<100?1:0));continue; }
        char path[1030];unsigned k=0;
        if(args[0][0]!='/') { const char *prefix="/bin/";while(*prefix)path[k++]=*prefix++; }
        for(unsigned j=0;args[0][j];j++) { if(k==1024)return 126;path[k++]=args[0][j]; }path[k]=0;
        long child=call5(220,17,0,0,0,0);
        if(child<0) { output(2,"sh: child limit or unsupported process state\n");return 125; }
        if(!child) {
            long result=call5(221,(long)path,(long)args,(long)env,0,0);
            output(2,result==-2?"sh: command not found\n":"sh: cannot execute command\n");
            return result==-2?127:126;
        }
        unsigned waitstatus=0;
        if(call5(260,child,(long)&waitstatus,0,0,0)!=child)return 125;
        status=(waitstatus>>8)&255;
    }
    return status;
}
