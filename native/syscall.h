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
static uint64_t io_signals_pending(void) { return schedule_active?schedule_tasks[process_pid-1].signal_pending:0; }
static unsigned io_signal_take(unsigned signal) {
    ScheduledTask *t=&schedule_tasks[process_pid-1];t->signal_pending&=~(UINT64_C(1)<<(signal-1));return t->signal_sender[signal-1];
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
        fs_fd[slot]=host+1;fs_cloexec[slot]=(unsigned char)(cloexec!=0);fs_ro[slot]=fs_ro[from];fs_dirs[slot]=fs_dirs[from];
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
static void net_credentials(unsigned char out[12]) {
    for(unsigned i=0;i<4;i++) { out[i]=(unsigned char)(process_pid>>(8*i));out[4+i]=(unsigned char)(guest_uid>>(8*i));out[8+i]=(unsigned char)(guest_gid>>(8*i)); }
}
/* Shared Linux syscall boundary: -3 continues, -2 blocks, -1 yields, >=0 exits. */
static int guest_syscall_dispatch(void) {
            uint64_t call=cpu->gpr[8];cpu->exclusive_valid=0;
            if(profiling && call<512) profile_syscalls[call]++;
            if(trace) fprintf(stderr,"syscall %" PRIu64 ": x0..5=[%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n",call,cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3],cpu->gpr[4],cpu->gpr[5]);
            if(call==139 && job_control_enabled) { signal_return();if(schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal)return -1;return -3; }
            if(call==134 || call==135) { cpu->gpr[0]=signal_call(call);if(schedule_active && (schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal))return -1;return -3; }
            if(call==71) { cpu->gpr[0]=linux_error(38);return -3; } /* sendfile: callers may use read/write fallback. */
            if(call==160) {
                unsigned char *out=lookup(cpu->gpr[0],390,2);
                if(!out)cpu->gpr[0]=linux_error(14);
                else {
                    const char *fields[]={"Linux",guest_hostname,"0.0.0-vmfree","VM-free interpreter","aarch64","localdomain"};
                    memset(out,0,390);for(unsigned i=0;i<6;i++)memcpy(out+i*65,fields[i],strlen(fields[i]));cpu->gpr[0]=0;
                }
                return -3;
            }
            if(call>=174 && call<=177) { cpu->gpr[0]=call<=175?guest_uid:guest_gid;return -3; } /* One mapped guest identity; no host privilege change. */
            if(call==151 || call==152) { cpu->gpr[0]=call==151?guest_uid:guest_gid;return -3; } /* setfsuid/setfsgid: file access uses the one identity; report it */
            if(call==144 || call==146 || call==147 || call==149) {
                /* setgid, setuid, setresuid, setresgid: guest uid 0 may switch to any
                   identity (as with CAP_SETUID/CAP_SETGID); others may only re-assert
                   their own. -1 leaves a setres* field unchanged. */
                int user=call==146 || call==147;uint32_t current=user?guest_uid:guest_gid,next=current;
                for(unsigned i=0;i<(call<147?1u:3u);i++) {
                    uint32_t v=(uint32_t)cpu->gpr[i];if(v==UINT32_MAX && call>=147)continue;
                    if(guest_uid && !(cap_held[0]&(user?(1u<<7):(1u<<6))) && v!=current) { cpu->gpr[0]=linux_error(1);return -3; } /* CAP_SETUID/CAP_SETGID */
                    next=v; /* real, effective and saved IDs are one value here */
                }
                if(user) { cap_after_setuid(guest_uid,next);guest_uid=next; } else guest_gid=next;
                cpu->gpr[0]=0;return -3;
            }
            if(call==148 || call==150) { /* getresuid/getresgid: real, effective and saved are one ID */
                uint32_t id=call==148?guest_uid:guest_gid;uint64_t result=0,ptr[3]={cpu->gpr[0],cpu->gpr[1],cpu->gpr[2]};
                for(unsigned i=0;i<3;i++) { unsigned char *out=lookup(ptr[i],4,2);if(!out) { result=linux_error(14);break; }fs_pack(out,0,id,4); }
                cpu->gpr[0]=result;return -3;
            }
            if(call==159) { cpu->gpr[0]=guest_uid && !(cap_held[0]&(1u<<6))?linux_error(1):cpu->gpr[0]>65536?linux_error(22):0;return -3; } /* setgroups: no supplementary groups are kept */
            if(call==158) { cpu->gpr[0]=0;return -3; }                                                          /* getgroups: none */
            if(call==54 || call==55) {
                /* fchownat/fchown: ownership is not tracked (every file shows the current
                   identity), so a valid request by root succeeds without effect. */
                int fd=-1;struct stat st;
                int writable;
                if(call==55) { fd=fs_host(cpu->gpr[0]);if(fd<0) { cpu->gpr[0]=linux_error(9);return -3; }writable=!fs_ro[fs_slot(cpu->gpr[0])]; }
                else {
                    if(cpu->gpr[4]&~UINT64_C(256|4096)) { cpu->gpr[0]=linux_error(22);return -3; }
                    char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(cpu->gpr[4]&256),NULL);
                    if(parent<0) { cpu->gpr[0]=fs_error();return -3; }
                    writable=fs_path_writable();
                    int r=fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW);int saved=errno;close(parent);errno=saved;
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
            if(call==157) { /* setsid: a new session (tracked as its process group); a group leader gets EPERM */
                unsigned *group=schedule_active?&schedule_tasks[process_pid-1].pgid:NULL;
                if(!group || *group==process_pid)cpu->gpr[0]=linux_error(1);
                else { *group=(unsigned)process_pid;io_epoch++;cpu->gpr[0]=process_pid; }
                return -3;
            }
            if(call==156) { /* getsid: the session is reported as the process group */
                uint64_t pid=cpu->gpr[0]?cpu->gpr[0]:process_pid;
                if(!schedule_active)cpu->gpr[0]=pid==process_pid?process_pid:linux_error(3);
                else if(pid>process_count+1 || !schedule_tasks[pid-1].state)cpu->gpr[0]=linux_error(3);
                else cpu->gpr[0]=schedule_tasks[pid-1].pgid;
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
            if(io_managed(cpu->gpr[0]) && (call==57 || call==63 || call==64 || call==25 || call==29 || call==80 || call==62)) { uint64_t result=io_call(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==220) { cpu->gpr[0]=process_clone();return -3; }
            if(call==221) { uint64_t result=process_exec();if(result)cpu->gpr[0]=result;return -3; }
            if(call==260) { uint64_t result=process_wait();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==172 || call==173) { cpu->gpr[0]=call==172?process_pid:(process_depth?process_children[process_pid-2].parent:0);return -3; }
            if(call==178) { cpu->gpr[0]=process_pid;return -3; } /* gettid: one thread per interpreted process */
            if(call==124) { cpu->gpr[0]=0;if(schedule_active)schedule_slice=0;return -3; } /* sched_yield ends the slice */
            if(call==65) { uint64_t result=guest_readv();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==66) { uint64_t result=guest_writev();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==17) { cpu->gpr[0]=fs_getcwd();return -3; }
            if(call==50) { cpu->gpr[0]=fs_fchdir();return -3; }
            if(call==49) { cpu->gpr[0]=fs_chdir();return -3; }
            if(call==29) { cpu->gpr[0]=(cpu->gpr[0]<=2 || fs_host(cpu->gpr[0])>=0 || net_host(cpu->gpr[0])>=0)?linux_error(25):linux_error(9);return -3; }
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
            if(call==123) { /* sched_getaffinity: one guest CPU per worker (the interpreter has one). */
                if(cpu->gpr[0] && cpu->gpr[0]!=process_pid)cpu->gpr[0]=linux_error(3);
                else if(cpu->gpr[1]<8 || (cpu->gpr[1]&7))cpu->gpr[0]=linux_error(22);
                else {
                    unsigned char *out=lookup(cpu->gpr[2],8,2);
                    if(!out)cpu->gpr[0]=linux_error(14);
                    else { unsigned n=workers_supported?worker_count:1;fs_pack(out,0,n>=64?UINT64_MAX:(UINT64_C(1)<<n)-1,8);cpu->gpr[0]=8; }
                }
                return -3;
            }
            if(call==96) { cpu->gpr[0]=process_pid; return -3; } /* Single guest thread; no futex waiters. */
            /* Optional Linux facilities: advertise absence so libc can use its
               fallback. Never report successful robust-list/rseq registration. */
            if(call==99 || call==293) { cpu->gpr[0]=linux_error(38);return -3; }
            if(call==167) { cpu->gpr[0]=guest_prctl();return -3; }
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
            if(call==261) { /* prlimit64: query the fixed guest stack (3) and descriptor (7) limits. */
                if(cpu->gpr[0] && cpu->gpr[0]!=process_pid)cpu->gpr[0]=linux_error(3);
                else if((cpu->gpr[1]!=3 && cpu->gpr[1]!=7) || cpu->gpr[2])cpu->gpr[0]=linux_error(95);
                else if(!cpu->gpr[3])cpu->gpr[0]=0;
                else {
                    unsigned char *out=lookup(cpu->gpr[3],16,2);uint64_t limit=cpu->gpr[1]==7?GUEST_FD_LIMIT:STACK_SIZE;
                    if(!out)cpu->gpr[0]=linux_error(14);
                    else { fs_pack(out,0,limit,8);fs_pack(out,8,limit,8);cpu->gpr[0]=0; }
                }
                return -3;
            }
            if(call==214) { cpu->gpr[0]=guest_brk(cpu->gpr[0]); return -3; }
            if(call==222) { cpu->gpr[0]=dynamic_enabled?guest_dynamic_mmap():guest_mmap(cpu->gpr[1],cpu->gpr[2],cpu->gpr[3],cpu->gpr[5]); return -3; }
            if(call==226 && dynamic_enabled) { cpu->gpr[0]=guest_mprotect();return -3; }
            if(call==215) { cpu->gpr[0]=guest_munmap(cpu->gpr[0],cpu->gpr[1]); return -3; }
            if(call==166 || call==52 || call==53 || call==78 || call==88 || call==36 || call==37 || call==48 || call==439 || call==45 || call==82 || call==83 || call==61 || (call==25 && fs_host(cpu->gpr[0])>=0) || call==79 || call==56 || call==34 || call==35 || call==38 || call==46 || call==62 || call==80 || ((call==57 || call==63 || call==64 || call==67 || call==68 || call==69 || call==70 || call==286 || call==287 || call==47 || call==84) && fs_host(cpu->gpr[0])>=0)) { cpu->gpr[0]=fs_call(call); return -3; }
            if((call>=67 && call<=70) || call==286 || call==287 || call==47 || call==84) { /* positioned I/O needs a regular file */
                uint64_t fd=cpu->gpr[0];cpu->gpr[0]=net_host(fd)>=0 || (io_managed(fd) && IO(fd).kind)?linux_error(29):linux_error(9);return -3;
            }
            if(call==101 || call==115) { uint64_t result=guest_sleep(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==73) { uint64_t result=net_poll();if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call==198 && schedule_active && !net_scheduled) { cpu->gpr[0]=linux_error(95);return -3; }
            if(call==198) { cpu->gpr[0]=net_socket(cpu->gpr[0],cpu->gpr[1],cpu->gpr[2]); return -3; }
            if(call==199) { cpu->gpr[0]=schedule_active && !net_scheduled?linux_error(95):net_socketpair(cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3]);return -3; }
            if(call==243) { cpu->gpr[0]=linux_error(38);return -3; } /* recvmmsg */
            if((call==206 || call==207 || call==211 || call==212 || call==269) && net_host(cpu->gpr[0])<0) { /* sendto/recvfrom/sendmsg/recvmsg/sendmmsg need a socket */
                uint64_t fd=cpu->gpr[0];cpu->gpr[0]=(fs_host(fd)>=0 || (io_managed(fd) && IO(fd).kind))?linux_error(88):linux_error(9);return -3;
            }
            if(call==57 || call==63 || call==25 || call==203 || call==210 || call==242 || call==200 || call==201 || call==202 || call==204 || call==205 || call==206 || call==207 || call==208 || call==209 || call==211 || call==212 || call==269 || (call==64 && net_host(cpu->gpr[0])>=0)) { uint64_t result=net_call(call);if(result==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=result;return -3; }
            if(call!=64) {
                /* As on Linux, an unknown system call returns ENOSYS; programs probe and
                   fall back. Each number is reported once so container logs show gaps. */
                static unsigned char reported[512];
                if(call>=512 || !reported[call]) { if(call<512)reported[call]=1;fprintf(stderr,"vmfree: Linux syscall %" PRIu64 " is not implemented (ENOSYS)\n",call); }
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
static int guest_syscall_unlocked(void) {
    uint64_t call=cpu->gpr[8],fd=cpu->gpr[0],count=cpu->gpr[2];
    if(profiling || trace)return 0;
    int file=fs_host(fd)>=0 && !fs_dirs[fs_slot(fd)],socket=!file && net_host(fd)>=0;
    int epoll=call==22 && io_managed(fd) && IO(fd).kind==IO_EPOLL && epolls[IO(fd).host].refs==1;
    /* openat and newfstatat by path, except under /proc (process tables). */
    char path[1025];int named=(call==56 || call==79) && fs_string(cpu->gpr[1],path)==0 && strncmp(path,"/proc",5) && !(path[0]!='/' && fs_cwd && !strncmp(fs_cwd->path,"/proc",5));
    if(!epoll && !named && !((call==62 || call==63 || call==64 || call==65 || call==66 || call==67 || call==68 || call==80 || call==82 || call==83) && file) && !((call==63 || call==64 || call==65 || call==206 || call==207) && socket))return 0;
    cpu->exclusive_valid=0;
    int clamp=(call==63 || call==64 || call==206 || call==207) && count>1024*1024;
    if(clamp)cpu->gpr[2]=1024*1024;
    syscall_unlocked=1;syscall_abort=0;epoll_prepared.valid=0;net_unlocked_wait.count=0;
    uint64_t result=epoll?guest_epoll(call):call==65?guest_readv():call==66?guest_writev():named || file?fs_call(call):net_call(call);
    syscall_unlocked=0;
    if(clamp)cpu->gpr[2]=count;
    if(syscall_abort || result==IO_BLOCK)return 0;
    cpu->gpr[0]=result;return 1;
}
/* mmap, munmap, mprotect, madvise and brk change only the worker's own address
   space, its backings (atomic reference counts) and the atomic memory charge, so
   they too run without the runtime lock; the backend then syncs its tables. */
static int guest_memory_unlocked(void) {
    uint64_t call=cpu->gpr[8];
    if(!dynamic_enabled || profiling || trace)return 0;
    if(call!=222 && call!=215 && call!=226 && call!=233 && call!=214)return 0;
    cpu->exclusive_valid=0;
    cpu->gpr[0]=call==222?guest_dynamic_mmap():call==215?guest_munmap(cpu->gpr[0],cpu->gpr[1]):call==226?guest_mprotect():call==233?guest_madvise():guest_brk(cpu->gpr[0]);
    return 1;
}
#endif
static int guest_syscall(void) {
    uint64_t call=cpu->gpr[8],count=cpu->gpr[2];
    int clamp=(call==63 || call==64 || call==206 || call==207) && count>1024*1024;
    if(clamp)cpu->gpr[2]=1024*1024;
    net_wait.targeted=0; /* set again only by this call's own net_suspend */
    int result=guest_syscall_dispatch();
    if(clamp)cpu->gpr[2]=count;
    return result;
}
