/* Bounded IPv4 TCP and UDP, with optional scheduler-aware nonblocking hosts. Guests
   reach the host loopback; with --outbound they also reach other IPv4 addresses. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
/* One descriptor table per process: each guest number names a socket, file or
   pipe-like (I/O) slot of its layer. New descriptors take the lowest free number,
   as POSIX requires (shells, daemons and PostgreSQL rely on it). The map and the
   layers' reverse tables are per-process state, saved with each task. */
#define GUEST_FD_LIMIT 4096u
#define FD_NET 1u
#define FD_FILE 2u
#define FD_IO 3u
/* Per-process tables live in the scheduler's task records: the scheduler points
   these at the current task's tables (a switch copies nothing); before the
   scheduler starts they are the first process's own. */
typedef struct { unsigned char kind[GUEST_FD_LIMIT];unsigned short index[GUEST_FD_LIMIT]; } FdTable;
static FdTable fd_table_first={{FD_IO,FD_IO,FD_IO},{0,1,2}};
static _Thread_local FdTable *fd_table=&fd_table_first; /* 0..2: standard I/O slots 0..2 */
#define fd_kind (fd_table->kind)
#define fd_index (fd_table->index)
static int fd_lowest(uint64_t min) { for(uint64_t fd=min;fd<GUEST_FD_LIMIT;fd++)if(!fd_kind[fd])return (int)fd;return -1; }
static int fd_slot(uint64_t fd,unsigned kind) { return fd<GUEST_FD_LIMIT && fd_kind[fd]==kind?fd_index[fd]:-1; }
static void fd_bind(uint64_t fd,unsigned kind,unsigned index) { fd_kind[fd]=(unsigned char)kind;fd_index[fd]=(unsigned short)index; }
static void fd_unbind(uint64_t fd) { if(fd<GUEST_FD_LIMIT) { fd_kind[fd]=0;fd_index[fd]=0; } }
#define NET_FD_LIMIT 1024u /* sockets one wait can cover */
#define NET_SLOTS 1024u
/* fd: zero means unused, else host descriptor + 1; flags: descriptor-local CLOEXEC
   only; guest: slot -> guest descriptor. */
typedef struct { int fd[NET_SLOTS];unsigned flags[NET_SLOTS];unsigned short guest[NET_SLOTS]; } NetTable;
static NetTable net_table_first;
static _Thread_local NetTable *net_table=&net_table_first;
#define net_fd (net_table->fd)
#define net_flags (net_table->flags)
#define net_guest (net_table->guest)
static int net_slot(uint64_t guest) { return fd_slot(guest,FD_NET); }
#define NET_BACKLOG_LIMIT 128u
/* Published ports (--publish GUEST:HOST). A guest bind or connect on GUEST uses
   HOST on the host loopback; getsockname/getpeername translate back. Unpublished
   ports keep host-loopback networking unchanged. */
#define NET_PUBLISH_LIMIT 32
static uint16_t net_publish_guest[NET_PUBLISH_LIMIT],net_publish_host[NET_PUBLISH_LIMIT];
static unsigned net_publish_count;
static void net_publish(const char *spec) {
    char *end;errno=0;unsigned long guest=strtoul(spec,&end,10),host=0;
    if(!errno && *end==':' && end!=spec && end[1]>='0' && end[1]<='9') host=strtoul(end+1,&end,10);
    if(errno || *end || !guest || guest>65535 || !host || host>65535 || net_publish_count==NET_PUBLISH_LIMIT) fail("Use --publish GUEST:HOST with ports 1..65535, at most 32 times");
    for(unsigned i=0;i<net_publish_count;i++)
        if(net_publish_guest[i]==guest || net_publish_host[i]==host) fail("Duplicate published port");
    net_publish_guest[net_publish_count]=(uint16_t)guest;net_publish_host[net_publish_count++]=(uint16_t)host;
}
/* Ports in network byte order. */
static uint16_t net_port_map(uint16_t port,int to_host) {
    uint16_t value=ntohs(port);
    for(unsigned i=0;i<net_publish_count;i++)
        if(value==(to_host?net_publish_guest[i]:net_publish_host[i])) return htons(to_host?net_publish_host[i]:net_publish_guest[i]);
    return port;
}
static int net_scheduled;
static int net_outbound; /* --outbound: connect/sendto may reach any IPv4 address */
#define NET_UDP (1u<<30)  /* net_adopt flag: datagram socket */
#define NET_UNIX (1u<<29) /* net_adopt flag: AF_UNIX socket */
static uint64_t net_wait_blocks;
static int io_input_host(uint64_t guest); /* --stdin's host descriptor, polled like a socket */
/* targeted: only these sockets, the deadline, a signal, a futex wake or a pipe that
   recorded the task in its waiters can end the wait, so the scheduler wakes the task
   for those alone; other blocked calls retry after any I/O event (io_epoch). */
typedef struct { struct pollfd fds[NET_FD_LIMIT];unsigned count,active,targeted;uint64_t deadline,at_pc; } NetWait;
static NetWait net_wait_first;
static _Thread_local NetWait *net_wait_now=&net_wait_first;
#define net_wait (*net_wait_now)
static uint64_t net_now(void) {
    struct timespec value;if(clock_gettime(CLOCK_MONOTONIC,&value))fail("Cannot read monotonic clock");
    return (uint64_t)value.tv_sec*1000+(unsigned long)value.tv_nsec/1000000;
}
/* Blocking calls wait without limit unless the guest sets SO_RCVTIMEO/SO_SNDTIMEO
   or passes a poll timeout, as on Linux. */
