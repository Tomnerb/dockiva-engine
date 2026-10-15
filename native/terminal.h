/* M23: opt-in canonical host input and guest-only foreground groups.
   No host terminal flags, foreground process group, or descriptor flags change. */
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
static int terminal_ready(int wait) {
    fd_set input;FD_ZERO(&input);FD_SET(0,&input);
    struct timeval timeout={0,wait?50000:0};
    return select(wait==2?0:1,wait==2?NULL:&input,NULL,NULL,&timeout);
}
static volatile sig_atomic_t terminal_interrupt,terminal_terminate,terminal_suspend,terminal_resume;
static struct sigaction terminal_old_actions[6];
static int terminal_signal_numbers[6],terminal_installed;
static void terminal_signal(int number) {
    if(number==SIGINT || number==SIGUSR2)terminal_interrupt=1;
    else if(number==SIGTSTP || number==SIGURG)terminal_suspend=1;
    else if(number==SIGCONT)terminal_resume=1;
    else terminal_terminate=1;
}
static void terminal_restore(void) {
    while(terminal_installed) {
        terminal_installed--;
        sigaction(terminal_signal_numbers[terminal_installed],&terminal_old_actions[terminal_installed],NULL);
    }
}
static void terminal_install(int number,void (*handler)(int)) {
    struct sigaction action;memset(&action,0,sizeof(action));action.sa_handler=handler;
    sigemptyset(&action.sa_mask);
    if(terminal_installed==6 || sigaction(number,&action,&terminal_old_actions[terminal_installed])) {
        terminal_restore();fail("Cannot install terminal signal handler");
    }
    terminal_signal_numbers[terminal_installed++]=number;
}
static void terminal_init(void) {
    terminal_install(SIGINT,terminal_forwarded?SIG_IGN:terminal_signal);
    terminal_install(SIGTERM,terminal_signal);
    if(terminal_forwarded)terminal_install(SIGUSR2,terminal_signal);
    if(job_control_enabled) {
        terminal_install(SIGTSTP,terminal_forwarded?SIG_IGN:terminal_signal);
        terminal_install(SIGCONT,terminal_signal);
        if(terminal_forwarded)terminal_install(SIGURG,terminal_signal);
    }
    atexit(terminal_restore);io_fds[0]=(GuestIO){IO_INPUT,0,0,NULL,NULL,NULL,NULL,NULL};
}
static void terminal_pump(int wait) {
    if(!terminal_enabled)return;
    if(terminal_suspend || terminal_resume) {
        unsigned signal=terminal_suspend?20:18;terminal_suspend=terminal_resume=0;
        for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==terminal_foreground)
            schedule_signal(&schedule_tasks[i],signal);
    }
    if(terminal_interrupt) {
        terminal_interrupt=0;
        for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==terminal_foreground) {
            schedule_signal(&schedule_tasks[i],2);
        }
    }
    if(terminal_terminate) {
        terminal_terminate=0;schedule_signal(&schedule_tasks[0],15);
    }
    int result=terminal_ready(wait);
    if(result>0 || (result<0 && errno!=EINTR)) {
        /* Only wake input waiters: unrelated pipe deadlocks stay detectable. */
        for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state==2 && schedule_tasks[i].input_wait)
            schedule_tasks[i].state=1;
    }
}
static uint64_t terminal_read(unsigned char *data,size_t count) {
    ScheduledTask *current=&schedule_tasks[process_pid-1];
    if(current->pgid!=terminal_foreground) {
        if(!job_control_enabled || ((current->signal_ignored|current->signal_mask)&(UINT64_C(1)<<20)))return linux_error(5);
        for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==current->pgid)
            schedule_signal(&schedule_tasks[i],21);
        return IO_BLOCK;
    }
    int ready=terminal_ready(0);
    if(ready<0 && errno!=EINTR)return net_error();
    if(ready<=0) { terminal_waiting=1;return IO_BLOCK; }
    ssize_t result=read(0,data,count);
    if(result<0 && (errno==EINTR || errno==EAGAIN)) { terminal_waiting=1;return IO_BLOCK; }
    return result<0?net_error():(uint64_t)result;
}
static int terminal_group_exists(uint64_t group) {
    for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==group)return 1;
    return 0;
}
static uint64_t terminal_group(uint64_t call) {
    if(!schedule_active)return linux_error(95);                 /* bash and sh ask even without a terminal */
    uint64_t pid=cpu->gpr[0]?cpu->gpr[0]:process_pid;
    if(pid>process_count+1 || !schedule_tasks[pid-1].state)return linux_error(3);
    if(call==155)return schedule_tasks[pid-1].pgid;
    if(pid!=process_pid && (pid==1 || process_children[pid-2].parent!=process_pid))return linux_error(3);
    if(pid!=process_pid && schedule_tasks[pid-1].execed)return linux_error(13);
    uint64_t group=cpu->gpr[1]?cpu->gpr[1]:pid;
    if((int64_t)group<0 || group>INT32_MAX)return linux_error(22);
    if(group!=pid && !terminal_group_exists(group))return linux_error(1);
    schedule_tasks[pid-1].pgid=(unsigned)group;io_epoch++;return 0;
}
/* Linux termios flags (field 0 iflag, 1 oflag, 3 lflag) and control characters,
   with their host equivalents: TCGETS/TCSETS pass through to a host terminal. */
