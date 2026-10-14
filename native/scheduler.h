/* Threads (CLONE_THREAD, native backend). A process gets a thread table on its
   first extra thread (or futex wait); without one it is single-threaded (tid ==
   pid) and the task itself is what the scheduler runs. With one, each thread is
   scheduled on its own, so threads of a process run on several workers at once.
   They share the process's memory, descriptors and signal handlers; each has its
   own registers, wait, signal mask and thread-directed pending signals. */
#define THREAD_LIMIT 128
typedef struct {
    unsigned state;               /* 0 free, 1 runnable, 2 futex wait, 3 blocked call (retries when woken) */
    unsigned worker;              /* 1 + the index of the worker running it, or 0 */
    unsigned tid,private_wait,realtime,blocked,input_wait;
    uint32_t bitset;
    uint64_t clear_tid,futex_address,deadline_ns,epoch,wakeups;
    GuestCpuState regs;           /* while it is not running */
    NetWait network;
    uint64_t signal_pending,signal_mask;
    unsigned fault_signal,fault_code;uint64_t fault_addr; /* a synchronous fault of this thread awaiting delivery */
    unsigned wait_masked;uint64_t wait_saved_mask;       /* ppoll/pselect6 with a mask: the thread's own, to restore */
    MemoryBacking *futex_backing; /* a futex in shared memory: keyed by its memory, across processes */
    size_t futex_offset;
} Thread;
typedef struct { unsigned cursor,top;Thread thread[THREAD_LIMIT]; } ThreadTable; /* top: 1 + the highest slot ever used */
/* Scheduled execution: one root plus at most PROCESS_LIMIT live children; reaped
   child slots are reused, so a long-running tree may fork indefinitely.
   Saved tasks own their image/descriptors; a running task owns its worker's globals.
   worker: 1 + the index of the worker running a single-threaded task, or 0;
   running: workers running threads of a threaded one. mm serializes the runtime
   work of a process's threads (its memory above all). exit_pending: the process
   ended while other threads still ran; the last of them frees it. */
typedef struct {
    ProcessImage image;IoTable io;NetTable net;NetWait network_wait;
    ProcShared ps;FsTable fs;FdTable fds;
    unsigned vfork_child,saved,state,terminate_signal,pgid,input_wait,stop_signal,execed,blocked,worker;uint64_t epoch,wakeups;
    uint64_t signal_mask,signal_ignored,signal_pending,timer_deadline,timer_interval;
    uint64_t signal_flags[64],signal_action_mask[64],signal_restorer[64],signal_handler[64];
    unsigned signal_sender[64];
    /* A fault raised as a signal (SIGSEGV, SIGILL): siginfo's si_code and si_addr. */
    unsigned signal_fault_code[64];uint64_t signal_fault_addr[64];
    unsigned suspending;uint64_t suspend_mask; /* rt_sigsuspend: the mask to restore after its handler */
    unsigned wait_masked;uint64_t wait_saved_mask; /* ppoll/pselect6 of a single-threaded process (Thread has its own) */
    unsigned sigwaiting;uint64_t sigwait_set,sigwait_info; /* rt_sigtimedwait: the signals it takes, its siginfo (mask in suspend_mask) */
    ThreadTable *threads;unsigned running,exit_pending,exit_signal;int exit_result;
    pthread_mutex_t mm;
} ScheduledTask;
static uint64_t schedule_switches,schedule_cancellations;
static ScheduledTask schedule_tasks[PROCESS_LIMIT+1];
/* Workers (--cpus N): N host threads run tasks at once, each on its own CPU
   backend context (a vCPU). Guest code runs in parallel; everything else (system
   calls, faults, scheduling) holds runtime_mutex. The lock holder's current-process
   state lives in the usual globals: a worker saves them when it releases the lock
   and installs its own when it takes it. With one worker there is no lock. */
