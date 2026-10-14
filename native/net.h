/* Bounded IPv4 TCP and UDP, with optional scheduler-aware nonblocking hosts. Guests
   reach the host loopback; with --outbound they also reach other IPv4 addresses.
   IPv6 sockets are dual-stack ones over IPv4 (see net_v6_to_v4). */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
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
/* --network DIR --ip A.B.C.D: a private bridge network. The container's TCP ports
   become private host-loopback ports (unless published); DIR holds a file per
   listening port, "<ip>:<port>", with its host port, so the container's own
   loopback and its peers (by address) reach it. <subnet>.1 is the gateway: the Mac. */
static char net_dir[1024];
static uint32_t net_ip; /* host byte order; 0: the Mac's network, shared */
#define NET_PRIVATE_LIMIT 256
static struct { uint16_t guest,host; } net_private[NET_PRIVATE_LIMIT]; /* this runtime's listening ports, network byte order */
static unsigned net_private_count;
static _Thread_local uint16_t net_binding_port; /* guest port of the bind in progress (private network) */
static void net_port_file(char *out,size_t size,uint32_t ip,uint16_t port) {
    snprintf(out,size,"%s/%u.%u.%u.%u:%u",net_dir,ip>>24,(ip>>16)&255,(ip>>8)&255,ip&255,(unsigned)ntohs(port));
}
static uint16_t net_private_file(uint32_t ip,uint16_t port) { /* the host port in a port file, or 0 */
    char path[1100];net_port_file(path,sizeof path,ip,port);
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;
    char text[16];ssize_t n=read(fd,text,sizeof text-1);close(fd);if(n<=0)return 0;text[n]=0;
    unsigned long value=strtoul(text,NULL,10);return value && value<65536?htons((uint16_t)value):0;
}
static uint16_t net_private_lookup(uint32_t ip,uint16_t port) {
    if(ip==net_ip)for(unsigned i=0;i<net_private_count;i++)if(net_private[i].guest==port)return net_private[i].host;
    return net_private_file(ip,port);
}
static void net_private_record(uint16_t guest,uint16_t host) {
    if(net_private_count<NET_PRIVATE_LIMIT) { net_private[net_private_count].guest=guest;net_private[net_private_count++].host=host; }
    char path[1100],temp[1120];net_port_file(path,sizeof path,net_ip,guest);snprintf(temp,sizeof temp,"%s.%d",path,(int)getpid());
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return;
    char text[16];int n=snprintf(text,sizeof text,"%u\n",(unsigned)ntohs(host));
    int ok=write(fd,text,(size_t)n)==n;close(fd);
    if(!ok || rename(temp,path)<0)unlink(temp);
}
static void net_private_forget(uint16_t guest) {
    for(unsigned i=0;i<net_private_count;i++)if(net_private[i].guest==guest) {
        uint16_t host=net_private[i].host;net_private[i]=net_private[--net_private_count];
        if(net_private_file(net_ip,guest)==host) { char path[1100];net_port_file(path,sizeof path,net_ip,guest);unlink(path); }
        return;
    }
}
static uint16_t net_published(uint16_t port) { /* host port for a published guest port, or 0 */
    for(unsigned i=0;i<net_publish_count;i++)if(ntohs(port)==net_publish_guest[i])return htons(net_publish_host[i]);
    return 0;
}
/* Ports in network byte order. */
static uint16_t net_port_map(uint16_t port,int to_host) {
    uint16_t value=ntohs(port);
    for(unsigned i=0;i<net_publish_count;i++)
        if(value==(to_host?net_publish_guest[i]:net_publish_host[i])) return htons(to_host?net_publish_host[i]:net_publish_guest[i]);
    if(!to_host)for(unsigned i=0;i<net_private_count;i++)if(net_private[i].host==port)return net_private[i].guest;
    return port;
}
static int net_scheduled;
static int net_outbound; /* --outbound: connect/sendto may reach any IPv4 address */
#define NET_UDP (1u<<30)  /* net_adopt flag: datagram socket */
#define NET_UNIX (1u<<29) /* net_adopt flag: AF_UNIX socket */
#define NET_V6 (1u<<28)   /* net_adopt flag: AF_INET6 socket */
static uint64_t net_wait_blocks;
static int io_poll_host(uint64_t guest); /* --stdin's host descriptor or a timerfd's kqueue, polled like a socket */
static int io_managed(uint64_t fd);static int fs_host(uint64_t n); /* io.h and fs.h, for select */
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
/* unix: AF_UNIX; named: 2 bound, 4 connected, to the guest path in path. v6: AF_INET6;
   v6only: IPV6_V6ONLY set, and bound: v6_port and v6_address are the guest's (see net_bind_v6only). */
static struct NetDescription { int host;unsigned refs,flags,connecting,read_timeout,write_timeout,reads,writes,udp,unix_socket,named;char path[108];uint16_t private_port;
    unsigned v6,v6only,v6_bound;uint16_t v6_port;unsigned char v6_address[16]; } net_descriptions[NET_DESCRIPTIONS];