static const struct { unsigned field,linux_bit;tcflag_t host; } terminal_flags[]={
    {0,1,IGNBRK},{0,2,BRKINT},{0,4,IGNPAR},{0,8,PARMRK},{0,16,INPCK},{0,32,ISTRIP},{0,64,INLCR},{0,128,IGNCR},
    {0,256,ICRNL},{0,1024,IXON},{0,2048,IXANY},{0,4096,IXOFF},{0,8192,IMAXBEL},{0,16384,IUTF8},{1,1,OPOST},{1,4,ONLCR},
    {1,8,OCRNL},{1,16,ONOCR},{1,32,ONLRET},
    {3,1,ISIG},{3,2,ICANON},{3,8,ECHO},{3,16,ECHOE},{3,32,ECHOK},{3,64,ECHONL},{3,128,NOFLSH},{3,256,TOSTOP},
    {3,512,ECHOCTL},{3,1024,ECHOPRT},{3,2048,ECHOKE},{3,32768,IEXTEN}};
static const unsigned terminal_cc_host[]={VINTR,VQUIT,VERASE,VKILL,VEOF,VTIME,VMIN,VSTART,VSTOP,VSUSP,VEOL,VREPRINT,VDISCARD,VWERASE,VLNEXT,VEOL2};
static const unsigned terminal_cc_linux[]={0,1,2,3,4,5,6,8,9,10,11,12,13,14,15,16};
static tcflag_t *terminal_field(struct termios *t,unsigned field) { return field==0?&t->c_iflag:field==1?&t->c_oflag:&t->c_lflag; }
static uint64_t terminal_ioctl(GuestIO *fd) {
    if((fd->kind!=IO_INPUT && fd->kind!=IO_OUTPUT) || !isatty(fd->host))return linux_error(25);
    uint64_t request=cpu->gpr[1];
    if(request==0x5401) {                                     /* TCGETS */
        unsigned char *p=lookup(cpu->gpr[2],36,2);if(!p)return linux_error(14);
        struct termios host;if(tcgetattr(fd->host,&host))return net_error();
        unsigned fields[4]={0,0,0x30|0x80|0xf,0};                 /* CS8|CREAD|B38400 */
        for(unsigned i=0;i<sizeof(terminal_flags)/sizeof(*terminal_flags);i++)
            if(*terminal_field(&host,terminal_flags[i].field)&terminal_flags[i].host)fields[terminal_flags[i].field]|=terminal_flags[i].linux_bit;
        memset(p,0,36);for(unsigned i=0;i<4;i++)fs_pack(p,i*4,fields[i],4);
        for(unsigned i=0;i<sizeof(terminal_cc_host)/sizeof(*terminal_cc_host);i++)p[17+terminal_cc_linux[i]]=host.c_cc[terminal_cc_host[i]];
        return 0;
    }
    if(request>=0x5402 && request<=0x5404) {                 /* TCSETS, TCSETSW, TCSETSF */
        const unsigned char *p=lookup(cpu->gpr[2],36,4);if(!p)return linux_error(14);
        struct termios host;if(tcgetattr(fd->host,&host))return net_error();
        for(unsigned i=0;i<sizeof(terminal_flags)/sizeof(*terminal_flags);i++) {
            tcflag_t *field=terminal_field(&host,terminal_flags[i].field);
            if(le(p+terminal_flags[i].field*4,4)&terminal_flags[i].linux_bit)*field|=terminal_flags[i].host;else *field&=~terminal_flags[i].host;
        }
        for(unsigned i=0;i<sizeof(terminal_cc_host)/sizeof(*terminal_cc_host);i++)host.c_cc[terminal_cc_host[i]]=p[17+terminal_cc_linux[i]];
        int when=request==0x5402?TCSANOW:request==0x5403?TCSADRAIN:TCSAFLUSH;
        return tcsetattr(fd->host,when,&host)?net_error():0;
    }
    if(request==0x540f || request==0x5410) {
        unsigned char *p=lookup(cpu->gpr[2],4,request==0x540f?2:4);if(!p)return linux_error(14);
        if(request==0x540f)fs_pack(p,0,terminal_foreground,4);
        else {
            uint64_t group=le(p,4);
            if(!group || group>INT32_MAX)return linux_error(22);
            if(!terminal_group_exists(group))return linux_error(3);
            ScheduledTask *current=&schedule_tasks[process_pid-1];
            if(current->pgid!=terminal_foreground && terminal_group_exists(terminal_foreground) && !((current->signal_ignored|current->signal_mask)&(UINT64_C(1)<<21))) {
                if(!job_control_enabled)return linux_error(5);
                for(unsigned i=0;i<=process_count;i++)if(schedule_tasks[i].state && schedule_tasks[i].pgid==current->pgid)
                    schedule_signal(&schedule_tasks[i],22);
                return IO_BLOCK;
            }
            terminal_foreground=(unsigned)group;io_epoch++;
        }
        return 0;
    }
    if(request==0x5413) {
        unsigned char *p=lookup(cpu->gpr[2],8,2);if(!p)return linux_error(14);
        struct winsize size;if(ioctl(fd->host,TIOCGWINSZ,&size))return net_error();
        fs_pack(p,0,size.ws_row,2);fs_pack(p,2,size.ws_col,2);
        fs_pack(p,4,size.ws_xpixel,2);fs_pack(p,6,size.ws_ypixel,2);return 0;
    }
    return linux_error(95);
}
/* Linux output processing for terminal writes (c_oflag OPOST with ONLCR, OCRNL, ONOCR
   and ONLRET), done here: Darwin's repeats a CR when its queue fills between the CR
   and LF of ONLCR. The host terminal keeps the guest's flags (its echo uses them)
   and has raw output only while the runtime writes. A short write that ends
   between a CR and its LF leaves the LF pending, written first next time.
   ponytail: the column (for ONOCR) counts only these writes, not echo. */