#define NET_FOREVER UINT32_MAX
/* The sockets an unlocked call would have waited on (it aborted instead): the CPU
   backend may wait for them itself, without any lock, before taking the slow path. */
static _Thread_local struct { unsigned count,timeout;struct pollfd fds[NET_FD_LIMIT]; } net_unlocked_wait;
static uint64_t net_suspend(const struct pollfd *fds,unsigned count,unsigned timeout) {
    if(syscall_unlocked) {                                       /* blocking needs the scheduler */
        if(count>NET_FD_LIMIT)count=NET_FD_LIMIT;
        memcpy(net_unlocked_wait.fds,fds,count*sizeof(*fds));net_unlocked_wait.count=count;net_unlocked_wait.timeout=timeout;
        syscall_abort=1;return IO_BLOCK;
    }
    uint64_t now=net_now();
    if(net_wait.active && net_wait.at_pc==cpu->pc && now>=net_wait.deadline) { net_wait.active=0;return linux_error(11); }
    /* +1: now is truncated to the millisecond, and a wait must last at least timeout. */
    if(!net_wait.active || net_wait.at_pc!=cpu->pc)net_wait.deadline=timeout==NET_FOREVER?UINT64_MAX:now+timeout+1;
    net_wait_blocks++;net_wait.active=1;net_wait.targeted=1;net_wait.at_pc=cpu->pc;net_wait.count=count;
    if(count)memcpy(net_wait.fds,fds,count*sizeof(*fds));
    return IO_BLOCK;
}
static uint64_t net_suspend_one(int fd,short events,unsigned timeout) {
    struct pollfd wait={fd,events,0};return net_suspend(&wait,1,timeout);
}
static uint64_t net_error(void) {
    unsigned e;
    switch(errno) {
        case EBADF:e=9;break;case EFAULT:e=14;break;case EINVAL:e=22;break;
        case EACCES:case EPERM:e=13;break;case EADDRINUSE:e=98;break;
        case EADDRNOTAVAIL:e=99;break;case ENOTSOCK:e=88;break;
        case ECONNRESET:e=104;break;case ECONNABORTED:e=103;break;
        case EPIPE:e=32;break;case EINTR:e=4;break;case EAGAIN:e=11;break;
        case ENOMEM:e=12;break;case EMFILE:e=24;break;case ENFILE:e=23;break;
        case EINPROGRESS:e=115;break;case EALREADY:e=114;break;case ECONNREFUSED:e=111;break;
        case ENOBUFS:e=105;break;case ENOTCONN:e=107;break;case ETIMEDOUT:e=110;break;
        case EDESTADDRREQ:e=89;break;case EMSGSIZE:e=90;break;case EOPNOTSUPP:e=95;break;
        case ENETDOWN:e=100;break;case ENETUNREACH:e=101;break;case EHOSTUNREACH:e=113;break;
        default:e=5;break;
    }
    return linux_error(e);
}
/* Shared open descriptions survive fork and close only on the last guest FD.
   Keep status flags/timeouts shared; descriptor CLOEXEC stays process-local. */
/* Indexed by host descriptor (one description per host socket); the worker caps
   host descriptors below NET_DESCRIPTIONS. */
#define NET_DESCRIPTIONS 4096u
/* reads/writes count guest read-side (read, readv, accept) and write-side calls;
   edge-triggered epoll treats a change as new activity. */
