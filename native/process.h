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
/* owner: the process whose threads use the image (hvf_share_image): every sharer's
   runtime work takes that process's mm lock and runs on its page tables, and its pid
   stays taken while the image lives (process_slot_free). 0 for a single-threaded
   vfork pair: each uses its own (the parent waits). */
struct SharedImage { unsigned refs,owner;ProcessImage state; };
/* A process's memory lives in one place that every thread uses: its own image
   (image_home, set when a worker loads the process) or the shared image: for a
   vfork pair, and for a threaded process (its vfork children share it while its
   other threads run on, as CLONE_VM does). ProcessImage copies made by fork and exec
   are snapshots, not independently owning deep copies; those paths duplicate or
   share backings. */
static _Thread_local GuestAddressSpace *image_home_scheduled=&runtime_context.memory;
#define image_home (schedule_active?image_home_scheduled:address_space) /* unscheduled: wherever memory is now */
static GuestAddressSpace *image_place(SharedImage *shared) { return shared?&shared->state.memory:image_home; }
static void image_save(ProcessImage *im) {
    im->shared=shared_image;im->cpu=*cpu;im->mask=fs_umask;
    if(&im->memory!=address_space) { address_space_assign(&im->memory,address_space);memset(im->memory.lookup_cache,0,sizeof(im->memory.lookup_cache)); }
}
static void image_restore(const ProcessImage *im) {
    GuestAddressSpace *place=image_place(im->shared);
    const GuestAddressSpace *source=im->shared?&im->shared->state.memory:&im->memory;
    shared_image=im->shared;address_space=place;
    address_space_assign(place,source);
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
        if(address_space!=image_home)address_space_assign(image_home,&shared->state.memory);   /* the last sharer takes the memory */
        shared_image=NULL;free(shared);address_space=image_home;
    }
    file_share_image_gone(address_space);
    address_space_thaw_dead(address_space);
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
/* exec builds the new image apart from the process (exec_build), then swaps it in
   (process_exec_at). Under the hypervisor a single-threaded process builds it
   without the runtime lock (guest_syscall_unlocked): path lookup, file, ELF and
   stack took most of an exec, while every other process waited for the lock. The
   call is then redone under the lock and finds it ready. exec_lock serializes
   the loading: the loader's caches and buffers are shared. */
static pthread_mutex_t exec_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct ExecStage {
    int ready;uint64_t pid,pc,dir,arg[3],generation;unsigned r; /* the call it was built for */
    GuestIds ids;GuestCpuState cpu;char exe[1025],cmdline[sizeof(proc_cmdline[0])];unsigned cmdline_size;
    int execfd;uint64_t execfd_slot; /* an x86 program's host descriptor plus one, and its AT_EXECFD's address: numbered at the exec (x86_exec_args) */
    GuestAddressSpace space;
} exec_stage;
static void exec_discard(void) {
    if(exec_stage.ready) { address_space_destroy(&exec_stage.space);exec_stage.ready=0; }
    if(exec_stage.execfd) { close(exec_stage.execfd-1);exec_stage.execfd=0; }
}
/* execve (path, argv, envp in x0..x2), or execveat (dirfd in x0, the rest from x1: r=1),
   whose path may be relative to dirfd: the new image into exec_stage. 0 or an error. */
static _Thread_local int exec_unlocked;               /* exec_build runs without the runtime lock */
static int64_t guest_fd_path(const char *path);      /* syscall.h */
/* The open file an exec names instead of a path: execveat(fd, "", AT_EMPTY_PATH)
   (fexecve), or /dev/fd/N and /proc/self/fd/N (musl's fexecve without execveat).
   apk 3 runs its package scripts from a memfd so. -1 for a path. */
