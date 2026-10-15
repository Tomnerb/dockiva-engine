/* Bounded adapters needed by the M18 static BusyBox fixture. */
/* Guest CLOCK_MONOTONIC: Darwin's CLOCK_UPTIME_RAW excludes sleep like Linux (and matches the vDSO). */
#ifdef CLOCK_UPTIME_RAW
#define GUEST_MONOTONIC CLOCK_UPTIME_RAW
#else
#define GUEST_MONOTONIC CLOCK_MONOTONIC
#endif
#include <sys/uio.h>
static uint64_t guest_gettimeofday(void) {
    unsigned char *tv=cpu->gpr[0]?lookup(cpu->gpr[0],16,2):NULL,*tz=cpu->gpr[1]?lookup(cpu->gpr[1],8,2):NULL;
    if((cpu->gpr[0] && !tv) || (cpu->gpr[1] && !tz))return linux_error(14);
    struct timeval value;if(gettimeofday(&value,NULL)<0)return net_error();
    if(tv) { fs_pack(tv,0,(uint64_t)value.tv_sec,8);fs_pack(tv,8,(uint64_t)value.tv_usec,8); }
    if(tz)memset(tz,0,8); /* Guest kernel timezone is UTC; no host timezone forwarded. */
    return 0;
}
/* Short reads bound host work. Only the urandom mode and NONBLOCK are supported. */
static uint64_t guest_random(void) {
    if(cpu->gpr[2]&~UINT64_C(1))return linux_error(22);
    unsigned count=cpu->gpr[1]>256?256:(unsigned)cpu->gpr[1];if(!count)return 0;
    unsigned char *out=lookup(cpu->gpr[0],count,2);if(!out)return linux_error(14);
#if defined(__APPLE__)
    /* Darwin's initialized kernel CSPRNG provides up to 256 bytes per call. */
    unsigned char bytes[256];if(getentropy(bytes,count)<0)return net_error();
    memcpy(out,bytes,count);return count;
#elif defined(__linux__)
    ssize_t result=getrandom(out,count,(unsigned)cpu->gpr[2]);
    return result<0?net_error():(uint64_t)result;
#else
    return linux_error(38);
#endif
}
/* Explicit Linux clock IDs; never forward a guest-selected ID to Darwin. */
static uint64_t guest_clock(uint64_t call) {
    /* CPU-time clocks report the runtime process/thread, which runs the guest.
       Monotonic clocks exclude sleep, as on Linux (and as the vDSO computes);
       BOOTTIME includes it. */
    static const clockid_t clocks[]={CLOCK_REALTIME,GUEST_MONOTONIC,CLOCK_PROCESS_CPUTIME_ID,CLOCK_THREAD_CPUTIME_ID,
                                     GUEST_MONOTONIC,CLOCK_REALTIME,GUEST_MONOTONIC,CLOCK_MONOTONIC}; /* ..._COARSE, BOOTTIME */
    if(cpu->gpr[0]>=sizeof(clocks)/sizeof(clocks[0]))return linux_error(22);
    clockid_t clock=clocks[cpu->gpr[0]];
    unsigned char *out=NULL;
    if(cpu->gpr[1]) { out=lookup(cpu->gpr[1],16,2);if(!out)return linux_error(14); }
    else if(call==113)return linux_error(14);
    struct timespec value;
    int result=call==113?clock_gettime(clock,&value):clock_getres(clock,&value);
    if(result<0)return net_error();
    if(call==113 && cpu->gpr[0]==7) { uint64_t up=proc_boottime_ns();value.tv_sec=(time_t)(up/1000000000u);value.tv_nsec=(long)(up%1000000000u); } /* fs.h */
    if(out) { fs_pack(out,0,(uint64_t)value.tv_sec,8);fs_pack(out,8,(uint64_t)value.tv_nsec,8); }
    return 0;
}
/* getrusage: SELF and THREAD report the host runtime process (one per container);
   CHILDREN reports zero. Linux struct rusage is 144 bytes, ru_maxrss in KiB. */
