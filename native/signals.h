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
        if(action[0]>1 && !job_control_enabled)return linux_error(95);
        /* Without SA_RESTORER (Go on arm64), handlers return through the vDSO's
           rt_sigreturn trampoline, as on Linux; without a vDSO a restorer is required. */
        if(action[0]>1 && ((action[1]&UINT64_C(0x04000000))?!action[2]:!vdso_backing))return linux_error(95);
        if(action[1]&~UINT64_C(0xdc000007))return linux_error(95); /* SA_ONSTACK (0x08000000) included */
        /* Unsupported flag bits fail instead of silently changing delivery. */
    }
    unsigned index=(unsigned)signal-1;uint64_t bit=UINT64_C(1)<<index;
    if(out) {
        fs_pack(out,0,(task->signal_ignored&bit)?1:task->signal_handler[index],8);
        fs_pack(out,8,task->signal_flags[index],8);fs_pack(out,16,task->signal_restorer[index],8);
        fs_pack(out,24,task->signal_action_mask[index],8);
    }
    if(in) {
        if(action[0]==1) { task->signal_ignored|=bit;task->signal_pending&=~bit; }
        else task->signal_ignored&=~bit;
        task->signal_handler[index]=action[0]>1?action[0]:0;
        task->signal_flags[index]=action[1];task->signal_restorer[index]=action[2];
        task->signal_action_mask[index]=action[3];
    }
    return 0;
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
    uint64_t pending=(task->signal_pending|own) & ~*mask & ~task->signal_ignored;
    if(!pending)return;
    unsigned index=0;while(!(pending&(UINT64_C(1)<<index)))index++;
    uint64_t bit=UINT64_C(1)<<index,handler=task->signal_handler[index],flags=task->signal_flags[index];
    if(own&bit)th->signal_pending&=~bit;else task->signal_pending&=~bit;
    if(!handler) { schedule_signal(task,index+1);return; }
    int on_altstack=cpu->altstack_size && cpu->sp>cpu->altstack_sp && cpu->sp-cpu->altstack_sp<=cpu->altstack_size;
    uint64_t base=(flags&UINT64_C(0x08000000)) && cpu->altstack_size && !on_altstack?cpu->altstack_sp+cpu->altstack_size:cpu->sp;
    uint64_t frame=base>=SIGNAL_FRAME_SIZE?(base-SIGNAL_FRAME_SIZE)&~UINT64_C(15):0;
    unsigned char *out=frame?lookup(frame,SIGNAL_FRAME_SIZE,2):NULL;
    uint64_t restorer=(flags&UINT64_C(0x04000000))?task->signal_restorer[index]:VDSO_SIGRETURN;
    if(!out || (handler&3) || !lookup(handler,4,1) || !lookup(restorer,4,1)) {
        task->terminate_signal=11;return;
    }
    if(blocked && !(flags&UINT64_C(0x10000000))) { cpu->pc+=4;cpu->gpr[0]=linux_error(4);net_wait.active=0; }
    memset(out,0,SIGNAL_FRAME_SIZE);
    fs_pack(out,0,index+1,4);fs_pack(out,16,task->signal_sender[index],4);
    fs_pack(out,144,cpu->altstack_sp,8);fs_pack(out,160,cpu->altstack_size,8);   /* uc_stack, as interrupted */
    fs_pack(out,152,!cpu->altstack_size?2:on_altstack?1:0,4);                    /* SS_DISABLE / SS_ONSTACK */
    fs_pack(out,168,*mask,8);
    for(unsigned i=0;i<31;i++)fs_pack(out,312+i*8,cpu->gpr[i],8);
    fs_pack(out,560,cpu->sp,8);fs_pack(out,568,cpu->pc,8);
    fs_pack(out,576,((uint64_t)cpu->flag_n<<31)|((uint64_t)cpu->flag_z<<30)|((uint64_t)cpu->flag_c<<29)|((uint64_t)cpu->flag_v<<28),8);
    fs_pack(out,592,UINT64_C(0x46508001),4);fs_pack(out,596,528,4);
    memcpy(out+608,cpu->simd,sizeof(cpu->simd));
    fs_pack(out,4688,cpu->gpr[29],8);fs_pack(out,4696,cpu->gpr[30],8);
    cpu->gpr[0]=index+1;cpu->gpr[1]=frame;cpu->gpr[2]=frame+128;
    cpu->gpr[29]=frame+4688;cpu->gpr[30]=restorer;cpu->sp=frame;cpu->pc=handler;
    *mask|=task->signal_action_mask[index];
    if(!(flags&UINT64_C(0x40000000)))*mask|=bit;
    *mask&=~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
    if(flags&UINT64_C(0x80000000))task->signal_handler[index]=0;
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