#define WORKER_LIMIT 64
typedef struct Worker {
    RuntimeContext *context;unsigned index;int task;         /* task: the slot it has loaded, or -1 */
    void *backend;                                           /* the CPU backend's per-worker state */
    atomic_int in_guest;                                     /* guest code is (about to be) running */
} Worker;
static Worker worker_first={.context=&runtime_context,.task=-1},*workers[WORKER_LIMIT]={&worker_first};
static _Thread_local Worker *worker=&worker_first;
static unsigned worker_count=1,worker_threads;               /* worker_threads: other workers are live */
static int workers_supported;                                /* the CPU backend releases the lock around guest code */
static unsigned guest_cpus(void) { return workers_supported?worker_count:1; } /* one guest CPU per worker */
static pthread_mutex_t runtime_mutex=PTHREAD_MUTEX_INITIALIZER;
static void (*worker_interrupt)(Worker *w);                  /* the backend stops w's guest code soon */
static _Thread_local int thread_now=-1;                      /* the thread this worker runs, or -1 */
static uint64_t thread_time_ns(int realtime) {
    struct timespec now;if(clock_gettime(realtime?CLOCK_REALTIME:GUEST_MONOTONIC,&now))fail("Cannot read clock");
    return (uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec;
}
static int process_threaded(void) { return schedule_active && schedule_tasks[process_pid-1].threads; }
static Thread *task_thread(ScheduledTask *t) { return t->threads && thread_now>=0?&t->threads->thread[thread_now]:NULL; }
/* The calling thread (NULL in a single-threaded or unscheduled process). */
static Thread *thread_current(void) { return schedule_active?task_thread(&schedule_tasks[process_pid-1]):NULL; }
/* The calling thread's blocked signals. */
static uint64_t *signal_mask_now(void) {
    Thread *th=thread_current();
    return th?&th->signal_mask:&schedule_tasks[process_pid-1].signal_mask;
}
/* Signals blocked by every thread of t: a process-directed one then stays pending. */
static uint64_t task_signals_blocked(ScheduledTask *t) {
    if(!t->threads)return t->signal_mask;
    uint64_t all=UINT64_MAX;
    for(unsigned i=0;i<t->threads->top;i++)if(t->threads->thread[i].state)all&=t->threads->thread[i].signal_mask;
    return all;
}
/* Workers running t's code leave it soon (a signal, the process stopping or ending). */
static void schedule_interrupt(ScheduledTask *t) {
    int index=(int)(t-schedule_tasks);
    atomic_thread_fence(memory_order_seq_cst);     /* the request is visible before in_guest is read */
    for(unsigned i=0;worker_interrupt && i<worker_count;i++)if(workers[i]!=worker && workers[i]->task==index)worker_interrupt(workers[i]);
}
static void mm_lock(ScheduledTask *t) { if(schedule_active)pthread_mutex_lock(&t->mm); }
static void mm_unlock(ScheduledTask *t) { if(schedule_active)pthread_mutex_unlock(&t->mm); }
static void schedule_notify(void);
static _Thread_local int runtime_held; /* this worker holds runtime_mutex */
/* Around guest execution and host waits; no-ops with a single worker. */
static void runtime_unlock(void) {
    if(!worker_threads)return;
    runtime_held=0;schedule_notify();pthread_mutex_unlock(&runtime_mutex);
}
/* The lock is held for microseconds at a time: spin briefly before sleeping in the kernel. */
static void runtime_lock(Worker *w) {
    (void)w;
    if(!worker_threads)return;
    for(unsigned spin=0;spin<2000;spin++) {
        if(!pthread_mutex_trylock(&runtime_mutex)) { runtime_held=1;return; }
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
    pthread_mutex_lock(&runtime_mutex);runtime_held=1;
}
static int process_slot_free(unsigned slot) {
    if(process_children[slot].pid || process_children[slot].live)return 0;
    if(schedule_tasks[slot+1].state || schedule_tasks[slot+1].saved)return 0;
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==slot+2)return 0;
    return 1;
}
/* A blocked task's own event: it retries its call (a targeted wait ignores io_epoch).
   wakeups also counts events for a running task, which then must not block on them.
   In a threaded process every thread blocked in a call retries. */
static uint64_t schedule_events;
static void schedule_wake(ScheduledTask *task) {
    task->wakeups++;if(task->state==2)task->state=1;
    if(task->threads)for(unsigned i=0;i<task->threads->top;i++) {
        Thread *th=&task->threads->thread[i];
        th->wakeups++;if(th->state==3)th->state=1;
    }
    schedule_events++;
}
static void schedule_wake_mask(uint64_t mask) {
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)if(mask&(UINT64_C(1)<<i))schedule_wake(&schedule_tasks[i]);
}
static void schedule_signal(ScheduledTask *task,unsigned signal);
static void schedule_signal_apply(ScheduledTask *task,unsigned signal) {
    uint64_t bit=UINT64_C(1)<<(signal-1);
    unsigned slot=(unsigned)(task-schedule_tasks);
    if(job_control_enabled && signal==18) {
        task->stop_signal=0;task->signal_pending&=~(UINT64_C(15)<<18);
        if(task->state==3) {
            task->state=1;io_epoch++;schedule_wake(task);
            if(slot) { process_children[slot-1].event_status=65535;schedule_signal(&schedule_tasks[process_children[slot-1].parent-1],17); }
        }
    }
    if(job_control_enabled && signal>=19 && signal<=22)task->signal_pending&=~(UINT64_C(1)<<17);
    if(job_control_enabled && signal==19) { task->stop_signal=19;io_epoch++;schedule_wake(task);return; }
    if(signal!=9 && (task->signal_ignored&bit))return;
    if(job_control_enabled && task->state==3 && signal!=9 && signal!=18) { task->signal_pending|=bit;return; }
    task->signal_sender[signal-1]=(unsigned)process_pid;
    if(signal!=9 && ((task_signals_blocked(task)&bit) || task->signal_handler[signal-1] || (task->sigwaiting && (task->sigwait_set&bit)))) {
        task->signal_pending|=bit;io_epoch++;schedule_wake(task);
        if(task->threads)for(unsigned i=0;i<task->threads->top;i++) {    /* a futex wait that can take it ends (EINTR) */
            Thread *th=&task->threads->thread[i];
            if(th->state==2 && !(th->signal_mask&bit)) { th->state=1;th->regs.gpr[0]=linux_error(4); }
        }
        return;
    }
    if(job_control_enabled && (signal==17 || signal==18 || signal==23 || signal==28))return; /* default: ignore (CHLD CONT URG WINCH) */
    if(job_control_enabled && signal>=20 && signal<=22) { task->stop_signal=signal;io_epoch++;schedule_wake(task);return; }
    if(!task->terminate_signal || signal==9)task->terminate_signal=signal;
    io_epoch++;schedule_wake(task);
}
/* A target running on another worker leaves its guest code to take the signal. */
static void schedule_signal(ScheduledTask *task,unsigned signal) {
    schedule_signal_apply(task,signal);schedule_interrupt(task);
}
/* The memory budget (--memory-limit-mib) cannot cover a page this process needs:
   like Linux's OOM killer in a container's cgroup, kill the process (SIGKILL; the
   container exits 137 when it is PID 1) and leave --oom-file for the engine
   (docker inspect's OOMKilled). Decided under the runtime lock: a worker without it
   returns, and the access is retried with the lock. Always returns 0. */