static struct NetDescription *net_host_description(int host) {
    return host>=0 && (unsigned)host<NET_DESCRIPTIONS && net_descriptions[host].refs?&net_descriptions[host]:NULL;
}
static struct NetDescription *net_description(unsigned slot) {
    struct NetDescription *description=net_host_description(net_fd[slot]-1);
    if(!description)fail("Missing socket open description");
    return description;
}
static int net_description_release(struct NetDescription *description) {
    int result=0;
    if(!--description->refs) {
        if(description->private_port)net_private_forget(description->private_port);
        epoll_forget_socket(description->host);result=close(description->host);memset(description,0,sizeof(*description));
    }
    return result;
}
static int net_release(unsigned slot) {
    int result=net_description_release(net_description(slot));
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
        net_descriptions[host]=(struct NetDescription){host,1,guest_flags&2048,0,NET_FOREVER,NET_FOREVER,0,0,(guest_flags&NET_UDP)!=0,(guest_flags&NET_UNIX)!=0,0,{0},0,(guest_flags&NET_V6)!=0,0,0,0,{0}};
        net_fd[i]=host+1;net_flags[i]=guest_flags&524288;net_guest[i]=(unsigned short)guest;fd_bind((uint64_t)guest,FD_NET,i);return (uint64_t)guest; }
    close(host);return linux_error(24);
}
static uint64_t net_socket(uint64_t domain,uint64_t type,uint64_t protocol) {
    if(domain!=2 && domain!=1 && domain!=10) return linux_error(97);
    unsigned v6=domain==10?NET_V6:0;if(v6)domain=2;
    uint64_t kind=type&~(UINT64_C(2048)|524288);                              /* SOCK_STREAM 1, SOCK_DGRAM 2 */
    if(domain==1 && ((kind!=1 && kind!=2) || protocol))return linux_error(kind==5?94:93); /* no SOCK_SEQPACKET on macOS */
    if(domain==2 && (kind!=1 || (protocol!=0 && protocol!=6)) && (kind!=2 || (protocol!=0 && protocol!=17))) return linux_error(95);
    int host=socket(domain==1?AF_UNIX:AF_INET,kind==2?SOCK_DGRAM:SOCK_STREAM,0);
    if(host>=0 && (fcntl(host,F_SETFD,FD_CLOEXEC)<0 || (((type&2048) || net_scheduled) && fcntl(host,F_SETFL,O_NONBLOCK)<0))) {
        uint64_t e=net_error();close(host);return e;
    }
    return net_adopt(host,((unsigned)type&526336)|(kind==2 && domain==2?NET_UDP:0)|(domain==1?NET_UNIX:0)|v6);
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
/* IPv6 sockets are IPv4 ones underneath, like Linux dual-stack sockets: :: is the
   wildcard, ::1 the loopback and ::ffff:a.b.c.d that IPv4 address. Other IPv6
   addresses are unreachable, as on Docker's default networks (clients fall back to
   IPv4). Returns 0 for those. */
static int net_v6_to_v4(const unsigned char *a,unsigned char v4[4]) {
    static const unsigned char zero[15],mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
    if(!memcmp(a,zero,15) && a[15]<=1) { v4[0]=a[15]?127:0;v4[1]=v4[2]=0;v4[3]=a[15];return 1; }
    if(!memcmp(a,mapped,12)) { memcpy(v4,a+12,4);return 1; }
    return 0;
}
static uint64_t net_address(uint64_t guest,uint64_t length,struct sockaddr_in *address,int binding,int udp,int v6) {
    unsigned char mapped[8];const unsigned char *p;
    if(v6) {                                                     /* sockaddr_in6: family, port, flowinfo, address, scope */
        if(length<24) return linux_error(22);
        const unsigned char *q=lookup(guest,24,4);if(!q) return linux_error(14);
        if(le(q,2)!=10) return linux_error(97);
        mapped[0]=2;mapped[1]=0;memcpy(mapped+2,q+2,2);
        if(!net_v6_to_v4(q+8,mapped+4)) return linux_error(binding?99:101);  /* EADDRNOTAVAIL, ENETUNREACH */
        p=mapped;
    } else {
        if(length<16) return linux_error(22);
        if(!(p=lookup(guest,16,4))) return linux_error(14);
        if(le(p,2)!=2) return linux_error(97);
    }
    int loopback=p[4]==127 && p[5]==0 && p[6]==0 && p[7]==1, wildcard=!p[4] && !p[5] && !p[6] && !p[7];
    uint16_t port;memcpy(&port,p+2,2);
    memset(address,0,sizeof(*address));
#ifdef __APPLE__
    address->sin_len=sizeof(*address);
#endif
    address->sin_family=AF_INET;
    if(net_ip && !udp) {
        uint32_t to=(uint32_t)p[4]<<24|(uint32_t)p[5]<<16|(uint32_t)p[6]<<8|p[7];
        int own=loopback || wildcard || to==net_ip,peer=(to&0xffffff00u)==(net_ip&0xffffff00u);
        address->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(binding) {
            if(!own)return linux_error(99);                                          /* EADDRNOTAVAIL */
            if(port && net_private_lookup(net_ip,port) && !net_published(port))return linux_error(98);
            net_binding_port=port;address->sin_port=net_published(port);              /* else a private port */
            return 0;
        }
        if(peer && (to&255)==1) { address->sin_port=port;return 0; }                 /* the gateway: the Mac */
        if(own || peer) {
            uint16_t host=net_private_lookup(own?net_ip:to,port);
            if(!host)return linux_error(111);                                        /* ECONNREFUSED */
            address->sin_port=host;return 0;
        }
    }
    if(loopback || wildcard) {
        address->sin_port=net_port_map(port,1);address->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(binding && wildcard && udp && !port && net_outbound)address->sin_addr.s_addr=htonl(INADDR_ANY);
        return 0;
    }
    if(binding || !net_outbound) return linux_error(13);
    address->sin_port=port;memcpy(&address->sin_addr,p+4,4);return 0;
}
/* A host address as the guest's: Linux sockaddr_in (16 bytes), or sockaddr_in6 (28)
   on an IPv6 socket, with loopback ports translated back. Returns its size. */
static unsigned net_wire(unsigned char wire[28],const void *host,int v6) {
    memset(wire,0,28);
    if(((const struct sockaddr *)host)->sa_family==AF_INET6) {                  /* an IPV6_V6ONLY listener's peer */
        const struct sockaddr_in6 *a=host;wire[0]=10;memcpy(wire+2,&a->sin6_port,2);memcpy(wire+8,&a->sin6_addr,16);return 28;
    }
    const struct sockaddr_in *a=host;
    uint16_t port=a->sin_addr.s_addr==htonl(INADDR_LOOPBACK)?net_port_map(a->sin_port,0):a->sin_port;
    memcpy(wire+2,&port,2);
    if(!v6) { wire[0]=2;memcpy(wire+4,&a->sin_addr,4);return 16; }
    wire[0]=10;uint32_t ip=ntohl(a->sin_addr.s_addr);
    if(ip==INADDR_LOOPBACK)wire[23]=1;                                          /* ::1 */
    else if(ip) { wire[18]=wire[19]=255;memcpy(wire+20,&a->sin_addr,4); }       /* ::ffff:a.b.c.d */
    return 28;
}
/* Copies an address to the guest, truncated to its capacity; the length written back is the full size. */
static uint64_t net_put_wire(const unsigned char *wire,unsigned size,uint64_t out,uint64_t length) {
    unsigned char *len=lookup(length,4,4),*lenout=lookup(length,4,2);
    if(!len || !lenout) return linux_error(14);
    uint32_t capacity=(uint32_t)le(len,4);if(capacity>INT32_MAX) return linux_error(22);
    unsigned bytes=capacity<size?capacity:size;
    unsigned char *p=bytes?lookup(out,bytes,2):NULL;if(bytes && !p) return linux_error(14);
    if(bytes) memcpy(p,wire,bytes);
    for(unsigned i=0;i<4;i++) lenout[i]=(unsigned char)(size>>(8*i));
    return 0;
}
static uint64_t net_put_address(const void *address,uint64_t out,uint64_t length,int v6) {
    unsigned char wire[28];return net_put_wire(wire,net_wire(wire,address,v6),out,length);
}
/* Readiness of a non-socket guest fd (pipes, stdio, files) for ppoll; -1 if unknown. Defined in io.h. */
static int io_poll_ready(uint64_t guest,unsigned events);
/* sendmsg/recvmsg: Linux msghdr {name, namelen, iov, iovlen, control, controllen,
   flags} (56 bytes). Addresses on UDP; no ancillary data. */
static void fs_pack(unsigned char *p,unsigned offset,uint64_t value,unsigned size); /* fs.h */
static int fd_carry_send(uint64_t guestfd);                 /* syscall.h: SCM_RIGHTS */
static int64_t fd_carry_receive(int carrier,int cloexec);
static void fd_carry_cancel(int carrier);
#define NET_RIGHTS_MAX 253                                    /* SCM_MAX_FD */
static uint64_t net_message(int fd,unsigned slot,uint64_t header,uint64_t flag_word,int receiving) {
    int udp=(int)net_description(slot)->udp,unix_socket=(int)net_description(slot)->unix_socket,v6=(int)net_description(slot)->v6;unsigned flags=(unsigned)flag_word;
    if(flags&~(64u|16384u|(receiving?2u|0x40000000u:0u)))return linux_error(95);
    unsigned char *h=lookup(header,56,receiving?6:4);if(!h)return linux_error(14);
    uint64_t name=le(h,8),namelen=le(h+8,4),table=le(h+16,8),count=le(h+24,8),control=le(h+32,8),controllen=le(h+40,8);
    if(count>1024)return linux_error(22);
    /* SCM_RIGHTS on unix sockets: each descriptor travels as a carrier (fd_carry_send);
       credentials (SCM_CREDENTIALS) are the container's anyway and are dropped. */
    static int carriers[NET_RIGHTS_MAX];unsigned carried=0;
    static unsigned char host_control[CMSG_SPACE(NET_RIGHTS_MAX*sizeof(int))];
    if(!receiving && controllen) {
        const unsigned char *c=controllen<=4096?lookup(control,controllen,4):NULL;if(!c)return linux_error(controllen>4096?105:14);
        for(uint64_t at=0;at+16<=controllen;) {
            uint64_t length=le(c+at,8);unsigned level=(unsigned)le(c+at+8,4),type=(unsigned)le(c+at+12,4);
            if(length<16 || length>controllen-at) { for(unsigned i=0;i<carried;i++)fd_carry_cancel(carriers[i]);return linux_error(22); }
            if(level==1 && type==1) {
                if(!unix_socket) { for(unsigned i=0;i<carried;i++)fd_carry_cancel(carriers[i]);return linux_error(22); }
                for(uint64_t k=16;k+4<=length;k+=4) {
                    int carrier=carried<NET_RIGHTS_MAX?fd_carry_send(le(c+at+k,4)):-22;
                    if(carrier<0) { for(unsigned i=0;i<carried;i++)fd_carry_cancel(carriers[i]);return linux_error((uint64_t)-carrier); }
                    carriers[carried++]=carrier;
                }
            }
            at+=(length+7)&~UINT64_C(7);
        }
    }
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
    struct sockaddr_storage address;struct msghdr m;memset(&m,0,sizeof(m));
    m.msg_iov=vec;m.msg_iovlen=(int)count;
    if(name && udp && !receiving) { uint64_t invalid=net_address(name,namelen,(struct sockaddr_in *)&address,0,1,v6);if(invalid)return invalid; }
    if(name && udp) { m.msg_name=&address;m.msg_namelen=receiving?sizeof(address):sizeof(struct sockaddr_in); }
    if(carried) {
        m.msg_control=host_control;m.msg_controllen=(socklen_t)CMSG_SPACE(carried*sizeof(int));
        struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=(socklen_t)CMSG_LEN(carried*sizeof(int));
        memcpy(CMSG_DATA(c),carriers,carried*sizeof(int));
    }
    unsigned room=receiving && unix_socket && !(flags&2) && controllen>=20?(unsigned)((controllen-16)/4):0;  /* never PEEK carriers */
    if(room) { if(room>NET_RIGHTS_MAX)room=NET_RIGHTS_MAX;m.msg_control=host_control;m.msg_controllen=(socklen_t)CMSG_SPACE(room*sizeof(int)); }
    if(receiving)net_description(slot)->reads++;else net_description(slot)->writes++;
    ssize_t result=receiving?recvmsg(fd,&m,(flags&2)?MSG_PEEK:0):sendmsg(fd,&m,0);
    if(carried) {                                       /* sent: the message holds its own copies */
        int sent=result>=0;
        for(unsigned i=0;i<carried;i++) { if(sent)close(carriers[i]);else fd_carry_cancel(carriers[i]); }
    }
    if(result<0 && net_scheduled && !(flags&64) && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
        return net_suspend_one(fd,receiving?POLLIN:POLLOUT,receiving?net_description(slot)->read_timeout:net_description(slot)->write_timeout);
    net_wait.active=0;if(result<0)return net_error();
    if(receiving) {
        if(name && udp) { uint64_t invalid=net_put_address(&address,name,header+8,v6);if(invalid)return invalid; }
        else memset(h+8,0,4);
        memset(h+40,0,8);memset(h+48,0,4);if(m.msg_flags&MSG_TRUNC)h[48]=0x20;            /* Linux MSG_TRUNC */
        if(room && m.msg_controllen) {
            /* Install the descriptors received; those that do not fit are closed (MSG_CTRUNC). */
            unsigned char *out=lookup(control,controllen,2);unsigned installed=0,truncated=(m.msg_flags&MSG_CTRUNC)!=0;
            for(struct cmsghdr *c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)) {
                if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS)continue;
                unsigned n=(unsigned)((c->cmsg_len-CMSG_LEN(0))/sizeof(int));int *carrier=(int *)(void *)CMSG_DATA(c);
                for(unsigned i=0;i<n;i++) {
                    int64_t guest=out && installed<room?fd_carry_receive(carrier[i],(flags&0x40000000u)!=0):-1;
                    if(guest<0) { fd_carry_cancel(carrier[i]);truncated=1;continue; }
                    fs_pack(out,16+installed*4,(uint64_t)guest,4);installed++;
                }
            }
            if(installed) {
                fs_pack(out,0,16+installed*4,8);fs_pack(out,8,1,4);fs_pack(out,12,1,4);
                fs_pack(h+40,0,(16+installed*4+7)&~7u,8);
            }
            if(truncated)h[48]|=8;                                                           /* Linux MSG_CTRUNC */
        }
    }
    return (uint64_t)result;
}
/* ioctl on a socket: FIONBIO, FIONREAD (TIOCINQ), FIOCLEX and FIONCLEX. */
/* Interface queries on a socket (Java's NetworkInterface; Elasticsearch derives its
   publish address from them): lo, and eth0 on a private network with its /24 (the
   Mac is .1). Linux struct ifreq is 40 bytes: the name, then a 24-byte union. */
