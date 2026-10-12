/* Socket dup/dup3/F_DUPFD share the open description; blocking waits default to
   no timeout; ppoll mixes sockets with pipes, stdio and invalid descriptors. */
#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static unsigned char *memory;
static void pollfd_at(unsigned index,int32_t fd,uint16_t events) {
    unsigned char *p=memory+0x100+index*8;fs_pack(p,0,(uint32_t)fd,4);fs_pack(p,4,events,2);fs_pack(p,6,0,2);
}
static unsigned revents(unsigned index) { return (unsigned)le(memory+0x100+index*8+6,2); }
static uint64_t ppoll_now(unsigned count) {
    memset(memory+0x200,0,16); /* zero timespec: return at once */
    cpu->gpr[0]=0x1100;cpu->gpr[1]=count;cpu->gpr[2]=0x1200;cpu->gpr[3]=0;return net_poll();
}
int main(void) {
    MemoryBacking *backing=backing_alloc(4096,1);assert(backing);memory=backing_data(backing);
    address_space->seg[0]=(Segment){0x1000,4096,6,backing,0};address_space->nseg=1;address_space_changed(address_space);

    /* dup keeps the socket alive after the original closes (busybox nc moves its accepted fd). */
    uint64_t s=net_socket(2,1,0);assert(s>=3 && s<3+NET_SLOTS);
    assert(net_description((unsigned)s-3)->read_timeout==NET_FOREVER && net_description((unsigned)s-3)->write_timeout==NET_FOREVER);
    int fd=net_free_fd(3);assert(fd>=0 && (uint64_t)fd!=s);
    assert(net_dup(s,(uint64_t)fd,0)==(uint64_t)fd && net_host((uint64_t)fd)==net_host(s));
    assert(net_description((unsigned)s-3)->refs==2);
    cpu->gpr[0]=s;assert(net_call(57)==0);
    assert(net_host(s)<0 && net_host((uint64_t)fd)>=0 && net_description((unsigned)fd-3)->refs==1);
    /* dup3 onto an occupied socket slot closes the old socket first. */
    uint64_t other=net_socket(2,1,0);int other_host=net_host(other);
    assert(io_dup((uint64_t)fd,other,524288)==other && net_host(other)==net_host((uint64_t)fd) && (net_flags[other-3]&524288));
    assert(fcntl(other_host,F_GETFD)<0);                           /* replaced socket was closed */
    assert(io_dup((uint64_t)fd,(uint64_t)fd,0)==linux_error(22));
    assert(io_dup((uint64_t)fd,1,0)==linux_error(95));             /* sockets on stdio: unsupported */

    /* SO_RCVTIMEO: zero means no timeout; long values are accepted. */
    net_scheduled=1;
    memset(memory,0,16);cpu->gpr[0]=(uint64_t)fd;cpu->gpr[1]=1;cpu->gpr[2]=20;cpu->gpr[3]=0x1000;cpu->gpr[4]=16;
    assert(net_call(208)==0 && net_description((unsigned)fd-3)->read_timeout==NET_FOREVER);
    fs_pack(memory,0,120,8);assert(net_call(208)==0 && net_description((unsigned)fd-3)->read_timeout==120000);
    net_scheduled=0;

    /* ppoll over a pipe, stdin, a socket and an invalid descriptor. */
    cpu->gpr[0]=0x1300;cpu->gpr[1]=2048;assert(io_pipe()==0);
    unsigned r=(unsigned)le(memory+0x300,4),w=(unsigned)le(memory+0x304,4);
    pollfd_at(0,(int32_t)r,1);pollfd_at(1,0,1);pollfd_at(2,(int32_t)fd,1);pollfd_at(3,50,1);pollfd_at(4,(int32_t)w,4);
    assert(ppoll_now(5)==3);                                       /* stdin EOF, invalid fd, writable pipe */
    assert(revents(0)==0 && revents(1)==1 && revents(2)==0 && revents(3)==32 && revents(4)==4);
    memory[0x400]='x';cpu->gpr[0]=w;cpu->gpr[1]=0x1400;cpu->gpr[2]=1;assert(io_call(64)==1);
    assert(ppoll_now(1)==1 && revents(0)==1);                     /* pipe data readable */
    cpu->gpr[0]=w;assert(io_call(57)==0);
    cpu->gpr[0]=r;cpu->gpr[1]=0x1400;cpu->gpr[2]=1;assert(io_call(63)==1);
    pollfd_at(0,(int32_t)r,1);assert(ppoll_now(1)==1 && revents(0)==16); /* writer gone: POLLHUP */

    net_cleanup();io_cleanup();puts("net_dup_poll ok");return 0;
}