static const char *oom_file;
static int memory_exhausted(void) {
    memory_oom=1;
    if(oom_file) { int fd=open(oom_file,O_WRONLY|O_CREAT|O_CLOEXEC,0600);if(fd>=0)close(fd); }
    if(!schedule_active) { fprintf(stderr,"vmfree: out of memory: killed process 1\n");exit(137); }
    if(worker_threads && !runtime_held)return 0;
    ScheduledTask *t=&schedule_tasks[process_pid-1];
    if(t->terminate_signal!=9) {
        fprintf(stderr,"vmfree: out of memory: killed process %" PRIu64 "\n",process_pid);
        schedule_signal(t,9);
    }
    return 0;
}
static void schedule_pending(ScheduledTask *task) {
    uint64_t blocked=task==&schedule_tasks[process_pid-1]?*signal_mask_now():task_signals_blocked(task);
    uint64_t ready=task->signal_pending & ~blocked & ~task->signal_ignored;
    for(unsigned i=0;i<64;i++)if(ready&(UINT64_C(1)<<i)) {
        task->signal_pending&=~(UINT64_C(1)<<i);schedule_signal(task,i+1);
    }
}
/* A vfork child exited or execed: its parent runs again, and the link goes before
   the child's slot can be reused by another process. */
static void schedule_vfork_done(uint64_t pid) {
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)if(schedule_tasks[i].vfork_child==pid)schedule_tasks[i].vfork_child=0;
}
static void schedule_exec_reset(void) {
    ScheduledTask *task=&schedule_tasks[process_pid-1];
    task->execed=1;schedule_vfork_done(process_pid);
    memset(task->signal_handler,0,sizeof(task->signal_handler));
    memset(task->signal_flags,0,sizeof(task->signal_flags));
    memset(task->signal_action_mask,0,sizeof(task->signal_action_mask));
    memset(task->signal_restorer,0,sizeof(task->signal_restorer));
}
/* Guest IDs only: never pass a guest PID or signal to a host kill API. */
static uint64_t process_kill(void) {
    if(!schedule_active)return linux_error(95);
    int64_t pid=(int64_t)cpu->gpr[0];uint64_t signal=cpu->gpr[1];
    if(signal>64)return linux_error(22);
    /* Without job control only interrupt/kill/pipe/terminate; with it, every signal. */
    if(signal && !job_control_enabled && signal!=2 && signal!=9 && signal!=13 && signal!=15)return linux_error(95);
    if(pid<=0) {
        if(!job_control_enabled || pid==-1 || pid==INT64_MIN)return linux_error(95);
        uint64_t group=pid? (uint64_t)-pid:schedule_tasks[process_pid-1].pgid;int found=0;
        for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].pgid==group &&
            (schedule_tasks[i].state || (i && process_children[i-1].pid))) {
            found=1;
            if(signal && schedule_tasks[i].state) {
                schedule_signal(&schedule_tasks[i],(unsigned)signal);
            }
        }
        return found?0:linux_error(3);
    }
    if(pid>(int64_t)process_count+1)return linux_error(3);
    ScheduledTask *target=&schedule_tasks[pid-1];
    if(!target->state) {
        /* A waitable zombie retains its guest PID until reaped. */
        return pid>1 && process_children[pid-2].pid==(uint64_t)pid?0:linux_error(3);
    }
    if(signal) {
        schedule_signal(target,(unsigned)signal);
    }
    return 0;
}
/* The descriptor tables are copied only into a record that does not already hold
   them (a new child, the first switch away from the first process). */