static void net_put_ipv4(unsigned char *at,uint32_t address) { memset(at,0,16);at[0]=2;for(unsigned i=0;i<4;i++)at[4+i]=(unsigned char)(address>>(24-8*i)); }
static uint64_t net_interface_ioctl(uint64_t request) {
    unsigned count=net_ip?2:1;
    if(request==0x8912) {                                                           /* SIOCGIFCONF */
        unsigned char *conf=lookup(cpu->gpr[2],16,6);if(!conf)return linux_error(14);
        uint64_t length=le(conf,4),buffer=le(conf+8,8);
        if(!buffer) { fs_pack(conf,0,count*40,4);return 0; }                         /* the size needed */
        unsigned n=length/40<count?(unsigned)(length/40):count;
        unsigned char *out=n?lookup(buffer,n*40,2):NULL;if(n && !out)return linux_error(14);
        for(unsigned i=0;i<n;i++) {
            memset(out+i*40,0,40);strcpy((char *)out+i*40,i?"eth0":"lo");
            net_put_ipv4(out+i*40+16,i?net_ip:UINT32_C(0x7f000001));
        }
        fs_pack(conf,0,n*40,4);return 0;
    }
    unsigned char *ifr=lookup(cpu->gpr[2],40,6);if(!ifr)return linux_error(14);
    char name[17]={0};memcpy(name,ifr,16);
    int index=!strcmp(name,"lo")?0:(net_ip && !strcmp(name,"eth0"))?1:-1;
    if(index<0)return linux_error(19);                                              /* ENODEV */
    unsigned char *u=ifr+16;
    switch(request) {
        case 0x8913:memset(u,0,24);fs_pack(u,0,index?0x1043:0x49,2);return 0;      /* SIOCGIFFLAGS: UP, RUNNING, LOOPBACK or BROADCAST|MULTICAST */
        case 0x8915:net_put_ipv4(u,index?net_ip:UINT32_C(0x7f000001));return 0;    /* SIOCGIFADDR */
        case 0x891b:net_put_ipv4(u,index?UINT32_C(0xffffff00):UINT32_C(0xff000000));return 0; /* SIOCGIFNETMASK */
        case 0x8919:if(!index)return linux_error(99);net_put_ipv4(u,net_ip|0xff);return 0; /* SIOCGIFBRDADDR */
        case 0x8921:memset(u,0,24);fs_pack(u,0,index?1500:65536,4);return 0;      /* SIOCGIFMTU */
        case 0x8933:memset(u,0,24);fs_pack(u,0,(uint64_t)index+1,4);return 0;     /* SIOCGIFINDEX */
        case 0x8927: {                                                              /* SIOCGIFHWADDR: 02:42 and the address, as Docker does */
            memset(u,0,24);fs_pack(u,0,index?1:772,2);
            if(index) { u[2]=0x02;u[3]=0x42;for(unsigned i=0;i<4;i++)u[4+i]=(unsigned char)(net_ip>>(24-8*i)); }
            return 0;
        }
        default:return linux_error(22);
    }
}
static uint64_t net_ioctl(void) {
    int fd=net_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
    unsigned slot=(unsigned)net_slot(cpu->gpr[0]);uint64_t request=cpu->gpr[1];
    if(request==0x5451 || request==0x5450) { net_flags[slot]=(net_flags[slot]&~524288u)|(request==0x5451?524288u:0);return 0; }
    if(request==0x5452) { const unsigned char *p=lookup(cpu->gpr[2],4,4);return p?0:linux_error(14); } /* FIOASYNC: accepted; no SIGIO is sent (nginx's channels need none) */
    if(request==0x5421) {
        const unsigned char *p=lookup(cpu->gpr[2],4,4);if(!p)return linux_error(14);
        int on=le(p,4)!=0;
        if(net_scheduled) { struct NetDescription *d=net_description(slot);d->flags=(d->flags&~2048u)|(on?2048u:0);return 0; }
        int f=fcntl(fd,F_GETFL);if(f<0)return net_error();
        return fcntl(fd,F_SETFL,on?f|O_NONBLOCK:f&~O_NONBLOCK)<0?net_error():0;
    }
    if(request==0x541B) {
        int n=0;if(ioctl(fd,FIONREAD,&n)<0)return net_error();
        unsigned char *p=lookup(cpu->gpr[2],4,2);if(!p)return linux_error(14);
        for(unsigned i=0;i<4;i++)p[i]=(unsigned char)((unsigned)n>>(8*i));
        return 0;
    }
    if(request>=0x8912 && request<=0x8933)return net_interface_ioctl(request);
    return linux_error(25);
}
/* Common socket options: Linux level and name to the host's; 0 when unknown. */
static int net_option(uint64_t level,uint64_t name,int *host_level,int *host_name) {
    if(level==1) { /* SOL_SOCKET */
        *host_level=SOL_SOCKET;
        *host_name=name==2?SO_REUSEADDR:name==9?SO_KEEPALIVE:name==7?SO_SNDBUF:name==8?SO_RCVBUF:name==15?SO_REUSEPORT:name==13?SO_LINGER:name==6?SO_BROADCAST:-1;
    } else if(level==0 && name==1) { *host_level=IPPROTO_IP;*host_name=IP_TOS;return 1; } /* IP_TOS */
    else if(level==6) { /* IPPROTO_TCP */
        *host_level=IPPROTO_TCP;
        *host_name=name==1?TCP_NODELAY:name==3?TCP_NOPUSH:name==4?TCP_KEEPALIVE:name==5?TCP_KEEPINTVL:name==6?TCP_KEEPCNT:-1; /* TCP_CORK: Darwin's TCP_NOPUSH */
    } else *host_name=-1;
    return *host_name>=0;
}
/* bind of an IPV6_V6ONLY socket. Servers pair one with an IPv4 listener on the same
   port (nginx's [::]:80, PostgreSQL, Redis), and both cannot be IPv4 underneath: it
   becomes a real IPv6 socket on the host's ::1, at a port of its own. getsockname
   reports the guest's address. ponytail: other runs and published ports reach the
   IPv4 listener only (clients' IPv6 is IPv4 underneath); a server listening on a
   V6ONLY socket alone is unreachable. */
