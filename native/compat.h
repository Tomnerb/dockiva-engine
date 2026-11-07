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
        if(!pipe->readers && pipe->fifo && process_set_others(pipe->want_read,0))return io_would_block(&IO(guestfd));   /* its reader is opening */
        if(!pipe->readers)return linux_error(32);
        if(total<=4096 && total>PIPE_CAP-pipe->used)return io_would_block(&IO(guestfd));
    }
    if(file<0 && !managed) {
        if(network>=0)__atomic_fetch_add(&net_description((unsigned)net_slot(guestfd))->writes,1,__ATOMIC_RELAXED); /* also without the lock */
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
static void cap_after_setuid(const GuestIds *old,const GuestIds *now) {
    if((!old->ruid || !old->uid || !old->suid) && now->ruid && now->uid && now->suid && !prctl_keepcaps)cap_held[0]=cap_held[1]=0;
}
static int net_raw_allowed(void) { return !guest_uid && (cap_held[0]&(1u<<13)); } /* root with CAP_NET_RAW */
static char prctl_name[16];
/* PR_SET_MEM_MODEL / PR_GET_MEM_MODEL (the Apple/Asahi kernel ABI; FEX asks for TSO
   with it): model 1 runs the calling thread under the CPU's TSO memory model, kept
   across clone and fork, reset by exec. Offered only where the vCPUs take
   ACTLR_EL1.EnTSO (hvf_backend.h sets guest_tso); elsewhere EINVAL, as from a kernel
   without it, and translators order memory accesses themselves. */
static int guest_tso;
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
        case 0x4d4d444c:if(!guest_tso || arg>1)return linux_error(22);cpu->mem_model=(unsigned)arg;return 0; /* PR_SET_MEM_MODEL */
        case 0x6d4d444c:return guest_tso?cpu->mem_model:linux_error(22);                           /* PR_GET_MEM_MODEL */
    }
    return linux_error(22);
}

/* Linux abstract names (sun_path[0]==0; any bytes, and the address length is part of
   the name) are sockets in one host directory per container (--abstract-dir, shared
   with its execs; else a private temporary one), each named by a hash of the name.
   A name is freed when its last socket closes, as on Linux; one left by a run that
   ended without closing it is taken over when nothing answers on it.
   ponytail: a socket bound but not yet listening in another run counts as stale. */
static char net_abstract_dir[1024];
static int net_abstract_fd=-1,net_unix_home=-1;
static void net_abstract_cleanup(void) {                          /* the private directory */
    DIR *d=opendir(net_abstract_dir);struct dirent *e;
    if(d) { while((e=readdir(d)))if(e->d_name[0]=='a')unlinkat(dirfd(d),e->d_name,0);closedir(d); }
    rmdir(net_abstract_dir);
}
static int net_abstract_home(void) {
    if(net_abstract_fd>=0)return net_abstract_fd;
    if(!net_abstract_dir[0]) {
        strcpy(net_abstract_dir,"/tmp/dockiva-abstract.XXXXXX");
        if(!mkdtemp(net_abstract_dir)) { net_abstract_dir[0]=0;return -1; }
        atexit(net_abstract_cleanup);
    }
    return net_abstract_fd=open(net_abstract_dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
}
static void net_abstract_leaf(const struct NetDescription *d,char leaf[24]) {
    uint64_t h=UINT64_C(14695981039346656037)^d->abstract;       /* FNV-1a over the length and bytes */
    for(unsigned i=0;i<d->abstract;i++)h=(h^(unsigned char)d->path[i])*UINT64_C(1099511628211);
    snprintf(leaf,24,"a%016llx",(unsigned long long)h);
}
/* Called as the description's last reference goes: free the name if it is still this socket's. */
static void net_abstract_release(const struct NetDescription *d) {
    char leaf[24];struct stat st;net_abstract_leaf(d,leaf);
    if(net_abstract_fd>=0 && !fstatat(net_abstract_fd,leaf,&st,AT_SYMLINK_NOFOLLOW) && st.st_ino==d->abstract_ino)unlinkat(net_abstract_fd,leaf,0);
}
static uint64_t net_abstract_address(int fd,const unsigned char *name,unsigned length,int binding,struct NetDescription *d) {
    if(binding)for(unsigned i=0;i<net_descriptions_top;i++) {       /* this run's own names */
        const struct NetDescription *o=&net_descriptions[i];
        if(o->refs && o->named==2 && o->abstract==length && !memcmp(o->path,name,length))return linux_error(98);
    }
    int home=net_abstract_home();if(home<0)return net_error();
    struct NetDescription named=*d;memcpy(named.path,name,length);named.abstract=length;
    struct sockaddr_un host;memset(&host,0,sizeof(host));host.sun_family=AF_UNIX;net_abstract_leaf(&named,host.sun_path);
#ifdef __APPLE__
    host.sun_len=(unsigned char)SUN_LEN(&host);
#endif
    if(net_unix_home<0 && (net_unix_home=open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC))<0)return net_error();
    if(fchdir(home)<0)return net_error();
    int result=binding?bind(fd,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host)):connect(fd,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host));
    if(result<0 && binding && errno==EADDRINUSE) {                  /* left by an ended run? */
        int type=SOCK_STREAM;socklen_t size=sizeof(type);getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size);
        int probe=socket(AF_UNIX,type,0),alive=probe>=0 && !connect(probe,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host));
        int refused=!alive && probe>=0 && (errno==ECONNREFUSED || errno==ENOENT);
        if(probe>=0)close(probe);
        if(refused && !unlink(host.sun_path))result=bind(fd,(struct sockaddr *)&host,(socklen_t)SUN_LEN(&host));
        else errno=EADDRINUSE;
    }
    struct stat st;if(!result && binding && !stat(host.sun_path,&st))named.abstract_ino=st.st_ino;
    int saved=errno;
    if(fchdir(net_unix_home)<0)fail("Cannot restore the runtime directory");
    if(result<0) {
        if(!binding && (saved==ENOENT || saved==ECONNREFUSED))return linux_error(111);
        if(!binding && (saved==EINPROGRESS || saved==EAGAIN))return linux_error(11);   /* full backlog */
        errno=saved;return net_error();
    }
    memcpy(d->path,named.path,sizeof(d->path));d->abstract=length;d->abstract_ino=binding?named.abstract_ino:0;
    return 0;
}
/* AF_UNIX addresses are guest paths, resolved inside the root like any path (a
   final symlink is followed by connect, not by bind). macOS caps sun_path at 104
   bytes, so the host socket is bound or connected by its leaf name after a brief
   fchdir into the resolved parent; the syscall layer runs on one thread. A bind of
   the family alone (length 2) autobinds an abstract name of five hex digits. */