static void task_save(ScheduledTask *t) {
    image_save(&t->image);t->saved=1;
    if(proc_now!=&t->ps)t->ps=*proc_now;
    if(io_table!=&t->io)t->io=*io_table;
    if(net_table!=&t->net)t->net=*net_table;
    if(net_wait_now!=&t->network_wait)t->network_wait=net_wait;
    if(fs_table!=&t->fs)t->fs=*fs_table;
    if(fd_table!=&t->fds)t->fds=*fd_table;
}
static void task_load(ScheduledTask *t) {
    proc_now=&t->ps;image_home_scheduled=&t->image.memory;image_restore(&t->image);t->saved=0;
    io_table=&t->io;net_table=&t->net;net_wait_now=&t->network_wait;fs_table=&t->fs;fd_table=&t->fds;
}
static void task_free(void) {
    fs_cwd_release();image_free();io_cleanup();net_cleanup();fs_locks_release(0,0,process_pid);io_fifo_forget(process_pid);
    for(unsigned i=0;i<FS_SLOTS;i++) {
        fs_directory_release(fs_dirs[i]);
        if(fs_fd[i])close(fs_fd[i]-1);
    }
    memset(fs_dirs,0,sizeof(fs_dirs));memset(fs_fd,0,sizeof(fs_fd));
    memset(fs_cloexec,0,sizeof(fs_cloexec));
}
static void schedule_cleanup(void) {
    static ScheduledTask active;task_save(&active); /* static: the tables stay in use afterwards */
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)if(schedule_tasks[i].saved) {
        task_load(&schedule_tasks[i]);task_free();schedule_tasks[i].state=0;schedule_tasks[i].terminate_signal=0;
        free(schedule_tasks[i].threads);schedule_tasks[i].threads=NULL;
    }
    task_load(&active);schedule_live=0;
}
static void schedule_child(uint64_t pid) {
    ScheduledTask *t=&schedule_tasks[pid-1];memset(t,0,sizeof(*t)); /* a reused slot starts clean */
    pthread_mutex_init(&t->mm,NULL);
    task_save(t);t->state=1;t->pgid=schedule_tasks[process_pid-1].pgid;
    ScheduledTask *parent=&schedule_tasks[process_pid-1];
    t->signal_mask=*signal_mask_now();t->signal_ignored=parent->signal_ignored; /* the forking thread's mask */
    memcpy(t->signal_flags,parent->signal_flags,sizeof(t->signal_flags));
    memcpy(t->signal_action_mask,parent->signal_action_mask,sizeof(t->signal_action_mask));
    memcpy(t->signal_restorer,parent->signal_restorer,sizeof(t->signal_restorer));
    memcpy(t->signal_handler,parent->signal_handler,sizeof(t->signal_handler));
    schedule_live++;
}
static void schedule_vfork(uint64_t pid) {
    schedule_tasks[process_pid-1].vfork_child=(unsigned)pid;
    schedule_slice=0;
}
/* --signals: stop and control signals sent to the runtime (docker stop/kill)
   go to the container's first process, as Linux delivers them to PID 1. With
   --stdin on a terminal, the terminal's ^C, ^\ and resizes (SIGINT, SIGQUIT,
   SIGWINCH) go to its foreground process group instead, as a Linux tty sends them. */
static volatile sig_atomic_t host_signal_pending[32];
static void host_signal_catch(int number) { if(number>0 && number<32)host_signal_pending[number]=1; }
static const int host_signal_numbers[]={SIGTERM,SIGINT,SIGQUIT,SIGHUP,SIGUSR1,SIGUSR2,SIGWINCH};
static void host_signals_install(void) {
    struct sigaction action;memset(&action,0,sizeof(action));action.sa_handler=host_signal_catch;sigemptyset(&action.sa_mask);
    for(unsigned i=0;i<sizeof(host_signal_numbers)/sizeof(*host_signal_numbers);i++)
        if(sigaction(host_signal_numbers[i],&action,NULL))fail("Cannot install host signal handler");
}
static void host_signal_pump(void) {
    static const unsigned linux_number[]={15,2,3,1,10,12,28};
    static int tty=-1;if(tty<0)tty=stdin_host && isatty(0);
    for(unsigned i=0;i<sizeof(host_signal_numbers)/sizeof(*host_signal_numbers);i++) {
        if(!host_signal_pending[host_signal_numbers[i]])continue;
        host_signal_pending[host_signal_numbers[i]]=0;
        if(tty && (linux_number[i]==2 || linux_number[i]==3 || linux_number[i]==28)) {
            for(unsigned t=0;t<=process_count;t++)if(schedule_tasks[t].state && schedule_tasks[t].pgid==terminal_foreground)schedule_signal(&schedule_tasks[t],linux_number[i]);
        }
        else if(linux_number[i]!=28 && schedule_tasks[0].state)schedule_signal(&schedule_tasks[0],linux_number[i]);
    }
}
/* Idle workers wait on schedule_idle_cond; one of them (the poller) waits in poll()
   on the blocked tasks' sockets and the poke pipe instead. Whoever makes work
   runnable wakes them as it releases the lock (schedule_notify). */
static pthread_cond_t schedule_idle_cond=PTHREAD_COND_INITIALIZER;
static unsigned schedule_idle,schedule_polling,schedule_done,schedule_parked;
static uint64_t schedule_notified_events,schedule_notified_epoch;
static int schedule_poke[2]={-1,-1},schedule_result;
static void schedule_kick(void) {
    if(schedule_idle)(schedule_done?pthread_cond_broadcast:pthread_cond_signal)(&schedule_idle_cond); /* one idle worker per event */
    if(schedule_polling && schedule_poke[1]>=0) { char c=0;(void)!write(schedule_poke[1],&c,1); }
}
static void schedule_notify(void) {
    if(schedule_events==schedule_notified_events && io_epoch==schedule_notified_epoch)return;
    schedule_notified_events=schedule_events;schedule_notified_epoch=io_epoch;schedule_kick();
}
/* Attach this worker to thread i of t: the process's state is shared in place;
   the registers and the wait are the thread's. Detach stores the registers. */