static int64_t exec_named_fd(uint64_t dir,unsigned r,char path[1025]) {
    const unsigned char *first=lookup(cpu->gpr[r],1,4);path[0]=0;     /* fs_string refuses "" (ENOENT) */
    if(first && !*first)return r && (cpu->gpr[4]&0x1000)?(int64_t)(dir&0xffffffffu):-1;
    return fs_string(cpu->gpr[r],path)<0?-1:guest_fd_path(path);
}
/* That file, a descriptor of its own, or -errno (memfs.h's convention). */
static int exec_fd_open(int64_t n,char exe[1025]) {
    int host=n<GUEST_FD_LIMIT?fs_host((uint64_t)n):-1;
    if(host<0)return n<GUEST_FD_LIMIT && fd_kind[n]?-13:-9;           /* a pipe or socket: EACCES */
    int fd=fcntl(host,F_DUPFD_CLOEXEC,0);if(fd<0)return -errno;
    snprintf(exe,1025,"/dev/fd/%lld",(long long)n);return fd;
}
static uint64_t exec_build_held(uint64_t dir,unsigned r) {
    if(exec_stage.execfd) { close(exec_stage.execfd-1);exec_stage.execfd=0; }   /* an earlier build's that failed */
    char leaf[1025],exe[1025];int saved;
    char named_path[1025];int64_t named=exec_named_fd(dir,r,named_path);
    int fd=named>=0?exec_fd_open(named,exe):mem_exec_open(dir,cpu->gpr[r],exe,0);   /* memfs: a host copy */
    if(fd==MEM_HOST_PATH) {
        syscall_unlocked=exec_unlocked;syscall_abort=0;int parent=fs_lookup(dir,cpu->gpr[r],leaf,1|FS_DIRECT,exe);syscall_unlocked=0;
        if(parent<0 && mem_redirect_take()) { mem_exec_done();fd=mem_exec_open(dir,cpu->gpr[r],exe,1); } /* reached memfs through a link */
        else {
            mem_exec_done();
            if(syscall_abort) { if(parent>=0)close(parent);return linux_error(11); } /* needs the lock: the locked redo builds it */
            if(parent<0)return fs_error();
            struct stat known;int have=!fs_leaf_stat(parent,leaf,&known);
            fd=exec_open_at(parent,leaf,O_RDONLY|FS_NOFOLLOW_OPEN|O_NONBLOCK|O_CLOEXEC,have?&known:NULL);
            saved=errno;close(parent);errno=saved;if(fd<0)return fs_error();
        }
        if(fd==MEM_HOST_PATH)return linux_error(2);
    }
    if(fd<0)return linux_error((unsigned)-fd);
    struct stat st;
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || !(st.st_mode&0111)) { close(fd);return linux_error(13); }
    /* Each worker's own (execs build in parallel): anonymous memory, not charged. */
    static _Thread_local struct ExecStrings { char buffer[EXEC_STRINGS],*args[EXEC_ENTRIES],*env[EXEC_ENTRIES],*rebuilt[EXEC_ENTRIES]; } *strings;
    if(!strings) { void *p=mmap(NULL,sizeof(*strings),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED) { close(fd);return linux_error(12); }strings=p; }
    char *buffer=strings->buffer,**args=strings->args,**env=strings->env,**rebuilt=strings->rebuilt;unsigned argc,envc;size_t used=0;
    int error=process_vector(cpu->gpr[r+1],args,&argc,buffer,&used);
    if(!error)error=process_vector(cpu->gpr[r+2],env,&envc,buffer,&used);
    if(error || !argc) { close(fd);return linux_error(error?error:22); }
    /* "#!INTERPRETER [ONE-ARGUMENT]" runs INTERPRETER [ARG] SCRIPT ARGV[1..], as execve does.
       An interpreter may itself be a script (Oracle Linux's /usr/bin/env is), four
       levels deep at most, as on Linux. */
    char script[1025];                             /* fexecve of a script: the interpreter opens /dev/fd/N, as on Linux */
    if(named>=0)strcpy(script,named_path[0]?named_path:exe);
    else if(fs_string(cpu->gpr[r],script)<0) { close(fd);return linux_error(14); }
    for(unsigned depth=0;;depth++) {
        char head[256];ssize_t got=pread(fd,head,sizeof(head)-1,0);
        if(got<2 || head[0]!='#' || head[1]!='!')break;
        if(depth==4) { close(fd);return linux_error(40); }                           /* ELOOP */
        /* Its interpreter reads it through /dev/fd/N, which this runtime opens as a dup
           (one offset, not Linux's fresh open): from the start, wherever writing it left it. */
        if(!depth && named>=0)(void)lseek(fd,0,SEEK_SET);
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
            size_t length=strlen(extra[i])+1;if(used+length>EXEC_STRINGS) { close(fd);return linux_error(7); }
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
    int x86=x86_elf(fd);
    if(x86 && !x86_translator[0]) { close(fd);return linux_error(8); }            /* no translator: ENOEXEC, as Linux without binfmt_misc */
    { /* binfmt_misc with FEX registered (x86_exec_args): FEX runs an x86 program; FEX's own through its ld.so */
        const char *line[EXEC_ENTRIES];
        int n=x86_exec_args(exe,x86,(const char *const *)args,argc,buffer,&used,EXEC_STRINGS,line,EXEC_ENTRIES);
        if(n<0) { close(fd);return linux_error(7); }
        if(n>0) {
            for(int i=0;i<n;i++)args[i]=(char *)line[i];
            x86_godebug((const char **)env,&envc,EXEC_ENTRIES,buffer,&used,EXEC_STRINGS);  /* also FEX run again by FEX */
            if(x86) { int program=dup(fd);if(program<0) { close(fd);return linux_error(24); }exec_stage.execfd=program+1; }
            argc=(unsigned)n;close(fd);
            if(x86)snprintf(exe,sizeof(exe),"%s/bin/FEX",x86_translator);              /* /proc/self/exe: FEX finds FEXServer beside it */
            if((fd=fs_open_root_file(args[0]))<0)return fs_error();
            if(fstat(fd,&st)<0) { saved=errno;close(fd);errno=saved;return fs_error(); }
        }
    }
    FILE *input=fdopen(fd,"rb");if(!input) { close(fd);return linux_error(12); }
    /* A set-user-ID or set-group-ID file runs with its owner as effective and saved
       ID, unless no_new_privs is set. ponytail: the owner is root (image files are;
       per-file ownership is not tracked), and no mount is nosuid. */
    /* A set-user-ID or set-group-ID file runs with its owner as effective and saved
       ID, unless no_new_privs is set. ponytail: the owner is root (image files are;
       per-file ownership is not tracked), and no mount is nosuid. */
    struct ExecStage *stage=&exec_stage;
    GuestIds caller=guest_ids;
    if(!prctl_no_new_privs && (st.st_mode&S_ISUID))guest_ids.uid=guest_ids.suid=0;
    if(!prctl_no_new_privs && (st.st_mode&S_ISGID))guest_ids.gid=guest_ids.sgid=0;
    /* The loader builds into the stage: this worker's address_space and cpu point there
       meanwhile. Loader failures leave the calling image intact. load_stream closes input. */
    GuestAddressSpace *home=address_space;GuestCpuState *regs=cpu;
    stage->space.map_capacity=home->map_capacity;address_space_reset(&stage->space);memset(&stage->cpu,0,sizeof(stage->cpu));
    pthread_mutex_lock(&exec_lock);
    address_space=&stage->space;cpu=&stage->cpu;exec_staging=1;exec_loading=1;
    if(setjmp(exec_recovery)) {
        exec_loading=exec_staging=0;pthread_mutex_unlock(&exec_lock);
        address_space_destroy(&stage->space);address_space=home;cpu=regs;guest_ids=caller;return linux_error(8);
    }
    load_stream(input);heap_init();
    if(exec_stage.execfd)startup_execfd=0x7fffffff;          /* numbered at the exec: the lowest free descriptor then */
    startup((int)argc,(const char **)args,envc,(const char **)env);
    if(exec_stage.execfd)exec_stage.execfd_slot=startup_execfd_slot;
    startup_execfd=-1;
    exec_loading=exec_staging=0;pthread_mutex_unlock(&exec_lock);address_space=home;cpu=regs;
    stage->ids=guest_ids;guest_ids=caller;
    snprintf(stage->exe,sizeof(stage->exe),"%s",exe);
    stage->cmdline_size=0;                                /* as proc_set_cmdline */
    for(unsigned i=0;i<argc;i++) {
        size_t length=strlen(args[i])+1;if(stage->cmdline_size+length>sizeof(stage->cmdline))break;
        memcpy(stage->cmdline+stage->cmdline_size,args[i],length);stage->cmdline_size+=(unsigned)length;
    }
    return 0;
}
static uint64_t exec_build(uint64_t dir,unsigned r) {
    struct ExecStage *stage=&exec_stage;exec_discard();
    uint64_t result=exec_build_held(dir,r);
    if(result)return result;
    stage->ready=1;stage->pid=process_pid;stage->pc=cpu->pc;stage->dir=dir;stage->r=r;stage->generation=address_space->generation;
    for(unsigned i=0;i<3;i++)stage->arg[i]=cpu->gpr[r+i];
    return 0;
}
static uint64_t process_exec_at(uint64_t dir,unsigned r) {
    struct ExecStage *stage=&exec_stage;
    /* The stage is this call's: no guest code ran in this process since it was built
       (only single-threaded processes build early). A vfork child shares a threaded
       parent's memory, whose mappings change meanwhile (the generation): the strings
       were read during this call, as Linux reads them. */
    int ready=stage->ready && stage->pid==process_pid && stage->pc==cpu->pc && stage->dir==dir && stage->r==r &&
              (stage->generation==address_space->generation || shared_image) && stage->arg[0]==cpu->gpr[r] && stage->arg[1]==cpu->gpr[r+1] && stage->arg[2]==cpu->gpr[r+2];
    if(!ready) { uint64_t result=exec_build(dir,r);if(result)return result; }
    stage->ready=0;
    GuestIds caller=guest_ids;guest_ids=stage->ids;
    ProcessImage old;image_save(&old);image_clear();
    address_space_assign(address_space,&stage->space);address_space_changed(address_space);*cpu=stage->cpu;
    address_space_reset(&stage->space);                   /* the process owns its memory now */
    if(schedule_active && process_pid && process_pid<=PROCESS_LIMIT+1)memset(&proc_block->mm_cache[process_pid-1],0,sizeof(HvfMmCache));   /* its own, after a vfork */
    if(process_pid && process_pid<=PROCESS_LIMIT+1) { memcpy(proc_cmdline[process_pid],stage->cmdline,stage->cmdline_size);proc_cmdline_size[process_pid]=stage->cmdline_size; }
    proc_publish(0);process_exec_count++;
    if(process_pid<=PROCESS_LIMIT+1)snprintf(proc_exe[process_pid],sizeof(proc_exe[0]),"%s",strlen(stage->exe)<sizeof(proc_exe[0])?stage->exe:"");
    ProcessImage next;image_save(&next);image_restore(&old);image_free();image_restore(&next);
    /* M19 preserves regular guest descriptors. CLOEXEC is tracked by the FS adapter. */
    for(unsigned i=0;i<fs_top;i++)if(FS(i).fd && FS(i).cloexec) {
        if(FS(i).dir) { fs_directory_release(FS(i).dir);FS(i).dir=NULL; }
        if(!fs_pack_drop(i))close(FS(i).fd-1);FS(i).fd=0;FS(i).cloexec=0;fd_unbind(FS(i).guest);
    }
    if(stage->execfd) { /* binfmt_misc's O flag: the x86 program's descriptor, the lowest free one, into AT_EXECFD */
        uint64_t guest=fs_install(stage->execfd-1,0,1,0,NULL);stage->execfd=0;
        if((int64_t)guest>=0 && stage->execfd_slot) {
            store_le(stage->execfd_slot,guest,8);
            for(unsigned i=0;i+1<address_space->auxv_count;i+=2)if(address_space->auxv[i]==2)address_space->auxv[i+1]=guest; /* /proc/self/auxv */
        }
    }
    prctl_keepcaps=0;prctl_name[0]=0;       /* exec clears keepcaps and names the process after its program again */
    if(guest_uid)cap_held[0]=cap_held[1]=0; /* no file capabilities: a non-root exec loses them */
    else if(caller.uid) { cap_held[0]=UINT32_MAX;cap_held[1]=0x1ffu; } /* a set-user-ID-root exec gains them all */
    /* Sockets survive exec unless SOCK_CLOEXEC/FD_CLOEXEC is set, as on Linux. */
    for(unsigned i=0;i<net_top;i++)if(net_fd[i] && (net_flags[i]&524288))(void)net_release(i);
    net_wait.active=0;
    io_exec();
    if(schedule_active)schedule_exec_reset();
    return 0;
}
/* fork's undo: close the child's duplicates of the parent's files (the pack's
   placeholder is shared, never duplicated). */