static uint64_t net_bind_v6only(unsigned slot,int fd) {
    struct NetDescription *d=net_description(slot);
    if(d->v6_bound)return linux_error(22);
    if(cpu->gpr[2]<24)return linux_error(22);
    const unsigned char *q=lookup(cpu->gpr[1],24,4);if(!q)return linux_error(14);
    if(le(q,2)!=10)return linux_error(97);
    unsigned char v4[4];if(!net_v6_to_v4(q+8,v4) || q[23]>1)return linux_error(99); /* :: or ::1 only */
    int host=socket(AF_INET6,d->udp?SOCK_DGRAM:SOCK_STREAM,0),one=1,flags=fcntl(fd,F_GETFL);
    struct timeval timeout={5,0};
    struct sockaddr_in6 local;memset(&local,0,sizeof(local));
#ifdef __APPLE__
    local.sin6_len=sizeof(local);
#endif
    local.sin6_family=AF_INET6;local.sin6_addr=in6addr_loopback;
    if(host<0 || flags<0 || setsockopt(host,IPPROTO_IPV6,IPV6_V6ONLY,&one,sizeof(one)) || setsockopt(host,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))
       || setsockopt(host,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) || fcntl(host,F_SETFL,flags)<0
       || bind(host,(struct sockaddr *)&local,sizeof(local))<0 || dup2(host,fd)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
        uint64_t e=net_error();if(host>=0)close(host);return e;
    }
    close(host);
    memcpy(&d->v6_port,q+2,2);memcpy(d->v6_address,q+8,16);d->v6_bound=1;
    if(!d->v6_port) { socklen_t size=sizeof(local);if(!getsockname(fd,(struct sockaddr *)&local,&size))d->v6_port=local.sin6_port; }
    return 0;
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
        else if(cpu->gpr[2]==39)value=net_description(slot)->unix_socket?1:net_description(slot)->v6?10:2;
        for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(value>>(8*i));
        if(cpu->gpr[2]==17)net_credentials(out);
        for(unsigned i=0;i<4;i++)lenout[i]=(unsigned char)(want>>(8*i));
        return 0;
    }
    if((number==208 || number==209) && cpu->gpr[1]==41 && net_description(slot)->v6 && (cpu->gpr[2]==26 || cpu->gpr[2]==67)) {
        /* IPV6_V6ONLY (recorded; see net_bind_v6only) and IPV6_TCLASS (accepted, reads back 0). */
        struct NetDescription *d=net_description(slot);
        if(number==208) {
            const unsigned char *p=cpu->gpr[4]>=4?lookup(cpu->gpr[3],4,4):NULL;if(!p)return linux_error(cpu->gpr[4]<4?22:14);
            if(cpu->gpr[2]==26) { if(d->v6_bound)return linux_error(22);d->v6only=le(p,4)!=0; }
            return 0;
        }
        unsigned char *len=lookup(cpu->gpr[4],4,4),*lenout=lookup(cpu->gpr[4],4,2),*out=lookup(cpu->gpr[3],4,2);
        if(!len || !lenout || !out)return linux_error(14);if(le(len,4)<4)return linux_error(22);
        fs_pack(out,0,cpu->gpr[2]==26?d->v6only:0,4);fs_pack(lenout,0,4,4);return 0;
    }
    if((number==208 || number==209) && cpu->gpr[1]==1 && cpu->gpr[2]==12) {
        /* SO_PRIORITY has no Darwin counterpart: accepted, and reads back 0 (Erlang's
           inet driver saves it around TOS changes). */
        if(number==208)return cpu->gpr[4]<4?linux_error(22):lookup(cpu->gpr[3],4,4)?0:linux_error(14);
        unsigned char *len=lookup(cpu->gpr[4],4,4),*lenout=lookup(cpu->gpr[4],4,2),*out=lookup(cpu->gpr[3],4,2);
        if(!len || !lenout || !out)return linux_error(14);if(le(len,4)<4)return linux_error(22);
        fs_pack(out,0,0,4);fs_pack(lenout,0,4,4);return 0;
    }
    if(number==209 && !(cpu->gpr[1]==1 && cpu->gpr[2]==4)) {
        /* Options set through setsockopt read back from the host (Java reads its buffer sizes). */
        int host_level,host_name;if(!net_option(cpu->gpr[1],cpu->gpr[2],&host_level,&host_name))return linux_error(92);
        unsigned want=host_name==SO_LINGER?8:4;
        unsigned char *len=lookup(cpu->gpr[4],4,4),*lenout=lookup(cpu->gpr[4],4,2),*out=lookup(cpu->gpr[3],want,2);
        if(!len || !lenout || !out)return linux_error(14);if(le(len,4)<want)return linux_error(22);
        if(host_name==SO_LINGER) {
            struct linger value;socklen_t size=sizeof(value);if(getsockopt(fd,SOL_SOCKET,SO_LINGER,&value,&size))return net_error();
            fs_pack(out,0,(unsigned)value.l_onoff,4);fs_pack(out,4,(unsigned)value.l_linger,4);
        } else { int value=0;socklen_t size=sizeof(value);if(getsockopt(fd,host_level,host_name,&value,&size))return net_error();fs_pack(out,0,(unsigned)value,4); }
        fs_pack(lenout,0,want,4);return 0;
    }
    if(number==209 && net_scheduled) {
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
        if(!net_option(level,name,&host_level,&host_name))return linux_error(92);
        if(host_name==SO_LINGER) {
            if(cpu->gpr[4]!=8)return linux_error(22);
            const unsigned char *p=lookup(cpu->gpr[3],8,4);if(!p)return linux_error(14);
            struct linger value={(int)le(p,4)!=0,(int)le(p+4,4)};
            return setsockopt(fd,SOL_SOCKET,SO_LINGER,&value,sizeof(value))<0?net_error():0;
        }
        if(cpu->gpr[4]<4) return linux_error(22);
        const unsigned char *p=lookup(cpu->gpr[3],4,4);if(!p) return linux_error(14);
        int value=(int)le(p,4);if(host_name==SO_REUSEADDR || host_name==SO_KEEPALIVE || host_name==SO_REUSEPORT || host_name==TCP_NODELAY || host_name==TCP_NOPUSH || host_name==SO_BROADCAST)value=value!=0;
        return setsockopt(fd,host_level,host_name,&value,sizeof(value))<0?net_error():0;
    }
    if((number==200 || number==203) && net_description(slot)->unix_socket) {
        uint64_t result=net_unix_address(fd,cpu->gpr[1],cpu->gpr[2],number==200,net_description(slot)->path);
        if(!result)net_description(slot)->named=number==200?2:4;
        return result;
    }
    if(number==200 && net_description(slot)->v6only)return net_bind_v6only(slot,fd);
    if(number==200 || number==203) {
        net_binding_port=0;
        struct sockaddr_in address;uint64_t invalid=net_address(cpu->gpr[1],cpu->gpr[2],&address,number==200,(int)net_description(slot)->udp,(int)net_description(slot)->v6);
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
        if(!result && number==200 && net_ip && net_binding_port && !net_description(slot)->udp) {
            struct sockaddr_in bound;socklen_t size=sizeof(bound);
            if(!getsockname(fd,(struct sockaddr *)&bound,&size)) { net_private_record(net_binding_port,bound.sin_port);net_description(slot)->private_port=net_binding_port; }
        }
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
        if(net_description(slot)->unix_socket || net_description(slot)->v6_bound)return listen(fd,(int)backlog)<0?net_error():0;
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
        if(cpu->gpr[1]==8 || cpu->gpr[1]==9)return 0; /* F_SETOWN, F_GETOWN: accepted; no SIGIO is sent (see FIOASYNC) */
        return linux_error(95);
    }
    if(number==210) { if(cpu->gpr[1]>2)return linux_error(22);return shutdown(fd,(int)cpu->gpr[1])<0?net_error():0; }
    if(number==202 || number==242) {
        net_description(slot)->reads++;
        if(number==242 && (cpu->gpr[3]&~UINT64_C(526336)))return linux_error(22);
        struct sockaddr_storage storage;memset(&storage,0,sizeof(storage));socklen_t peer_size=sizeof(storage);
        int unix_socket=(int)net_description(slot)->unix_socket,v6=(int)net_description(slot)->v6;
        int host=accept(fd,cpu->gpr[1]?(struct sockaddr *)&storage:NULL,cpu->gpr[1]?&peer_size:NULL);
        if(host<0) {
            if(net_scheduled && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))return net_suspend_one(fd,POLLIN,net_description(slot)->read_timeout);
            net_wait.active=0;return net_error();
        }
        net_wait.active=0;
        /* Linux consumes the accepted connection even if copying its peer
           address fails. Never expose the Darwin sockaddr layout to guests. */
        if(cpu->gpr[1]) {
            unsigned char wire[28]={1,0};                                /* a Unix peer is unnamed */
            uint64_t invalid=net_put_wire(wire,unix_socket?2:net_wire(wire,&storage,v6),cpu->gpr[1],cpu->gpr[2]);
            if(invalid) { close(host);return invalid; }
        }
        /* Linux accept does not inherit O_NONBLOCK; enforce this on both hosts. */
        int flags=fcntl(host,F_GETFL);
        if(flags<0 || fcntl(host,F_SETFD,FD_CLOEXEC)<0 || fcntl(host,F_SETFL,(flags&~O_NONBLOCK)|((net_scheduled || (number==242 && (cpu->gpr[3]&2048)))?O_NONBLOCK:0))<0) {
            uint64_t e=net_error();close(host);return e;
        }
        return net_adopt(host,(number==242?(unsigned)cpu->gpr[3]:0)|(unix_socket?NET_UNIX:0)|(v6?NET_V6:0));
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
        struct NetDescription *d=net_description(slot);unsigned char wire[28]={10,0};
        if(number==204 && d->v6_bound) {                             /* the address the guest bound */
            memcpy(wire+2,&d->v6_port,2);memcpy(wire+8,d->v6_address,16);return net_put_wire(wire,28,cpu->gpr[1],cpu->gpr[2]);
        }
        struct sockaddr_storage address;socklen_t length=sizeof(address);
        int result=number==204?getsockname(fd,(struct sockaddr *)&address,&length):getpeername(fd,(struct sockaddr *)&address,&length);
        if(result<0) return net_error();
        return net_put_wire(wire,net_wire(wire,&address,(int)d->v6),cpu->gpr[1],cpu->gpr[2]);
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
        struct sockaddr_in to;struct sockaddr_storage from;socklen_t from_size=sizeof(from);int addressed=number>=206 && cpu->gpr[4] && udp,v6=(int)net_description(slot)->v6;
        if(addressed && number==206) { uint64_t invalid=net_address(cpu->gpr[4],cpu->gpr[5],&to,0,1,v6);if(invalid)return invalid; }
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
        if(result>=0 && addressed && receiving) { uint64_t invalid=net_put_address(&from,cpu->gpr[4],cpu->gpr[5],v6);if(invalid)return invalid; }
        if(result<0 && net_scheduled && !(flags&64) && !(net_description(slot)->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
            return net_suspend_one(fd,receiving?POLLIN:POLLOUT,receiving?net_description(slot)->read_timeout:net_description(slot)->write_timeout);
        net_wait.active=0;return result<0?net_error():(uint64_t)result;
    }
    return linux_error(38);
}