static void thread_attach(ScheduledTask *t,int i) {
    Thread *th=&t->threads->thread[i];
    proc_now=&t->ps;image_home_scheduled=&t->image.memory;address_space=image_place(shared_image);
    io_table=&t->io;net_table=&t->net;fs_table=&t->fs;fd_table=&t->fds;
    net_wait_now=&th->network;thread_now=i;*cpu=th->regs;cpu->exclusive_valid=0;t->saved=0;
}
static void thread_detach(ScheduledTask *t) {
    Thread *th=task_thread(t);if(th)th->regs=*cpu;
    thread_now=-1;if(!t->running)t->saved=1;
}
static void schedule_enter(int task,int thread) {
    if(thread>=0)thread_attach(&schedule_tasks[task],thread);
    else { thread_now=-1;task_load(&schedule_tasks[task]); }
}
static void schedule_leave(int task) {
    ScheduledTask *t=&schedule_tasks[task];
    if(task_thread(t))thread_detach(t);else task_save(t);
}
/* A thread that is not running and may run now: runnable, a futex wait whose
   deadline passed, or a generic blocked call after any I/O event. */
static int thread_runnable(const Thread *th) {
    if(!th->state || th->worker)return 0;
    if(th->state==1)return 1;
    if(th->state==2)return th->deadline_ns && thread_time_ns((int)th->realtime)>=th->deadline_ns;
    return !(th->network.active && th->network.targeted) && th->epoch!=io_epoch;
}
/* wait: block (without the lock) until a blocked task's socket is ready, its
   deadline passes, or the poke pipe is written. Returns the number of tasks (and
   threads) in a network wait or a timed futex wait. Only one worker waits here at
   a time. Owners are task*(THREAD_LIMIT+1) plus 1+thread for a thread. */
#define NETWORK_PUMP_FDS ((PROCESS_LIMIT+1)*NET_FD_LIMIT)
static int network_pump(int wait) {
    if(!net_scheduled)return 0;
    static struct pollfd now_fds[NETWORK_PUMP_FDS+1],wait_fds[NETWORK_PUMP_FDS+1];
    static unsigned now_owner[NETWORK_PUMP_FDS],wait_owner[NETWORK_PUMP_FDS];
    struct pollfd *fds=wait?wait_fds:now_fds;unsigned *owner=wait?wait_owner:now_owner;
    unsigned count=0,pending=0;int timeout=wait?50:0;uint64_t now=net_now();
    for(unsigned i=0;i<=process_count;i++) {
        ScheduledTask *t=&schedule_tasks[i];
        if(t->threads && t->state) {
            for(unsigned j=0;j<t->threads->top;j++) {
                Thread *th=&t->threads->thread[j];
                if(th->worker)continue;
                if(th->state==2 && !th->deadline_ns) { pending++;continue; } /* a signal may end it */
                if(th->state==2) {                                    /* a timed futex wait */
                    pending++;uint64_t at=thread_time_ns((int)th->realtime);
                    uint64_t left=th->deadline_ns>at?(th->deadline_ns-at)/1000000+1:0;
                    if(left<(uint64_t)timeout)timeout=(int)left;
                    continue;
                }
                NetWait *w=&th->network;
                if(th->state!=3 || !w->active)continue;
                pending++;
                if(now>=w->deadline) { th->state=1;timeout=0;continue; }
                if(wait && w->deadline-now<(unsigned)timeout)timeout=(int)(w->deadline-now);
                for(unsigned k=0;k<w->count && count<NETWORK_PUMP_FDS;k++) { fds[count]=w->fds[k];owner[count++]=i*(THREAD_LIMIT+1)+1+j; }
            }
            continue;
        }
        NetWait *w=&t->network_wait;
        if(t->state!=2 || !w->active || t->worker)continue;
        pending++;
        if(now>=w->deadline) { t->state=1;timeout=0;continue; }
        if(wait && w->deadline-now<(unsigned)timeout)timeout=(int)(w->deadline-now);
        for(unsigned j=0;j<w->count && count<NETWORK_PUMP_FDS;j++) { fds[count]=w->fds[j];owner[count++]=i*(THREAD_LIMIT+1); }
    }
    if(!pending)return 0;
    unsigned watched=count;
    if(wait && worker_threads)fds[watched++]=(struct pollfd){schedule_poke[0],POLLIN,0};
    Worker *self=worker;
    if(wait) { schedule_polling=1;runtime_unlock(); }
    int result=poll(fds,watched,timeout);
    if(wait) {
        runtime_lock(self);schedule_polling=0;
        if(worker_threads && fds[count].revents) { char drain[64];while(read(schedule_poke[0],drain,sizeof(drain))==(ssize_t)sizeof(drain)) {} }
    }
    if(result<0 && errno!=EINTR)fail("Network readiness polling failed");
    unsigned woken=0;
    /* The lock was free during a wait: a task may have run, moved on or exited since. */
    if(result>0)for(unsigned i=0;i<count;i++) {
        if(!fds[i].revents)continue;
        ScheduledTask *t=&schedule_tasks[owner[i]/(THREAD_LIMIT+1)];unsigned j=owner[i]%(THREAD_LIMIT+1);
        if(!j) { if(!t->threads && t->state==2) { t->state=1;woken++; } }
        else if(t->threads && t->threads->thread[j-1].state==3) { t->threads->thread[j-1].state=1;woken++; }
    }
    if(woken)schedule_events++;
    return (int)pending;
}
static int schedule_runnable(const ScheduledTask *t) {
    if(t->worker)return 0;
    if(t->vfork_child) {
        const ScheduledTask *child=&schedule_tasks[t->vfork_child-1];
        if(child->state && !child->execed)return 0;
    }
    if(t->state==2 && t->network_wait.active && t->network_wait.targeted && !t->terminate_signal && !t->stop_signal)return 0;
    return t->state && (t->terminate_signal || (t->state!=3 && (t->stop_signal || t->state==1 || t->epoch!=io_epoch)));
}
/* A thread of threaded task t to run now, or -1. A pending termination or stop
   runs on any thread that is not already running. */
