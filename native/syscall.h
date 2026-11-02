/* kill(pid, signal) for a positive guest pid, without disturbing the guest's registers. */
static uint64_t guest_signal_process(uint64_t pid,uint64_t signal) {
    uint64_t saved0=cpu->gpr[0],saved1=cpu->gpr[1];
    cpu->gpr[0]=pid;cpu->gpr[1]=signal;uint64_t result=process_kill();
    cpu->gpr[0]=saved0;cpu->gpr[1]=saved1;return result;
}
/* /dev/fd/N, /proc/self/fd/N (or this process's pid) and /dev/std{in,out,err}
   name an open descriptor; opening one duplicates it (bash process substitution
   passes pipes this way). Returns N, or -1 for any other path. */
static int64_t guest_fd_path(const char *path) {
    const char *p=NULL;
    if(!strcmp(path,"/dev/stdin"))return 0;
    if(!strcmp(path,"/dev/stdout"))return 1;
    if(!strcmp(path,"/dev/stderr"))return 2;
    if(!strncmp(path,"/dev/fd/",8))p=path+8;
    else if(!strncmp(path,"/proc/self/fd/",14))p=path+14;
    else if(!strncmp(path,"/proc/",6)) {
        uint64_t pid=0;const char *q=path+6;while(*q>='0' && *q<='9' && pid<100000)pid=pid*10+(uint64_t)(*q++-'0');
        if(q>path+6 && pid==process_pid && !strncmp(q,"/fd/",4))p=q+4;
    }
    if(!p || !*p)return -1;
    int64_t n=0;for(;*p;p++) { if(*p<'0' || *p>'9' || n>100000)return -1;n=n*10+(*p-'0'); }
    return n;
}
static uint64_t guest_ppid_of(uint64_t pid) { return pid>1?process_children[pid-2].parent:0; }
static uint64_t guest_ppid(void) { return guest_ppid_of(process_pid); }
/* For /proc: a process's live threads, its state (R runnable or running, S blocked,
   T stopped), its user and group, and its threads with their own state and CPU time. */
static unsigned guest_threads_of(uint64_t pid) {
    const ThreadTable *table=schedule_active?schedule_tasks[pid-1].threads:NULL;if(!table)return 1;
    unsigned live=0;for(unsigned i=0;i<table->top;i++)live+=table->thread[i].state!=0;
    return live?live:1;
}
/* A process's group (session 0) or session (1). */
static uint64_t guest_group_of(uint64_t pid,int session) {
    if(!schedule_active)return pid;
    ScheduledTask *t=&schedule_tasks[pid-1];return session?t->sid:t->pgid;
}
static char guest_state_of(uint64_t pid) {
    if(!schedule_active)return 'R';
    const ScheduledTask *t=&schedule_tasks[pid-1];
    if(t->state==3)return 'T';
    if(t->threads) { for(unsigned i=0;i<t->threads->top;i++)if(t->threads->thread[i].state==1)return 'R';return 'S'; }
    return t->state==2?'S':'R';
}
/* setgid 144, setuid 146, setregid 143, setreuid 145, setresuid 147, setresgid 149,
   as Linux: with CAP_SETUID/CAP_SETGID (guest root) any ID; otherwise each new ID
   must be the current real, effective or saved one. setuid/setgid set all three
   when privileged, else the effective one; setre* also set the saved ID to the new
   effective one when the real one is set or the effective one changes from the real. */
static void cap_after_setuid(const GuestIds *old,const GuestIds *now);
static uint64_t guest_setid(uint64_t call) {
    int user=call==145 || call==146 || call==147;
    GuestIds next=guest_ids;uint32_t *r=user?&next.ruid:&next.rgid,*e=user?&next.uid:&next.gid,*s=user?&next.suid:&next.sgid;
    uint32_t old[3]={*r,*e,*s},v[3]={(uint32_t)cpu->gpr[0],(uint32_t)cpu->gpr[1],(uint32_t)cpu->gpr[2]};
    int privileged=!guest_uid || (cap_held[0]&(user?(1u<<7):(1u<<6)));
    unsigned n=call==144 || call==146?1:call==143 || call==145?2:3;
    for(unsigned i=0;i<n;i++) {
        if(v[i]==UINT32_MAX && n>1)continue;                                   /* -1 leaves a field unchanged */
        if(!privileged && v[i]!=old[0] && v[i]!=old[1] && v[i]!=old[2])return linux_error(1);
        if(n==1 && !privileged && v[0]!=old[0] && v[0]!=old[2])return linux_error(1);           /* set[ug]id: the real or saved ID */
        if(n==2 && i==0 && !privileged && v[0]!=old[0] && v[0]!=old[1])return linux_error(1); /* setre*: real from real or effective */
    }
    if(n==1) { if(privileged)*r=*s=v[0];*e=v[0]; }
    else if(n==2) {
        if(v[0]!=UINT32_MAX)*r=v[0];
        if(v[1]!=UINT32_MAX)*e=v[1];
        if(v[0]!=UINT32_MAX || (v[1]!=UINT32_MAX && v[1]!=old[0]))*s=*e;
    } else { if(v[0]!=UINT32_MAX)*r=v[0];if(v[1]!=UINT32_MAX)*e=v[1];if(v[2]!=UINT32_MAX)*s=v[2]; }
    if(user)cap_after_setuid(&guest_ids,&next);
    guest_ids=next;return 0;
}
static void guest_ids_of(uint64_t pid,uint32_t uid[3],uint32_t gid[3]) {
    const ProcShared *ps=schedule_active && pid && pid<=PROCESS_LIMIT+1?&schedule_tasks[pid-1].ps:proc_now;
    uid[0]=ps->ids.ruid;uid[1]=ps->ids.uid;uid[2]=ps->ids.suid;gid[0]=ps->ids.rgid;gid[1]=ps->ids.gid;gid[2]=ps->ids.sgid;
}
/* A process's memory for /proc. Another process's is read under its mm lock while a
   worker runs it; own_held: the caller holds the current process's. */
static void proc_memory(uint64_t pid,int own_held,uint64_t *vsz,uint64_t *rss) {
    if(!schedule_active || !pid || pid>PROCESS_LIMIT+1) { if(pid==process_pid)proc_space_memory(address_space,vsz,rss);else *vsz=*rss=0;return; }
    ScheduledTask *t=&schedule_tasks[pid-1];
    pthread_mutex_t *mm=mm_of(t);                                       /* a vfork child's: its owner's */
    int lock=!(own_held && (pid==process_pid || mm==mm_held)) && (t->worker || t->running);
    if(lock)pthread_mutex_lock(mm);
    proc_space_memory(t->ps.shared?&t->ps.shared->state.memory:&t->image.memory,vsz,rss);
    if(lock)pthread_mutex_unlock(mm);
}
static unsigned guest_thread_list(uint64_t pid,unsigned *tids,char *states,uint64_t (*cpu)[2],unsigned capacity) {
    const ThreadTable *table=schedule_active && capacity?schedule_tasks[pid-1].threads:NULL;
    char state=guest_state_of(pid);
    if(!table) { tids[0]=(unsigned)pid;states[0]=state;proc_cpu_of(pid,cpu[0]);return 1; }
    unsigned n=0;
    for(unsigned i=0;i<table->top && n<capacity;i++) {
        const Thread *th=&table->thread[i];if(!th->state)continue;
        tids[n]=th->tid==pid?(unsigned)pid:th->tid;states[n]=state=='T'?'T':th->state==1?'R':'S';
        cpu[n][0]=th->cpu_ns[0];cpu[n][1]=th->cpu_ns[1];proc_cpu_live(pid,th->cpu_ns,cpu[n]);
        if(th->tid==pid && n) { unsigned t=tids[0];tids[0]=tids[n];tids[n]=t;char c=states[0];states[0]=states[n];states[n]=c;  /* the main thread first */
            uint64_t a=cpu[0][0],b=cpu[0][1];cpu[0][0]=cpu[n][0];cpu[0][1]=cpu[n][1];cpu[n][0]=a;cpu[n][1]=b; }
        n++;
    }
    return n;
}
static void guest_signal_sets(uint64_t pid,uint64_t sets[4]) {
    if(!schedule_active || !pid || pid>PROCESS_LIMIT+1)return;
    ScheduledTask *t=&schedule_tasks[pid-1];
    sets[0]=t->signal_pending;sets[1]=pid==process_pid?*signal_mask_now():t->signal_mask;sets[2]=t->signal_ignored;
    for(unsigned i=0;i<64;i++)if(t->signal_handler[i] && !(t->signal_ignored&(UINT64_C(1)<<i)))sets[3]|=UINT64_C(1)<<i;
}
static int guest_process_exists(uint64_t pid) { return pid==process_pid || (schedule_active && pid && pid<=PROCESS_LIMIT+1 && schedule_tasks[pid-1].state); }
static uint64_t io_signals_pending(void) { return schedule_active?schedule_tasks[process_pid-1].signal_pending:0; }
static unsigned io_signal_take(unsigned signal) {
    ScheduledTask *t=&schedule_tasks[process_pid-1];t->signal_pending&=~(UINT64_C(1)<<(signal-1));return t->signal_sender[signal-1];
}
static void fd_mirror(uint64_t fd) {
    if(!fd_mirror_base || fd>=GUEST_FD_LIMIT || !process_pid || process_pid>PROCESS_LIMIT+1)return;
    unsigned slot=fd_index[fd],m=0;
    if(fd_kind[fd]==FD_FILE && fs_fd[slot]) {
        int flags=fcntl(fs_fd[slot]-1,F_GETFL);
        if(flags>=0)m=FD_MIRROR_FILE|(unsigned)(flags&O_ACCMODE)|((flags&O_APPEND)?4u:0)|(fs_cloexec[slot]?FD_MIRROR_CLOEXEC:0);
    } else if(fd_kind[fd]==FD_IO && io_fds[slot].kind==IO_EPOLL)m=FD_MIRROR_EPOLL;
    __atomic_store_n(&fd_mirror_base[process_pid*4096+fd],(unsigned char)m,__ATOMIC_RELEASE);
}
/* close() for any kind of descriptor. */
static uint64_t guest_close(uint64_t fd) {
    int slot=fd<GUEST_FD_LIMIT?(int)fd_index[fd]:-1;
    if(slot<0 || !fd_kind[fd])return linux_error(9);
    if(fd_kind[fd]==FD_NET)return net_release((unsigned)slot)<0?net_error():0;
    if(fd_kind[fd]==FD_FILE) {
        if(fs_dirs[slot]) { fs_directory_release(fs_dirs[slot]);fs_dirs[slot]=NULL; }
        int host=fs_fd[slot]-1;fs_fd[slot]=0;fs_cloexec[slot]=0;fd_unbind(fd);return close(host)<0?fs_error():0;
    }
    io_release(&io_fds[slot]);fd_unbind(fd);return 0;
}
/* dup, dup2/dup3 and F_DUPFD: target (or, when it is -1, the lowest free number
   >= min) shares oldfd's open description, whatever its kind; dup2 closes the old
   target silently. */