static void process_close_files(const int *child_fds,unsigned files) {
    for(unsigned i=0;i<files;i++)if(child_fds[i] && !FS(i).pack)close(child_fds[i]-1);
}
/* Whether a vfork shares this process's memory with the child (else a private copy). */
static int process_vfork_shares(void) { return !process_threaded() || (shared_image && shared_image->owner==process_pid); }
static uint64_t process_clone(void) {
    fork_shared_private=0;
    if(epoll_live() && !schedule_active)return linux_error(95); /* scheduled children share inherited epoll instances */
    /* vfork shares memory, but never files or signal handlers. The scheduler
       holds the calling thread until child exec or exit. A threaded process's other
       threads run on meanwhile, in the same memory (its shared image); without one
       the child gets a private copy (like fork) and no thread waits for it. A
       copy made every page of a go command copy-on-write at each of its compiles. */
    unsigned vfork=cpu->gpr[0]==(17|0x100|0x4000);
    unsigned share=vfork && process_vfork_shares();
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
    if(!schedule_active)for(unsigned i=0;i<net_top;i++)if(net_fd[i])return linux_error(95);
    if(!share)mm_cache_range(MAP_BASE,address_space->map_capacity);       /* parked regions are unmapped before anything is copied */
    /* Every file slot holds a regular file, a directory with its shared cursor
       (fs_open) or a device (fs_dev): the child can share each as it is, with no
       host call per descriptor under the lock (a go command holds dozens). */
    /* The parent's slots while the child's stand in, on the heap (a whole table would
       not fit a worker's stack) and only below the tables' tops. */
    unsigned files=fs_top,io_used=io_top();
    size_t files_bytes=files*sizeof(FsSlot),io_bytes=io_used*sizeof(GuestIO);
    unsigned char *saved=malloc(files_bytes+io_bytes+files*sizeof(int));if(!saved)return linux_error(12);
    FsSlot *parent_files=(FsSlot *)(void *)saved;GuestIO *parent_io=(GuestIO *)(void *)(saved+files_bytes);int *child_fds=(int *)(void *)(saved+files_bytes+io_bytes);
    memcpy(parent_files,fs_table->slot,files_bytes);memset(child_fds,0,files*sizeof(int));
    for(unsigned i=0;i<files;i++)FS(i).sole=0;                         /* parent and child share each description */
    for(unsigned i=0;i<files;i++)if(FS(i).fd) {
        int fd=FS(i).pack?FS(i).fd-1:dup(FS(i).fd-1);                    /* the pack's placeholder: shared */
        if(fd<0) { process_close_files(child_fds,i);free(saved);return linux_error(24); }
        child_fds[i]=fd+1;
    }
    if(io_clone(parent_io,io_used)<0) { process_close_files(child_fds,files);free(saved);return linux_error(24); }
    GuestCwd *parent_cwd=fs_cwd;
    if(share && !shared_image) {
        shared_image=calloc(1,sizeof(*shared_image));
        if(!shared_image) {
            io_restore(parent_io,io_used);process_close_files(child_fds,files);free(saved);
            return linux_error(12);
        }
        shared_image->refs=1;
        address_space_assign(&shared_image->state.memory,address_space);address_space=&shared_image->state.memory; /* parent and child use it from now on */
    }
    ProcessImage parent,child;image_save(&parent);
    child.shared=parent.shared;child.cpu=parent.cpu;child.mask=parent.mask;address_space_assign(&child.memory,&parent.memory);
    if(share)shared_image->refs++;
    else {
        child.shared=NULL;
        if(!address_space_fork_private(&child.memory,&parent.memory)) {
            io_restore(parent_io,io_used);process_close_files(child_fds,files);free(saved);
            return linux_error(12);
        }
    }
    if(schedule_active)net_retain_all();
    if(fs_cwd)fs_cwd->refs++;
    for(unsigned i=0;i<files;i++) { if(FS(i).dir)FS(i).dir->refs++;if(FS(i).pack)pack_ref(FS(i).pack); }   /* the child shares each description */
    image_restore(&child);
    if(vfork && child_stack)cpu->sp=child_stack;
    memset(address_space->lookup_cache,0,sizeof(address_space->lookup_cache));for(unsigned i=0;i<files;i++)FS(i).fd=child_fds[i];
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
        memcpy(io_fds,parent_io,io_bytes);
        for(unsigned i=0;i<files;i++) { FS(i).fd=parent_files[i].fd;FS(i).cloexec=parent_files[i].cloexec; }
        free(saved);
        process_children[slot].pid=pid;process_children[slot].parent=process_pid;process_children[slot].live=1;
        if(!vfork && (flags&0x100000))fs_pack(lookup(parent_tid,4,2),0,(unsigned)pid,4);
        return pid;
    }
    process_depth=1;process_pid=pid;
    GuestIds parent_ids=guest_ids;GuestPriv parent_priv=proc_now->priv;
    static unsigned char parent_kind[GUEST_FD_LIMIT];static unsigned short parent_index[GUEST_FD_LIMIT],parent_net[NET_SLOTS],parent_io_guest[IO_END];
    memcpy(parent_kind,fd_kind,sizeof(fd_kind));memcpy(parent_index,fd_index,sizeof(fd_index));
    memcpy(parent_net,net_guest,sizeof(net_guest));memcpy(parent_io_guest,io_guest,sizeof(io_guest));
    unsigned status=(unsigned)run(0);
    guest_ids=parent_ids;proc_now->priv=parent_priv; /* the child's identity, prctl and capability changes stay in the child */
    fs_cwd_release();fs_cwd=parent_cwd;image_free();image_restore(&parent);
    io_restore(parent_io,io_used);
    net_cleanup();memset(net_fd,0,net_top*sizeof(net_fd[0]));
    unsigned used=fs_top;                                                 /* the child's files too */
    for(unsigned i=0;i<used;i++) {
        if(FS(i).dir) { fs_directory_release(FS(i).dir);FS(i).dir=NULL; }
        if(!fs_pack_drop(i) && FS(i).fd)close(FS(i).fd-1);
        FS(i).fd=0;FS(i).cloexec=0;
    }
    for(unsigned i=0;i<files;i++) { FS(i).fd=parent_files[i].fd;FS(i).cloexec=parent_files[i].cloexec;FS(i).dir=parent_files[i].dir;FS(i).guest=parent_files[i].guest; }
    free(saved);
    memcpy(fd_kind,parent_kind,sizeof(fd_kind));memcpy(fd_index,parent_index,sizeof(fd_index));
    memcpy(net_guest,parent_net,sizeof(net_guest));memcpy(io_guest,parent_io_guest,sizeof(io_guest));
    process_depth=0;process_pid=1;
    process_children[slot].pid=pid;process_children[slot].parent=1;process_children[slot].status=status<<8;
    if(flags&0x100000)fs_pack(lookup(parent_tid,4,2),0,(unsigned)pid,4);
    return pid;
}
/* wait4's answer: the status, and the rusage (not measured: zeros). 0 if either cannot be
   written. Only with an answer, as Linux: a write prepares the memory, and the checks
   this made while the call blocked had a forked parent copy its memory each time. */