static int task_pick_thread(ScheduledTask *t) {
    if(!t->state || t->exit_pending)return -1;
    if(t->vfork_child) {
        const ScheduledTask *child=&schedule_tasks[t->vfork_child-1];
        if(child->state && !child->execed)return -1;
    }
    int urgent=t->terminate_signal || (t->stop_signal && t->state!=3);
    if(t->state==3 && !t->terminate_signal)return -1;                      /* stopped */
    ThreadTable *p=t->threads;
    for(unsigned n=1;n<=p->top;n++) {
        unsigned i=(p->cursor+n)%p->top;Thread *th=&p->thread[i];
        if(!th->state || th->worker)continue;
        if(urgent || thread_runnable(th)) { p->cursor=i;return (int)i; }
    }
    return -1;
}
static int timer_pump(void) {
    uint64_t now=net_now();int active=0;
    for(unsigned i=0;i<=process_count;i++) {
        ScheduledTask *t=&schedule_tasks[i];if(!t->state || !t->timer_deadline)continue;
        active=1;if(now<t->timer_deadline)continue;
        t->timer_deadline=t->timer_interval?now+t->timer_interval:0;schedule_signal(t,14);
    }
    return active;
}
/* The next runnable task after `after` (round robin) and its thread (-1 for a
   single-threaded task), else -1. */
static int schedule_pick(unsigned after,int *thread) {
    for(unsigned scanned=0;scanned<=PROCESS_LIMIT;scanned++) {
        after=(after+1)%(PROCESS_LIMIT+1);
        ScheduledTask *t=&schedule_tasks[after];
        if(!t->threads) { if(schedule_runnable(t)) { *thread=-1;return (int)after; } continue; }
        int i=task_pick_thread(t);
        if(i>=0) { *thread=i;return (int)after; }
    }
    return -1;
}
static void schedule_wait_idle(unsigned ms) {
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=(long)ms*1000000;if(until.tv_nsec>=1000000000) { until.tv_sec++;until.tv_nsec-=1000000000; }
    schedule_idle++;(void)pthread_cond_timedwait(&schedule_idle_cond,&runtime_mutex,&until);schedule_idle--;
}
/* Nothing is runnable for this worker: wait for work, or diagnose a deadlock. */
static void schedule_idle_step(void) {
    terminal_pump(0);host_signal_pump();timer_pump();
    int thread;if(schedule_pick(0,&thread)>=0)return;
    unsigned running=0;for(unsigned i=0;i<worker_count;i++)running+=workers[i]->task>=0 && workers[i]!=worker;
    if(!schedule_polling && network_pump(1)) { terminal_pump(0);return; }
    if(worker_threads && (schedule_polling || running)) { schedule_wait_idle(10);return; }
    if(timer_pump()) {
        if(worker_threads)schedule_wait_idle(10);else (void)poll(NULL,0,10);
        return;
    }
    int input=0,stopped=0;
    for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state==2 && schedule_tasks[i].input_wait)input=1;
    for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state==3)stopped=1;
    if(job_control_enabled && stopped && !input) { terminal_pump(2);return; }
    /* A process in rt_sigsuspend (nginx's master) or rt_sigtimedwait (tini) waits for a
       signal from outside: docker stop or kill. That is not a deadlock. */
    for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && (schedule_tasks[i].suspending || schedule_tasks[i].sigwaiting)) {
        if(worker_threads)schedule_wait_idle(10);else (void)poll(NULL,0,10);
        return;
    }
    if(terminal_enabled && input) { terminal_pump(1);return; }
    /* Remove the current globals' non-owning alias before fatal cleanup. */
    fs_cwd=NULL;image_clear();memset(net_fd,0,sizeof(net_fd));memset(&net_wait,0,sizeof(net_wait));memset(io_fds,0,sizeof(io_fds));memset(fs_fd,0,sizeof(fs_fd));memset(fs_dirs,0,sizeof(fs_dirs));
    fail("Guest process deadlock: no runnable task");
}
/* An empty task: a worker without a task loads it, so its globals never alias a
   task that another worker may run, free or clean up. Only ever read. */