/* ppoll over sockets, pipes, stdio and files; no signal mask, at most 1024 entries.
   Sockets wait on the host; pipe changes wake the task through io_epoch. */
static uint64_t net_poll_wait(void) {
    if(cpu->gpr[1]>NET_FD_LIMIT)return linux_error(22);
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
        if(fds[i].fd<0 && guest>=0)fds[i].fd=io_poll_host((uint64_t)guest);
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
/* pselect6 as ppoll over the descriptors in its sets (Linux fd_set: 64 per word).
   The signal mask is ignored, as ppoll's is unsupported; the timeout is not updated. */
static uint64_t net_select_wait(void) {
    uint64_t nfds=cpu->gpr[0];if(nfds>NET_FD_LIMIT)return linux_error(22);
    unsigned bytes=(unsigned)((nfds+63)/64*8);
    /* The sets are read before they are rewritten (in and out are the same memory). */
    static unsigned char wanted[3][NET_FD_LIMIT/8];
    unsigned char *in[3]={0},*out[3]={0};
    for(unsigned s=0;s<3;s++)if(cpu->gpr[1+s] && bytes) {
        const unsigned char *p=lookup(cpu->gpr[1+s],bytes,4);out[s]=lookup(cpu->gpr[1+s],bytes,2);if(!p || !out[s])return linux_error(14);
        memcpy(wanted[s],p,bytes);in[s]=wanted[s];
    }
    unsigned timeout=NET_FOREVER;
    if(cpu->gpr[4]) {
        unsigned char *ts=lookup(cpu->gpr[4],16,4);if(!ts)return linux_error(14);
        uint64_t sec=le(ts,8),ns=le(ts+8,8);if(ns>=1000000000 || (int64_t)sec<0)return linux_error(22);
        uint64_t ms=sec>UINT32_MAX/1000?NET_FOREVER:sec*1000+(ns+999999)/1000000;
        timeout=ms>=NET_FOREVER?NET_FOREVER:(unsigned)ms;
    }
    static struct pollfd fds[NET_FD_LIMIT];static int other[NET_FD_LIMIT];static unsigned guest[NET_FD_LIMIT];
    unsigned count=0,ready_other=0;
    for(unsigned fd=0;fd<nfds;fd++) {
        unsigned bit=fd%8,byte=fd/8,events=0;
        for(unsigned s=0;s<3;s++)if(in[s] && (in[s][byte]>>bit&1))events|=s==0?1u:s==1?4u:2u;
        if(!events)continue;
        int host=net_host(fd);if(host<0)host=io_poll_host(fd);
        if(host<0 && !io_managed(fd) && fs_host(fd)<0)return linux_error(9);
        fds[count]=(struct pollfd){host,(short)(((events&1)?POLLIN:0)|((events&4)?POLLOUT:0)|((events&2)?POLLPRI:0)),0};
        other[count]=0;guest[count]=fd;
        if(host<0) { int r=io_managed(fd)?io_poll_ready(fd,events&5):(int)(events&5);if(r<0)r=0;other[count]=r;if(r)ready_other++; }
        count++;
    }
    int wait_ms=ready_other || net_scheduled?0:timeout==NET_FOREVER?-1:timeout>INT32_MAX?INT32_MAX:(int)timeout;
    int result=count?poll(fds,count,wait_ms):0;
    if(result==0 && !ready_other && timeout && net_scheduled) {
        uint64_t wait=net_suspend(fds,count,timeout);
        for(unsigned i=0;i<count;i++)if(fds[i].fd<0)net_wait.targeted=0;
        if(wait==IO_BLOCK)return wait;
    }
    net_wait.active=0;
    if(result<0)return net_error();
    for(unsigned s=0;s<3;s++)if(out[s])memset(out[s],0,bytes);
    uint64_t total=0;
    for(unsigned i=0;i<count;i++) {
        unsigned fd=guest[i],bit=fd%8,byte=fd/8;short e=fds[i].revents;
        unsigned value=fds[i].fd<0?(unsigned)other[i]:(((e&(POLLIN|POLLHUP|POLLERR))?1u:0)|((e&(POLLOUT|POLLERR))?4u:0)|((e&POLLPRI)?2u:0));
        if(fds[i].fd<0 && (value&(8|16)))value|=(value&8)?5u:1u;   /* a hung-up pipe reads end of file: readable, as on Linux */
        if((value&1) && out[0] && (in[0][byte]>>bit&1)) { out[0][byte]|=(unsigned char)(1u<<bit);total++; }
        if((value&4) && out[1] && (in[1][byte]>>bit&1)) { out[1][byte]|=(unsigned char)(1u<<bit);total++; }
        if((value&2) && out[2] && (in[2][byte]>>bit&1)) { out[2][byte]|=(unsigned char)(1u<<bit);total++; }
    }
    return total;
}
/* ppoll and pselect6 wait under the signal mask they are given, if any, and put the
   thread's own back when they return (signal_deliver does it, in the handler's
   frame, when a signal it lets through ends the wait with EINTR). */
static int signal_wait_mask(uint64_t mask);   /* signals.h: 1 if a signal is already deliverable */
static void signal_wait_unmask(void);
static uint64_t net_masked_wait(uint64_t (*wait)(void),uint64_t pointer) {
    if(pointer) {
        const unsigned char *set=lookup(pointer,8,4);if(!set)return linux_error(14);
        if(signal_wait_mask(le(set,8)))return linux_error(4);
    }
    uint64_t result=wait();
    if(pointer && result!=IO_BLOCK)signal_wait_unmask();
    return result;
}
static uint64_t net_poll(void) {
    if(cpu->gpr[3] && cpu->gpr[4]!=8)return linux_error(22);
    return net_masked_wait(net_poll_wait,cpu->gpr[3]);
}
static uint64_t net_select(void) {
    /* x5: {const sigset_t *mask, size_t size}, as glibc and musl pass it */
    uint64_t pointer=0;
    if(cpu->gpr[5]) {
        const unsigned char *pair=lookup(cpu->gpr[5],16,4);if(!pair)return linux_error(14);
        pointer=le(pair,8);if(pointer && le(pair+8,8)!=8)return linux_error(22);
    }
    return net_masked_wait(net_select_wait,pointer);
}
