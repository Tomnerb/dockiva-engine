/* seccomp filters (SECCOMP_SET_MODE_FILTER, PR_SET_SECCOMP): classic BPF programs run
   on every system call of the process that installed them, and inherited across
   fork and exec, as on Linux. Elasticsearch 9 refuses to start without one (it
   denies itself exec). Filters are kept per process: TSYNC is implied, and a filter
   installed by one thread applies to its siblings too. Actions: ALLOW and LOG run the
   call, ERRNO fails it, KILL_* and TRAP end the process with SIGSYS, TRACE and
   USER_NOTIF (no tracer, no listener) fail it with ENOSYS. */
#define SECCOMP_INSTRUCTIONS 4096u
typedef struct { uint16_t code;uint8_t jt,jf;uint32_t k; } SeccompInstruction;
typedef struct SeccompFilter { unsigned refs,count;struct SeccompFilter *next;SeccompInstruction program[]; } SeccompFilter;
static SeccompFilter *seccomp_filters[PROCESS_LIMIT+2];        /* by pid: the newest filter, linked to older ones */

static void seccomp_release(SeccompFilter *f) {
    while(f && !--f->refs) { SeccompFilter *next=f->next;free(f);f=next; }
}
static void seccomp_forget(uint64_t pid) {
    if(pid && pid<=PROCESS_LIMIT+1) { seccomp_release(seccomp_filters[pid]);seccomp_filters[pid]=NULL; }
}
static void seccomp_inherit(uint64_t child,uint64_t parent) {
    if(!child || child>PROCESS_LIMIT+1 || !parent || parent>PROCESS_LIMIT+1)return;
    seccomp_forget(child);
    if((seccomp_filters[child]=seccomp_filters[parent]))seccomp_filters[child]->refs++;
}
/* Runs one program over struct seccomp_data {nr, arch, instruction_pointer, args[6]}. */
static uint32_t seccomp_run(const SeccompFilter *f,const uint32_t data[16]) {
    uint32_t a=0,x=0,memory[16]={0};
    for(unsigned pc=0;pc<f->count;pc++) {
        const SeccompInstruction *i=&f->program[pc];
        switch(i->code) {
            case 0x20:a=data[i->k/4];break;                                    /* ld [k] */
            case 0x80:a=sizeof(uint32_t)*16;break;                            /* ld #len */
            case 0x00:a=i->k;break;                                            /* ld #k */
            case 0x01:x=i->k;break;                                            /* ldx #k */
            case 0x81:x=sizeof(uint32_t)*16;break;                            /* ldx #len */
            case 0x60:a=memory[i->k];break;case 0x61:x=memory[i->k];break;   /* ld/ldx M[k] */
            case 0x02:memory[i->k]=a;break;case 0x03:memory[i->k]=x;break;   /* st/stx M[k] */
            case 0x04:a+=i->k;break;case 0x0c:a+=x;break;
            case 0x14:a-=i->k;break;case 0x1c:a-=x;break;
            case 0x24:a*=i->k;break;case 0x2c:a*=x;break;
            case 0x34:a/=i->k;break;case 0x3c:a=x?a/x:0;break;
            case 0x94:a%=i->k;break;case 0x9c:a=x?a%x:0;break;
            case 0x44:a|=i->k;break;case 0x4c:a|=x;break;
            case 0x54:a&=i->k;break;case 0x5c:a&=x;break;
            case 0xa4:a^=i->k;break;case 0xac:a^=x;break;
            case 0x64:a<<=i->k;break;case 0x6c:a=x<32?a<<x:0;break;
            case 0x74:a>>=i->k;break;case 0x7c:a=x<32?a>>x:0;break;
            case 0x84:a=0u-a;break;
            case 0x05:pc+=i->k;break;                                          /* ja */
            case 0x15:pc+=a==i->k?i->jt:i->jf;break;case 0x1d:pc+=a==x?i->jt:i->jf;break;
            case 0x25:pc+=a>i->k?i->jt:i->jf;break;case 0x2d:pc+=a>x?i->jt:i->jf;break;
            case 0x35:pc+=a>=i->k?i->jt:i->jf;break;case 0x3d:pc+=a>=x?i->jt:i->jf;break;
            case 0x45:pc+=(a&i->k)?i->jt:i->jf;break;case 0x4d:pc+=(a&x)?i->jt:i->jf;break;
            case 0x06:return i->k;case 0x16:return a;                        /* ret #k, ret a */
            case 0x07:x=a;break;case 0x87:a=x;break;                          /* tax, txa */
            default:return 0;                                                  /* unreachable: checked on install */
        }
    }
    return 0;
}
/* Checks a program as Linux's seccomp_check_filter does: known instructions, aligned
   loads inside seccomp_data, scratch slots in range, jumps forward and in bounds,
   ending in a return. */
