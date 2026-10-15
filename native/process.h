static void seccomp_inherit(uint64_t child,uint64_t parent); /* seccomp.h */
/* Bounded guest processes; optional scheduled trees. No host process is spawned. */
/* Thread-local: each scheduler worker runs its own current process. */
static _Thread_local uint64_t process_steps,process_pid=1;
static _Thread_local unsigned process_depth;
static unsigned process_count,process_exec_count;
static int process_tree_enabled;
static struct { uint64_t pid,parent; unsigned status,live,event_status; } process_children[PROCESS_LIMIT];
static int run(uint64_t steps);
static int schedule_run(uint64_t steps);
static void schedule_child(uint64_t pid);
static void schedule_vfork(uint64_t pid);
static void signal_deliver(int blocked);
static void schedule_exec_reset(void);
static int process_threaded(void);
static unsigned schedule_live;
typedef struct SharedImage SharedImage;
typedef struct {
    SharedImage *shared;
    GuestAddressSpace memory;
    GuestCpuState cpu;
    unsigned mask;
} ProcessImage;
struct SharedImage { unsigned refs;ProcessImage state; };
/* A process's memory lives in one place that every thread uses: its own image
   (image_home, set when a worker loads the process) or, for a vfork pair, the
   shared image. ProcessImage copies made by fork and exec are snapshots, not
   independently owning deep copies; those paths duplicate or share backings. */
static _Thread_local GuestAddressSpace *image_home_scheduled=&runtime_context.memory;
#define image_home (schedule_active?image_home_scheduled:address_space) /* unscheduled: wherever memory is now */
static GuestAddressSpace *image_place(SharedImage *shared) { return shared?&shared->state.memory:image_home; }
static void image_save(ProcessImage *im) {
    im->shared=shared_image;im->cpu=*cpu;im->mask=fs_umask;
    if(&im->memory!=address_space) { im->memory=*address_space;memset(im->memory.lookup_cache,0,sizeof(im->memory.lookup_cache)); }
}
static void image_restore(const ProcessImage *im) {
    GuestAddressSpace *place=image_place(im->shared);
    const GuestAddressSpace *source=im->shared?&im->shared->state.memory:&im->memory;
    shared_image=im->shared;address_space=place;
    if(source!=place)*place=*source;
    *cpu=im->cpu;fs_umask=im->mask;
    address_space_touched(address_space);cpu->exclusive_valid=0;
}
/* Leave any shared image alone: the other sharer keeps using it. */
static void image_clear(void) {
    shared_image=NULL;address_space=image_home;address_space_reset(address_space);memset(cpu,0,sizeof(*cpu));
}
static int image_destroy_later(GuestAddressSpace *space); /* scheduler.h: after the runtime lock is released */
static void image_free(void) {
    if(shared_image) {
        SharedImage *shared=shared_image;
        if(--shared->refs) { image_clear();return; }
        if(address_space!=image_home)*image_home=shared->state.memory;   /* the last sharer takes the memory */
        shared_image=NULL;free(shared);address_space=image_home;
    }
    if(!image_destroy_later(address_space))address_space_destroy(address_space);
    memset(cpu,0,sizeof(*cpu));
}
/* Strings may contain arbitrary bytes; bounded pointers are never host pointers. */
static int process_vector(uint64_t ptr,char **out,unsigned *count,char *buffer,size_t *used) {
    *count=0;if(!ptr)return 0;
    for(unsigned i=0;i<=EXEC_ENTRIES;i++) {
        if(ptr>UINT64_MAX-i*8)return 14;
        unsigned char *p=lookup(ptr+i*8,8,4);if(!p)return 14;
        uint64_t addr=le(p,8);if(!addr)return 0;if(i==EXEC_ENTRIES)return 7;
        out[(*count)++]=buffer+*used;
        for(uint64_t j=0;;j++) {
            if(*used==EXEC_STRINGS)return 7;
            if(addr>UINT64_MAX-j)return 14;
            p=lookup(addr+j,1,4);if(!p)return 14;
            buffer[(*used)++]=(char)*p;if(!*p)break;
        }
    }
    return 7;
}
/* A child slot is reusable once the child was reaped (pid cleared), its task is
   gone and no process group is named after it; its pid (2+slot) is then reused,
   as Linux eventually reuses pids. Fresh slots extend process_count. */