static uint64_t guest_dup(uint64_t oldfd,int64_t target,uint64_t min,int cloexec) {
    if(oldfd>=GUEST_FD_LIMIT || !fd_kind[oldfd])return linux_error(9);
    unsigned kind=fd_kind[oldfd],from=fd_index[oldfd];int slot=-1;
    if(target>=0 && (uint64_t)target==oldfd)return (uint64_t)target;
    if(kind==FD_NET) { for(unsigned i=0;i<NET_SLOTS && slot<0;i++)if(!net_fd[i])slot=(int)i; }
    else if(kind==FD_FILE) { for(unsigned i=0;i<FS_SLOTS && slot<0;i++)if(!fs_fd[i])slot=(int)i; }
    else for(unsigned i=3;i<IO_END && slot<0;i++)if(!io_fds[i].kind)slot=(int)i;
    int64_t fd=target>=0?target:fd_lowest(min);
    if(slot<0 || fd<0)return linux_error(24);
    if(kind==FD_FILE) {
        int host=dup(fs_fd[from]-1);if(host<0)return fs_error();
        if(target>=0 && fd_kind[fd])(void)guest_close((uint64_t)fd);
        fs_fd[slot]=host+1;fs_cloexec[slot]=(unsigned char)(cloexec!=0);fs_ro[slot]=fs_ro[from];fs_dev[slot]=fs_dev[from];fs_dirs[slot]=fs_dirs[from];
        if(fs_dirs[slot])fs_dirs[slot]->refs++;
        fs_guest[slot]=(unsigned short)fd;
    } else if(kind==FD_NET) {
        if(target>=0 && fd_kind[fd])(void)guest_close((uint64_t)fd);
        net_fd[slot]=net_fd[from];net_description((unsigned)slot)->refs++;net_flags[slot]=cloexec?524288:0;
        net_guest[slot]=(unsigned short)fd;
    } else {
        GuestIO copy;if(io_copy(&copy,&io_fds[from])<0)return linux_error(24);
        if(target>=0 && fd_kind[fd])(void)guest_close((uint64_t)fd);
        copy.cloexec=(unsigned)(cloexec!=0);io_fds[slot]=copy;io_guest[slot]=(unsigned short)fd;
    }
    fd_bind((uint64_t)fd,kind,(unsigned)slot);return (uint64_t)fd;
}
/* SCM_RIGHTS: a descriptor in flight between processes of the container. The sender
   takes a reference (as dup does) into an entry; a host pipe holding the entry's
   number is what travels in the host message, so ordering and message boundaries
   are the host socket's. The receiver installs the reference as its own descriptor.
   ponytail: an entry whose message is never received (its socket closes unread)
   keeps its reference; free entries on socket close if that matters. */
#define FD_CARRIES 256
static struct FdCarry { unsigned kind,used;int host;unsigned char ro,dev;GuestDirectory *dir;GuestIO io; } fd_carries[FD_CARRIES];
static void fd_carry_drop(struct FdCarry *c) {
    if(c->kind==FD_FILE) { close(c->host);fs_directory_release(c->dir); }
    else if(c->kind==FD_NET) { struct NetDescription *d=net_host_description(c->host-1);if(d)(void)net_description_release(d); }
    else io_release(&c->io);
    memset(c,0,sizeof(*c));
}
/* A carrier host descriptor for guestfd, or -errno. */
static int fd_carry_send(uint64_t guestfd) {
    if(guestfd>=GUEST_FD_LIMIT || !fd_kind[guestfd])return -9;
    unsigned i=0;while(i<FD_CARRIES && fd_carries[i].used)i++;
    if(i==FD_CARRIES)return -109;                                                  /* ETOOMANYREFS */
    struct FdCarry *c=&fd_carries[i];unsigned from=fd_index[guestfd];
    *c=(struct FdCarry){0};c->kind=fd_kind[guestfd];
    if(c->kind==FD_FILE) {
        c->host=dup(fs_fd[from]-1);if(c->host<0) { c->kind=0;return -24; }
        c->ro=fs_ro[from];c->dev=fs_dev[from];c->dir=fs_dirs[from];if(c->dir)c->dir->refs++;
    } else if(c->kind==FD_NET) { c->host=net_fd[from];net_description(from)->refs++; }
    else if(io_copy(&c->io,&io_fds[from])<0) { c->kind=0;return -24; }
    int ends[2];
    if(pipe(ends)<0) { fd_carry_drop(c);return -24; }
    ssize_t wrote=write(ends[1],&i,sizeof(i));close(ends[1]);fcntl(ends[0],F_SETFD,FD_CLOEXEC);
    if(wrote!=(ssize_t)sizeof(i)) { close(ends[0]);fd_carry_drop(c);return -24; }
    c->used=1;return ends[0];
}
static struct FdCarry *fd_carry_take(int carrier) {
    unsigned i=FD_CARRIES;ssize_t got=read(carrier,&i,sizeof(i));close(carrier);
    return got==(ssize_t)sizeof(i) && i<FD_CARRIES && fd_carries[i].used?&fd_carries[i]:NULL;
}
/* A message that was not sent, or a descriptor the receiver cannot take. */
static void fd_carry_cancel(int carrier) { struct FdCarry *c=fd_carry_take(carrier);if(c)fd_carry_drop(c); }
/* Install a received descriptor: its new number, or -1 (no room: dropped). */
static int64_t fd_carry_receive(int carrier,int cloexec) {
    struct FdCarry *c=fd_carry_take(carrier);if(!c)return -1;
    int slot=-1,fd=fd_lowest(0);
    if(c->kind==FD_NET) { for(unsigned i=0;i<NET_SLOTS && slot<0;i++)if(!net_fd[i])slot=(int)i; }
    else if(c->kind==FD_FILE) { for(unsigned i=0;i<FS_SLOTS && slot<0;i++)if(!fs_fd[i])slot=(int)i; }
    else for(unsigned i=3;i<IO_END && slot<0;i++)if(!io_fds[i].kind)slot=(int)i;
    if(slot<0 || fd<0) { fd_carry_drop(c);return -1; }
    if(c->kind==FD_FILE) { fs_fd[slot]=c->host+1;fs_cloexec[slot]=(unsigned char)(cloexec!=0);fs_ro[slot]=c->ro;fs_dev[slot]=c->dev;fs_dirs[slot]=c->dir;fs_guest[slot]=(unsigned short)fd; }
    else if(c->kind==FD_NET) { net_fd[slot]=c->host;net_flags[slot]=cloexec?524288:0;net_guest[slot]=(unsigned short)fd; }
    else { c->io.cloexec=(unsigned)(cloexec!=0);io_fds[slot]=c->io;io_guest[slot]=(unsigned short)fd; }
    fd_bind((uint64_t)fd,c->kind,(unsigned)slot);memset(c,0,sizeof(*c));
    return fd;
}
/* sendfile(out, in, offset, count) from a file to a socket (nginx serves static
   files this way), through Darwin's sendfile, or to a file (fs_copy_between). Other
   destinations: EINVAL, on which callers (busybox, coreutils) copy with read and write. */
