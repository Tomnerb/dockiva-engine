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
static unsigned terminal_foreground=1;
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
    atexit(terminal_restore);io_fds[0]=(GuestIO){IO_INPUT,0,0,NULL,NULL};
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
    if(!terminal_enabled || !schedule_active)return linux_error(95);
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
static uint64_t terminal_ioctl(GuestIO *fd) {
    if((fd->kind!=IO_INPUT && fd->kind!=IO_OUTPUT) || !isatty(fd->host))return linux_error(25);
    uint64_t request=cpu->gpr[1];
    if(request==0x5401 && job_control_enabled) {
        unsigned char *p=lookup(cpu->gpr[2],36,2);if(!p)return linux_error(14);
        struct termios host;if(tcgetattr(fd->host,&host))return net_error();
        memset(p,0,36);
        unsigned input=((host.c_iflag&ICRNL)?256:0)|((host.c_iflag&IXON)?1024:0);
        unsigned output=((host.c_oflag&OPOST)?1:0)|((host.c_oflag&ONLCR)?4:0);
        unsigned local=((host.c_lflag&ISIG)?1:0)|((host.c_lflag&ICANON)?2:0)|((host.c_lflag&ECHO)?8:0)|((host.c_lflag&IEXTEN)?32768:0);
        fs_pack(p,0,input,4);fs_pack(p,4,output,4);fs_pack(p,8,0x30|0x80,4);fs_pack(p,12,local,4);
        const unsigned slots[]={VINTR,VQUIT,VERASE,VKILL,VEOF,VTIME,VMIN,VSTART,VSTOP,VSUSP};
        const unsigned target[]={0,1,2,3,4,5,6,8,9,10};
        for(unsigned i=0;i<sizeof(slots)/sizeof(*slots);i++)p[17+target[i]]=host.c_cc[slots[i]];
        return 0;
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