static int process_slot_free(unsigned slot);
static unsigned process_slot(void) {
    for(unsigned i=0;i<process_count;i++)if(process_slot_free(i))return i;
    return process_count<PROCESS_LIMIT?process_count:PROCESS_LIMIT;
}
/* execve (path, argv, envp in x0..x2), or execveat (dirfd in x0, the rest from x1: r=1),
   whose path may be relative to dirfd. */
static uint64_t process_exec_at(uint64_t dir,unsigned r) {
    char leaf[1025],exe[1025];int parent=fs_lookup(dir,cpu->gpr[r],leaf,1|FS_DIRECT,exe);if(parent<0)return fs_error();
    int fd=openat(parent,leaf,O_RDONLY|FS_NOFOLLOW_OPEN|O_NONBLOCK|O_CLOEXEC);
    int saved=errno;close(parent);errno=saved;if(fd<0)return fs_error();
    struct stat st;
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || !(st.st_mode&0111)) { close(fd);return linux_error(13); }
    static char buffer[EXEC_STRINGS],*args[EXEC_ENTRIES],*env[EXEC_ENTRIES],*rebuilt[EXEC_ENTRIES];unsigned argc,envc;size_t used=0;
    int error=process_vector(cpu->gpr[r+1],args,&argc,buffer,&used);
    if(!error)error=process_vector(cpu->gpr[r+2],env,&envc,buffer,&used);
    if(error || !argc) { close(fd);return linux_error(error?error:22); }
    /* "#!INTERPRETER [ONE-ARGUMENT]" runs INTERPRETER [ARG] SCRIPT ARGV[1..], as execve does.
       An interpreter may itself be a script (Oracle Linux's /usr/bin/env is), four
       levels deep at most, as on Linux. */
    char script[1025];if(fs_string(cpu->gpr[r],script)<0) { close(fd);return linux_error(14); }
    for(unsigned depth=0;;depth++) {
        char head[256];ssize_t got=pread(fd,head,sizeof(head)-1,0);
        if(got<2 || head[0]!='#' || head[1]!='!')break;
        if(depth==4) { close(fd);return linux_error(40); }                           /* ELOOP */
        head[got]=0;char *end=strchr(head,'\n');
        if(!end && got==(ssize_t)sizeof(head)-1) { close(fd);return linux_error(8); }
        if(end)*end=0;
        char *p=head+2;while(*p==' ' || *p=='\t')p++;
        char *interpreter=p;while(*p && *p!=' ' && *p!='\t' && *p!='\r')p++;
        char *argument=NULL;
        if(*p) { *p++=0;while(*p==' ' || *p=='\t')p++;if(*p) { argument=p;char *t=p+strlen(p);while(t>p && (t[-1]==' ' || t[-1]=='\t' || t[-1]=='\r'))*--t=0; } }
        if(interpreter[0]!='/' || strlen(interpreter)>1024 || argc+(argument?2u:1u)>EXEC_ENTRIES) { close(fd);return linux_error(8); }
        char *extra[3]={interpreter,argument,script};unsigned n=0;
        for(unsigned i=0;i<3;i++) {
            if(!extra[i])continue;
            size_t length=strlen(extra[i])+1;if(used+length>sizeof(buffer)) { close(fd);return linux_error(7); }
            memcpy(buffer+used,extra[i],length);rebuilt[n++]=buffer+used;used+=length;
        }
        for(unsigned i=1;i<argc;i++)rebuilt[n++]=args[i];
        memcpy(args,rebuilt,n*sizeof(*args));argc=n;
        strcpy(script,interpreter);                                                   /* the next level's script */
        close(fd);
        char leaf2[1025];int dir=fs_walk(fs_root,"/",script,1,leaf2,exe);if(dir<0)return fs_error(); /* /proc/self/exe is the interpreter */
        fd=openat(dir,leaf2,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);saved=errno;close(dir);errno=saved;if(fd<0)return fs_error();
        if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || !(st.st_mode&0111)) { close(fd);return linux_error(8); }
    }
    FILE *input=fdopen(fd,"rb");if(!input) { close(fd);return linux_error(12); }
    /* A set-user-ID or set-group-ID file runs with its owner as effective and saved
       ID, unless no_new_privs is set. ponytail: the owner is root (image files are;
       per-file ownership is not tracked), and no mount is nosuid. */
    GuestIds caller=guest_ids;
    if(!prctl_no_new_privs && (st.st_mode&S_ISUID))guest_ids.uid=guest_ids.suid=0;
    if(!prctl_no_new_privs && (st.st_mode&S_ISGID))guest_ids.gid=guest_ids.sgid=0;
    ProcessImage old;image_save(&old);image_clear();
    /* Loader failures must leave the calling image intact. load_stream closes input. */
    exec_loading=1;
    if(setjmp(exec_recovery)) {
        exec_loading=0;image_free();image_restore(&old);guest_ids=caller;return linux_error(8);
    }
    load_stream(input);heap_init();startup((int)argc,(const char **)args,envc,(const char **)env);
    exec_loading=0;process_exec_count++;
    if(process_pid<=PROCESS_LIMIT+1)snprintf(proc_exe[process_pid],sizeof(proc_exe[0]),"%s",strlen(exe)<sizeof(proc_exe[0])?exe:"");
    ProcessImage next;image_save(&next);image_restore(&old);image_free();image_restore(&next);
    /* M19 preserves regular guest descriptors. CLOEXEC is tracked by the FS adapter. */
    for(unsigned i=0;i<FS_SLOTS;i++)if(fs_fd[i] && fs_cloexec[i]) {
        if(fs_dirs[i]) { fs_directory_release(fs_dirs[i]);fs_dirs[i]=NULL; }
        close(fs_fd[i]-1);fs_fd[i]=0;fs_cloexec[i]=0;fd_unbind(fs_guest[i]);
    }
    if(guest_uid)cap_held[0]=cap_held[1]=0; /* no file capabilities: a non-root exec loses them */
    else if(caller.uid) { cap_held[0]=UINT32_MAX;cap_held[1]=0x1ffu; } /* a set-user-ID-root exec gains them all */
    /* Sockets survive exec unless SOCK_CLOEXEC/FD_CLOEXEC is set, as on Linux. */
    for(unsigned i=0;i<NET_SLOTS;i++)if(net_fd[i] && (net_flags[i]&524288))(void)net_release(i);
    net_wait.active=0;
    io_exec();
    if(schedule_active)schedule_exec_reset();
    return 0;
}
static uint64_t process_clone(void) {
    if(epoll_live() && !schedule_active)return linux_error(95); /* scheduled children share inherited epoll instances */
    /* vfork shares memory, but never files or signal handlers. The scheduler
       holds the parent until child exec or exit. General threads remain unsupported. */
    unsigned vfork=cpu->gpr[0]==(17|0x100|0x4000);
    /* Other threads keep running during a threaded process's vfork, so its child
       gets a private copy (like fork) and no thread waits for it. */
    unsigned share=vfork && !process_threaded();
    /* glibc's fork adds CHILD_SETTID|CHILD_CLEARTID|PARENT_SETTID (0x1300000): the child
       PID goes to child_tid in the child and parent_tid in the parent. CLEARTID has no
       observer once a private child exits. */
    uint64_t flags=cpu->gpr[0],parent_tid=cpu->gpr[2],child_tid=cpu->gpr[4];
    if(vfork) {
        if(!schedule_active)return linux_error(95);
        if(cpu->gpr[1] && ((cpu->gpr[1]&15) || !lookup(cpu->gpr[1]-1,1,2)))return linux_error(22);
    } else if((flags&~UINT64_C(0x1300000))!=17 || cpu->gpr[1])return linux_error(95);
    if(!vfork && (((flags&0x100000) && !lookup(parent_tid,4,2)) || ((flags&0x1000000) && !lookup(child_tid,4,2))))return linux_error(14);
    uint64_t child_stack=cpu->gpr[1];
    unsigned slot=process_slot();
    if((process_depth && !process_tree_enabled) || slot==PROCESS_LIMIT || (schedule_active && schedule_live==PROCESS_LIMIT))return linux_error(11);
    /* A scheduled child keeps its own socket table (task_save) sharing each open
       description; the unscheduled path runs the child inline and then clears the
       socket table, so it still refuses sockets. */
    if(!schedule_active)for(unsigned i=0;i<NET_SLOTS;i++)if(net_fd[i])return linux_error(95);
    for(unsigned i=0;i<FS_SLOTS;i++)if(fs_fd[i]) {
        struct stat st;if(fstat(fs_fd[i]-1,&st)<0)return fs_error();
        if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISCHR(st.st_mode))return linux_error(95);
        /* Directory handles require the shared cursor allocated by fs_open. */
        if(S_ISDIR(st.st_mode) && !fs_dirs[i])return linux_error(95);
    }
    GuestDirectory *parent_dirs[FS_SLOTS];memcpy(parent_dirs,fs_dirs,sizeof(fs_dirs));
    int parent_fds[FS_SLOTS],child_fds[FS_SLOTS]={0};unsigned char parent_cloexec[FS_SLOTS];
    memcpy(parent_fds,fs_fd,sizeof(fs_fd));memcpy(parent_cloexec,fs_cloexec,sizeof(fs_cloexec));
    for(unsigned i=0;i<FS_SLOTS;i++)if(fs_fd[i]) {
        int fd=dup(fs_fd[i]-1);
        if(fd<0) { for(unsigned j=0;j<i;j++)if(child_fds[j])close(child_fds[j]-1);return linux_error(24); }
        child_fds[i]=fd+1;
    }
    GuestIO parent_io[IO_END];
    if(io_clone(parent_io)<0) { for(unsigned i=0;i<FS_SLOTS;i++)if(child_fds[i])close(child_fds[i]-1);return linux_error(24); }
    GuestCwd *parent_cwd=fs_cwd;
    if(share && !shared_image) {
        shared_image=calloc(1,sizeof(*shared_image));
        if(!shared_image) {
            io_restore(parent_io);for(unsigned i=0;i<FS_SLOTS;i++)if(child_fds[i])close(child_fds[i]-1);
            return linux_error(12);
        }
        shared_image->refs=1;
        shared_image->state.memory=*address_space;address_space=&shared_image->state.memory; /* parent and child use it from now on */
    }
    ProcessImage parent,child;image_save(&parent);child=parent;
    if(share)shared_image->refs++;
    else {
        child.shared=NULL;
        if(!address_space_fork_private(&child.memory,&parent.memory)) {
            io_restore(parent_io);
            for(unsigned i=0;i<FS_SLOTS;i++)if(child_fds[i])close(child_fds[i]-1);
            return linux_error(12);
        }
    }
    if(schedule_active)net_retain_all();
    if(fs_cwd)fs_cwd->refs++;
    for(unsigned i=0;i<FS_SLOTS;i++)if(fs_dirs[i])fs_dirs[i]->refs++;
    image_restore(&child);
    if(vfork && child_stack)cpu->sp=child_stack;
    memset(address_space->lookup_cache,0,sizeof(address_space->lookup_cache));memcpy(fs_fd,child_fds,sizeof(fs_fd));
    uint64_t pid=2+slot;cpu->gpr[0]=0;
    memcpy(proc_exe[pid],proc_exe[process_pid],sizeof(proc_exe[0])); /* /proc/<pid>/exe and cmdline are inherited */
    memcpy(proc_cmdline[pid],proc_cmdline[process_pid],sizeof(proc_cmdline[0]));proc_cmdline_size[pid]=proc_cmdline_size[process_pid];
    proc_start(pid);                                                                   /* its start time and CPU time */
    seccomp_inherit(pid,process_pid);                                                  /* seccomp filters are inherited */
    if(!vfork && (flags&0x1000000))fs_pack(lookup(child_tid,4,2),0,(unsigned)pid,4); /* child's private memory */
    if(slot==process_count)process_count++;
    memset(&process_children[slot],0,sizeof(process_children[slot]));
    if(schedule_active) {
        schedule_child(pid);proc_publish(0);if(share)schedule_vfork(pid);fs_cwd=parent_cwd;image_restore(&parent);
        memcpy(io_fds,parent_io,sizeof(io_fds));memcpy(fs_fd,parent_fds,sizeof(fs_fd));
        memcpy(fs_cloexec,parent_cloexec,sizeof(fs_cloexec));
        process_children[slot].pid=pid;process_children[slot].parent=process_pid;process_children[slot].live=1;
        if(!vfork && (flags&0x100000))fs_pack(lookup(parent_tid,4,2),0,(unsigned)pid,4);
        return pid;
    }
    process_depth=1;process_pid=pid;
    GuestIds parent_ids=guest_ids;
    static unsigned char parent_kind[GUEST_FD_LIMIT];static unsigned short parent_index[GUEST_FD_LIMIT],parent_net[NET_SLOTS],parent_fs[FS_SLOTS],parent_io_guest[IO_END];
    memcpy(parent_kind,fd_kind,sizeof(fd_kind));memcpy(parent_index,fd_index,sizeof(fd_index));
    memcpy(parent_net,net_guest,sizeof(net_guest));memcpy(parent_fs,fs_guest,sizeof(fs_guest));memcpy(parent_io_guest,io_guest,sizeof(io_guest));
    unsigned status=(unsigned)run(0);
    guest_ids=parent_ids; /* the child's identity changes stay in the child */
    fs_cwd_release();fs_cwd=parent_cwd;image_free();image_restore(&parent);
    io_restore(parent_io);
    net_cleanup();memset(net_fd,0,sizeof(net_fd));
    for(unsigned i=0;i<FS_SLOTS;i++) {
        if(fs_dirs[i]) { fs_directory_release(fs_dirs[i]);fs_dirs[i]=NULL; }
        if(fs_fd[i])close(fs_fd[i]-1);

    }
    memcpy(fs_fd,parent_fds,sizeof(fs_fd));memcpy(fs_cloexec,parent_cloexec,sizeof(fs_cloexec));memcpy(fs_dirs,parent_dirs,sizeof(fs_dirs));
    memcpy(fd_kind,parent_kind,sizeof(fd_kind));memcpy(fd_index,parent_index,sizeof(fd_index));
    memcpy(net_guest,parent_net,sizeof(net_guest));memcpy(fs_guest,parent_fs,sizeof(fs_guest));memcpy(io_guest,parent_io_guest,sizeof(io_guest));
    process_depth=0;process_pid=1;
    process_children[slot].pid=pid;process_children[slot].parent=1;process_children[slot].status=status<<8;
    if(flags&0x100000)fs_pack(lookup(parent_tid,4,2),0,(unsigned)pid,4);
    return pid;
}
static uint64_t process_wait(void) {
    cpu->gpr[2]&=~UINT64_C(0x60000000);                                   /* __WALL, __WNOTHREAD: no effect here */
    if(cpu->gpr[2]&~(job_control_enabled?UINT64_C(11):UINT64_C(1)))return linux_error(95);
    if(cpu->gpr[3]) { unsigned char *usage=lookup(cpu->gpr[3],144,2);if(!usage)return linux_error(14);memset(usage,0,144); } /* rusage: not measured */
    int64_t pid=(int64_t)cpu->gpr[0];if(pid<=0)pid=-1;                    /* ponytail: a process group waits for any child */
    if(process_depth && !process_tree_enabled)return linux_error(10);
    unsigned live=0;
    if(cpu->gpr[1] && !lookup(cpu->gpr[1],4,2))return linux_error(14);
    for(unsigned i=0;i<process_count;i++)if(process_children[i].pid && process_children[i].parent==process_pid && (pid==-1 || (uint64_t)pid==process_children[i].pid)) {
        if(process_children[i].live) {
            unsigned event=process_children[i].event_status;
            if(event && ((event==65535 && (cpu->gpr[2]&8)) || (event!=65535 && (cpu->gpr[2]&2)))) {
                if(cpu->gpr[1])fs_pack(lookup(cpu->gpr[1],4,2),0,event,4);
                process_children[i].event_status=0;return process_children[i].pid;
            }
            live=1;continue;
        }
        if(cpu->gpr[1]) {
            unsigned char *p=lookup(cpu->gpr[1],4,2);if(!p)return linux_error(14);
            fs_pack(p,0,process_children[i].status,4);
        }
        uint64_t result=process_children[i].pid;process_children[i].pid=0;return result;
    }
    return live?((cpu->gpr[2]&1)?0:IO_BLOCK):linux_error(10);
}