static uint64_t guest_getrusage(void) {
    int64_t who=(int64_t)cpu->gpr[0];
    if(who!=0 && who!=-1 && who!=1)return linux_error(22);
    unsigned char *out=lookup(cpu->gpr[1],144,2);if(!out)return linux_error(14);
    memset(out,0,144);
    if(who==-1)return 0;
    struct rusage usage;if(getrusage(RUSAGE_SELF,&usage)<0)return net_error();
    fs_pack(out,0,(uint64_t)usage.ru_utime.tv_sec,8);fs_pack(out,8,(uint64_t)usage.ru_utime.tv_usec,8);
    fs_pack(out,16,(uint64_t)usage.ru_stime.tv_sec,8);fs_pack(out,24,(uint64_t)usage.ru_stime.tv_usec,8);
#if defined(__APPLE__)
    fs_pack(out,32,(uint64_t)usage.ru_maxrss/1024,8);
#else
    fs_pack(out,32,(uint64_t)usage.ru_maxrss,8);
#endif
    fs_pack(out,64,(uint64_t)usage.ru_minflt,8);fs_pack(out,72,(uint64_t)usage.ru_majflt,8);
    fs_pack(out,128,(uint64_t)usage.ru_nvcsw,8);fs_pack(out,136,(uint64_t)usage.ru_nivcsw,8);
    return 0;
}
/* nanosleep (101) and clock_nanosleep (115) as an empty ppoll, so a sleep
   suspends only this guest task under the scheduler. Sleeps are never
   interrupted, so the remaining-time output is left untouched. */