#ifdef __APPLE__
struct sf_hdtr;
int sendfile(int,int,off_t,off_t *,struct sf_hdtr *,int); /* <sys/socket.h> hides it from strict C11 */
#endif
static uint64_t guest_sendfile(void) {
#ifndef __APPLE__
    return linux_error(22);
#else
    int in=fs_host(cpu->gpr[1]),out=net_host(cpu->gpr[0]);
    if(in>=0 && fs_host(cpu->gpr[0])>=0)return fs_copy_between(cpu->gpr[1],cpu->gpr[0],cpu->gpr[2],0,cpu->gpr[3]);
    if(in<0 || (out<0 && !fd_kind[cpu->gpr[0]<GUEST_FD_LIMIT?cpu->gpr[0]:0]))return linux_error(9);
    if(out<0 || fs_dirs[fs_slot(cpu->gpr[1])])return linux_error(22);
    unsigned slot=(unsigned)net_slot(cpu->gpr[0]);
    unsigned char *at=cpu->gpr[2]?lookup(cpu->gpr[2],8,6):NULL;if(cpu->gpr[2] && !at)return linux_error(14);
    off_t offset=at?(off_t)le(at,8):lseek(in,0,SEEK_CUR);if(offset<0)return at?linux_error(22):fs_error();
    uint64_t count=cpu->gpr[3]>0x7ffff000?0x7ffff000:cpu->gpr[3];
    if(!count)return 0;                                         /* Darwin's 0 means "to the end" */
    off_t sent=(off_t)count;__atomic_fetch_add(&net_description(slot)->writes,1,__ATOMIC_RELAXED); /* also without the lock */
    if(sendfile(in,out,offset,&sent,NULL,0)<0 && !(errno==EAGAIN && sent>0)) {
        if(errno==EAGAIN && net_scheduled && !(net_description(slot)->flags&2048))
            return net_suspend_one(out,POLLOUT,net_description(slot)->write_timeout);
        return net_error();
    }
    net_wait.active=0;
    if(at)fs_pack(at,0,(uint64_t)(offset+sent),8);else lseek(in,offset+sent,SEEK_SET);
    return (uint64_t)sent;
#endif
}
static void net_credentials(unsigned char out[12]) {
    for(unsigned i=0;i<4;i++) { out[i]=(unsigned char)(process_pid>>(8*i));out[4+i]=(unsigned char)(guest_uid>>(8*i));out[8+i]=(unsigned char)(guest_gid>>(8*i)); }
}
/* Shared Linux syscall boundary: -3 continues, -2 blocks, -1 yields, >=0 exits. */
/* Resource limits, kept for getrlimit/setrlimit/prlimit64 (one table: the guests
   of a container share it). Docker-like values; descriptors are capped at the real
   per-process table (GUEST_FD_LIMIT) and the stack at its fixed size. */
#define RLIM_INF UINT64_MAX
static uint64_t guest_rlimits[16][2]={{RLIM_INF,RLIM_INF},{RLIM_INF,RLIM_INF},{RLIM_INF,RLIM_INF},{STACK_SIZE,RLIM_INF},
    {0,RLIM_INF},{RLIM_INF,RLIM_INF},{RLIM_INF,RLIM_INF},{GUEST_FD_LIMIT,GUEST_FD_LIMIT},{8<<20,8<<20},{RLIM_INF,RLIM_INF},
    {RLIM_INF,RLIM_INF},{63413,63413},{819200,819200},{0,0},{0,0},{RLIM_INF,RLIM_INF}};
