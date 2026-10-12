/* M22 small shell: bounded concurrent pipelines and ordered redirections. */
#include "syscall.h"
#define COMMANDS 32
#define ARGS 32
#define REDIRS 8
struct Redir { unsigned target,kind;char *path;int source; };
static struct Command { char *argv[ARGS+1];unsigned argc,condition,piped,nredir;struct Redir redir[REDIRS]; } commands[COMMANDS];
static char words[4097];static unsigned used;
static int space(char c) { return c==' '||c=='\t'; }
static int separator(char c) { return !c || space(c) || c==';'||c=='&'||c=='|'||c=='<'||c=='>'; }
static char *word(const char **input) {
    const char *s=*input;char *result=words+used,quote=0;int seen=0;
    while(*s && (quote || !separator(*s))) {
        char c=*s++;seen=1;
        if(c=='\''||c=='"') { if(!quote) { quote=c;continue; }if(quote==c) { quote=0;continue; } }
        if(c=='\\' && quote!='\'') { if(!*s)return 0;c=*s++; }
        else if(quote!='\'' && (c=='$'||c=='`'||c=='\n'||c=='('||c==')'||c=='*'||c=='?'||c=='['||c=='#'))return 0;
        if(used==4096)return 0;words[used++]=c;
    }
    if(!seen || quote || used==4096)return 0;
    words[used++]=0;*input=s;return result;
}
static int parse(const char *s) {
    unsigned ci=0;
    while(*s) {
        while(space(*s))s++;if(!*s)break;
        struct Command *cmd=&commands[ci];
        if(*s==';'||*s=='&'||*s=='|') {
            if(!cmd->argc)return -1;unsigned condition=0;
            if(*s==';')s++;
            else if(*s=='|' && s[1]!='|') { cmd->piped=1;s++; }
            else { char op=*s++;if(*s++!=op)return -1;condition=op=='&'?1:2; }
            if(++ci==COMMANDS)return -1;commands[ci].condition=condition;continue;
        }
        unsigned target=99;
        const char *digits=s;while(*digits>='0' && *digits<='9')digits++;
        if(digits>s && (*digits=='<'||*digits=='>')) {
            if(digits!=s+1 || *s>'2')return -1;target=(unsigned)(*s++-'0');
        }
        if(*s=='<'||*s=='>') {
            if(cmd->nredir==REDIRS)return -1;
            char op=*s++;struct Redir *r=&cmd->redir[cmd->nredir++];r->target=target==99?(op=='<'?0:1):target;
            r->kind=op=='<'?0:1;
            if(*s=='&') {
                s++;r->kind=3;
                if(*s=='-') { r->kind=4;s++; }
                else { if(*s<'0'||*s>'2')return -1;r->source=*s++-'0'; }
                if(!separator(*s))return -1;
            } else {
                if(op=='>' && *s=='>') { r->kind=2;s++; }
                while(space(*s))s++;r->path=word(&s);if(!r->path)return -1;
            }
            continue;
        }
        if(cmd->argc==ARGS)return -1;cmd->argv[cmd->argc++]=word(&s);if(!cmd->argv[cmd->argc-1])return -1;
    }
    if(!commands[ci].argc) {
        if(commands[ci].nredir || (ci && (commands[ci].condition || commands[ci-1].piped)))return -1;
        return (int)ci;
    }
    return (int)ci+1;
}
static int builtin(struct Command *cmd,int status) {
    char **args=cmd->argv;unsigned n=cmd->argc;
    if(equal(args[0],"exit")) { if(n>2)return 2;int value=n==1?status:number(args[1]);return value<0?2:value; }
    if(equal(args[0],"true")||equal(args[0],"false"))return equal(args[0],"false");
    if(equal(args[0],"status")) {
        char out[5];out[0]='0'+status/100;out[1]='0'+status/10%10;out[2]='0'+status%10;out[3]='\n';out[4]=0;
        const char *text=out+(status<10?2:status<100?1:0);
        if(output(1,text)!=(long)length(text))return 1;return status;
    }
    return -1;
}
static int duplicate(long from,long to) { return from==to?(int)call5(25,from,1,0,0,0):(int)call5(24,from,to,0,0,0); }
static int redirect(struct Command *cmd) {
    for(unsigned j=0;j<cmd->nredir;j++) {
        struct Redir *r=&cmd->redir[j];
        if(r->kind==4) { long result=call5(57,r->target,0,0,0,0);if(result<0 && result!=-9)return -1;continue; }
        if(r->kind==3) { if(duplicate(r->source,r->target)<0)return -1;continue; }
        long flags=(r->kind==0?0:r->kind==1?(1|64|512):(1|64|1024))|524288;
        long fd=call5(56,-100,(long)r->path,flags,0666,0);if(fd<0)return -1;
        int result=duplicate(fd,r->target);call5(57,fd,0,0,0,0);if(result<0)return -1;
    }
    return 0;
}
static int execute(struct Command *cmd,char **env,int status) {
    int result=builtin(cmd,status);if(result>=0)return result;
    char path[1030];unsigned k=0;char **args=cmd->argv;
    if(args[0][0]!='/') { const char *prefix="/bin/";while(*prefix)path[k++]=*prefix++; }
    for(unsigned j=0;args[0][j];j++) { if(k==1024)return 126;path[k++]=args[0][j]; }path[k]=0;
    long error=call5(221,(long)path,(long)args,(long)env,0,0);
    output(2,error==-2?"sh: command not found\n":"sh: cannot execute command\n");return error==-2?127:126;
}
int guest_main(unsigned long argc,char **argv,char **env) {
    if(argc!=3 || !equal(argv[1],"-c") || length(argv[2])>4096) { output(2,"usage: sh -c COMMAND (max 4096 bytes)\n");return 2; }
    int count=parse(argv[2]),status=0;
    if(count>=0) { unsigned stages=1;for(int i=0;i<count;i++) { if(commands[i].piped) { if(++stages>4)count=-1; }else stages=1;if(count<0)break; } }if(count<0) { output(2,"sh: unsupported syntax or command limit\n");return 2; }
    for(int first=0;first<count;) {
        int last=first;while(commands[last].piped)last++;
        unsigned condition=commands[first].condition;
        if((condition==1 && status) || (condition==2 && !status)) { first=last+1;continue; }
        if(first==last && !commands[first].nredir) {
            int code=builtin(&commands[first],status);
            if(code>=0) { status=code;if(equal(commands[first].argv[0],"exit"))return status;first++;continue; }
        }
        long input=-1;int previous=status;long children[4];unsigned launched=0;
        for(int i=first;i<=last;i++) {
            unsigned endpoints[2]={0,0};
            if(i<last && call5(59,(long)endpoints,524288,0,0,0)<0) { if(input>=0)call5(57,input,0,0,0,0);output(2,"sh: cannot create blocking pipe\n");return 125; }
            long child=call5(220,17,0,0,0,0);
            if(child<0) { output(2,"sh: child limit or unsupported process state\n");return 125; }
            if(!child) {
                if(input>=0 && duplicate(input,0)<0)return 125;
                if(i<last && duplicate(endpoints[1],1)<0)return 125;
                if(input>=0)call5(57,input,0,0,0,0);
                if(i<last) { call5(57,endpoints[0],0,0,0,0);call5(57,endpoints[1],0,0,0,0); }
                if(redirect(&commands[i])<0) { output(2,"sh: redirection failed\n");return 1; }
                return execute(&commands[i],env,previous);
            }
            if(input>=0)call5(57,input,0,0,0,0);input=-1;
            if(i<last) { call5(57,endpoints[1],0,0,0,0);input=endpoints[0]; }
            children[launched++]=child;
        }
        status=0;
        for(unsigned i=0;i<launched;i++) {
            unsigned waitstatus=0;if(call5(260,children[i],(long)&waitstatus,0,0,0)!=children[i])return 125;
            int code=(waitstatus&127)?128+(waitstatus&127):(waitstatus>>8)&255;if(!status && code)status=code;
        }
        if(first==last && equal(commands[first].argv[0],"exit"))return status;
        first=last+1;
    }
    return status;
}