static uint64_t guest_sleep(uint64_t call) {
    uint64_t request=call==101?cpu->gpr[0]:cpu->gpr[2];
    unsigned char *ts=lookup(request,16,4);if(!ts)return linux_error(14);
    uint64_t sec=le(ts,8),ns=le(ts+8,8);if(ns>=1000000000 || (int64_t)sec<0)return linux_error(22);
    if(call==115) {
        if(cpu->gpr[0]>1)return linux_error(22);
        if(cpu->gpr[1]&~UINT64_C(1))return linux_error(22);
        if(cpu->gpr[1]&1) { /* TIMER_ABSTIME: convert to a relative sleep */
            struct timespec now;clock_gettime(cpu->gpr[0]?GUEST_MONOTONIC:CLOCK_REALTIME,&now);
            int64_t left=((int64_t)sec-now.tv_sec)*1000000000+((int64_t)ns-now.tv_nsec);
            if(left<=0 && !net_wait.active)return 0;
            if(left<0)left=0;
            fs_pack(ts,0,(uint64_t)(left/1000000000),8);fs_pack(ts,8,(uint64_t)(left%1000000000),8);
            /* ponytail: rewrites the guest's timespec in place; restored below. */
        }
    }
    uint64_t saved[4]={cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3]};
    cpu->gpr[0]=0;cpu->gpr[1]=0;cpu->gpr[2]=request;cpu->gpr[3]=0;
    uint64_t result=net_poll();
    cpu->gpr[0]=saved[0];cpu->gpr[1]=saved[1];cpu->gpr[2]=saved[2];cpu->gpr[3]=saved[3];
    if(call==115 && (cpu->gpr[1]&1)) { fs_pack(ts,0,sec,8);fs_pack(ts,8,ns,8); }
    return result==IO_BLOCK || (int64_t)result<0?result:0;
}
static uint64_t guest_writev(void) {
    uint64_t guestfd=cpu->gpr[0],count=cpu->gpr[2],total=0;
    int file=fs_host(guestfd),network=net_host(guestfd),managed=io_managed(guestfd);
    if((managed && !IO(guestfd).kind) || (!managed && file<0 && network<0))return linux_error(9);
    if(managed && (IO(guestfd).kind==IO_READ || IO(guestfd).kind==IO_EOF || IO(guestfd).kind==IO_INPUT))return linux_error(9);
    if(count>1024)return linux_error(22); /* UIO_MAXIOV */
    if(!count)return 0;
    unsigned char *table=lookup(cpu->gpr[1],count*16,4);if(!table)return linux_error(14);
    static _Thread_local struct iovec vec[1024];static _Thread_local uint64_t addresses[1024]; /* per worker */
    for(unsigned i=0;i<count;i++) {
        uint64_t addr=le(table+i*16,8),length=le(table+i*16+8,8);
        if(length>1024*1024-total) { length=1024*1024-total;count=i+1; } /* short transfer past 1 MiB */
        vec[i].iov_base=length?lookup(addr,length,4):(void *)"";
        if(!vec[i].iov_base)return linux_error(14);
        vec[i].iov_len=(size_t)length;addresses[i]=addr;total+=length;
    }
    if(managed && IO(guestfd).kind==IO_WRITE && total) {
        struct GuestPipe *pipe=IO(guestfd).pipe;
        if(!pipe->readers && pipe->fifo && pipe->want_read)return io_would_block(&IO(guestfd));   /* its reader is opening */
        if(!pipe->readers)return linux_error(32);
        if(total<=4096 && total>PIPE_CAP-pipe->used)return io_would_block(&IO(guestfd));
    }
    if(file<0 && !managed) {
        if(network>=0)net_description((unsigned)net_slot(guestfd))->writes++;
        ssize_t n=writev(network>=0?network:(int)guestfd,vec,(int)count);
        if(n<0 && net_scheduled && !(net_description((unsigned)net_slot(guestfd))->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
            return net_suspend_one(network,POLLOUT,net_description((unsigned)net_slot(guestfd))->write_timeout);
        if(n<0 && network>=0 && (errno==EAGAIN || errno==EWOULDBLOCK))return net_write_again();
        net_wait.active=0;return n<0?net_error():(uint64_t)n;
    }
    uint64_t old1=cpu->gpr[1],old2=cpu->gpr[2],written=0,result=0;
    for(unsigned i=0;i<count;i++) {
        cpu->gpr[1]=addresses[i];cpu->gpr[2]=vec[i].iov_len;
        uint64_t n=managed?io_call(64):fs_call(64);
        if((int64_t)n<0) { result=written?written:n;break; }
        written+=n;result=written;if(n<vec[i].iov_len)break;
    }
    cpu->gpr[1]=old1;cpu->gpr[2]=old2;return result;
}
/* Validate every destination before consuming stream bytes. Socket readv uses
   one host read, preserving nonblocking and partial-read behavior. Files and
   sockets are read without the runtime lock (guest_syscall_unlocked): because every
   destination is resolved, and made private, before anything is read, an attempt
   that must be redone under the lock has consumed nothing. */
static uint64_t guest_readv(void) {
    uint64_t guestfd=cpu->gpr[0],count=cpu->gpr[2],total=0;
    int file=fs_host(guestfd),network=net_host(guestfd),managed=io_managed(guestfd);
    if((managed && !IO(guestfd).kind) || (!managed && file<0 && network<0))return linux_error(9);
    if(managed && (IO(guestfd).kind==IO_WRITE || IO(guestfd).kind==IO_OUTPUT))return linux_error(9);
    if(count>1024)return linux_error(22); /* UIO_MAXIOV */if(!count)return 0;
    unsigned char *table=lookup(cpu->gpr[1],count*16,4);if(!table)return linux_error(14);
    static _Thread_local struct iovec vec[1024];static _Thread_local uint64_t addresses[1024]; /* per worker */
    for(unsigned i=0;i<count;i++) {
        uint64_t addr=le(table+i*16,8),length=le(table+i*16+8,8);
        if(length>1024*1024-total) { length=1024*1024-total;count=i+1; } /* short transfer past 1 MiB */
        vec[i].iov_base=length?lookup(addr,length,2):(void *)"";
        if(!vec[i].iov_base)return linux_error(14);
        vec[i].iov_len=(size_t)length;addresses[i]=addr;total+=length;
    }
    if(network>=0) {
        net_description((unsigned)net_slot(guestfd))->reads++;
        ssize_t n=readv(network,vec,(int)count);
        if(n<0 && net_scheduled && !(net_description((unsigned)net_slot(guestfd))->flags&2048) && (errno==EAGAIN || errno==EWOULDBLOCK))
            return net_suspend_one(network,POLLIN,net_description((unsigned)net_slot(guestfd))->read_timeout);
        net_wait.active=0;return n<0?net_error():(uint64_t)n;
    }
    uint64_t old1=cpu->gpr[1],old2=cpu->gpr[2],received=0,result=0;
    for(unsigned i=0;i<count;i++) {
        cpu->gpr[1]=addresses[i];cpu->gpr[2]=vec[i].iov_len;
        uint64_t n=managed?io_call(63):fs_call(63);
        if((int64_t)n<0) { result=received?received:n;break; }
        received+=n;result=received;if(n<vec[i].iov_len)break;
    }
    cpu->gpr[1]=old1;cpu->gpr[2]=old2;return result;
}

/* prctl: the options common programs use for privilege dropping and naming.
   Capabilities are not modelled beyond "root holds all"; PDEATHSIG is recorded
   but no signal is sent when a parent exits. Unknown options return EINVAL. */
static unsigned prctl_keepcaps,prctl_no_new_privs,prctl_dumpable=1,prctl_pdeathsig;
/* Permitted capabilities (0..40). Root starts with all; a switch away from root
   keeps them only with PR_SET_KEEPCAPS, and a non-root exec clears them. */
static uint32_t cap_held[2]={UINT32_MAX,0x1ffu};
static void cap_after_setuid(uint32_t old_uid,uint32_t new_uid) {
    if(!old_uid && new_uid && !prctl_keepcaps)cap_held[0]=cap_held[1]=0;
}
static char prctl_name[16];
static uint64_t seccomp_install(uint64_t flags,uint64_t address); /* seccomp.h */
static int seccomp_active(void);
static uint64_t guest_prctl(void) {
    uint64_t option=cpu->gpr[0],arg=cpu->gpr[1];
    switch(option) {
        case 1:if(arg>64)return linux_error(22);prctl_pdeathsig=(unsigned)arg;return 0;              /* PR_SET_PDEATHSIG */
        case 2: { unsigned char *out=lookup(arg,4,2);if(!out)return linux_error(14);fs_pack(out,0,prctl_pdeathsig,4);return 0; }
        case 3:return prctl_dumpable;                                                              /* PR_GET_DUMPABLE */
        case 4:if(arg>1)return linux_error(22);prctl_dumpable=(unsigned)arg;return 0;
        case 7:return prctl_keepcaps;                                                              /* PR_GET_KEEPCAPS */
        case 8:if(arg>1)return linux_error(22);prctl_keepcaps=(unsigned)arg;return 0;
        case 15: {                                                                                 /* PR_SET_NAME */
            for(unsigned i=0;i<15;i++) { unsigned char *c=lookup(arg+i,1,4);if(!c)return linux_error(14);prctl_name[i]=(char)*c;if(!*c)break; }
            prctl_name[15]=0;return 0;
        }
        case 16: { unsigned char *out=lookup(arg,16,2);if(!out)return linux_error(14);memcpy(out,prctl_name,16);return 0; }
        case 23:return arg<=40?1:linux_error(22);                                                  /* PR_CAPBSET_READ: full bounding set */
        case 38:if(arg!=1 || cpu->gpr[2] || cpu->gpr[3] || cpu->gpr[4])return linux_error(22);prctl_no_new_privs=1;return 0;
        case 39:return prctl_no_new_privs;
        case 21:return seccomp_active()?2:0;                                                       /* PR_GET_SECCOMP */
        case 22:return arg==2?seccomp_install(0,cpu->gpr[2]):linux_error(22);                     /* PR_SET_SECCOMP: filters only */
    }
    return linux_error(22);
}

/* AF_UNIX addresses are guest paths, resolved inside the root like any path (a
   final symlink is followed by connect, not by bind). macOS caps sun_path at 104
   bytes, so the host socket is bound or connected by its leaf name after a brief
   fchdir into the resolved parent; the syscall layer runs on one thread. Linux
   abstract names (a leading NUL) have no macOS equivalent and are unsupported. */
static int net_unix_home=-1;
static uint64_t net_unix_address(int fd,uint64_t address,uint64_t length,int binding,char path[108]) {
    if(length<3)return linux_error(22);
    if(length>110)length=110;                                     /* sizeof(struct sockaddr_un) */
    const unsigned char *p=lookup(address,length,4);if(!p)return linux_error(14);
    if(le(p,2)!=1)return linux_error(97);
    char guest[109];size_t n=0;
    while(n<length-2 && p[2+n]) {
        unsigned char c=p[2+n];
        if(c<32 || c==127)return linux_error(13);
        guest[n++]=(char)c;
    }
    guest[n]=0;
    if(!n)return linux_error(p[2]==0 && length>3?95:22);          /* abstract or empty name */
    if(n>107)return linux_error(36);
    if(net_unix_home<0 && (net_unix_home=open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC))<0)return net_error();
    int base=fs_cwd?fs_cwd->fd:fs_root;const char *base_path=fs_cwd?fs_cwd->path:"/";
    char leaf[1025],canon[1025];
    int parent=fs_walk(base,base_path,guest,!binding,leaf,canon);if(parent<0)return fs_error();
    if(binding && !fs_path_writable()) { close(parent);return linux_error(30); }
    if(!strcmp(leaf,".") || strlen(leaf)>=sizeof(((struct sockaddr_un *)0)->sun_path)) { close(parent);return linux_error(binding?22:111); }
    struct sockaddr_un host;memset(&host,0,sizeof(host));host.sun_family=AF_UNIX;strcpy(host.sun_path,leaf);
#ifdef __APPLE__
    host.sun_len=(unsigned char)SUN_LEN(&host);
#endif
    if(fchdir(parent)<0) { int saved=errno;close(parent);errno=saved;return fs_error(); }
    int result=binding?bind(fd,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host)):connect(fd,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host));
    int saved=errno;
    if(fchdir(net_unix_home)<0)fail("Cannot restore the runtime directory");
    close(parent);
    if(result<0) {
        errno=saved;
        if(!binding && (saved==EINPROGRESS || saved==EAGAIN))return linux_error(11);   /* full backlog */
        return fs_error();
    }
    if(binding)fs_entries++;
    strcpy(path,canon[0] && strlen(canon)<108?canon:guest);
    return 0;
}