static ScheduledTask schedule_spare;
static void schedule_unload(void) { task_load(&schedule_spare);thread_now=-1; }
/* The process in slot current ended with result (an exit status, or 128+signal):
   free it (it is loaded here and no thread of it runs) and tell its parent.
   Returns 1 when it was the root: the run is over. */
static int schedule_finish(int current,int result,unsigned terminating) {
    ScheduledTask *t=&schedule_tasks[current];
    task_free();t->state=0;t->terminate_signal=0;t->exit_pending=0;t->running=0;io_epoch++;schedule_vfork_done((uint64_t)current+1);
    free(t->threads);t->threads=NULL;thread_now=-1;
    if(!current) {                                       /* the root exited: every worker stops */
        schedule_result=result;schedule_done=1;schedule_kick();
        for(unsigned i=0;i<worker_count;i++)if(workers[i]!=worker && worker_interrupt)worker_interrupt(workers[i]);
        worker->task=-1;return 1;
    }
    for(unsigned i=0;i<process_count;i++)if(process_children[i].parent==(uint64_t)current+1)process_children[i].parent=1;
    process_children[current-1].live=0;process_children[current-1].status=terminating?terminating:(unsigned)result<<8;schedule_live--;
    proc_publish((uint64_t)current+1);
    ScheduledTask *parent=&schedule_tasks[process_children[current-1].parent-1];
    if(job_control_enabled)schedule_signal(parent,17);
    if((parent->signal_ignored&(UINT64_C(1)<<16)) || (parent->signal_flags[16]&2))process_children[current-1].pid=0;
    return 0;
}
#define THREAD_EXIT (-4)  /* run(): the current thread ended, other threads of the process live on */
/* Readiness of blocked tasks' sockets is polled on scheduling decisions only this
   often while no worker is idle (an idle worker polls continuously instead): each
   poll covers every blocked task under the runtime lock, and one poll wakes them all. */
#define NETWORK_PUMP_INTERVAL_NS 250000
static uint64_t network_pump_last_ns;
/* Run tasks until the root exits (worker 0, which returns its status) or another
   worker sees the run end (it returns -1). Holds the lock throughout, except while
   guest code runs or the worker waits. current: the task already loaded, or -1. */