/* unix: AF_UNIX; named: 2 bound, 4 connected, to the guest path in path. */
static struct NetDescription { int host;unsigned refs,flags,connecting,read_timeout,write_timeout,reads,writes,udp,unix_socket,named;char path[108]; } net_descriptions[NET_DESCRIPTIONS];
static struct NetDescription *net_host_description(int host) {
    return host>=0 && (unsigned)host<NET_DESCRIPTIONS && net_descriptions[host].refs?&net_descriptions[host]:NULL;
}
static struct NetDescription *net_description(unsigned slot) {
    struct NetDescription *description=net_host_description(net_fd[slot]-1);
    if(!description)fail("Missing socket open description");
    return description;
}
static int net_release(unsigned slot) {
    struct NetDescription *description=net_description(slot);int result=0;
    if(!--description->refs) { epoll_forget_socket(description->host);result=close(description->host);memset(description,0,sizeof(*description)); }
    fd_unbind(net_guest[slot]);net_fd[slot]=0;net_flags[slot]=0;return result;
}
static void net_retain_all(void) {
    for(unsigned i=0;i<NET_SLOTS;i++)if(net_fd[i])net_description(i)->refs++;
}
static void net_cleanup(void) {
    for(unsigned i=0;i<NET_SLOTS;i++)if(net_fd[i])net_release(i);
    memset(&net_wait,0,sizeof(net_wait));
}
static int net_host(uint64_t guest) { int slot=net_slot(guest);return slot>=0 && net_fd[slot]?net_fd[slot]-1:-1; }
static uint64_t net_adopt(int host,unsigned guest_flags) {
    if(host<0) return net_error();
    struct timeval timeout={5,0};
    if(setsockopt(host,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) || setsockopt(host,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) {
        uint64_t error=net_error();close(host);return error;
    }
    if((unsigned)host>=NET_DESCRIPTIONS || net_descriptions[host].refs) { close(host);return linux_error(24); }
    int guest=fd_lowest(0);
    if(guest>=0)for(unsigned i=0;i<NET_SLOTS;i++) if(!net_fd[i]) {
        net_descriptions[host]=(struct NetDescription){host,1,guest_flags&2048,0,NET_FOREVER,NET_FOREVER,0,0,(guest_flags&NET_UDP)!=0,(guest_flags&NET_UNIX)!=0,0,{0}};
        net_fd[i]=host+1;net_flags[i]=guest_flags&524288;net_guest[i]=(unsigned short)guest;fd_bind((uint64_t)guest,FD_NET,i);return (uint64_t)guest; }
    close(host);return linux_error(24);
}
static uint64_t net_socket(uint64_t domain,uint64_t type,uint64_t protocol) {
    if(domain!=2 && domain!=1) return linux_error(97);
    uint64_t kind=type&~(UINT64_C(2048)|524288);                              /* SOCK_STREAM 1, SOCK_DGRAM 2 */
    if(domain==1 && ((kind!=1 && kind!=2) || protocol))return linux_error(kind==5?94:93); /* no SOCK_SEQPACKET on macOS */
    if(domain==2 && (kind!=1 || (protocol!=0 && protocol!=6)) && (kind!=2 || (protocol!=0 && protocol!=17))) return linux_error(95);
    int host=socket(domain==1?AF_UNIX:AF_INET,kind==2?SOCK_DGRAM:SOCK_STREAM,0);
    if(host>=0 && (fcntl(host,F_SETFD,FD_CLOEXEC)<0 || (((type&2048) || net_scheduled) && fcntl(host,F_SETFL,O_NONBLOCK)<0))) {
        uint64_t e=net_error();close(host);return e;
    }
    return net_adopt(host,((unsigned)type&526336)|(kind==2 && domain==2?NET_UDP:0)|(domain==1?NET_UNIX:0));
}
/* socketpair(AF_UNIX): two connected host Unix sockets. */
static uint64_t net_socketpair(uint64_t domain,uint64_t type,uint64_t protocol,uint64_t vector) {
    if(domain!=1)return linux_error(domain==2?95:97);
    uint64_t kind=type&~(UINT64_C(2048)|524288);
    if((kind!=1 && kind!=2) || protocol)return linux_error(kind==5?94:93);
    unsigned char *out=lookup(vector,8,2);if(!out)return linux_error(14);
    int pair[2];if(socketpair(AF_UNIX,kind==2?SOCK_DGRAM:SOCK_STREAM,0,pair)<0)return net_error();
    for(int i=0;i<2;i++)
        if(fcntl(pair[i],F_SETFD,FD_CLOEXEC)<0 || (((type&2048) || net_scheduled) && fcntl(pair[i],F_SETFL,O_NONBLOCK)<0)) { uint64_t e=net_error();close(pair[0]);close(pair[1]);return e; }
    uint64_t first=net_adopt(pair[0],((unsigned)type&526336)|NET_UNIX);
    if((int64_t)first<0) { close(pair[1]);return first; }
    uint64_t second=net_adopt(pair[1],((unsigned)type&526336)|NET_UNIX);
    if((int64_t)second<0) { (void)net_release((unsigned)net_slot(first));return second; }
    for(unsigned i=0;i<4;i++) { out[i]=(unsigned char)(first>>(8*i));out[4+i]=(unsigned char)(second>>(8*i)); }
    return 0;
}
/* struct ucred {pid, uid, gid} for SO_PEERCRED; defined with the process layer. */
static void net_credentials(unsigned char out[12]);
/* bind/connect of an AF_UNIX socket to a guest path; defined with the file layer. */
static uint64_t net_unix_address(int fd,uint64_t address,uint64_t length,int binding,char path[108]);
/* A guest IPv4 sockaddr as a host address. Loopback and the wildcard (as on Linux)
   mean the host loopback, with published-port translation; servers that bind the
   wildcard are not exposed beyond it. Other addresses need --outbound and cannot
   be bound. A UDP client binding the wildcard to an ephemeral port keeps the
   wildcard, so replies from outside arrive. */
static uint64_t net_address(uint64_t guest,uint64_t length,struct sockaddr_in *address,int binding,int udp) {
    if(length<16) return linux_error(22);
    const unsigned char *p=lookup(guest,16,4);if(!p) return linux_error(14);
    if(le(p,2)!=2) return linux_error(97);
    int loopback=p[4]==127 && p[5]==0 && p[6]==0 && p[7]==1, wildcard=!p[4] && !p[5] && !p[6] && !p[7];
    uint16_t port;memcpy(&port,p+2,2);
    memset(address,0,sizeof(*address));
#ifdef __APPLE__
    address->sin_len=sizeof(*address);
#endif
    address->sin_family=AF_INET;
    if(loopback || wildcard) {
        address->sin_port=net_port_map(port,1);address->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(binding && wildcard && udp && !port && net_outbound)address->sin_addr.s_addr=htonl(INADDR_ANY);
        return 0;
    }
    if(binding || !net_outbound) return linux_error(13);
    address->sin_port=port;memcpy(&address->sin_addr,p+4,4);return 0;
}
/* Linux sockaddr_in (16 bytes) for a host address, truncated to the guest's
   capacity; the length written back is always 16. */
static uint64_t net_put_address(const struct sockaddr_in *address,uint64_t out,uint64_t length) {
    unsigned char *len=lookup(length,4,4),*lenout=lookup(length,4,2);
    if(!len || !lenout) return linux_error(14);
    uint32_t capacity=(uint32_t)le(len,4);if(capacity>INT32_MAX) return linux_error(22);
    unsigned bytes=capacity<16?capacity:16;
    unsigned char *p=bytes?lookup(out,bytes,2):NULL;if(bytes && !p) return linux_error(14);
    uint16_t port=address->sin_addr.s_addr==htonl(INADDR_LOOPBACK)?net_port_map(address->sin_port,0):address->sin_port;
    unsigned char wire[16]={2,0};memcpy(wire+2,&port,2);memcpy(wire+4,&address->sin_addr,4);
    if(bytes) memcpy(p,wire,bytes);
    for(unsigned i=0;i<4;i++) lenout[i]=(unsigned char)(16u>>(8*i));
    return 0;
}
/* Readiness of a non-socket guest fd (pipes, stdio, files) for ppoll; -1 if unknown. Defined in io.h. */
static int io_poll_ready(uint64_t guest,unsigned events);
/* sendmsg/recvmsg: Linux msghdr {name, namelen, iov, iovlen, control, controllen,
   flags} (56 bytes). Addresses on UDP; no ancillary data. */
static uint64_t net_message(int fd,unsigned slot,uint64_t header,uint64_t flag_word,int receiving) {
    int udp=(int)net_description(slot)->udp;unsigned flags=(unsigned)flag_word;
    if(flags&~(64u|16384u|(receiving?2u:0u)))return linux_error(95);
    unsigned char *h=lookup(header,56,receiving?6:4);if(!h)return linux_error(14);
    uint64_t name=le(h,8),namelen=le(h+8,4),table=le(h+16,8),count=le(h+24,8),controllen=le(h+40,8);
    if(count>1024)return linux_error(22);
    if(!receiving && controllen)return linux_error(95);
    if(!receiving && name && !udp)return linux_error(106);
    unsigned char *entries=count?lookup(table,count*16,4):NULL;if(count && !entries)return linux_error(14);
    static struct iovec vec[1024];uint64_t total=0;
    for(unsigned i=0;i<count;i++) {
        uint64_t base=le(entries+i*16,8),length=le(entries+i*16+8,8);
        if(length>1024*1024-total)length=1024*1024-total;                     /* short transfer past 1 MiB */
        vec[i].iov_base=length?lookup(base,length,receiving?2:4):(void *)"";
        if(!vec[i].iov_base)return linux_error(14);
        vec[i].iov_len=(size_t)length;total+=length;
    }
    struct sockaddr_in address;struct msghdr m;memset(&m,0,sizeof(m));
    m.msg_iov=vec;m.msg_iovlen=(int)count;
    if(name && udp && !receiving) { uint64_t invalid=net_address(name,namelen,&address,0,1);if(invalid)return invalid; }
    if(name && udp) { m.msg_name=&address;m.msg_namelen=sizeof(address); }
    if(receiving)net_description(slot)->reads++;else net_description(slot)->writes++;
    ssize_t result=receiving?recvmsg(fd,&m,(flags&2)?MSG_PEEK:0):sendmsg(fd,&m,0);
    if(result<0 && net_scheduled && !(flags&64) && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
        return net_suspend_one(fd,receiving?POLLIN:POLLOUT,receiving?net_description(slot)->read_timeout:net_description(slot)->write_timeout);
    net_wait.active=0;if(result<0)return net_error();
    if(receiving) {
        if(name && udp) { uint64_t invalid=net_put_address(&address,name,header+8);if(invalid)return invalid; }
        else memset(h+8,0,4);
        memset(h+40,0,8);memset(h+48,0,4);if(m.msg_flags&MSG_TRUNC)h[48]=0x20;            /* Linux MSG_TRUNC */
    }
    return (uint64_t)result;
}
static uint64_t net_call(uint64_t number) {
    int fd=net_host(cpu->gpr[0]);if(fd<0) return linux_error(9);
    unsigned slot=(unsigned)net_slot(cpu->gpr[0]);
    if(number==57) { net_wait.active=0;return net_release(slot)<0?net_error():0; }
    if(number==208 && net_scheduled && cpu->gpr[1]==1 && (cpu->gpr[2]==20 || cpu->gpr[2]==21)) {
        if(cpu->gpr[4]!=16)return linux_error(22);
        unsigned char *p=lookup(cpu->gpr[3],16,4);if(!p)return linux_error(14);
        uint64_t sec=le(p,8),us=le(p+8,8);
        if(us>=1000000)return linux_error(33); /* Linux EDOM */
        uint64_t ms=sec>UINT32_MAX/1000?NET_FOREVER:sec*1000+(us+999)/1000;
        unsigned timeout=!ms || ms>=NET_FOREVER?NET_FOREVER:(unsigned)ms; /* zero means no timeout */
        if(cpu->gpr[2]==20)net_description(slot)->read_timeout=timeout;else net_description(slot)->write_timeout=timeout;
        return 0;
    }
    if(number==209 && net_scheduled && cpu->gpr[1]==1 && (cpu->gpr[2]==3 || cpu->gpr[2]==39 || cpu->gpr[2]==17)) {
        /* SO_TYPE, SO_DOMAIN, and SO_PEERCRED (peers share this container's
           identity; the pid reported is the caller's). */
        unsigned want=cpu->gpr[2]==17?12:4;
        unsigned char *len=lookup(cpu->gpr[4],4,4),*lenout=lookup(cpu->gpr[4],4,2),*out=lookup(cpu->gpr[3],want,2);
        if(!len || !lenout || !out)return linux_error(14);if(le(len,4)<want)return linux_error(22);
        if(cpu->gpr[2]==17 && !net_description(slot)->unix_socket)return linux_error(92);
        uint64_t value=0;
        if(cpu->gpr[2]==3) { int type=0;socklen_t size=sizeof(type);if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size))return net_error();value=(uint64_t)type; }
        else if(cpu->gpr[2]==39)value=net_description(slot)->unix_socket?1:2;
        for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(value>>(8*i));
        if(cpu->gpr[2]==17)net_credentials(out);
        for(unsigned i=0;i<4;i++)lenout[i]=(unsigned char)(want>>(8*i));
        return 0;
    }
    if(number==209 && net_scheduled) {
        if(cpu->gpr[1]!=1 || cpu->gpr[2]!=4)return linux_error(95);
        unsigned char *len=lookup(cpu->gpr[4],4,4),*lenout=lookup(cpu->gpr[4],4,2),*out=lookup(cpu->gpr[3],4,2);
        if(!len || !lenout || !out)return linux_error(14);if(le(len,4)<4)return linux_error(22);
        int error=0;socklen_t size=sizeof(error);
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size))return net_error();
        uint64_t result=0;if(error) { errno=error;result=0-net_error(); }
        for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(result>>(8*i));
        lenout[0]=4;lenout[1]=lenout[2]=lenout[3]=0;return 0;
    }
    if(number==208) {
        /* Common options, mapped from Linux numbers to the host's. Unknown ones: ENOPROTOOPT. */
        uint64_t level=cpu->gpr[1],name=cpu->gpr[2];int host_level=-1,host_name=-1;
        /* IP_MTU_DISCOVER and IP_RECVERR (glibc's resolver requires it): accepted;
           ICMP errors on connected UDP sockets still surface as receive errors. */
        if(level==0 && (name==10 || name==11)) return 0;
        if(level==1) { /* SOL_SOCKET */
            host_level=SOL_SOCKET;
            host_name=name==2?SO_REUSEADDR:name==9?SO_KEEPALIVE:name==7?SO_SNDBUF:name==8?SO_RCVBUF:name==15?SO_REUSEPORT:name==13?SO_LINGER:name==6?SO_BROADCAST:-1;
        } else if(level==6) { /* IPPROTO_TCP */
            host_level=IPPROTO_TCP;
            host_name=name==1?TCP_NODELAY:name==4?TCP_KEEPALIVE:name==5?TCP_KEEPINTVL:name==6?TCP_KEEPCNT:-1;
        }
        if(host_name<0)return linux_error(92);
        if(host_name==SO_LINGER) {
            if(cpu->gpr[4]!=8)return linux_error(22);
            const unsigned char *p=lookup(cpu->gpr[3],8,4);if(!p)return linux_error(14);
            struct linger value={(int)le(p,4)!=0,(int)le(p+4,4)};
            return setsockopt(fd,SOL_SOCKET,SO_LINGER,&value,sizeof(value))<0?net_error():0;
        }
        if(cpu->gpr[4]<4) return linux_error(22);
        const unsigned char *p=lookup(cpu->gpr[3],4,4);if(!p) return linux_error(14);
        int value=(int)le(p,4);if(host_name==SO_REUSEADDR || host_name==SO_KEEPALIVE || host_name==SO_REUSEPORT || host_name==TCP_NODELAY || host_name==SO_BROADCAST)value=value!=0;
        return setsockopt(fd,host_level,host_name,&value,sizeof(value))<0?net_error():0;
    }
    if((number==200 || number==203) && net_description(slot)->unix_socket) {
        uint64_t result=net_unix_address(fd,cpu->gpr[1],cpu->gpr[2],number==200,net_description(slot)->path);
        if(!result)net_description(slot)->named=number==200?2:4;
        return result;
    }
    if(number==200 || number==203) {
        struct sockaddr_in address;uint64_t invalid=net_address(cpu->gpr[1],cpu->gpr[2],&address,number==200,(int)net_description(slot)->udp);
        if(invalid) return invalid;
        if(number==203 && net_scheduled && net_description(slot)->connecting) {
            struct pollfd ready={fd,POLLOUT,0};int n=poll(&ready,1,0);
            if(n<0)return net_error();
            if(!n) {
                if(net_description(slot)->flags&2048)return linux_error(114);
                uint64_t wait=net_suspend_one(fd,POLLOUT,net_description(slot)->write_timeout);return wait==IO_BLOCK?wait:linux_error(110);
            }
            int error=0;socklen_t size=sizeof(error);
            if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size))return net_error();
            net_description(slot)->connecting=0;net_wait.active=0;
            if(error) { errno=error;return net_error(); }return 0;
        }
        int result=number==200?bind(fd,(struct sockaddr *)&address,sizeof(address)):connect(fd,(struct sockaddr *)&address,sizeof(address));
        if(result<0 && number==203 && net_scheduled && (errno==EINPROGRESS || errno==EALREADY)) {
            net_description(slot)->connecting=1;
            if(net_description(slot)->flags&2048)return linux_error(115);
            return net_suspend_one(fd,POLLOUT,net_description(slot)->write_timeout);
        }
        net_wait.active=0;return result<0?net_error():0;
    }
    if(number==201) {
        /* Linux takes an int, then compares it as unsigned against somaxconn.
           Negative and oversized requests are capped, not rejected. Keep our
           bounded policy instead of inheriting the host's queue limit. */
        uint32_t requested=(uint32_t)cpu->gpr[1];
        unsigned backlog=requested>NET_BACKLOG_LIMIT?NET_BACKLOG_LIMIT:requested;
        if(net_description(slot)->unix_socket)return listen(fd,(int)backlog)<0?net_error():0;
        /* Prevent listen() from implicitly binding an unbound socket to wildcard. */
        struct sockaddr_in address;socklen_t length=sizeof(address);
        if(getsockname(fd,(struct sockaddr *)&address,&length)<0) return net_error();
        if(address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) return linux_error(13);
        struct timeval wait_for_client={30,0};
        if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&wait_for_client,sizeof(wait_for_client))) return net_error();
        if(trace)fprintf(stderr,"listen requested=%" PRId32 " effective=%u\n",(int32_t)requested,backlog);
        return listen(fd,(int)backlog)<0?net_error():0;
    }
    if(number==25) {
        if(net_scheduled && cpu->gpr[1]==1)return (net_flags[slot]&524288)?1:0;
        if(net_scheduled && cpu->gpr[1]==2) { if(cpu->gpr[2]>1)return linux_error(22);net_flags[slot]=(net_flags[slot]&~524288u)|(cpu->gpr[2]?524288u:0);return 0; }
        if(net_scheduled && cpu->gpr[1]==3)return 2|(net_description(slot)->flags&2048);
        if(cpu->gpr[1]==3) { int f=fcntl(fd,F_GETFL);return f<0?net_error():(uint64_t)(2|((f&O_NONBLOCK)?2048:0)); }
        if(cpu->gpr[1]==4) {
            /* Like Linux F_SETFL, ignore access-mode and fixed flags (O_RDWR, O_LARGEFILE...);
               only O_NONBLOCK changes. O_ASYNC (8192) and O_DIRECT are not supported. */
            if(cpu->gpr[2]&(8192|65536))return linux_error(95);
            if(net_scheduled) { net_description(slot)->flags=(unsigned)cpu->gpr[2]&2048;return 0; }
            int f=fcntl(fd,F_GETFL);if(f<0)return net_error();
            return fcntl(fd,F_SETFL,(f&~O_NONBLOCK)|((cpu->gpr[2]&2048)?O_NONBLOCK:0))<0?net_error():0;
        }
        return linux_error(95);
    }
    if(number==210) { if(cpu->gpr[1]>2)return linux_error(22);return shutdown(fd,(int)cpu->gpr[1])<0?net_error():0; }
    if(number==202 || number==242) {
        net_description(slot)->reads++;
        if(number==242 && (cpu->gpr[3]&~UINT64_C(526336)))return linux_error(22);
        struct sockaddr_storage storage;memset(&storage,0,sizeof(storage));socklen_t peer_size=sizeof(storage);
        struct sockaddr_in peer;int unix_socket=(int)net_description(slot)->unix_socket;
        int host=accept(fd,cpu->gpr[1]?(struct sockaddr *)&storage:NULL,cpu->gpr[1]?&peer_size:NULL);
        memcpy(&peer,&storage,sizeof(peer));
        if(host<0) {
            if(net_scheduled && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))return net_suspend_one(fd,POLLIN,net_description(slot)->read_timeout);
            net_wait.active=0;return net_error();
        }
        net_wait.active=0;
        /* Linux consumes the accepted connection even if copying its peer
           address fails. Never expose the Darwin sockaddr layout to guests. */
        if(cpu->gpr[1]) {
            unsigned char *len=lookup(cpu->gpr[2],4,4),*lenout=lookup(cpu->gpr[2],4,2);
            if(!len || !lenout) { close(host);return linux_error(14); }
            uint32_t capacity=(uint32_t)le(len,4);
            if(capacity>INT32_MAX) { close(host);return linux_error(22); }
            unsigned size=unix_socket?2:16;                          /* a Unix peer is unnamed */
            unsigned bytes=capacity<size?capacity:size;
            unsigned char *out=bytes?lookup(cpu->gpr[1],bytes,2):NULL;
            if(bytes && !out) { close(host);return linux_error(14); }
            unsigned char wire[16]={2,0};
            if(unix_socket)wire[0]=1;
            else { memcpy(wire+2,&peer.sin_port,2);memcpy(wire+4,&peer.sin_addr,4); }
            if(bytes)memcpy(out,wire,bytes);
            for(unsigned i=0;i<4;i++)lenout[i]=(unsigned char)(size>>(8*i));
        }
        /* Linux accept does not inherit O_NONBLOCK; enforce this on both hosts. */
        int flags=fcntl(host,F_GETFL);
        if(flags<0 || fcntl(host,F_SETFD,FD_CLOEXEC)<0 || fcntl(host,F_SETFL,(flags&~O_NONBLOCK)|((net_scheduled || (number==242 && (cpu->gpr[3]&2048)))?O_NONBLOCK:0))<0) {
            uint64_t e=net_error();close(host);return e;
        }
        return net_adopt(host,(number==242?(unsigned)cpu->gpr[3]:0)|(unix_socket?NET_UNIX:0));
    }
    if((number==204 || number==205) && net_description(slot)->unix_socket) {
        /* The guest path this socket was bound (getsockname) or connected (getpeername) to. */
        struct NetDescription *d=net_description(slot);
        unsigned char *lenp=lookup(cpu->gpr[2],4,4),*lenw=lookup(cpu->gpr[2],4,2);if(!lenp || !lenw)return linux_error(14);
        uint32_t capacity=(uint32_t)le(lenp,4);if(capacity>INT32_MAX)return linux_error(22);
        int named=d->named==(number==204?2u:4u);
        if(number==205 && !named && d->named!=4) { struct sockaddr_storage peer;socklen_t size=sizeof(peer);if(getpeername(fd,(struct sockaddr *)&peer,&size)<0)return net_error(); }
        unsigned char wire[110]={1,0};unsigned size=2;
        if(named) { size_t n=strlen(d->path);memcpy(wire+2,d->path,n+1);size=(unsigned)(n+3); }
        unsigned bytes=capacity<size?capacity:size;
        unsigned char *out=bytes?lookup(cpu->gpr[1],bytes,2):NULL;if(bytes && !out)return linux_error(14);
        if(bytes)memcpy(out,wire,bytes);
        for(unsigned i=0;i<4;i++)lenw[i]=(unsigned char)(size>>(8*i));
        return 0;
    }
    if(number==204 || number==205) {
        unsigned char *lenp=lookup(cpu->gpr[2],4,4), *lenw=lookup(cpu->gpr[2],4,2);
        if(!lenp || !lenw) return linux_error(14);
        uint64_t capacity=le(lenp,4), bytes=capacity<16?capacity:16;
        if(capacity>INT32_MAX)return linux_error(22);
        unsigned char *out=bytes?lookup(cpu->gpr[1],bytes,2):NULL;
        if(bytes && !out) return linux_error(14);
        struct sockaddr_in address;socklen_t length=sizeof(address);
        int result=number==204?getsockname(fd,(struct sockaddr *)&address,&length):getpeername(fd,(struct sockaddr *)&address,&length);
        if(result<0) return net_error();
        uint16_t port=address.sin_addr.s_addr==htonl(INADDR_LOOPBACK)?net_port_map(address.sin_port,0):address.sin_port;
        unsigned char wire[16]={2,0};memcpy(wire+2,&port,2);memcpy(wire+4,&address.sin_addr,4);
        if(bytes) memcpy(out,wire,(size_t)bytes);
        lenw[0]=16;lenw[1]=lenw[2]=lenw[3]=0;return 0;
    }
    if(number==269) {
        /* sendmmsg: struct mmsghdr is a msghdr plus its msg_len (64 bytes). glibc's
           resolver sends its A and AAAA queries this way, without a fallback. */
        uint64_t vector=cpu->gpr[1],count=cpu->gpr[2]>1024?1024:cpu->gpr[2],flags=cpu->gpr[3],sent=0;
        for(;sent<count;sent++) {
            uint64_t result=net_message(fd,slot,vector+sent*64,flags,0);
            if(result==IO_BLOCK || (int64_t)result<0) { if(!sent)return result;net_wait.active=0;break; }
            unsigned char *length=lookup(vector+sent*64+56,4,2);if(!length)return sent?sent:linux_error(14);
            for(unsigned i=0;i<4;i++)length[i]=(unsigned char)(result>>(8*i));
        }
        return sent;
    }
    if(number==211 || number==212)return net_message(fd,slot,cpu->gpr[1],cpu->gpr[2],number==212);
    if(number==63 || number==64 || number==206 || number==207) {
        /* read/write, and sendto/recvfrom (with addresses on UDP). Linux flags:
           MSG_PEEK 2, MSG_DONTWAIT 64, MSG_NOSIGNAL 16384 (SIGPIPE is ignored anyway). */
        int receiving=number==63 || number==207,udp=(int)net_description(slot)->udp;
        unsigned flags=number>=206?(unsigned)cpu->gpr[3]:0;
        if(flags&~(2u|64u|16384u) || ((flags&2) && !receiving))return linux_error(95);
        if(number==206 && cpu->gpr[4] && !udp)return linux_error(106);         /* EISCONN: TCP takes no address */
        struct sockaddr_in to,from;socklen_t from_size=sizeof(from);int addressed=number>=206 && cpu->gpr[4] && udp;
        if(addressed && number==206) { uint64_t invalid=net_address(cpu->gpr[4],cpu->gpr[5],&to,0,1);if(invalid)return invalid; }
        if(addressed && number==207 && (!lookup(cpu->gpr[5],4,2) || !lookup(cpu->gpr[5],4,4)))return linux_error(14);
        /* Activity counters for edge-triggered epoll; descriptions are shared across workers. */
        __atomic_fetch_add(receiving?&net_description(slot)->reads:&net_description(slot)->writes,1,__ATOMIC_RELAXED);
        if(cpu->gpr[2]>1024*1024) return linux_error(22);
        unsigned char *buffer=cpu->gpr[2]?lookup(cpu->gpr[1],cpu->gpr[2],receiving?2:4):(unsigned char *)"";
        if(!buffer) return linux_error(14);
        if(number==207 && cpu->gpr[4] && !udp) {                                /* TCP reports no source address */
            unsigned char *length=lookup(cpu->gpr[5],4,2);if(!length)return linux_error(14);
            memset(length,0,4);
        }
        int host_flags=(flags&2)?MSG_PEEK:0;
        ssize_t result=receiving?(addressed?recvfrom(fd,buffer,(size_t)cpu->gpr[2],host_flags,(struct sockaddr *)&from,&from_size):recv(fd,buffer,(size_t)cpu->gpr[2],host_flags))
                                :(addressed?sendto(fd,buffer,(size_t)cpu->gpr[2],0,(struct sockaddr *)&to,sizeof(to)):send(fd,buffer,(size_t)cpu->gpr[2],0));
        if(result>=0 && addressed && receiving) { uint64_t invalid=net_put_address(&from,cpu->gpr[4],cpu->gpr[5]);if(invalid)return invalid; }
        if(result<0 && net_scheduled && !(flags&64) && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
            return net_suspend_one(fd,receiving?POLLIN:POLLOUT,receiving?net_description(slot)->read_timeout:net_description(slot)->write_timeout);
        net_wait.active=0;return result<0?net_error():(uint64_t)result;
    }
    return linux_error(38);
}