/* waitid(idtype, id, siginfo, options): P_ALL 0 or P_PID 1 (P_PGID 2 as any child);
   WEXITED 4 (and WSTOPPED 2, WCONTINUED 8 under job control), WNOHANG 1, WNOWAIT
   0x1000000 (look, do not reap: Go checks waitability so). */
static uint64_t process_waitid(void) {
    uint64_t type=cpu->gpr[0],id=cpu->gpr[1],info=cpu->gpr[2],options=cpu->gpr[3]&~UINT64_C(0x60000000);
    if(type>2 || !(options&(4|2|8)) || options&~UINT64_C(0x100000f))return linux_error(22);
    if(process_depth && !process_tree_enabled)return linux_error(10);
    unsigned char *out=info?lookup(info,128,2):NULL;if(info && !out)return linux_error(14);
    if(cpu->gpr[4]) { unsigned char *usage=lookup(cpu->gpr[4],144,2);if(!usage)return linux_error(14);memset(usage,0,144); }
    unsigned live=0;
    for(unsigned i=0;i<process_count;i++) {
        if(!process_children[i].pid || process_children[i].parent!=process_pid || (type==1 && id!=process_children[i].pid))continue;
        if(process_children[i].live) { live=1;continue; }
        if(!(options&4)) { live=1;continue; }
        unsigned status=process_children[i].status,signal=status&0x7f;
        if(out) {
            memset(out,0,128);fs_pack(out,0,17,4);                           /* SIGCHLD */
            fs_pack(out,8,signal?((status&0x80)?3:2):1,4);                  /* CLD_DUMPED, CLD_KILLED, CLD_EXITED */
            fs_pack(out,16,process_children[i].pid,4);fs_pack(out,20,guest_uid,4);
            fs_pack(out,24,signal?signal:(status>>8)&0xff,4);
        }
        if(!(options&0x1000000))process_children[i].pid=0;
        return 0;
    }
    if(!live)return linux_error(10);
    if(options&1) { if(out)memset(out,0,128);return 0; }
    return IO_BLOCK;
}