static int64_t mem_unix_address(int fd,const char *guest,int binding,int probe,struct NetDescription *d,char rewrite[1025]); /* memfs.h */
static uint64_t net_unix_host(int fd,int binding,struct NetDescription *d,int parent,const char *leaf,const char *canon,const char *guest);
static uint64_t net_unix_address(int fd,uint64_t address,uint64_t length,int binding,struct NetDescription *d) {
    if(length==2 && binding) {
        const unsigned char *p=lookup(address,2,4);if(!p)return linux_error(14);
        if(le(p,2)!=1)return linux_error(97);
        static unsigned next;if(!next)next=(unsigned)getpid()*2654435761u;
        for(unsigned i=0;i<0x100000;i++) {
            unsigned char name[7]={0};snprintf((char *)name+1,6,"%05x",(next++)&0xfffff);
            uint64_t result=net_abstract_address(fd,name,6,1,d);
            if(result!=linux_error(98))return result;
        }
        return linux_error(98);
    }
    if(length<3)return linux_error(22);
    if(length>110)length=110;                                     /* sizeof(struct sockaddr_un) */
    const unsigned char *p=lookup(address,length,4);if(!p)return linux_error(14);
    if(le(p,2)!=1)return linux_error(97);
    if(!p[2])return net_abstract_address(fd,p+2,(unsigned)length-2,binding,d);
    char guest[109];size_t n=0;
    while(n<length-2 && p[2+n]) {
        unsigned char c=p[2+n];
        if(c<32 || c==127)return linux_error(13);
        guest[n++]=(char)c;
    }
    guest[n]=0;
    if(n>107)return linux_error(36);
    if(net_unix_home<0 && (net_unix_home=open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC))<0)return net_error();
    for(int probe=0;;probe++) {                                   /* memfs (memfs.h): a socket node naming an abstract socket */
        char rewrite[1025];int64_t m=probe>1?MEM_HOST_PATH:mem_unix_address(fd,guest,binding,probe,d,rewrite);
        if(m==1 && strlen(rewrite)<sizeof(guest))strcpy(guest,rewrite);   /* a host path after all */
        else if(m!=MEM_HOST_PATH)return m<0?linux_error((unsigned)-m):0;
        int base=fs_cwd?fs_cwd->fd:fs_root;const char *base_path=fs_cwd?fs_cwd->path:"/";
        char leaf[1025],canon[1025];
        int parent=fs_walk(base,base_path,guest,!binding,leaf,canon);
        if(parent<0 && mem_redirect_take() && !probe)continue;        /* reached memfs through a link */
        if(parent<0)return fs_error();
        return net_unix_host(fd,binding,d,parent,leaf,canon,guest);
    }
}
static uint64_t net_unix_host(int fd,int binding,struct NetDescription *d,int parent,const char *leaf,const char *canon,const char *guest) {
    if(binding && !fs_path_writable()) { close(parent);return linux_error(30); }
    OvLeaf w=ov_leaf;char name[1025];snprintf(name,sizeof name,"%s",leaf);       /* image layers: made in the upper one (overlay.h) */
    if(binding && ov_prepare(&w,&parent,name,OV_CREATE)) { uint64_t e=errno==EEXIST?linux_error(98):fs_error();close(parent);return e; }
    leaf=name;
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
    if(binding) { fs_entries++;ov_created(&w,parent,leaf,0); }
    strcpy(d->path,canon[0] && strlen(canon)<108?canon:guest);d->abstract=0;
    return 0;
}