/* ppoll over sockets, pipes, stdio and files; no signal mask, at most 1024 entries.
   Sockets wait on the host; pipe changes wake the task through io_epoch. */
static uint64_t net_poll(void) {
    if(cpu->gpr[1]>NET_FD_LIMIT || cpu->gpr[3])return linux_error(95);
    unsigned count=(unsigned)cpu->gpr[1];
    unsigned char *in=count?lookup(cpu->gpr[0],count*8,4):NULL,*out=count?lookup(cpu->gpr[0],count*8,2):NULL;
    if(count && (!in || !out))return linux_error(14);
    unsigned timeout=NET_FOREVER;
    if(cpu->gpr[2]) {
        unsigned char *ts=lookup(cpu->gpr[2],16,4);if(!ts)return linux_error(14);
        uint64_t sec=le(ts,8),ns=le(ts+8,8);if(ns>=1000000000)return linux_error(22);
        uint64_t ms=sec>UINT32_MAX/1000?NET_FOREVER:sec*1000+(ns+999999)/1000000;
        timeout=ms>=NET_FOREVER?NET_FOREVER:(unsigned)ms;
    }
    static struct pollfd fds[NET_FD_LIMIT];static int other[NET_FD_LIMIT];unsigned ready_other=0;
    for(unsigned i=0;i<count;i++) {
        int32_t guest=(int32_t)le(in+i*8,4);unsigned events=(unsigned)le(in+i*8+4,2);
        fds[i].fd=guest<0?-1:net_host((uint64_t)guest);
        if(fds[i].fd<0 && guest>=0)fds[i].fd=io_input_host((uint64_t)guest);
        fds[i].events=(short)(((events&1)?POLLIN:0)|((events&4)?POLLOUT:0));fds[i].revents=0;
        other[i]=0;
        if(guest>=0 && fds[i].fd<0) { other[i]=io_poll_ready((uint64_t)guest,events);if(other[i]<0)other[i]=32;if(other[i])ready_other++; }
    }
    int wait_ms=ready_other || net_scheduled?0:timeout==NET_FOREVER?-1:timeout>INT32_MAX?INT32_MAX:(int)timeout;
    int result=poll(fds,count,wait_ms);
    if(result==0 && !ready_other && timeout && net_scheduled) {
        uint64_t wait=net_suspend(fds,count,(unsigned)timeout);
        for(unsigned i=0;i<count;i++)if(fds[i].fd<0 && (int32_t)le(in+i*8,4)>=0)net_wait.targeted=0; /* a pipe or stdio */
        if(wait==IO_BLOCK)return wait;
    }
    net_wait.active=0;
    /* Host profiling requests are not guest signals. Resume the guest poll loop. */
    if(result<0 && errno==EINTR && profiling && profile_requested) {
        profile_requested=0;profile_dump();return 0;
    }
    if(result<0)return net_error();
    unsigned ready=0;
    for(unsigned i=0;i<count;i++) {
        int32_t guest=(int32_t)le(in+i*8,4);short e=fds[i].revents;
        unsigned value=((e&POLLIN)?1:0)|((e&POLLOUT)?4:0)|((e&POLLERR)?8:0)|((e&POLLHUP)?16:0)|((e&POLLNVAL)?32:0);
        if(guest>=0 && fds[i].fd<0)value=(unsigned)other[i];
        out[i*8+6]=(unsigned char)value;out[i*8+7]=0;if(value)ready++;
    }
    return ready;
}