static unsigned terminal_column,terminal_lf_pending;
static signed char terminal_is_tty[3];
/* The output for in[0..count) (into out unless NULL), at most limit bytes of it;
   *used: the source bytes it covers. */
static size_t terminal_process(const unsigned char *in,size_t count,unsigned char *out,tcflag_t flags,unsigned *column,size_t limit,size_t *used) {
    size_t n=0,i=0;
    for(;i<count;i++) {
        unsigned char c=in[i],e[2]={c,0};unsigned k=1,col=*column;
        if(c=='\n') { if(flags&ONLRET)col=0;if(flags&ONLCR) { e[0]='\r';e[1]='\n';k=2;col=0; } }
        else if(c=='\r') { if((flags&ONOCR) && !col)k=0;else if(flags&OCRNL) { e[0]='\n';if(flags&ONLRET)col=0; } else col=0; }
        else if(c=='\t')col=(col|7)+1;
        else if(c=='\b') { if(col)col--; }
        else if(c>=32 && c!=127 && (c&0xc0)!=0x80)col++;              /* UTF-8 continuation bytes take no column */
        if(n+k>limit)break;
        if(out)memcpy(out+n,e,k);
        n+=k;*column=col;
    }
    *used=i;return n;
}
static ssize_t terminal_write(int fd,const unsigned char *data,size_t count) {
    struct termios host;
    if(!(terminal_enabled || stdin_host) || fd<0 || fd>2 || terminal_is_tty[fd]<0)return write(fd,data,count);
    if(tcgetattr(fd,&host)) { terminal_is_tty[fd]=-1;return write(fd,data,count); }
    tcflag_t flags=host.c_oflag;size_t used;
    if(!(flags&OPOST) || (!terminal_lf_pending && !memchr(data,'\n',count) && !memchr(data,'\r',count))) {
        ssize_t n=write(fd,data,count);
        if(n>0 && (flags&OPOST))terminal_process(data,(size_t)n,NULL,flags,&terminal_column,SIZE_MAX,&used);
        return n;
    }
    static unsigned char out[2*65536];
    if(count>65536)count=65536;                                          /* a short write */
    struct termios raw=host;raw.c_oflag&=~(tcflag_t)OPOST;
    if(tcsetattr(fd,TCSANOW,&raw))return write(fd,data,count);
    ssize_t result=-1;
    if(!terminal_lf_pending || write(fd,"\n",1)==1) {
        terminal_lf_pending=0;
        unsigned column=terminal_column;
        ssize_t n=write(fd,out,terminal_process(data,count,out,flags,&column,SIZE_MAX,&used));
        if(n>=0) {
            column=terminal_column;size_t done=terminal_process(data,count,NULL,flags,&column,(size_t)n,&used);
            if(done<(size_t)n) { used++;terminal_lf_pending=1;column=0; }   /* its CR went out alone */
            terminal_column=column;result=(ssize_t)used;
        }
    }
    int saved=errno;tcsetattr(fd,TCSANOW,&host);errno=saved;
    return result;
}