static int schedule_loop(int current) {
    for(;;) {
        if(schedule_done) {
            if(current>=0) { schedule_leave(current);schedule_unload();current=-1; }
            worker->task=-1;return worker==&worker_first?schedule_result:-1;
        }
        if(current<0) {
            worker->task=-1;
            int thread=-1,next=schedule_pick(worker->index,&thread);
            if(next<0) { schedule_idle_step();continue; }
            current=next;schedule_enter(current,thread);
        }
        worker->task=current;
        if(!schedule_polling) {
            uint64_t now=thread_time_ns(0);
            if(now-network_pump_last_ns>=NETWORK_PUMP_INTERVAL_NS) { network_pump_last_ns=now;timer_pump();network_pump(0); }
        }
        terminal_pump(0);host_signal_pump();terminal_waiting=0;
        process_pid=(uint64_t)current+1;process_depth=current!=0;schedule_slice=4096;
        ScheduledTask *t=&schedule_tasks[current];
        Thread *th=task_thread(t);
        if(th) { th->worker=worker->index+1;t->running++; } else t->worker=worker->index+1;
        if(th && th->state==2)cpu->gpr[0]=linux_error(110);       /* picked because its futex wait timed out */
        unsigned *blocked_call=th?&th->blocked:&t->blocked;
        if(job_control_enabled && !t->terminate_signal && !t->stop_signal && !t->exit_pending) { mm_lock(t);signal_deliver((int)*blocked_call);mm_unlock(t); }
        if(th)th->state=1;else t->state=1;
        *blocked_call=0;
        /* Events for this task while it runs (other workers act meanwhile) count as
           having happened after it blocks: it retries rather than miss them. */
        uint64_t epoch_seen=io_epoch,wakeups_seen=th?th->wakeups:t->wakeups;
        int result=t->exit_pending?-1:t->terminate_signal?128+(int)t->terminate_signal:t->stop_signal?-3:run(0);
        if(!th && t->threads)th=task_thread(t);                  /* the slice made the process threaded */
        if(th) { th->worker=0;t->running--; } else t->worker=0;
        if(th && (t->exit_pending || !th->state)) {              /* the process ended, or exec ended this thread */
            th->state=0;thread_detach(t);
            if(t->exit_pending && !t->running && schedule_finish(current,t->exit_result,t->exit_signal))
                return worker==&worker_first?schedule_result:-1;
            schedule_unload();current=-1;continue;
        }
        int blocked=result==-2;
        if(t->stop_signal)result=-3;
        unsigned terminating=t->terminate_signal;
        if(terminating) { result=128+(int)terminating;schedule_cancellations++; }
        if(th && result==THREAD_EXIT) { th->state=0;thread_detach(t);schedule_events++;schedule_unload();current=-1;continue; }
        if(result>=0) {
            if(th && t->running) {                                /* other threads still run: the last one out frees it */
                t->exit_pending=1;t->exit_result=result;t->exit_signal=terminating;th->state=0;
                schedule_interrupt(t);thread_detach(t);schedule_unload();current=-1;continue;
            }
            if(schedule_finish(current,result,terminating))return worker==&worker_first?result:-1;
            schedule_unload();current=-1;continue;
        }
        if(th) {
            if(result==-3)th->state=1;
            else if(result==-2)th->state=th->state==2?2:th->wakeups==wakeups_seen?3:1;
            else th->state=1;
            th->input_wait=terminal_waiting;th->blocked=result==-2 && th->state!=2;
            th->epoch=worker_threads?epoch_seen:io_epoch;
            if(result==-3)t->state=3;
            if(result==-2 && th->network.active)schedule_kick();  /* a sleeping poller adds its sockets */
        } else {
            t->state=result==-3?3:result==-2 && t->wakeups==wakeups_seen?2:1;t->input_wait=terminal_waiting;t->blocked=(unsigned)blocked;
            t->epoch=worker_threads?epoch_seen:io_epoch;
            if(result==-2 && t->network_wait.active)schedule_kick();
        }
        if(result==-3) {
            if(current) {
                process_children[current-1].event_status=(t->stop_signal<<8)|127;
                ScheduledTask *parent=&schedule_tasks[process_children[current-1].parent-1];
                if(!(parent->signal_flags[16]&1))schedule_signal(parent,17);
            }
            t->stop_signal=0;io_epoch++;
        }
        int thread=-1,next=schedule_pick((unsigned)current,&thread);
        if(next==current && thread==thread_now)continue;      /* only this one can run: keep it loaded */
        int again=th?thread_runnable(th):schedule_runnable(t);
        schedule_leave(current);
        if(again)schedule_events++;                           /* an idle worker may take it */
        if(next>=0)schedule_switches++;
        current=next;
        if(current>=0)schedule_enter(current,thread);else schedule_unload();
    }
}
static void *schedule_worker(void *argument) {
    Worker *self=argument;
    pthread_mutex_lock(&runtime_mutex);runtime_held=1;
    worker=self;cpu=&self->context->cpu;address_space=image_home_scheduled=&self->context->memory;
    task_load(&schedule_spare);                              /* this thread's globals start empty */
    (void)schedule_loop(-1);
    schedule_parked++;pthread_cond_broadcast(&schedule_idle_cond);
    runtime_held=0;pthread_mutex_unlock(&runtime_mutex);
    return NULL;
}
static int schedule_run(uint64_t steps) {
    schedule_active=1;process_steps=steps;schedule_done=0;schedule_parked=0;
    for(unsigned i=0;i<=PROCESS_LIMIT;i++)pthread_mutex_init(&schedule_tasks[i].mm,NULL);
    task_save(&schedule_tasks[0]);task_load(&schedule_tasks[0]);    /* the first process now lives in its task */
    schedule_tasks[0].state=1;schedule_tasks[0].pgid=1;
    if(!workers_supported)worker_count=1;                  /* the interpreter runs guest code under the lock */
    for(unsigned i=1;i<worker_count;i++)if(!workers[i]) {
        Worker *w=calloc(1,sizeof(*w));RuntimeContext *context=calloc(1,sizeof(*context));
        if(!w || !context)fail("Cannot allocate a scheduler worker");
        w->context=context;w->index=i;w->task=-1;workers[i]=w;
    }
    if(worker_count>1) {
        if(pipe(schedule_poke) || fcntl(schedule_poke[0],F_SETFL,O_NONBLOCK) || fcntl(schedule_poke[1],F_SETFL,O_NONBLOCK))fail("Cannot create the scheduler wake pipe");
        worker_threads=1;pthread_mutex_lock(&runtime_mutex);runtime_held=1;worker_first.task=0;
        for(unsigned i=1;i<worker_count;i++) {
            Worker *w=workers[i];w->task=-1;
            pthread_t thread;
            if(pthread_create(&thread,NULL,schedule_worker,w) || pthread_detach(thread))fail("Cannot start a scheduler worker");
        }
    }
    int result=schedule_loop(0);
    if(worker_threads) {                                     /* the other workers park, then the run ends */
        while(schedule_parked<worker_count-1) {
            schedule_kick();for(unsigned i=1;i<worker_count;i++)if(worker_interrupt)worker_interrupt(workers[i]);
            schedule_wait_idle(10);
        }
        worker_threads=0;pthread_mutex_unlock(&runtime_mutex);
        close(schedule_poke[0]);close(schedule_poke[1]);schedule_poke[0]=schedule_poke[1]=-1;
    }
    schedule_cleanup();schedule_active=0;process_depth=0;process_pid=1;
    return result;
}
