#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
static int denied(void) { return errno==EACCES || errno==EPERM; }
int main(int argc,char **argv) {
    assert(argc>=7);const char *root=NULL,*outside=argv[argc-2];unsigned port=(unsigned)atoi(argv[argc-1]);int writable=0;
    for(int i=0;i<argc;i++) { if(!strcmp(argv[i],"--root") && i+1<argc)root=argv[i+1];if(!strcmp(argv[i],"--writable"))writable=1; }
    assert(root);int fd=open(outside,O_RDONLY);assert(fd<0 && denied());
    fd=open(outside,O_WRONLY|O_TRUNC);assert(fd<0 && denied());
    char path[4096];snprintf(path,sizeof(path),"%s/link",root);fd=open(path,O_RDONLY);assert(fd<0 && denied());
    snprintf(path,sizeof(path),"%s/hardlink",root);assert(link(outside,path)<0 && denied());
    snprintf(path,sizeof(path),"%s/new",root);fd=open(path,O_WRONLY|O_CREAT,0600);
    if(writable) { if(fd<0)perror("root create");assert(fd>=0);ssize_t count=write(fd,"ok",2);if(count!=2)perror("root write");assert(count==2);close(fd); }else assert(fd<0 && denied());
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(listener,(struct sockaddr *)&address,sizeof(address))<0 && denied());close(listener);
    int client=socket(AF_INET,SOCK_STREAM,0);assert(client>=0);address.sin_port=htons((unsigned short)port);
    assert(connect(client,(struct sockaddr *)&address,sizeof(address))==0);char byte;assert(read(client,&byte,1)==1 && byte=='k');close(client);
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);address.sin_addr.s_addr=htonl(INADDR_ANY);address.sin_port=0;
    assert(bind(fd,(struct sockaddr *)&address,sizeof(address))<0 && denied());close(fd);
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);assert(fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    address.sin_addr.s_addr=htonl(0xc0000201);address.sin_port=htons(9);
    assert(connect(fd,(struct sockaddr *)&address,sizeof(address))<0 && denied());close(fd);
    assert(fork()<0 && denied());
    char *args[]={"true",NULL};execv("/usr/bin/true",args);assert(denied());
    puts("PASS kernel filesystem, link, client-only network, fork and exec restrictions");return 0;
}
