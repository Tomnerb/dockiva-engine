#include "../m5/syscall.h"
struct pollfd {int fd;unsigned short events,revents;};
struct client { U fd,n,out,length;char request[2048],reply[8192]; };
static struct client clients[8];
static char response[8192];static U response_size;
static U portnum(const char *s) {U p=0;while(*s) {if(*s<'0'||*s>'9')return 65536;p=p*10+(U)(*s++-'0');if(p>65535)return p;}return p;}
static void address(unsigned char *a,U port) {for(U i=0;i<16;i++)a[i]=0;a[0]=2;a[4]=127;a[7]=1;U hi=0;while(port>=256) {hi++;port-=256;}a[2]=(unsigned char)hi;a[3]=(unsigned char)port;}
static void output(const char *s,U n) {sc(64,1,(U)s,n,0,0,0);}
static void ready(unsigned char *a) {
 U port=(U)a[2]*256+a[3];char text[11]={'P','O','R','T',' '};
 U divs[5]={10000,1000,100,10,1};for(U i=0;i<5;i++){text[i+5]='0';while(port>=divs[i]){port-=divs[i];text[i+5]++;}}text[10]='\n';output(text,11);
}
static int starts(const char *p,const char *s) {while(*s)if(*p++!=*s++)return 0;return 1;}
static int relay(U port) {
 U fd=sc(198,2,1,0,0,0,0);if(bad(fd))return 0;unsigned char a[16];address(a,port);
 if(bad(sc(203,fd,(U)a,16,0,0,0))) {sc(57,fd,0,0,0,0,0);return 0;}
 const char request[]="GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";U at=0;
 while(at<sizeof(request)-1) {U n=sc(64,fd,(U)(request+at),sizeof(request)-1-at,0,0,0);if(bad(n)||!n)break;at+=n;}
 response_size=0;
 while(response_size<sizeof(response)) {U n=sc(63,fd,(U)(response+response_size),sizeof(response)-response_size,0,0,0);if(!n)break;if(bad(n)){response_size=0;break;}response_size+=n;}
 sc(57,fd,0,0,0,0,0);return response_size>0;
}
int guest_main(U argc,char **argv,char **envp) {
 (void)envp;U port=argc>1?portnum(argv[1]):18080,upstream=argc>2?portnum(argv[2]):0;if(port>65535||upstream>65535)return 70;
 static const char header[]="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
 if(!upstream) {
  U file=sc(56,(U)-100,(U)"/www/index.html",0,0,0,0);if(bad(file))return 71;
  for(U i=0;i<sizeof(header)-1;i++)response[i]=header[i];
  U n=sc(63,file,(U)(response+sizeof(header)-1),4096,0,0,0);sc(57,file,0,0,0,0,0);if(bad(n))return 72;response_size=sizeof(header)-1+n;
  file=sc(56,(U)-100,(U)"/counter",66,0600,0,0);
  if(!bad(file)){char digit='0';sc(63,file,(U)&digit,1,0,0,0);digit=digit>='0'&&digit<'9'?digit+1:'1';sc(62,file,0,0,0,0,0);sc(64,file,(U)&digit,1,0,0,0);sc(57,file,0,0,0,0,0);}
 }
 U listener=sc(198,2,1|2048,0,0,0,0);if(bad(listener))return 73;
 unsigned int reuse=1;sc(208,listener,1,2,(U)&reuse,4,0);
 unsigned char a[16];address(a,port);
 /* Explicit Docker benchmark opt-in; default remains loopback. */
 if(argc>3) {
  if(!starts(argv[3],"--bind-any") || argv[3][10])return 76;
  a[4]=0;a[7]=0;
 }
 if(bad(sc(200,listener,(U)a,16,0,0,0))||bad(sc(201,listener,8,0,0,0,0)))return 74;
 unsigned int size=16;sc(204,listener,(U)a,(U)&size,0,0,0);ready(a);
 for(;;) {
  struct pollfd polls[9];polls[0].fd=(int)listener;polls[0].events=1;polls[0].revents=0;
  for(U i=0;i<8;i++){polls[i+1].fd=clients[i].fd?(int)clients[i].fd:-1;polls[i+1].events=clients[i].out?4:1;polls[i+1].revents=0;}
  U timeout[2]={1,0};U found=sc(73,(U)polls,9,(U)timeout,0,0,0);if(bad(found))return 75;
  if(polls[0].revents&1) {
   U fd=sc(242,listener,0,0,2048,0,0);
   if(!bad(fd)){U i=0;while(i<8 && clients[i].fd)i++;if(i==8)sc(57,fd,0,0,0,0,0);else{clients[i].fd=fd;clients[i].n=0;clients[i].out=0;}}
  }
  for(U i=0;i<8;i++) {
   struct client *c=&clients[i];if(!c->fd)continue;
   if(polls[i+1].revents&(8|16|32)){sc(57,c->fd,0,0,0,0,0);c->fd=0;continue;}
   if(!c->out && (polls[i+1].revents&1)) {
    U n=sc(63,c->fd,(U)(c->request+c->n),sizeof(c->request)-c->n,0,0,0);
    if(n==(U)-11)continue;
    if(bad(n)||!n){sc(57,c->fd,0,0,0,0,0);c->fd=0;continue;}c->n+=n;
    int complete=0;for(U j=3;j<c->n;j++)if(c->request[j-3]=='\r'&&c->request[j-2]=='\n'&&c->request[j-1]=='\r'&&c->request[j]=='\n')complete=1;
    if(!complete && c->n<sizeof(c->request))continue;
    if(!complete || !starts(c->request,"GET / ")) {
     static const char badrequest[]="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
     sc(64,c->fd,(U)badrequest,sizeof(badrequest)-1,0,0,0);sc(57,c->fd,0,0,0,0,0);c->fd=0;continue;
    }
    if(upstream && !relay(upstream)) {
     static const char unavailable[]="HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
     sc(64,c->fd,(U)unavailable,sizeof(unavailable)-1,0,0,0);sc(57,c->fd,0,0,0,0,0);c->fd=0;continue;
    }
    c->length=response_size;for(U j=0;j<response_size;j++)c->reply[j]=response[j];c->out=1;
   }
   if(c->out) {
    U at=c->out-1,n=sc(64,c->fd,(U)(c->reply+at),c->length-at,0,0,0);
    if(n==(U)-11)continue;
    if(bad(n)||!n || at+n==c->length){sc(57,c->fd,0,0,0,0,0);c->fd=0;}else c->out+=n;
   }
  }
 }
}
