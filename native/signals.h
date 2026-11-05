/* Linux ARM64 kernel sigaction layout: handler, flags, restorer, 64-bit mask.
   Caught handlers and frames are enabled only with the job-control scheduler. */
static uint64_t signal_call(uint64_t call) {
    if(cpu->gpr[3]!=8)return linux_error(22);
    ScheduledTask *task=&schedule_tasks[process_pid-1];
    if(call==135) {
        unsigned char *in=cpu->gpr[1]?lookup(cpu->gpr[1],8,4):NULL;
        unsigned char *out=cpu->gpr[2]?lookup(cpu->gpr[2],8,2):NULL;
        if((cpu->gpr[1] && !in) || (cpu->gpr[2] && !out))return linux_error(14);
        uint64_t *mask=signal_mask_now(),next=*mask;
        if(in) {
            uint64_t value=le(in,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
            if(cpu->gpr[0]==0)next|=value;
            else if(cpu->gpr[0]==1)next&=~value;
            else if(cpu->gpr[0]==2)next=value;
            else return linux_error(22);
        }
        if(out)fs_pack(out,0,*mask,8);
        *mask=next;schedule_pending(task);return 0;
    }
    uint64_t signal=cpu->gpr[0];
    if(!signal || signal>64 || signal==9 || signal==19)return linux_error(22);
    unsigned char *in=cpu->gpr[1]?lookup(cpu->gpr[1],32,4):NULL;
    unsigned char *out=cpu->gpr[2]?lookup(cpu->gpr[2],32,2):NULL;
    if((cpu->gpr[1] && !in) || (cpu->gpr[2] && !out))return linux_error(14);
    uint64_t action[4]={0};
    if(in) {
        for(unsigned i=0;i<4;i++)action[i]=le(in+i*8,8);
        action[1]&=UINT64_C(0xffffffff); /* musl's int sa_flags arrives sign-extended (SA_RESETHAND); Linux reads 32 bits */
        if(action[0]>1 && !job_control_enabled)return linux_error(95);
        /* Without SA_RESTORER (Go on arm64), handlers return through the vDSO's
           rt_sigreturn trampoline, as on Linux; without a vDSO a restorer is required. */
        if(action[0]>1 && ((action[1]&UINT64_C(0x04000000))?!action[2]:!vdso_backing))return linux_error(95);
        if(action[1]&~UINT64_C(0xdc000007))return linux_error(95); /* SA_ONSTACK (0x08000000) included */
        /* Unsupported flag bits fail instead of silently changing delivery. */
    }
    unsigned index=(unsigned)signal-1;uint64_t bit=UINT64_C(1)<<index;
    if(out) {
        fs_pack(out,0,(task_sig(task)->ignored&bit)?1:task_sig(task)->handler[index],8);
        fs_pack(out,8,task_sig(task)->flags[index],8);fs_pack(out,16,task_sig(task)->restorer[index],8);
        fs_pack(out,24,task_sig(task)->mask[index],8);
    }
    if(in) {
        if(action[0]==1) { task_sig(task)->ignored|=bit;task->signal_pending&=~bit; }
        else task_sig(task)->ignored&=~bit;
        task_sig(task)->handler[index]=action[0]>1?action[0]:0;
        task_sig(task)->flags[index]=action[1];task_sig(task)->restorer[index]=action[2];
        task_sig(task)->mask[index]=action[3];
    }
    return 0;
}

/* rt_sigsuspend(mask, 8): wait, under mask, for a signal with a handler (or that
   ends the process). The scheduler delivers it as an interrupted blocking call. */
static uint64_t signal_suspend(void) {
    if(!job_control_enabled || !schedule_active)return linux_error(95);
    if(cpu->gpr[1]!=8)return linux_error(22);
    ScheduledTask *task=&schedule_tasks[process_pid-1];uint64_t *mask=signal_mask_now();
    if(!task->suspending) {
        unsigned char *in=lookup(cpu->gpr[0],8,4);if(!in)return linux_error(14);
        task->suspend_mask=*mask;task->suspending=1;
        *mask=le(in,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
        schedule_pending(task);
    }
    return IO_BLOCK;
}
/* The thread's (or single-threaded process's) record of a mask set for one wait. */
static void signal_wait_state(unsigned **masked,uint64_t **saved) {
    Thread *th=thread_current();ScheduledTask *task=&schedule_tasks[process_pid-1];
    *masked=th?&th->wait_masked:&task->wait_masked;*saved=th?&th->wait_saved_mask:&task->wait_saved_mask;
}
static int signal_wait_mask(uint64_t mask) {
    if(!job_control_enabled || !schedule_active)return 0;                           /* no guest signals: nothing to unblock */
    unsigned *masked;uint64_t *saved,*now=signal_mask_now();signal_wait_state(&masked,&saved);
    if(!*masked) { *saved=*now;*masked=1; }                                         /* a retried wait keeps the first */
    *now=mask&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
    ScheduledTask *task=&schedule_tasks[process_pid-1];Thread *th=thread_current();
    return (((task->signal_pending|(th?th->signal_pending:0))&~*now&~task_sig(task)->ignored))!=0;
}
static void signal_wait_unmask(void) {
    if(!job_control_enabled || !schedule_active)return;
    unsigned *masked;uint64_t *saved;signal_wait_state(&masked,&saved);
    if(*masked) { *signal_mask_now()=*saved;*masked=0; }
}
/* A signal taken by rt_sigtimedwait: its siginfo (number, SI_USER, sender, uid) if
   asked for; returns the number. */
static uint64_t signal_waited(ScheduledTask *task,unsigned index) {
    unsigned char *info=task->sigwait_info?lookup(task->sigwait_info,128,2):NULL;
    if(info) { memset(info,0,128);fs_pack(info,0,index+1,4);fs_pack(info,16,task->signal_sender[index],4);fs_pack(info,20,guest_uid,4); }
    return index+1;
}
/* rt_sigtimedwait(set, info, timeout, 8): take a pending signal of set, or wait for
   one up to timeout (none: forever). Meanwhile set is unblocked as in rt_sigsuspend,
   and signal_deliver completes the call with the first that arrives; a wait that
   times out returns EAGAIN (re-executed after the sleep, like nanosleep). */
static uint64_t signal_timedwait(void) {
    if(!job_control_enabled || !schedule_active)return linux_error(95);
    if(cpu->gpr[3]!=8)return linux_error(22);
    ScheduledTask *task=&schedule_tasks[process_pid-1];Thread *th=thread_current();uint64_t *mask=signal_mask_now();
    if(!task->sigwaiting) {
        const unsigned char *in=lookup(cpu->gpr[0],8,4);if(!in)return linux_error(14);
        uint64_t set=le(in,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18)),own=th?th->signal_pending:0;
        uint64_t ready=(task->signal_pending|own)&set;
        if(ready) {
            unsigned index=0;while(!(ready&(UINT64_C(1)<<index)))index++;
            uint64_t bit=UINT64_C(1)<<index;if(own&bit)th->signal_pending&=~bit;else task->signal_pending&=~bit;
            task->sigwait_info=cpu->gpr[1];return signal_waited(task,index);
        }
        if(cpu->gpr[2]) {
            const unsigned char *ts=lookup(cpu->gpr[2],16,4);if(!ts)return linux_error(14);
            if(le(ts+8,8)>=1000000000 || (int64_t)le(ts,8)<0)return linux_error(22);
            if(!le(ts,8) && !le(ts+8,8))return linux_error(11);                     /* a poll */
        }
        task->sigwaiting=1;task->sigwait_set=set;task->sigwait_info=cpu->gpr[1];
        task->suspend_mask=*mask;*mask&=~set;
    }
    uint64_t saved[4]={cpu->gpr[0],cpu->gpr[1],cpu->gpr[2],cpu->gpr[3]};
    cpu->gpr[0]=0;cpu->gpr[1]=0;cpu->gpr[3]=0;                                       /* ppoll(NULL, 0, timeout, NULL) */
    uint64_t result=net_poll();
    cpu->gpr[0]=saved[0];cpu->gpr[1]=saved[1];cpu->gpr[2]=saved[2];cpu->gpr[3]=saved[3];
    if(result==IO_BLOCK)return result;
    task->sigwaiting=0;*mask=task->suspend_mask;
    return linux_error(11);
}
/* ARM64 rt_sigframe: 128-byte siginfo, ucontext, and aligned FPSIMD records.
   Offsets follow Linux arch/arm64/include/uapi/asm/{ucontext,sigcontext}.h.
   SA_ONSTACK handlers run on the thread's sigaltstack. No SVE or floating-point
   status changes yet. */
#define SIGNAL_FRAME_SIZE 4704u
static void signal_deliver(int blocked) {
    if(!job_control_enabled || !schedule_active)return;
    ScheduledTask *task=&schedule_tasks[process_pid-1];Thread *th=thread_current();
    uint64_t *mask=signal_mask_now(),own=th?th->signal_pending:0;     /* own: signals sent to this thread */
    uint64_t pending=(task->signal_pending|own) & ~*mask & ~task_sig(task)->ignored;
    if(!pending)return;
    unsigned index=0;while(!(pending&(UINT64_C(1)<<index)))index++;
    uint64_t bit=UINT64_C(1)<<index,handler=task_sig(task)->handler[index],flags=task_sig(task)->flags[index];
    int from_thread=(own&bit)!=0;
    if(from_thread)th->signal_pending&=~bit;else task->signal_pending&=~bit;
    if(task->sigwaiting && (task->sigwait_set&bit)) {          /* rt_sigtimedwait takes it */
        task->sigwaiting=0;*mask=task->suspend_mask;
        if(blocked)cpu->pc+=4;
        net_wait.active=0;cpu->gpr[0]=signal_waited(task,index);return;
    }
    if(!handler) { schedule_signal(task,index+1);return; }
    int on_altstack=cpu->altstack_size && cpu->sp>cpu->altstack_sp && cpu->sp-cpu->altstack_sp<=cpu->altstack_size;
    uint64_t base=(flags&UINT64_C(0x08000000)) && cpu->altstack_size && !on_altstack?cpu->altstack_sp+cpu->altstack_size:cpu->sp;
    uint64_t frame=base>=SIGNAL_FRAME_SIZE?(base-SIGNAL_FRAME_SIZE)&~UINT64_C(15):0;
    unsigned char *out=frame?lookup(frame,SIGNAL_FRAME_SIZE,2):NULL;
    uint64_t restorer=(flags&UINT64_C(0x04000000))?task_sig(task)->restorer[index]:VDSO_SIGRETURN;
    if(!out || (handler&3) || !lookup(handler,4,1) || !lookup(restorer,4,1)) {
        task->terminate_signal=11;return;
    }
    /* rt_sigsuspend: the handler's frame restores the mask it replaced, and it returns
       EINTR (never restarted); other interrupted calls restart under SA_RESTART. */
    uint64_t frame_mask=*mask;int suspended=(int)(task->suspending || task->sigwaiting);
    if(suspended) { frame_mask=task->suspend_mask;task->suspending=task->sigwaiting=0; }
    unsigned *wait_masked;uint64_t *wait_saved;signal_wait_state(&wait_masked,&wait_saved);
    if(*wait_masked) { frame_mask=*wait_saved;*wait_masked=0;suspended=1; }         /* ppoll/pselect6: EINTR, never restarted */
    /* Linux never restarts these after a handler, SA_RESTART or not (signal(7)): epoll_pwait(2),
       ppoll, pselect6, nanosleep and clock_nanosleep return EINTR. PostgreSQL's latches rely on it. */
    uint64_t call=cpu->gpr[8];
    int never_restart=call==22 || call==441 || call==73 || call==72 || call==101 || call==115;
    if(blocked && (suspended || never_restart || !(flags&UINT64_C(0x10000000)))) { cpu->pc+=4;cpu->gpr[0]=linux_error(4);net_wait.active=0;io_fifo_forget(process_pid); }
    memset(out,0,SIGNAL_FRAME_SIZE);
    fs_pack(out,0,index+1,4);fs_pack(out,16,task->signal_sender[index],4);
    if(from_thread && th->fault_signal==index+1) { /* a fault: si_code and si_addr */
        fs_pack(out,8,th->fault_code,4);fs_pack(out,16,th->fault_addr,8);th->fault_signal=0;
    } else if(task->signal_fault_code[index]) {
        fs_pack(out,8,task->signal_fault_code[index],4);fs_pack(out,16,task->signal_fault_addr[index],8);
        task->signal_fault_code[index]=0;
    } else if(from_thread)fs_pack(out,8,(uint64_t)-6,4);     /* SI_TKILL: glibc's setuid/setgid broadcast (SIGSETXID) requires it */
    fs_pack(out,144,cpu->altstack_sp,8);fs_pack(out,160,cpu->altstack_size,8);   /* uc_stack, as interrupted */
    fs_pack(out,152,!cpu->altstack_size?2:on_altstack?1:0,4);                    /* SS_DISABLE / SS_ONSTACK */
    fs_pack(out,168,frame_mask,8);
    for(unsigned i=0;i<31;i++)fs_pack(out,312+i*8,cpu->gpr[i],8);
    fs_pack(out,560,cpu->sp,8);fs_pack(out,568,cpu->pc,8);
    fs_pack(out,576,((uint64_t)cpu->flag_n<<31)|((uint64_t)cpu->flag_z<<30)|((uint64_t)cpu->flag_c<<29)|((uint64_t)cpu->flag_v<<28),8);
    fs_pack(out,592,UINT64_C(0x46508001),4);fs_pack(out,596,528,4);
    memcpy(out+608,cpu->simd,sizeof(cpu->simd));
    fs_pack(out,4688,cpu->gpr[29],8);fs_pack(out,4696,cpu->gpr[30],8);
    cpu->gpr[0]=index+1;cpu->gpr[1]=frame;cpu->gpr[2]=frame+128;
    cpu->gpr[29]=frame+4688;cpu->gpr[30]=restorer;cpu->sp=frame;cpu->pc=handler;
    *mask|=task_sig(task)->mask[index];
    if(!(flags&UINT64_C(0x40000000)))*mask|=bit;
    *mask&=~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
    if(flags&UINT64_C(0x80000000))task_sig(task)->handler[index]=0;
    cpu->exclusive_valid=0;
}
static void signal_return(void) {
    ScheduledTask *task=&schedule_tasks[process_pid-1];
    unsigned char *in=(cpu->sp&15)?NULL:lookup(cpu->sp,SIGNAL_FRAME_SIZE,4);
    if(!in || le(in+592,4)!=UINT64_C(0x46508001) || le(in+596,4)!=528 ||
       le(in+600,8) || le(in+1120,8) || le(in+576,8)&~UINT64_C(0xf0000000)) {
        task->terminate_signal=11;return;
    }
    uint64_t nextsp=le(in+560,8),nextpc=le(in+568,8),state=le(in+576,8);
    if((nextsp&15) || (nextpc&3) || !lookup(nextpc,4,1)) { task->terminate_signal=11;return; }
    uint64_t nextmask=le(in+168,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
    for(unsigned i=0;i<31;i++)cpu->gpr[i]=le(in+312+i*8,8);
    memcpy(cpu->simd,in+608,sizeof(cpu->simd));cpu->sp=nextsp;cpu->pc=nextpc;
    cpu->flag_n=(unsigned)(state>>31)&1;cpu->flag_z=(unsigned)(state>>30)&1;
    cpu->flag_c=(unsigned)(state>>29)&1;cpu->flag_v=(unsigned)(state>>28)&1;
    *signal_mask_now()=nextmask;cpu->exclusive_valid=0;schedule_pending(task);
}

/* ITIMER_REAL only. Millisecond host monotonic deadlines; no host timers or
   process signals are installed for guest alarms. Linux fork clears timers. */
static uint64_t signal_setitimer(void) {
    if(!job_control_enabled || !schedule_active)return linux_error(95);
    unsigned char *in=cpu->gpr[1]?lookup(cpu->gpr[1],32,4):NULL;
    if(cpu->gpr[1] && !in)return linux_error(14);
    uint64_t times[2]={0};
    if(in)for(unsigned i=0;i<2;i++) {
        uint64_t seconds=le(in+i*16,8),micros=le(in+i*16+8,8);
        if(seconds>86400 || micros>=1000000)return linux_error(22);
        times[i]=seconds*1000+(micros+999)/1000;
    }
    if(cpu->gpr[0]>2)return linux_error(22);if(cpu->gpr[0])return linux_error(95);
    ScheduledTask *t=&schedule_tasks[process_pid-1];uint64_t now=net_now();
    uint64_t remain=t->timer_deadline>now?t->timer_deadline-now:0,interval=t->timer_interval;
    t->timer_interval=times[1]?times[0]:0;t->timer_deadline=times[1]?now+times[1]:0;
    /* Linux installs the timer before copying the old value to userspace. */
    if(cpu->gpr[2]) {
        unsigned char *out=lookup(cpu->gpr[2],32,2);if(!out)return linux_error(14);
        fs_pack(out,0,interval/1000,8);fs_pack(out,8,(interval%1000)*1000,8);
        fs_pack(out,16,remain/1000,8);fs_pack(out,24,(remain%1000)*1000,8);
    }
    return 0;
}
/* sigaltstack: Linux stack_t {ss_sp, ss_flags, ss_size}. SS_DISABLE (2) removes
   the stack; SS_AUTODISARM is accepted and not applied. Changing the stack while
   running on it fails with EPERM, and stacks below MINSIGSTKSZ (5120) with ENOMEM. */
static uint64_t signal_altstack(void) {
    unsigned char *in=cpu->gpr[0]?lookup(cpu->gpr[0],24,4):NULL,*out=cpu->gpr[1]?lookup(cpu->gpr[1],24,2):NULL;
    if((cpu->gpr[0] && !in) || (cpu->gpr[1] && !out))return linux_error(14);
    int on=cpu->altstack_size && cpu->sp>cpu->altstack_sp && cpu->sp-cpu->altstack_sp<=cpu->altstack_size;
    uint64_t sp=cpu->altstack_sp,size=cpu->altstack_size;
    if(in) {
        if(on)return linux_error(1);
        uint64_t flags=le(in+8,4);
        if(flags&~(UINT64_C(2)|UINT64_C(0x80000000)))return linux_error(22);
        if(flags&2) { cpu->altstack_sp=cpu->altstack_size=0; }
        else {
            uint64_t next=le(in+16,8);if(next<5120)return linux_error(12);
            cpu->altstack_sp=le(in,8);cpu->altstack_size=next;
        }
    }
    if(out) { memset(out,0,24);fs_pack(out,0,sp,8);fs_pack(out,8,!size?2:on?1:0,4);fs_pack(out,16,size,8); }
    return 0;
}