static uint64_t guest_rlimit(uint64_t pid,uint64_t resource,uint64_t in,uint64_t out) {
    if(pid && pid!=process_pid && (pid>PROCESS_LIMIT+1 || !schedule_active || !schedule_tasks[pid-1].state))return linux_error(3);
    if(resource>=16)return linux_error(22);
    unsigned char *old=out?lookup(out,16,2):NULL,*next=in?lookup(in,16,4):NULL;
    if((out && !old) || (in && !next))return linux_error(14);
    uint64_t soft=next?le(next,8):0,hard=next?le(next+8,8):0;
    if(next) {
        if(soft>hard)return linux_error(22);
        if(hard>guest_rlimits[resource][1] && guest_uid)return linux_error(1);      /* only root raises a hard limit */
        if(resource==7 && hard>GUEST_FD_LIMIT)return linux_error(1);               /* the real descriptor table */
        if(resource==3 && soft>STACK_SIZE && soft!=RLIM_INF)soft=STACK_SIZE;        /* the stack is fixed */
    }
    if(old) { fs_pack(old,0,guest_rlimits[resource][0],8);fs_pack(old,8,guest_rlimits[resource][1],8); }
    if(next) { guest_rlimits[resource][0]=soft;guest_rlimits[resource][1]=hard; }
    return 0;
}
/* A process ID, or a thread ID of the calling process, that exists (0: the caller). */
static int guest_id_exists(uint64_t id) {
    if(!id || id==process_pid)return 1;
    if(!schedule_active)return 0;
    if(id<=PROCESS_LIMIT+1 && schedule_tasks[id-1].state)return 1;
    ThreadTable *t=schedule_tasks[process_pid-1].threads;
    if(t)for(unsigned i=0;i<t->top;i++)if(t->thread[i].state && t->thread[i].tid==id)return 1;
    return 0;
}
/* Rewrites a Linux struct stat (128 bytes, as newfstatat wrote it into out) as a struct statx. */
static void statx_from_stat(unsigned char *out) {
    unsigned char st[128];memcpy(st,out,128);memset(out,0,256);
    fs_pack(out,0,0x7ff,4);fs_pack(out,4,le(st+56,4),4);                     /* STATX_BASIC_STATS; blksize */
    fs_pack(out,16,le(st+20,4),4);fs_pack(out,20,le(st+24,4),4);fs_pack(out,24,le(st+28,4),4);
    fs_pack(out,28,le(st+16,4),2);fs_pack(out,32,le(st+8,8),8);               /* mode; ino */
    fs_pack(out,40,le(st+48,8),8);fs_pack(out,48,le(st+64,8),8);              /* size; blocks */
    const unsigned from[3]={72,104,88},to[3]={64,96,112};                     /* atime, ctime, mtime */
    for(unsigned i=0;i<3;i++) { fs_pack(out,to[i],le(st+from[i],8),8);fs_pack(out,to[i]+8,le(st+from[i]+8,8),4); }
    uint64_t rdev=le(st+32,8);
    fs_pack(out,128,((rdev>>8)&0xfff)|((rdev>>32)&~UINT64_C(0xfff)),4);fs_pack(out,132,(rdev&0xff)|((rdev>>12)&~UINT64_C(0xff)),4);
}
static int guest_syscall_dispatch(void) {
            uint64_t call=cpu->gpr[8];cpu->exclusive_valid=0;
            if(profiling && call<512)__atomic_fetch_add(&profile_syscalls[call],1,__ATOMIC_RELAXED);
            if(trace) fprintf(stderr,"pid %" PRIu64 " syscall %" PRIu64 ": x0..5=[%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n",process_pid,call,cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3],cpu->gpr[4],cpu->gpr[5]);
            if(trace && (call==56 || call==437 || call==281 || call==79 || call==48 || call==35 || call==78 || call==34)) { char path[1025];if(!fs_string(cpu->gpr[1],path))fprintf(stderr,"  path %s\n",path); }
            if(mem_mount_count && !mem_inside) { int r=mem_route(call);if(r!=MEM_PASS)return r; }   /* memfs.h */
            if(call==139 && job_control_enabled) { signal_return();if(schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal)return -1;return -3; }
            if(call==137) { uint64_t r=signal_timedwait();if(r==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=r;return -3; }
            if(call==133) { uint64_t r=signal_suspend();if(r==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=r;return -3; }
            if(call==134 || call==135) { cpu->gpr[0]=signal_call(call);if(schedule_active && (schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal))return -1;return -3; }
            if(call==285) { cpu->gpr[0]=cpu->gpr[5]?linux_error(22):fs_copy_between(cpu->gpr[0],cpu->gpr[2],cpu->gpr[1],cpu->gpr[3],cpu->gpr[4]);return -3; } /* copy_file_range */
            /* splice, tee: EINVAL, as for descriptors that cannot splice; callers fall back to
               read and write (GNU grep drains a pipe this way). ponytail: add a pipe-to-fd
               splice if a program needs one without the fallback. */
            if(call==76 || call==77) { cpu->gpr[0]=linux_error(22);return -3; }
            if(call==71) { uint64_t r=guest_sendfile();if(r==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=r;return -3; }
            if(call==436) {         /* close_range(first, last, flags): CLOSE_RANGE_CLOEXEC (4) marks instead of closing */
                uint64_t first=(uint32_t)cpu->gpr[0],last=(uint32_t)cpu->gpr[1],flags=cpu->gpr[2];
                if(first>last || (flags&~UINT64_C(6))) { cpu->gpr[0]=linux_error(22);return -3; }
                /* ponytail: CLOSE_RANGE_UNSHARE (2) is a no-op; threads keep one table */
                for(uint64_t fd=first;fd<=last && fd<GUEST_FD_LIMIT;fd++) {
                    if(!fd_kind[fd])continue;
                    unsigned slot=fd_index[fd];
                    if(flags&4) {
                        if(fd_kind[fd]==FD_FILE)fs_cloexec[slot]=1;else if(fd_kind[fd]==FD_NET)net_flags[slot]|=524288;else io_fds[slot].cloexec=1;
                        fd_mirror(fd);continue;
                    }
                    struct stat closing;
                    if(fd_kind[fd]==FD_FILE && !fstat(fs_fd[slot]-1,&closing)) { fs_locks_release(closing.st_dev,closing.st_ino,process_pid_now());fs_locks_release(closing.st_dev,closing.st_ino,FS_FLOCK_OWNER|fd); }
                    (void)guest_close(fd);
                }
                cpu->gpr[0]=0;return -3;
            }
            if(call==92) {          /* personality: 0xffffffff queries; kept per process across exec */
                static uint32_t persona[PROCESS_LIMIT+2];uint32_t old=process_pid<=PROCESS_LIMIT+1?persona[process_pid]:0; /* ponytail: a fork child starts at PER_LINUX */
                if((uint32_t)cpu->gpr[0]!=UINT32_MAX && process_pid<=PROCESS_LIMIT+1)persona[process_pid]=(uint32_t)cpu->gpr[0];
                cpu->gpr[0]=old;return -3;
            }
            if(call==160) {
                unsigned char *out=lookup(cpu->gpr[0],390,2);
                if(!out)cpu->gpr[0]=linux_error(14);
                else {
                    /* A current kernel version: glibc, MongoDB and the JVM refuse "too old" ones. */
                    const char *fields[]={"Linux",guest_hostname,"6.6.0-dockiva","#1 SMP PREEMPT_DYNAMIC Dockiva Engine","aarch64","(none)"};
                    memset(out,0,390);for(unsigned i=0;i<6;i++)memcpy(out+i*65,fields[i],strlen(fields[i]));cpu->gpr[0]=0;
                }
                return -3;
            }
            if(call>=174 && call<=177) { cpu->gpr[0]=call==174?guest_ids.ruid:call==175?guest_uid:call==176?guest_ids.rgid:guest_gid;return -3; } /* no host privilege change */
            if(call==151 || call==152) { cpu->gpr[0]=call==151?guest_uid:guest_gid;return -3; } /* setfsuid/setfsgid: file access uses the effective IDs; report them */
            if((call>=143 && call<=147) || call==149) { cpu->gpr[0]=guest_setid(call);return -3; }
            if(call==148 || call==150) { /* getresuid/getresgid: real, effective, saved */
                uint32_t id[3]={call==148?guest_ids.ruid:guest_ids.rgid,call==148?guest_uid:guest_gid,call==148?guest_ids.suid:guest_ids.sgid};
                uint64_t result=0,ptr[3]={cpu->gpr[0],cpu->gpr[1],cpu->gpr[2]};
                for(unsigned i=0;i<3;i++) { unsigned char *out=lookup(ptr[i],4,2);if(!out) { result=linux_error(14);break; }fs_pack(out,0,id[i],4); }
                cpu->gpr[0]=result;return -3;
            }
            if(call==159) { cpu->gpr[0]=guest_uid && !(cap_held[0]&(1u<<6))?linux_error(1):cpu->gpr[0]>65536?linux_error(22):0;return -3; } /* setgroups: no supplementary groups are kept */
            if(call==158) { cpu->gpr[0]=0;return -3; }                                                          /* getgroups: none */
            if(call==54 || call==55) {
                /* fchownat/fchown: ownership is not tracked (every file shows the current
                   identity), so a valid request by root succeeds without effect. */
                int fd=-1;struct stat st;
                int writable;
                if(call==55) { fd=fs_host(cpu->gpr[0]);if(fd<0) { cpu->gpr[0]=linux_error(9);return -3; }writable=fs_ro[fs_slot(cpu->gpr[0])]!=1; } /* 2: a lower file (overlay.h), no change to make */
                else {
                    if(cpu->gpr[4]&~UINT64_C(256|4096)) { cpu->gpr[0]=linux_error(22);return -3; }
                    char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(cpu->gpr[4]&256)|FS_DIRECT,NULL);
                    if(parent<0) { cpu->gpr[0]=fs_error();return -3; }
                    writable=fs_path_writable();
                    int r=fs_leaf_stat(parent,leaf,&st);int saved=errno;close(parent);errno=saved;
                    if(r<0) { cpu->gpr[0]=fs_error();return -3; }
                }
                if(fd>=0 && fstat(fd,&st)<0) { cpu->gpr[0]=fs_error();return -3; }
                uint32_t owner=(uint32_t)cpu->gpr[call==54?2:1],group=(uint32_t)cpu->gpr[call==54?3:2];
                if(!writable)cpu->gpr[0]=linux_error(30);
                else if(guest_uid && ((owner!=UINT32_MAX && owner!=guest_uid) || (group!=UINT32_MAX && group!=guest_gid)))cpu->gpr[0]=linux_error(1);
                else cpu->gpr[0]=0;
                return -3;
            }
            if(call==154 || call==155) { cpu->gpr[0]=terminal_group(call);return -3; }
            if(call==132) { cpu->gpr[0]=signal_altstack();return -3; }
            if(call==74) { cpu->gpr[0]=io_signalfd();return -3; }
            if(call==194) { cpu->gpr[0]=shm_get();return -3; }
            if(call==195) { cpu->gpr[0]=shm_control();return -3; }
            if(call==196) { cpu->gpr[0]=shm_attach();return -3; }
            if(call==197) { cpu->gpr[0]=shm_detach();return -3; }
            if(call==157) { /* setsid: a new session and process group; a group leader gets EPERM */
                unsigned *group=schedule_active?&schedule_tasks[process_pid-1].pgid:NULL;
                if(!group || *group==process_pid)cpu->gpr[0]=linux_error(1);
                else { *group=schedule_tasks[process_pid-1].sid=(unsigned)process_pid;io_epoch++;cpu->gpr[0]=process_pid; }
                return -3;
            }
            if(call==156) { /* getsid: inherited by children; only setsid starts one (setpgid does not) */
                uint64_t pid=cpu->gpr[0]?cpu->gpr[0]:process_pid;
                if(!schedule_active)cpu->gpr[0]=pid==process_pid?process_pid:linux_error(3);
                else if(pid>process_count+1 || !schedule_tasks[pid-1].state)cpu->gpr[0]=linux_error(3);
                else cpu->gpr[0]=guest_group_of(pid,1);
                return -3;
            }
            if(call==131 && job_control_enabled) { /* tgkill without threads: tid is the process */
                cpu->gpr[0]=(int64_t)cpu->gpr[0]<=0 || cpu->gpr[2]>64?linux_error(22):cpu->gpr[0]!=cpu->gpr[1]?linux_error(3):guest_signal_process(cpu->gpr[0],cpu->gpr[2]);
                if(schedule_active && (schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal))return -1;
                return -3;
            }
            if(call==129 || (call==130 && job_control_enabled)) { cpu->gpr[0]=(call==130 && (int64_t)cpu->gpr[0]<=0)?linux_error(22):process_kill();if(schedule_active && (schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal))return -1;return -3; }
            if(call>=20 && call<=22) { uint64_t result=guest_epoll(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==59) { cpu->gpr[0]=io_pipe();return -3; }
            if(call==19) { cpu->gpr[0]=io_eventfd();return -3; }
            if(call>=85 && call<=87) { cpu->gpr[0]=io_timerfd(call);return -3; }
            if(call>=26 && call<=28) { cpu->gpr[0]=io_inotify(call);return -3; }
            if(call==79 && (cpu->gpr[3]&0x1000) && (int64_t)cpu->gpr[0]>=0 && cpu->gpr[0]<GUEST_FD_LIMIT && fs_host(cpu->gpr[0])<0) {
                /* fstatat(fd, "", AT_EMPTY_PATH), glibc's fstat, on a pipe, socket or synthetic file: fstat */
                const unsigned char *name=lookup(cpu->gpr[1],1,4);
                if(name && !*name) {
                    uint64_t saved=cpu->gpr[1];cpu->gpr[1]=cpu->gpr[2];cpu->gpr[8]=80;
                    int result=guest_syscall_dispatch();
                    cpu->gpr[8]=79;if(result!=-3)cpu->gpr[1]=saved;return result;
                }
            }
            if(call==79 || call==48 || call==439) {   /* /dev/fd/N (bash's process substitution): the descriptor itself */
                char path[1025];int64_t n=fs_string(cpu->gpr[1],path)==0?guest_fd_path(path):-1;
                if(n>=0) {
                    if(n>=GUEST_FD_LIMIT || !fd_kind[n]) { cpu->gpr[0]=linux_error(2);return -3; }
                    if(call!=79) { cpu->gpr[0]=0;return -3; }
                    uint64_t name=cpu->gpr[1];
                    cpu->gpr[0]=(uint64_t)n;cpu->gpr[1]=cpu->gpr[2];cpu->gpr[8]=80;
                    int result=guest_syscall_dispatch();                                     /* fstat */
                    cpu->gpr[8]=79;cpu->gpr[1]=name;return result;
                }
            }
            if(call==56) {
                char path[1025];int64_t n=fs_string(cpu->gpr[1],path)==0?guest_fd_path(path):-1;
                if(n>=0) {
                    if(net_host((uint64_t)n)>=0) { cpu->gpr[0]=linux_error(6);return -3; }          /* ENXIO, as for a socket */
                    cpu->gpr[0]=guest_dup((uint64_t)n,-1,0,(cpu->gpr[2]&524288)!=0);return -3;
                }
            }
            if(call==23) { cpu->gpr[0]=guest_dup(cpu->gpr[0],-1,0,0);return -3; }                       /* dup */
            if(call==24) {                                                                           /* dup3 (musl's dup2) */
                if(cpu->gpr[2]&~UINT64_C(524288) || cpu->gpr[0]==cpu->gpr[1])cpu->gpr[0]=linux_error(22);
                else cpu->gpr[0]=cpu->gpr[1]>=GUEST_FD_LIMIT?linux_error(9):guest_dup(cpu->gpr[0],(int64_t)cpu->gpr[1],0,cpu->gpr[2]!=0);
                return -3;
            }
            if(call==25 && (cpu->gpr[1]==0 || cpu->gpr[1]==1030)) {                                  /* F_DUPFD(_CLOEXEC) */
                cpu->gpr[0]=cpu->gpr[2]>=GUEST_FD_LIMIT?linux_error(22):guest_dup(cpu->gpr[0],-1,cpu->gpr[2],cpu->gpr[1]==1030);return -3;
            }
            if(io_managed(cpu->gpr[0]) && (call==57 || call==63 || call==64 || call==25 || call==29 || call==80 || call==62 || call==67 || call==61)) { uint64_t result=io_call(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==220) { cpu->gpr[0]=process_clone();return -3; }
            if(call==221) { uint64_t result=process_exec_at((uint64_t)-100,0);if(result)cpu->gpr[0]=result;return -3; }
            /* execveat (FEX re-runs itself through it). ponytail: AT_EMPTY_PATH (fexecve) is
               ENOSYS, AT_SYMLINK_NOFOLLOW is not enforced. */
            if(call==281) {
                uint64_t flags=cpu->gpr[4];
                uint64_t result=(flags&~UINT64_C(0x1100))?linux_error(22):(flags&0x1000)?linux_error(38):process_exec_at(cpu->gpr[0],1);
                if(result)cpu->gpr[0]=result;return -3;
            }
            if(call==260 || call==95) {
                uint64_t result=call==95?process_waitid():process_wait();
                if(result==IO_BLOCK) {                          /* only a child's event (or a signal) can change the answer */
                    if(schedule_active) { Thread *th=thread_current();if(th)th->child_wait=1;else schedule_tasks[process_pid-1].child_wait=1; }
                    cpu->pc-=4;return -2;
                }
                cpu->gpr[0]=result;return -3;
            }
            if(call==153) { /* times: process CPU times are not measured; the clock in ticks (100 Hz) */
                if(cpu->gpr[0]) { unsigned char *out=lookup(cpu->gpr[0],32,2);if(!out) { cpu->gpr[0]=linux_error(14);return -3; }memset(out,0,32); }
                cpu->gpr[0]=net_now()/10;return -3;
            }
            if(call==172 || call==173) { cpu->gpr[0]=call==172?process_pid:guest_ppid();return -3; }
            if(call==178) { cpu->gpr[0]=process_pid;return -3; } /* gettid: one thread per interpreted process */
            if(call==124) { cpu->gpr[0]=0;if(schedule_active)schedule_slice=0;return -3; } /* sched_yield ends the slice */
            if(call==65) { uint64_t result=guest_readv();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==66) { uint64_t result=guest_writev();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==17) { cpu->gpr[0]=fs_getcwd();return -3; }
            if(call==50) { cpu->gpr[0]=fs_fchdir();return -3; }
            if(call==49) { cpu->gpr[0]=fs_chdir();return -3; }
            if(call==29) { /* ioctl: sockets; close-on-exec and nonblocking (no effect) on files */
                uint64_t fd=cpu->gpr[0],request=cpu->gpr[1];
                if(net_host(fd)>=0)cpu->gpr[0]=net_ioctl();
                else if(fs_host(fd)>=0 && (request==0x5451 || request==0x5450)) { fs_cloexec[fs_slot(fd)]=request==0x5451;fd_mirror(fd);cpu->gpr[0]=0; }
                else if(fs_host(fd)>=0 && request==0x5421)cpu->gpr[0]=0;
                else cpu->gpr[0]=(fd<=2 || fs_host(fd)>=0)?linux_error(25):linux_error(9);
                return -3;
            }
            if(call==93 || call==94) return (int)(cpu->gpr[0]&255);
            if(call==103) { cpu->gpr[0]=signal_setitimer();return -3; }
            if(call==113 || call==114) { cpu->gpr[0]=guest_clock(call);return -3; }
            if(call==278) { cpu->gpr[0]=guest_random();return -3; }
            if(call==169) { cpu->gpr[0]=guest_gettimeofday();return -3; }
            if(call==165) { cpu->gpr[0]=guest_getrusage();return -3; }
            if(call==179) { cpu->gpr[0]=guest_sysinfo();return -3; }
            if(call==223) {
                cpu->gpr[0]=guest_fadvise();
                if(trace)fprintf(stderr,"fadvise64 result=%" PRId64 " (cache hint only)\n",(int64_t)cpu->gpr[0]);
                return -3;
            }
            if(call==122) { cpu->gpr[0]=guest_id_exists(cpu->gpr[0])?0:linux_error(3);return -3; } /* sched_setaffinity: accepted */
            if(call==32 && fs_host(cpu->gpr[0])<0) {                                          /* flock on a pipe, socket or synthetic file */
                cpu->gpr[0]=cpu->gpr[0]<GUEST_FD_LIMIT && fd_kind[cpu->gpr[0]]?0:linux_error(9);return -3;
            }
            if(call>=118 && call<=121) {
                /* Scheduling policy, as an unprivileged Docker container sees it: every task is
                   SCHED_OTHER at priority 0; the real-time policies need CAP_SYS_NICE (EPERM).
                   .NET's Thread.Priority asks for them. */
                if(!guest_id_exists(cpu->gpr[0])) { cpu->gpr[0]=linux_error(3);return -3; }
                uint64_t param=call==119?cpu->gpr[2]:cpu->gpr[1],policy=call==119?cpu->gpr[1]&~UINT64_C(0x40000000):0;
                unsigned char *p=call==120?(unsigned char *)"":lookup(param,4,call==121?2:4);
                if(!p) { cpu->gpr[0]=linux_error(param?14:22);return -3; }
                if(call==121) { fs_pack(p,0,0,4);cpu->gpr[0]=0; }
                else if(call==120)cpu->gpr[0]=0;                                              /* SCHED_OTHER */
                else if(policy==1 || policy==2)cpu->gpr[0]=linux_error(1);                    /* SCHED_FIFO, SCHED_RR */
                else cpu->gpr[0]=(policy==0 || policy==3 || policy==5) && !le(p,4)?0:linux_error(22);
                return -3;
            }
            if(call==140 || call==141) {                                                         /* setpriority, getpriority */
                /* ponytail: nice values are accepted and not kept (the host schedules the runtime);
                   getpriority reports nice 0 (the raw call returns 20 - nice). */
                if(cpu->gpr[0]>2) { cpu->gpr[0]=linux_error(22);return -3; }
                cpu->gpr[0]=call==141?20:0;return -3;
            }
            if(call==125 || call==126) {                                                        /* sched_get_priority_max/min */
                uint64_t policy=cpu->gpr[0];
                cpu->gpr[0]=policy==1 || policy==2?(call==125?99:1):policy==0 || policy==3 || policy==5?0:linux_error(22);return -3;
            }
            if(call==232) { /* mincore: every page reported resident */
                uint64_t pages=(cpu->gpr[1]+4095)/4096;unsigned char *out=pages?lookup(cpu->gpr[2],pages,2):NULL;
                if(cpu->gpr[0]%4096 || (pages && !out))cpu->gpr[0]=linux_error(cpu->gpr[0]%4096?22:14);
                else { if(out)memset(out,1,pages);cpu->gpr[0]=0; }
                return -3;
            }
            if(call==123) { /* sched_getaffinity: one guest CPU per worker (the interpreter has one). */
                if(!guest_id_exists(cpu->gpr[0]))cpu->gpr[0]=linux_error(3);
                else if(cpu->gpr[1]<8 || (cpu->gpr[1]&7))cpu->gpr[0]=linux_error(22);
                else {
                    unsigned char *out=lookup(cpu->gpr[2],8,2);
                    if(!out)cpu->gpr[0]=linux_error(14);
                    else { unsigned n=guest_cpus();fs_pack(out,0,n>=64?UINT64_MAX:(UINT64_C(1)<<n)-1,8);cpu->gpr[0]=8; }
                }
                return -3;
            }
            if(call==96) { cpu->gpr[0]=process_pid; return -3; } /* Single guest thread; no futex waiters. */
            /* Optional Linux facilities: advertise absence so libc can use its
               fallback. Never report successful robust-list/rseq registration. */
            if(call==99 || call==293) { cpu->gpr[0]=linux_error(38);return -3; }
            if(call==167) { cpu->gpr[0]=guest_prctl();return -3; }
            if(call==277) { cpu->gpr[0]=guest_seccomp();return -3; }
            if(call==283) { /* membarrier: the native backend first moves running sibling threads through an exception */
                uint64_t cmd=cpu->gpr[0];const uint64_t supported=1|8|16; /* GLOBAL, PRIVATE_EXPEDITED, REGISTER_PRIVATE_EXPEDITED */
                cpu->gpr[0]=cmd==0?supported:(cmd&supported) && !(cmd&(cmd-1)) && !cpu->gpr[1]?0:linux_error(22);return -3;
            }
            if(call==90 || call==91) { cpu->gpr[0]=guest_capability(call==91);return -3; }
            if(call==233) {
                uint64_t advice=cpu->gpr[2];cpu->gpr[0]=guest_madvise();
                if(trace)fprintf(stderr,"madvise advice=%" PRIu64 " result=%" PRId64 "\n",advice,(int64_t)cpu->gpr[0]);
                return -3;
            }
            if(call==261 || call==163 || call==164) { /* prlimit64, getrlimit, setrlimit */
                uint64_t pid=call==261?cpu->gpr[0]:0,resource=call==261?cpu->gpr[1]:cpu->gpr[0];
                uint64_t in=call==261?cpu->gpr[2]:call==164?cpu->gpr[1]:0,out=call==261?cpu->gpr[3]:call==163?cpu->gpr[1]:0;
                cpu->gpr[0]=guest_rlimit(pid,resource,in,out);return -3;
            }
            if(call==168) { /* getcpu: the worker (vCPU) running the caller, node 0 */
                unsigned char *c=cpu->gpr[0]?lookup(cpu->gpr[0],4,2):NULL,*n=cpu->gpr[1]?lookup(cpu->gpr[1],4,2):NULL;
                if((cpu->gpr[0] && !c) || (cpu->gpr[1] && !n)) { cpu->gpr[0]=linux_error(14);return -3; }
                if(c)fs_pack(c,0,worker_count?worker->index%worker_count:0,4);if(n)fs_pack(n,0,0,4);
                cpu->gpr[0]=0;return -3;
            }
            if(call==279) { cpu->gpr[0]=fs_memfd();return -3; }
            if(call==214) { cpu->gpr[0]=guest_brk(cpu->gpr[0]); return -3; }
            if(call==222) { cpu->gpr[0]=dynamic_enabled?guest_dynamic_mmap():guest_mmap(cpu->gpr[1],cpu->gpr[2],cpu->gpr[3],cpu->gpr[5]); return -3; }
            if(call==226 && dynamic_enabled) { cpu->gpr[0]=guest_mprotect();return -3; }
            if(call==215) { cpu->gpr[0]=guest_munmap(cpu->gpr[0],cpu->gpr[1]); return -3; }
            if(call==227) { cpu->gpr[0]=guest_msync();return -3; }
            if(call==43 || call==44) { /* statfs, fstatfs: the Mac volume's space, as overlayfs (Docker's) */
                unsigned char *out=lookup(cpu->gpr[1],120,2);
                if(!out) { cpu->gpr[0]=linux_error(14);return -3; }
                if(call==44 && fs_host(cpu->gpr[0])<0 && net_host(cpu->gpr[0])<0 && !io_managed(cpu->gpr[0])) { cpu->gpr[0]=linux_error(9);return -3; }
                struct statfs host;
                if(fstatfs(fs_root>=0?fs_root:0,&host)<0) { cpu->gpr[0]=fs_error();return -3; }
                memset(out,0,120);
                fs_pack(out,0,0x794c7630,8);fs_pack(out,8,host.f_bsize,8);fs_pack(out,16,host.f_blocks,8);
                fs_pack(out,24,host.f_bfree,8);fs_pack(out,32,host.f_bavail,8);fs_pack(out,40,host.f_files,8);
                fs_pack(out,48,host.f_ffree,8);fs_pack(out,64,255,8);fs_pack(out,72,host.f_bsize,8);
                cpu->gpr[0]=0;return -3;
            }
            if(call==166 || call==52 || call==53 || call==78 || call==88 || call==36 || call==37 || call==48 || call==439 || call==45 || call==82 || call==83 || call==61 || (call==25 && fs_host(cpu->gpr[0])>=0) || call==79 || call==56 || call==437 || call==33 || call==34 || call==35 || call==38 || call==46 || call==62 || call==80 || ((call==57 || call==63 || call==64 || call==67 || call==68 || call==69 || call==70 || call==286 || call==287 || call==47 || call==84 || call==32) && fs_host(cpu->gpr[0])>=0)) {
                uint64_t result=fs_call(call);
                if(result==IO_BLOCK) { cpu->pc-=4;return -2; }  /* F_SETLKW waits for the lock */
                cpu->gpr[0]=result;return -3;
            }
            if((call>=67 && call<=70) || call==286 || call==287 || call==47 || call==84) { /* positioned I/O needs a regular file */
                uint64_t fd=cpu->gpr[0];cpu->gpr[0]=net_host(fd)>=0 || (io_managed(fd) && IO(fd).kind)?linux_error(29):linux_error(9);return -3;
            }
            if(call==101 || call==115) { uint64_t result=guest_sleep(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==72 || call==73) { uint64_t result=call==72?net_select():net_poll();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==198 && schedule_active && !net_scheduled) { cpu->gpr[0]=linux_error(95);return -3; }
            if(call==198) { cpu->gpr[0]=net_socket(cpu->gpr[0],cpu->gpr[1],cpu->gpr[2]); return -3; }
            if(call==199) { cpu->gpr[0]=schedule_active && !net_scheduled?linux_error(95):net_socketpair(cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3]);return -3; }
            if(call==243) { cpu->gpr[0]=linux_error(38);return -3; } /* recvmmsg */
            if(call==216 && dynamic_enabled) { cpu->gpr[0]=guest_mremap();return -3; }
            /* Probed with a fallback, so not worth a log line: io_uring, and mremap of a
               static program (malloc and realloc copy instead). */
            if(call==216 || call==425 || call==426 || call==427 || call==434 || call==235 || call==236 || call==237 || call==238 || call==239) { /* also NUMA policy */ cpu->gpr[0]=linux_error(38);return -3; } /* also pidfd_open */
            if(call==291) { /* statx: newfstatat (or fstat of a descriptor), rewritten in statx's layout */
                uint64_t dirfd=cpu->gpr[0],path=cpu->gpr[1],flags=cpu->gpr[2],buffer=cpu->gpr[4];
                unsigned char *out=lookup(buffer,256,2),*first=lookup(path,1,4);
                if(!out || !first) { cpu->gpr[0]=linux_error(14);return -3; }
                if(flags&~UINT64_C(0x100|0x800|0x1000|0x6000)) { cpu->gpr[0]=linux_error(22);return -3; }
                uint64_t saved[9];memcpy(saved,cpu->gpr,sizeof(saved));
                if(!*first && (flags&0x1000)) { cpu->gpr[0]=dirfd;cpu->gpr[1]=buffer;cpu->gpr[8]=80; }
                else { cpu->gpr[0]=dirfd;cpu->gpr[1]=path;cpu->gpr[2]=buffer;cpu->gpr[3]=flags&(0x100|0x800|0x1000);cpu->gpr[8]=79; }
                int r=guest_syscall_dispatch();uint64_t result=cpu->gpr[0];
                memcpy(cpu->gpr,saved,sizeof(saved));
                if(r!=-3) { cpu->gpr[0]=linux_error(38);return -3; }
                if((int64_t)result<0) { cpu->gpr[0]=result;return -3; }
                statx_from_stat(out);cpu->gpr[0]=0;return -3;
            }
            if((call==206 || call==207 || call==211 || call==212 || call==269) && net_host(cpu->gpr[0])<0) { /* sendto/recvfrom/sendmsg/recvmsg/sendmmsg need a socket */
                uint64_t fd=cpu->gpr[0];cpu->gpr[0]=(fs_host(fd)>=0 || (io_managed(fd) && IO(fd).kind))?linux_error(88):linux_error(9);return -3;
            }
            if(call==57 || call==63 || call==25 || call==203 || call==210 || call==242 || call==200 || call==201 || call==202 || call==204 || call==205 || call==206 || call==207 || call==208 || call==209 || call==211 || call==212 || call==269 || (call==64 && net_host(cpu->gpr[0])>=0)) { uint64_t result=net_call(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call!=64) {
                /* As on Linux, an unknown system call returns ENOSYS; programs probe and
                   fall back. Each number is reported once so container logs show gaps;
                   clone3 is not, glibc always falls back to clone. */
                static unsigned char reported[512];
                if(call!=435 && (call>=512 || !reported[call])) { if(call<512)reported[call]=1;fprintf(stderr,"dockiva: Linux syscall %" PRIu64 " is not implemented (ENOSYS)\n",call); }
                cpu->gpr[0]=linux_error(38);return -3;
            }
            if(cpu->gpr[0]!=1 && cpu->gpr[0]!=2) cpu->gpr[0]=UINT64_MAX-8;
            else {
                const unsigned char *payload=cpu->gpr[2]?lookup(cpu->gpr[1],cpu->gpr[2],4):(const unsigned char *)"";
                if(!payload) cpu->gpr[0]=UINT64_MAX-13;
                else {
                    ssize_t written=write((int)cpu->gpr[0],payload,(size_t)cpu->gpr[2]);
                    cpu->gpr[0]=written<0 ? UINT64_MAX-4 : (uint64_t)written;
                }
            }
    return -3;
}
/* read/write/sendto/recvfrom move at most 1 MiB per call: larger counts become a
   short transfer, as Linux allows, instead of EINVAL. The guest's x2 is preserved. */
/* The hot calls that touch only the worker's own process (its descriptor tables,
   memory it already owns) and host descriptors, served without the runtime lock:
   openat and newfstatat (not under /proc); lseek, read, readv, pread, write,
   writev, pwrite, fstat, fsync and fdatasync on files (fsync no longer stalls
   every other worker); read, readv, write, sendto and recvfrom on sockets;
   epoll_pwait on an epoll no other process shares. Returns 1 when done (result in
   x0), 0 when the call must run under the lock; an aborted attempt changed nothing. */
#ifdef VMFREE_HVF
static _Thread_local int fs_close_later=-1;
static void fs_close_pending(void) { if(fs_close_later>=0) { close(fs_close_later);fs_close_later=-1; } }
static int fs_unlocked_path(uint64_t address,char path[1025]) {
    return fs_string(address,path)==0 && strncmp(path,"/proc",5) && strncmp(path,"/dev/",5) && !(path[0]!='/' && fs_cwd && !strncmp(fs_cwd->path,"/proc",5));
}
/* Opening, creating, removing and stat'ing by path, reading, writing and syncing files
   hold no lock at all while the host works (tens of microseconds each), so threads
   of one process (go build's compiler, the go command) overlap their file I/O
   instead of queuing on its mm lock. Under it beforehand, guest strings and the
   buffer are copied and the base and file descriptors duplicated: a sibling's close,
   dup2, munmap, fork or exec meanwhile cannot hand their numbers to another file or
   free memory the call uses (fs_detached in fs.h). Under it afterwards, the thread
   must still live (a sibling's exec or exit_group ends it: the result is dropped),
   then output is copied to the guest (EFAULT if the buffer went away) and an opened
   file installed. Only for threaded processes: no one else waits on a lone thread's
   lock. Returns 1 when it served the call (result, or syscall_abort set: nothing
   changed), 0 when the caller must serve it. path: the first path argument, copied
   (the second for renameat), or NULL; statx: newfstatat fills a 256-byte statx. */
#define FS_DETACHED_BUFFER (1024u*1024u)
static int fs_call_detached(uint64_t call,const char *path,uint64_t *result,int statx) {
    static _Thread_local unsigned char *buffer;
    ScheduledTask *task=&schedule_tasks[process_pid-1];
    if(!schedule_active || !worker_threads || !task->threads)return 0;
    int sync=call==82 || call==83,transfer=sync || call==63 || call==64 || call==67 || call==68,paths=call==38?2:1,fd=-1;
    uint64_t in=0,out=0,size=0;
    if(transfer) {
        int slot=fs_slot(cpu->gpr[0]);size=sync?0:cpu->gpr[2];
        if(slot<0 || !fs_fd[slot] || fs_dirs[slot] || (!sync && (fs_dev[slot] || !size || size>FS_DETACHED_BUFFER || ((call==67 || call==68) && (int64_t)cpu->gpr[3]<0))))return 0;
        if(call==63 || call==67) { if(!lookup(cpu->gpr[1],size,2))return 0;out=cpu->gpr[1]; } else if(!sync)in=cpu->gpr[1];
        fs_detached.read_only=fs_ro[slot];
    } else {
        if(!path)return 0;
        if(call==56) {                                                     /* /sys: synthetic files, in io tables */
            unsigned slot=0;while(slot<FS_SLOTS && fs_fd[slot])slot++;
            if(!strncmp(path,"/sys/",5) || slot==FS_SLOTS || fd_lowest(0)<0)return 0;
        }
        else if(call==79) { out=cpu->gpr[2];size=statx?256:128; }
        else if(call==78) { if(!cpu->gpr[3] || (int64_t)(int32_t)cpu->gpr[3]<=0)return 0;out=cpu->gpr[2];size=cpu->gpr[3]>4096?4096:cpu->gpr[3]; }
        else if(call==88 && cpu->gpr[2]) { in=cpu->gpr[2];size=32; }
        else if(call!=48 && call!=439 && call!=34 && call!=35 && call!=38 && call!=88)return 0;
    }
    if(!buffer && !(buffer=malloc(FS_DETACHED_BUFFER)))return 0;
    if(in) { const unsigned char *data=lookup(in,size,4);if(!data)return 0;memcpy(buffer,data,(size_t)size); }
    fs_detached.memory_at=in?in:out;fs_detached.memory_size=size;fs_detached.memory=buffer;
    fs_detached.path[0][0]=fs_detached.path[1][0]=0;fs_detached.at[0]=fs_detached.at[1]=0;fs_detached.base[0]=fs_detached.base[1]=-1;
    for(int i=0;i<paths && !transfer;i++) {
        uint64_t at=cpu->gpr[1+2*i],dir=cpu->gpr[2*i];
        if(call==38 && !i) { if(fs_string(at,fs_detached.path[0])<0)break; } else strcpy(fs_detached.path[i],path);
        fs_detached.at[i]=at;fs_detached.dir[i]=dir;
        if(fs_detached.path[i][0]=='/')continue;
        int host=(int32_t)dir==-100?(fs_cwd?fs_cwd->fd:fs_root):fs_host(dir),slot=fs_slot(dir);
        const char *base_path=(int32_t)dir==-100?(fs_cwd?fs_cwd->path:"/"):slot>=0 && fs_dirs[slot]?fs_dirs[slot]->path:"";
        if(host<0 || (fs_detached.base[i]=dup(host))<0)break;
        strcpy(fs_detached.base_path[i],base_path);
    }
    if(transfer)fd=dup(fs_host(cpu->gpr[0]));
    if(transfer?fd<0:!fs_detached.path[paths-1][0] || (fs_detached.path[paths-1][0]!='/' && fs_detached.base[paths-1]<0) || (paths==2 && fs_detached.path[0][0]!='/' && fs_detached.base[0]<0)) {
        for(int i=0;i<2;i++)if(fs_detached.base[i]>=0)close(fs_detached.base[i]);
        return 0;
    }
    fs_detached.fd=-1;fs_detached.on=1;
    mm_unlock(task);
    uint64_t r=!transfer?fs_call(call):sync?(fs_sync(fd)<0?fs_error():0):call==63 || call==64?fs_transfer(fd,call):fs_transfer_at(fd,call==67?63:64,(int64_t)cpu->gpr[3]);
    if(!transfer && call==79 && statx && (int64_t)r>=0) { statx_from_stat(buffer);r=0; }
    fs_detached.on=0;
    if(fd>=0)close(fd);                                                     /* the call's own copies: no lock needed */
    for(int i=0;i<2;i++)if(fs_detached.base[i]>=0)close(fs_detached.base[i]);
    mm_lock(task);
    Thread *th=task_thread(task);
    if(syscall_abort || task->exit_pending || (th && !th->state)) {          /* nothing changed, or no one to tell */
        if(fs_detached.fd>=0) { close(fs_detached.fd);fs_directory_release(fs_detached.directory);fs_detached.fd=-1; }
        *result=r;return 1;
    }
    if(out && (int64_t)r>=0) {
        /* The call is done and cannot be redone under the runtime lock: pages a fork
           shared meanwhile are made private here, as a write fault does (mm lock only). */
        uint64_t n=call==79?size:r;
        syscall_unlocked=0;unsigned char *to=n?lookup(out,n,2):NULL;syscall_unlocked=1;
        if(n && !to)r=linux_error(14);else if(n)memcpy(to,buffer,(size_t)n);
    }
    if(fs_detached.fd>=0) { r=fs_install(fs_detached.fd,fs_detached.flags,fs_detached.opened_ro,fs_detached.opened_device,fs_detached.directory);fs_detached.fd=-1; }
    *result=r;return 1;
}
static int guest_syscall_unlocked(void) {
    uint64_t call=cpu->gpr[8],fd=cpu->gpr[0],count=cpu->gpr[2];
    if(trace)return 0;
    if(mem_mount_count) { int r=mem_route_unlocked(call);if(r>=0)return r; }        /* memfs.h: its own lock */
    int file=fs_host(fd)>=0 && !fs_dirs[fs_slot(fd)],socket=!file && net_host(fd)>=0;
    int epoll=call==22 && io_managed(fd) && IO(fd).kind==IO_EPOLL && epolls[IO(fd).host].refs==1;
    /* Calls by path, except under /proc (process tables) and /dev: openat, newfstatat,
       statx, faccessat(2), readlinkat, mkdirat, unlinkat, renameat (both paths) and
       utimensat. They use the process's descriptors, working directory and umask,
       which its threads change only under its mm lock, and the atomic entry count. */
    char path[1025];
    /* execve of a single-threaded process: its new image is built here, and the call
       redone under the lock only swaps it in (process_exec_at). */
    if(call==221 && schedule_active && !schedule_tasks[process_pid-1].threads && fs_unlocked_path(cpu->gpr[0],path)) {
        exec_unlocked=1;(void)exec_build((uint64_t)-100,0);exec_unlocked=0;return 0;
    }
    int named=(call==56 || call==79 || call==291 || call==48 || call==439 || call==78 || call==34 || call==35 || call==38 || (call==88 && cpu->gpr[1])) && fs_unlocked_path(cpu->gpr[1],path);
    if(named && call==38)named=fs_unlocked_path(cpu->gpr[3],path);
    if(call==291) {
        /* statx of a path (npm stats thousands): newfstatat without the lock, rewritten. */
        uint64_t flags=cpu->gpr[2],buffer=cpu->gpr[4];
        if(!named || !path[0] || (flags&~UINT64_C(0x100|0x800|0x6000)))return 0;
        unsigned char *out=lookup(buffer,256,2);if(!out)return 0;
        uint64_t saved[9];memcpy(saved,cpu->gpr,sizeof(saved));
        cpu->gpr[2]=buffer;cpu->gpr[3]=flags&(0x100|0x800);cpu->gpr[8]=79;
        cpu->exclusive_valid=0;syscall_unlocked=1;syscall_abort=0;
        uint64_t result;int detached=fs_call_detached(79,path,&result,1);  /* out is stale after a detached call */
        if(!detached)result=fs_call(79);
        syscall_unlocked=0;memcpy(cpu->gpr,saved,sizeof(saved));
        if(syscall_abort || result==IO_BLOCK || mem_redirect_take())return 0;
        if((int64_t)result>=0) { if(!detached)statx_from_stat(out);result=0; }
        cpu->gpr[0]=result;return 1;
    }
    /* Signal state that only this process's own threads change (they hold its mm
       lock), read by senders under the runtime lock as single words: getpid, gettid,
       sigaltstack; rt_sigaction, except SIG_IGN (it drops pending signals senders
       set); rt_sigprocmask, unless it makes a process-directed pending signal
       deliverable (schedule_pending sends it). A thread-directed one stops the
       guest before it resumes (hvf_must_stop) and is delivered under the lock. Go
       sets about 130 handlers in every process it starts. */
    if(call==172 || call==178 || call==132 || call==134 || call==135) {
        if(!schedule_active)return 0;
        ScheduledTask *task=&schedule_tasks[process_pid-1];
        if(call==172 || call==178) { Thread *th=task_thread(task);cpu->gpr[0]=call==178 && th?th->tid:process_pid;return 1; }
        cpu->exclusive_valid=0;syscall_unlocked=1;syscall_abort=0;
        const unsigned char *in=(call==134 || call==135) && cpu->gpr[1]?lookup(cpu->gpr[1],8,4):NULL;
        int locked=(call==134 || call==135) && cpu->gpr[1] && !in;
        if(call==134 && in && le(in,8)==1)locked=1;
        if(call==135 && in && cpu->gpr[3]==8) {
            uint64_t value=le(in,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18)),next=*signal_mask_now();
            if(cpu->gpr[0]==0)next|=value;else if(cpu->gpr[0]==1)next&=~value;else if(cpu->gpr[0]==2)next=value;
            if(task->signal_pending&~next&~task->signal_ignored)locked=1;
        }
        uint64_t result=locked || syscall_abort?0:call==132?signal_altstack():signal_call(call);
        syscall_unlocked=0;
        if(locked || syscall_abort)return 0;
        cpu->gpr[0]=result;return 1;
    }
    if(call==166 && schedule_active) {                                       /* umask: the process's own (tar asks twice per file) */
        unsigned previous=fs_umask;fs_umask=(unsigned)cpu->gpr[0]&0777;cpu->gpr[0]=previous;return 1;
    }
    /* close of a file (not a directory) while no record lock exists to release: only
       this process's tables, which its threads change under its mm lock. The host
       descriptor closes once that lock is released (fs_close_pending), as Linux
       drops the last reference after the number is free; a local file's close
       cannot fail. */
    if(call==57 && file && !__atomic_load_n(&fs_locks_taken,__ATOMIC_ACQUIRE)) {
        int slot=fs_slot(fd),host=fs_fd[slot]-1;
        fs_fd[slot]=0;fs_cloexec[slot]=0;fd_unbind(fd);
        fs_close_pending();fs_close_later=host;cpu->gpr[0]=0;return 1;
    }
    /* ppoll of sockets alone (libpq, pgbench): the host's readiness and the thread's own
       wait; a wait goes to the backend's direct wait, then the locked path. */
    if(call==73 && schedule_active && net_poll_unlocked()) {
        cpu->exclusive_valid=0;syscall_unlocked=1;syscall_abort=0;net_unlocked_wait.count=0;
        uint64_t result=net_poll_wait();syscall_unlocked=0;
        if(syscall_abort || result==IO_BLOCK)return 0;
        cpu->gpr[0]=result;return 1;
    }
    /* epoll_ctl of a file (Go offers every file it opens to its poller): EPERM, as Linux
       answers for files that cannot be polled, after the epoll descriptor's checks. */
    if(call==21 && fs_host(cpu->gpr[2])>=0 && io_managed(fd) && IO(fd).kind==IO_EPOLL) {
        unsigned op=(unsigned)cpu->gpr[1];cpu->gpr[0]=op<1 || op>3?linux_error(22):linux_error(1);return 1;
    }
    /* fcntl's descriptor flags on a file: this process's own (record locks take the lock). */
    if(call==25 && file && cpu->gpr[1]>=1 && cpu->gpr[1]<=4) {
        cpu->exclusive_valid=0;syscall_unlocked=1;syscall_abort=0;
        uint64_t result=fs_call(25);syscall_unlocked=0;
        if(syscall_abort)return 0;
        cpu->gpr[0]=result;return 1;
    }
    /* sendfile of a file at a given offset to a socket (nginx, per response): the
       host's sendfile on this process's descriptors; no file offset moves. */
    if(call==71 && socket && cpu->gpr[2] && fs_host(cpu->gpr[1])>=0) {
        cpu->exclusive_valid=0;syscall_unlocked=1;syscall_abort=0;net_unlocked_wait.count=0;
        uint64_t result=guest_sendfile();syscall_unlocked=0;
        if(syscall_abort || result==IO_BLOCK)return 0;
        cpu->gpr[0]=result;return 1;
    }
    if(!epoll && !named && !((call==62 || call==63 || call==64 || call==65 || call==66 || call==67 || call==68 || call==80 || call==82 || call==83 || (call==88 && !cpu->gpr[1])) && file) &&
       !((call==63 || call==64 || call==65 || call==206 || call==207 || (call==66 && !io_managed(fd))) && socket))return 0;
    cpu->exclusive_valid=0;
    int clamp=(call==63 || call==64 || call==206 || call==207) && count>1024*1024 && fs_host(cpu->gpr[0])<0; /* files: fs_transfer_at */
    if(clamp)cpu->gpr[2]=1024*1024;
    syscall_unlocked=1;syscall_abort=0;epoll_prepared.valid=0;net_unlocked_wait.count=0;
    uint64_t result;
    if(!((named || file) && !epoll && fs_call_detached(call,named?path:NULL,&result,0)))
        result=epoll?guest_epoll(call):call==65?guest_readv():call==66?guest_writev():named || file?fs_call(call):net_call(call);
    syscall_unlocked=0;
    if(clamp)cpu->gpr[2]=count;
    if(syscall_abort || result==IO_BLOCK || mem_redirect_take())return 0;   /* reached memfs: the locked path serves it */
    cpu->gpr[0]=result;return 1;
}
/* mmap, munmap, mprotect, madvise and brk change only the worker's own address
   space, its backings (atomic reference counts) and the atomic memory charge, so
   they too run without the runtime lock; the backend then syncs its tables. */
static int guest_memory_unlocked(void) {
    uint64_t call=cpu->gpr[8];
    if(!dynamic_enabled || trace)return 0;
    if(call!=222 && call!=215 && call!=226 && call!=233 && call!=214)return 0;
    cpu->exclusive_valid=0;
    cpu->gpr[0]=call==222?guest_dynamic_mmap():call==215?guest_munmap(cpu->gpr[0],cpu->gpr[1]):call==226?guest_mprotect():call==233?guest_madvise():guest_brk(cpu->gpr[0]);
    return 1;
}
#endif
static int guest_syscall(void) {
    uint64_t call=cpu->gpr[8],count=cpu->gpr[2];
    int clamp=(call==63 || call==64 || call==206 || call==207) && count>1024*1024 && fs_host(cpu->gpr[0])<0; /* files: fs_transfer_at */
    if(clamp)cpu->gpr[2]=1024*1024;
    net_wait.targeted=0; /* set again only by this call's own net_suspend */
    int result=guest_syscall_dispatch();
    if(result==-3 && net_wait.active && net_wait.at_pc==cpu->pc)net_wait.active=0; /* a wait it retried is over */
    if(clamp)cpu->gpr[2]=count;
    if(trace && result==-3 && cpu->gpr[0]>=(uint64_t)-4095)                /* failures, with what they were given */
        fprintf(stderr,"pid %" PRIu64 " syscall %" PRIu64 " failed: %" PRId64 "\n",process_pid,call,(int64_t)cpu->gpr[0]);
    return result;
}
