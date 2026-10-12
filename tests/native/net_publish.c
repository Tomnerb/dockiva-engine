/* --publish GUEST:HOST: a guest bind on GUEST (loopback or wildcard) listens on
   host loopback HOST; guest connects to GUEST reach it; guest-visible addresses
   keep GUEST. Unpublished ports and non-loopback addresses are unchanged. */
#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char *memory;
static uint16_t free_port(void) {
    int s=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t n=sizeof(a);assert(s>=0 && !bind(s,(struct sockaddr *)&a,sizeof(a)) && !getsockname(s,(struct sockaddr *)&a,&n));
    close(s);return ntohs(a.sin_port);
}
static uint64_t guest_addr(uint64_t call,uint64_t fd,unsigned a,unsigned b,unsigned c,unsigned d,uint16_t port) {
    unsigned char wire[16]={2,0,(unsigned char)(port>>8),(unsigned char)port,(unsigned char)a,(unsigned char)b,(unsigned char)c,(unsigned char)d};
    memcpy(memory,wire,16);cpu->gpr[0]=fd;cpu->gpr[1]=0x1000;cpu->gpr[2]=16;return net_call(call);
}
static uint16_t guest_name(uint64_t call,uint64_t fd) {
    memory[0x200]=16;memory[0x201]=memory[0x202]=memory[0x203]=0;
    cpu->gpr[0]=fd;cpu->gpr[1]=0x1100;cpu->gpr[2]=0x1200;assert(net_call(call)==0);
    assert(memory[0x104]==127 && memory[0x107]==1);
    return (uint16_t)(memory[0x102]<<8|memory[0x103]);
}
static uint16_t host_port(uint64_t fd) {
    struct sockaddr_in a;socklen_t n=sizeof(a);assert(!getsockname(net_host(fd),(struct sockaddr *)&a,&n));
    assert(a.sin_addr.s_addr==htonl(INADDR_LOOPBACK));return ntohs(a.sin_port);
}
int main(void) {
    MemoryBacking *backing=backing_alloc(4096,1);assert(backing);memory=backing_data(backing);
    address_space->seg[0]=(Segment){0x1000,4096,6,backing,0};address_space->nseg=1;address_space_changed(address_space);
    uint16_t published=free_port(),plain=free_port();
    char spec[32];snprintf(spec,sizeof spec,"80:%u",published);net_publish(spec);

    uint64_t server=net_socket(2,1,0);
    assert(guest_addr(200,server,0,0,0,0,80)==0);            /* wildcard bind of the guest port */
    assert(host_port(server)==published && guest_name(204,server)==80);
    cpu->gpr[0]=server;cpu->gpr[1]=8;assert(net_call(201)==0);

    int client=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in to={0};
    to.sin_family=AF_INET;to.sin_port=htons(published);to.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(client>=0 && !connect(client,(struct sockaddr *)&to,sizeof(to)));   /* host reaches HOST */
    close(client);

    uint64_t inside=net_socket(2,1,0);
    assert(guest_addr(203,inside,127,0,0,1,80)==0);          /* guest reaches its own GUEST port */
    assert(guest_name(205,inside)==80);

    uint64_t other=net_socket(2,1,0);
    assert(guest_addr(200,other,127,0,0,1,plain)==0 && host_port(other)==plain && guest_name(204,other)==plain);
    uint64_t lan=net_socket(2,1,0);
    assert(guest_addr(200,lan,10,0,0,1,plain)==linux_error(13));

    net_cleanup();puts("net_publish ok");return 0;
}
