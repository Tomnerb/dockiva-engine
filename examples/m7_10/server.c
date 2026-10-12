#include "../m5/syscall.h"
static int send_all(U fd,const char *p,U n) {
    while(n) { U k=sc(64,fd,(U)p,n,0,0,0);if(bad(k)||!k)return 0;p+=k;n-=k; }return 1;
}
static int starts(const char *p,const char *prefix) { while(*prefix) { if(*p++!=*prefix++)return 0; }return 1; }
static void ready(unsigned char *address) {
    U port=address[2];for(U i=0;i<8;i++)port+=port;port+=address[3];
    char line[11];line[0]='P';line[1]='O';line[2]='R';line[3]='T';line[4]=' ';
    static const U divisors[5]={10000,1000,100,10,1};
    for(U i=0;i<5;i++) { unsigned char digit='0';while(port>=divisors[i]) {port-=divisors[i];digit++;}line[5+i]=(char)digit; }
    line[10]='\n';send_all(1,line,11);
}
static const char missing[]="HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 10\r\nConnection: close\r\n\r\nNot found\n";
static const char method[]="HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain\r\nContent-Length: 19\r\nConnection: close\r\n\r\nMethod not allowed\n";
static const char too_large[]="HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Type: text/plain\r\nContent-Length: 18\r\nConnection: close\r\n\r\nHeaders too large\n";
int guest_main(U argc,char **argv,char **envp) {
    char ok[4096];
    U file=sc(56,(U)-100,(U)"/www/index.html",0,0,0,0);if(bad(file))return 80;
    U ok_size=sc(63,file,(U)ok,sizeof(ok),0,0,0);sc(57,file,0,0,0,0,0);if(bad(ok_size))return 81;
    (void)argv;(void)envp;
    U listener=sc(198,2,1,0,0,0,0);if(bad(listener))return 90;
    unsigned int reuse=1;if(bad(sc(208,listener,1,2,(U)&reuse,4,0)))return 95;
    unsigned char addr[16];for(U i=0;i<16;i++)addr[i]=0;
    addr[0]=2;addr[4]=127;addr[7]=1;
    if(argc==1) {addr[2]=70;addr[3]=160;} /* 18080; any argument selects ephemeral port. */
    if(bad(sc(200,listener,(U)addr,16,0,0,0)))return 91;
    if(bad(sc(201,listener,8,0,0,0,0)))return 92;
    unsigned int len=16;if(bad(sc(204,listener,(U)addr,(U)&len,0,0,0)))return 93;
    ready(addr);
    U client=sc(202,listener,0,0,0,0,0);if(bad(client))return 94;
    char request[2048];U n=0;int complete=0;
    while(n<sizeof(request)) {
        U count=sc(63,client,(U)(request+n),sizeof(request)-n,0,0,0);
        if(bad(count)||!count) {sc(57,client,0,0,0,0,0);sc(57,listener,0,0,0,0,0);return 4;}
        n+=count;
        for(U i=3;i<n;i++) if(request[i-3]=='\r' && request[i-2]=='\n' && request[i-1]=='\r' && request[i]=='\n')complete=1;
        if(complete)break;
    }
    const char *response;U size;
    if(!complete) {response=too_large;size=sizeof(too_large)-1;}
    else if(n<4 || !starts(request,"GET ")) {response=method;size=sizeof(method)-1;}
    else if(n>=6 && starts(request,"GET / ")) {response=ok;size=ok_size;}
    else {response=missing;size=sizeof(missing)-1;}
    int sent=1;
    if(response==ok) {
        static const char header[]="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
        sent=send_all(client,header,sizeof(header)-1);
    }
    if(sent)sent=send_all(client,response,size);
    sc(57,client,0,0,0,0,0);sc(57,listener,0,0,0,0,0);
    return sent?0:5;
}