static int process_wait_out(unsigned status) {
    if(cpu->gpr[1]) { unsigned char *p=lookup(cpu->gpr[1],4,2);if(!p)return 0;fs_pack(p,0,status,4); }
    if(cpu->gpr[3]) { unsigned char *usage=lookup(cpu->gpr[3],144,2);if(!usage)return 0;memset(usage,0,144); }
    return 1;
}
static uint64_t process_wait(void) {
    cpu->gpr[2]&=~UINT64_C(0x60000000);                                   /* __WALL, __WNOTHREAD: no effect here */
    if(cpu->gpr[2]&~(job_control_enabled?UINT64_C(11):UINT64_C(1)))return linux_error(95);
    int64_t pid=(int64_t)cpu->gpr[0];if(pid<=0)pid=-1;                    /* ponytail: a process group waits for any child */
    if(process_depth && !process_tree_enabled)return linux_error(10);
    unsigned live=0;
    for(unsigned i=0;i<process_count;i++)if(process_children[i].pid && process_children[i].parent==process_pid && (pid==-1 || (uint64_t)pid==process_children[i].pid)) {
        if(process_children[i].live) {
            unsigned event=process_children[i].event_status;
            if(event && ((event==65535 && (cpu->gpr[2]&8)) || (event!=65535 && (cpu->gpr[2]&2)))) {
                if(!process_wait_out(event))return linux_error(14);
                process_children[i].event_status=0;return process_children[i].pid;
            }
            live=1;continue;
        }
        if(!process_wait_out(process_children[i].status))return linux_error(14);
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