static int seccomp_valid(const SeccompInstruction *p,unsigned count) {
    for(unsigned pc=0;pc<count;pc++) {
        uint16_t c=p[pc].code;uint32_t k=p[pc].k;
        switch(c) {
            case 0x20:if(k>=64 || (k&3))return 0;break;
            case 0x60:case 0x61:case 0x02:case 0x03:if(k>=16)return 0;break;
            case 0x34:case 0x94:if(!k)return 0;break;
            case 0x64:case 0x74:if(k>=32)return 0;break;
            case 0x05:if(k>=count-pc-1)return 0;break;
            case 0x15:case 0x1d:case 0x25:case 0x2d:case 0x35:case 0x3d:case 0x45:case 0x4d:
                if(p[pc].jt>=count-pc-1 || p[pc].jf>=count-pc-1)return 0;break;
            case 0x80:case 0x00:case 0x01:case 0x81:case 0x04:case 0x0c:case 0x14:case 0x1c:case 0x24:case 0x2c:
            case 0x3c:case 0x9c:case 0x44:case 0x4c:case 0x54:case 0x5c:case 0xa4:case 0xac:case 0x6c:case 0x7c:
            case 0x84:case 0x06:case 0x16:case 0x07:case 0x87:break;
            default:return 0;
        }
    }
    return count && (p[count-1].code==0x06 || p[count-1].code==0x16);
}
/* Installs the sock_fprog {len, filter} at address (flags: TSYNC, LOG, SPEC_ALLOW). */
static uint64_t seccomp_install(uint64_t flags,uint64_t address) {
    /* Linux's order: EINVAL for flags, EFAULT for the fprog, EINVAL for its length, then
       EACCES (Elasticsearch probes with a NULL fprog before setting no_new_privs). */
    if(flags&~UINT64_C(7))return linux_error(22);
    const unsigned char *fprog=lookup(address,16,4);if(!fprog)return linux_error(14);
    unsigned count=(unsigned)le(fprog,2);uint64_t program=le(fprog+8,8);
    if(!count || count>SECCOMP_INSTRUCTIONS)return linux_error(22);
    if(!prctl_no_new_privs)return linux_error(13);                     /* no CAP_SYS_ADMIN in a container */
    if(!process_pid || process_pid>PROCESS_LIMIT+1)return linux_error(12);
    const unsigned char *raw=lookup(program,(uint64_t)count*8,4);if(!raw)return linux_error(14);
    unsigned depth=0;for(SeccompFilter *f=seccomp_filters[process_pid];f;f=f->next)depth+=f->count+4;
    if(depth+count>32768)return linux_error(12);                          /* MAX_INSNS_PER_PATH */
    SeccompFilter *f=malloc(sizeof(*f)+(size_t)count*sizeof(SeccompInstruction));if(!f)return linux_error(12);
    for(unsigned i=0;i<count;i++)f->program[i]=(SeccompInstruction){(uint16_t)le(raw+i*8,2),raw[i*8+2],raw[i*8+3],(uint32_t)le(raw+i*8+4,4)};
    if(!seccomp_valid(f->program,count)) { free(f);return linux_error(22); }
    f->refs=1;f->count=count;f->next=seccomp_filters[process_pid];seccomp_filters[process_pid]=f;
    return 0;
}
static uint64_t guest_seccomp(void) {
    switch(cpu->gpr[0]) {
        case 1:return seccomp_install(cpu->gpr[1],cpu->gpr[2]);           /* SECCOMP_SET_MODE_FILTER */
        case 2: {                                                         /* SECCOMP_GET_ACTION_AVAIL */
            if(cpu->gpr[1])return linux_error(22);
            const unsigned char *p=lookup(cpu->gpr[2],4,4);if(!p)return linux_error(14);
            uint32_t action=(uint32_t)le(p,4);
            return action==0x80000000u || action==0 || action==0x00030000u || action==0x00050000u || action==0x7ffc0000u || action==0x7fff0000u?0:linux_error(95);
        }
        default:return linux_error(22);                                   /* strict mode and notifications: unsupported */
    }
}
static int seccomp_active(void) { return process_pid && process_pid<=PROCESS_LIMIT+1 && seccomp_filters[process_pid]; }
/* Before a system call runs: 0 to run it, or the dispatch result with its outcome. */
static int seccomp_check(void) {
    if(!process_pid || process_pid>PROCESS_LIMIT+1 || !seccomp_filters[process_pid])return 0;
    uint32_t data[16];data[0]=(uint32_t)cpu->gpr[8];data[1]=0xc00000b7u;   /* AUDIT_ARCH_AARCH64 */
    data[2]=(uint32_t)cpu->pc;data[3]=(uint32_t)(cpu->pc>>32);
    for(unsigned i=0;i<6;i++) { data[4+i*2]=(uint32_t)cpu->gpr[i];data[5+i*2]=(uint32_t)(cpu->gpr[i]>>32); }
    uint32_t result=0x7fff0000u;
    for(SeccompFilter *f=seccomp_filters[process_pid];f;f=f->next) {
        uint32_t r=seccomp_run(f,data);
        if((int32_t)(r&0xffff0000u)<(int32_t)(result&0xffff0000u))result=r;   /* the most restrictive action wins */
    }
    uint32_t action=result&0xffff0000u;
    if(action==0x7fff0000u || action==0x7ffc0000u)return 0;               /* ALLOW, LOG */
    if(action==0x00050000u) { uint32_t e=result&0xffff;cpu->gpr[0]=linux_error(e>4095?4095:e);return -3; }
    if(action==0x7ff00000u || action==0x7fc00000u) { cpu->gpr[0]=linux_error(38);return -3; }
    /* KILL_PROCESS, KILL_THREAD, TRAP: SIGSYS. ponytail: the whole process ends, even
       for KILL_THREAD, and a TRAP handler is not run. */
    if(!schedule_active) { fprintf(stderr,"vmfree: seccomp: system call %u denied\n",data[0]);exit(128+31); }
    schedule_tasks[process_pid-1].terminate_signal=31;return -1;
}