/* capget/capset (versions 2 and 3): root holds every capability (0..40), others
   none. capset may keep or drop within that, never add. */
static uint64_t guest_capability(int set) {
    unsigned char *header=lookup(cpu->gpr[0],8,set?4:2);if(!header)return linux_error(14);
    uint32_t version=(uint32_t)le(header,4);int32_t pid=(int32_t)le(header+4,4);
    if(version!=0x20071026 && version!=0x20080522) {
        if(!set) { unsigned char *w=lookup(cpu->gpr[0],4,2);if(w)fs_pack(w,0,0x20080522,4); }
        return cpu->gpr[1] || set?linux_error(22):0;
    }
    if(pid<0 || (pid && (uint64_t)pid!=process_pid))return set?linux_error(1):linux_error(3);
    uint32_t *held=cap_held;
    if(!cpu->gpr[1])return set?linux_error(14):0;
    unsigned char *data=lookup(cpu->gpr[1],24,set?4:2);if(!data)return linux_error(14);
    if(set) {
        for(unsigned i=0;i<2;i++)for(unsigned k=0;k<3;k++)if((uint32_t)le(data+i*12+k*4,4)&~held[i])return linux_error(1);
        for(unsigned i=0;i<2;i++)held[i]=(uint32_t)le(data+i*12+4,4); /* the new permitted set */
    } else for(unsigned i=0;i<2;i++) { fs_pack(data,i*12,held[i],4);fs_pack(data,i*12+4,held[i],4);fs_pack(data,i*12+8,0,4); }
    return 0;
}

#include "scheduler.h"
