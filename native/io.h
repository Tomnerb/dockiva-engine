#ifdef __APPLE__
#include <sys/event.h>
#endif
/* M20: bounded nonblocking pipes and duplicated regular/standard descriptors.
   Guest 0..2 and 96..127 belong here; existing filesystem/network ranges stay put. */
static int terminal_enabled,terminal_forwarded;
static int stdin_host; /* --stdin: guest fd 0 reads host fd 0 (a pipe, socket or pty) */
static unsigned terminal_foreground=1; /* the guest process group that owns the terminal */
static char guest_hostname[65]="vmfree"; /* uname's nodename (--hostname) */
static _Thread_local int terminal_waiting;
static int job_control_enabled;
static void terminal_pump(int wait);
static uint64_t terminal_read(unsigned char *data,size_t count);
static _Thread_local unsigned schedule_slice;
static uint64_t io_epoch,io_read_blocks,io_write_blocks;
/* Pipes, epoll, signalfd, standard I/O and their duplicates: io_fds is indexed by
   slot (0..2 are standard I/O), IO(fd) by guest descriptor. */
#define IO_END 512u /* slots */
static int io_slot(uint64_t fd) { return fd_slot(fd,FD_IO); }
#define IO(fd) io_fds[io_slot(fd)] /* a managed descriptor's entry */
#define PIPE_CAP (1024u*1024u)
#define PIPE_SLOTS 64
#define IO_FILE 1
#define IO_OUTPUT 2
#define IO_EOF 3
#define IO_READ 4
#define IO_WRITE 5
#define IO_INPUT 6
#define IO_EPOLL 7
#define IO_SIGNAL 8 /* signalfd */
#define IO_SIGNALFDS 128 /* open signalfd descriptions in the container */
#define IO_EVENT 9  /* eventfd: a pipe slot's used is the counter; readers counts descriptors */
#define IO_TIMER 10 /* timerfd: host is the description's kqueue */
#define IO_TIMERS 128
#define IO_TEXT 11  /* procfs-like text (/proc/mounts...): read-only, seekable */
static void epoll_ref(int slot,int change);
static void epoll_forget_socket(int host);
static void epoll_forget_pipe(void *pipe,unsigned kind);
/* waiters: scheduler task slots blocked in epoll_wait on this pipe (woken by its next change). */
static struct GuestPipe { unsigned char *data;size_t used;unsigned readers,writers,read_nonblock,write_nonblock,reads,writes,semaphore;uint64_t waiters; } io_pipes[PIPE_SLOTS];
static void schedule_wake_mask(uint64_t mask);
static void io_pipe_changed(struct GuestPipe *p) {
    io_epoch++;
    if(p->waiters) { uint64_t mask=p->waiters;p->waiters=0;schedule_wake_mask(mask); }
}
/* A signalfd's open description: the signals it reports, shared by dup and fork. */
static struct GuestSignalfd { unsigned refs,nonblock;uint64_t mask; } io_signalfds[IO_SIGNALFDS];
/* A timerfd's open description: its clock, next expiration (0: disarmed) and period
   in nanoseconds, and a host kqueue holding a one-shot timer for that expiration. */
static struct GuestTimer { unsigned refs,nonblock;clockid_t clock;int kq;uint64_t deadline,interval; } io_timers[IO_TIMERS];
/* A synthetic file's contents, shared by dup and fork, and the shared position. */
struct GuestText { unsigned refs;size_t size,position;unsigned char *data; };
typedef struct { unsigned kind,cloexec;int host;struct GuestPipe *pipe;struct GuestSignalfd *signals;struct GuestTimer *timer;struct GuestText *text; } GuestIO;
static uint64_t io_signals_pending(void);            /* the calling process's pending signals */
static unsigned io_signal_take(unsigned signal);     /* consume one; returns its sender's pid */
static uint64_t terminal_ioctl(GuestIO *fd);
/* The current process's I/O slots; guest: slot -> guest descriptor. */
typedef struct { GuestIO fds[IO_END];unsigned short guest[IO_END]; } IoTable;
static IoTable io_table_first={{[0]={IO_EOF,0,0,NULL,NULL,NULL,NULL},[1]={IO_OUTPUT,0,1,NULL,NULL,NULL,NULL},[2]={IO_OUTPUT,0,2,NULL,NULL,NULL,NULL}},{0,1,2}};
static _Thread_local IoTable *io_table=&io_table_first;
#define io_fds (io_table->fds)
#define io_guest (io_table->guest)
static int io_managed(uint64_t fd) { return io_slot(fd)>=0; }
/* A host descriptor to poll like a socket for this guest one: --stdin's, or a timerfd's kqueue. */
static int io_poll_host(uint64_t guest) {
    if(!io_managed(guest))return -1;
    if(IO(guest).kind==IO_TIMER)return IO(guest).host;
    return stdin_host && !terminal_enabled && IO(guest).kind==IO_INPUT?IO(guest).host:-1;
}
static void io_release(GuestIO *f) {
    if(f->kind==IO_FILE)close(f->host);
    if(f->kind==IO_EPOLL)epoll_ref(f->host,-1);
    if(f->kind==IO_SIGNAL && !--f->signals->refs) { epoll_forget_pipe(f->signals,IO_SIGNAL);memset(f->signals,0,sizeof(*f->signals)); }
    if(f->kind==IO_TIMER && !--f->timer->refs) { epoll_forget_socket(f->timer->kq);close(f->timer->kq);memset(f->timer,0,sizeof(*f->timer)); }
    if(f->kind==IO_TEXT && !--f->text->refs) { free(f->text->data);free(f->text); }
    if(f->kind==IO_READ || f->kind==IO_WRITE || f->kind==IO_EVENT) {
        struct GuestPipe *p=f->pipe;io_pipe_changed(p);
        if(f->kind!=IO_WRITE)p->readers--;else p->writers--;
        if((f->kind!=IO_WRITE && !p->readers) || (f->kind==IO_WRITE && !p->writers))epoll_forget_pipe(p,f->kind);
        if(!p->readers && !p->writers) { free(p->data);memset(p,0,sizeof(*p)); }
    }
    memset(f,0,sizeof(*f));
}
static void io_cleanup(void) { for(unsigned i=0;i<IO_END;i++)io_release(&io_fds[i]); }
static int io_copy(GuestIO *out,const GuestIO *source) {
    *out=*source;
    if(source->kind==IO_EPOLL)epoll_ref(source->host,1);
    if(source->kind==IO_SIGNAL)source->signals->refs++;
    if(source->kind==IO_TIMER)source->timer->refs++;
    if(source->kind==IO_TEXT)source->text->refs++;
    if(source->kind==IO_FILE) { out->host=dup(source->host);if(out->host<0) { memset(out,0,sizeof(*out));return -1; } }
    if(source->kind==IO_READ || source->kind==IO_EVENT)source->pipe->readers++;
    if(source->kind==IO_WRITE)source->pipe->writers++;
    return 0;
}
/* Claim the lowest free descriptor >= min for a free I/O slot; the caller fills IO(fd). */
static int io_free_slot(unsigned min) {
    int guest=fd_lowest(min);if(guest<0)return -1;
    for(unsigned i=3;i<IO_END;i++)if(!io_fds[i].kind) { io_guest[i]=(unsigned short)guest;fd_bind((uint64_t)guest,FD_IO,i);return guest; }
    return -1;
}
static int io_poll_ready(uint64_t guest,unsigned events) {
    unsigned in=(events&1)?1:0,out=(events&4)?4:0;
    if(fs_host(guest)>=0)return (int)(in|out);
    if(!io_managed(guest) || !IO(guest).kind)return -1;
    const GuestIO *f=&IO(guest);
    switch(f->kind) {
        case IO_FILE:case IO_TEXT:return (int)(in|out);
        case IO_EOF:return (int)in;    /* reads return end of file at once */
        case IO_OUTPUT:return (int)out;
        case IO_READ:return (int)((f->pipe->used?in:0)|(f->pipe->writers?0:16));
        case IO_WRITE:return f->pipe->readers?(int)(f->pipe->used<PIPE_CAP?out:0):8;
        case IO_SIGNAL:return (io_signals_pending()&f->signals->mask)?(int)in:0;
        case IO_EVENT:return (int)((f->pipe->used?in:0)|out);
        default:return 0;              /* terminal input and epoll handles: not ready */
    }
}
static unsigned io_nonblocking(const GuestIO *f) { return f->kind==IO_READ?f->pipe->read_nonblock:f->pipe->write_nonblock; }
static uint64_t io_would_block(const GuestIO *f) {
    if(schedule_active && !io_nonblocking(f)) { if(f->kind==IO_READ)io_read_blocks++;else io_write_blocks++;return IO_BLOCK; }
    return linux_error(11);
}
static uint64_t io_pipe(void) {
    if(cpu->gpr[1] & ~(UINT64_C(2048)|524288))return linux_error(22);
    /* Blocking pipe semantics require a scheduler; refuse them explicitly. */
    if(!(cpu->gpr[1]&2048) && !schedule_active)return linux_error(95);
    unsigned char *out=lookup(cpu->gpr[0],8,2);if(!out)return linux_error(14);
    int r=io_free_slot(0);if(r<0)return linux_error(24);
    io_fds[io_slot((uint64_t)r)].kind=IO_EOF;                                   /* hold r's slot while claiming w */
    int w=io_free_slot(0);io_fds[io_slot((uint64_t)r)].kind=0;
    if(w<0) { fd_unbind((uint64_t)r);return linux_error(24); }
    unsigned i=0;while(i<PIPE_SLOTS && io_pipes[i].data)i++;
    if(i==PIPE_SLOTS) { fd_unbind((uint64_t)r);fd_unbind((uint64_t)w);return linux_error(24); }
    struct GuestPipe *p=&io_pipes[i];p->data=malloc(PIPE_CAP);if(!p->data) { fd_unbind((uint64_t)r);fd_unbind((uint64_t)w);return linux_error(12); }
    p->used=0;p->readers=p->writers=1;p->read_nonblock=p->write_nonblock=(cpu->gpr[1]&2048)!=0;
    unsigned cloexec=(cpu->gpr[1]&524288)!=0;
    IO(r)=(GuestIO){IO_READ,cloexec,-1,p,NULL,NULL,NULL};IO(w)=(GuestIO){IO_WRITE,cloexec,-1,p,NULL,NULL,NULL};
    fs_pack(out,0,(unsigned)r,4);fs_pack(out,4,(unsigned)w,4);return 0;
}
/* eventfd2(initval, flags): EFD_SEMAPHORE 1, EFD_NONBLOCK, EFD_CLOEXEC. */
static uint64_t io_eventfd(void) {
    uint64_t flags=cpu->gpr[1];
    if(flags&~(UINT64_C(1)|2048|524288) || cpu->gpr[0]>UINT32_MAX)return linux_error(22);
    if(!(flags&2048) && !schedule_active)return linux_error(95);
    unsigned i=0;while(i<PIPE_SLOTS && io_pipes[i].data)i++;
    if(i==PIPE_SLOTS)return linux_error(24);
    int fd=io_free_slot(0);if(fd<0)return linux_error(24);
    struct GuestPipe *p=&io_pipes[i];if(!(p->data=malloc(1))) { fd_unbind((uint64_t)fd);return linux_error(12); }
    p->used=(size_t)cpu->gpr[0];p->readers=1;p->writers=0;p->semaphore=(flags&1)!=0;
    p->read_nonblock=p->write_nonblock=(flags&2048)!=0;
    IO(fd)=(GuestIO){IO_EVENT,(flags&524288)!=0,-1,p,NULL,NULL,NULL};
    return (uint64_t)fd;
}
/* read/write of an eventfd: 8-byte counter values. */
static uint64_t io_event_transfer(GuestIO *f,int writing) {
    if(cpu->gpr[2]<8)return linux_error(22);
    unsigned char *p=lookup(cpu->gpr[1],8,writing?4:2);if(!p)return linux_error(14);
    struct GuestPipe *e=f->pipe;
    if(writing) {
        uint64_t value=le(p,8);if(value==UINT64_MAX)return linux_error(22);
        if(value>UINT64_MAX-1-e->used)return e->write_nonblock || !schedule_active?linux_error(11):IO_BLOCK;
        e->used+=value;e->writes++;io_pipe_changed(e);return 8;
    }
    if(!e->used)return e->write_nonblock || !schedule_active?linux_error(11):IO_BLOCK;
    uint64_t value=e->semaphore?1:e->used;e->used-=value;fs_pack(p,0,value,8);
    e->reads++;io_pipe_changed(e);return 8;
}
/* A synthetic read-only file (fs_open): its own copy of the text, a new descriptor. */
static uint64_t io_text_open(const void *data,size_t size,int cloexec) {
    struct GuestText *t=calloc(1,sizeof(*t));if(!t || !(t->data=malloc(size?size:1))) { free(t);return linux_error(12); }
    memcpy(t->data,data,size);t->size=size;t->refs=1;
    int fd=io_free_slot(0);if(fd<0) { free(t->data);free(t);return linux_error(24); }
    IO(fd)=(GuestIO){IO_TEXT,cloexec!=0,-1,NULL,NULL,NULL,t};
    return (uint64_t)fd;
}
/* timerfd: expirations are counted from the guest's clock; the kqueue only wakes
   waiters (blocking reads, poll, select and epoll treat it like a socket). */
static uint64_t io_timer_now(clockid_t clock) { struct timespec t;clock_gettime(clock,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
#ifdef __APPLE__
/* Consume a fired wake-up and arm one for the next expiration (none when disarmed). */
static void io_timer_arm(struct GuestTimer *t) {
    struct kevent e;struct timespec zero={0,0};
    while(kevent(t->kq,NULL,0,&e,1,&zero)>0) {}
    if(!t->deadline) { EV_SET(&e,1,EVFILT_TIMER,EV_DELETE,0,0,NULL);(void)kevent(t->kq,&e,1,NULL,0,NULL);return; }
    uint64_t now=io_timer_now(t->clock),delay=t->deadline>now?t->deadline-now:1;
    EV_SET(&e,1,EVFILT_TIMER,EV_ADD|EV_ONESHOT,NOTE_NSECONDS,(intptr_t)(delay>INT64_MAX?INT64_MAX:delay),NULL);(void)kevent(t->kq,&e,1,NULL,0,NULL);
}
#endif
/* Expirations due now (advancing past them when consume is set) and the time to the next. */
static uint64_t io_timer_due(struct GuestTimer *t,int consume,uint64_t *remaining) {
    *remaining=0;if(!t->deadline)return 0;
    uint64_t now=io_timer_now(t->clock),deadline=t->deadline,n=0;
    if(now>=deadline) {
        n=1;
        if(t->interval) { n+=(now-deadline)/t->interval;deadline+=n*t->interval; } else deadline=0;
    }
    if(deadline)*remaining=deadline-now;
    if(consume)t->deadline=deadline;
    return n;
}
static uint64_t io_timer_read(struct GuestTimer *t) {
    if(cpu->gpr[2]<8)return linux_error(22);
    unsigned char *p=lookup(cpu->gpr[1],8,2);if(!p)return linux_error(14);
    uint64_t remaining,n=io_timer_due(t,1,&remaining);
#ifdef __APPLE__
    io_timer_arm(t);
#endif
    if(n || t->nonblock || !schedule_active) {
        net_wait.active=0;                                    /* a previous suspend on the kqueue is over */
        if(!n)return linux_error(11);
        fs_pack(p,0,n,8);return 8;
    }
    return net_suspend_one(t->kq,POLLIN,NET_FOREVER);
}
static void io_timer_pack(unsigned char *out,uint64_t interval,uint64_t value) {
    fs_pack(out,0,interval/1000000000u,8);fs_pack(out,8,interval%1000000000u,8);
    fs_pack(out,16,value/1000000000u,8);fs_pack(out,24,value%1000000000u,8);
}
/* timerfd_create (85), timerfd_settime (86), timerfd_gettime (87). */
static uint64_t io_timerfd(uint64_t call) {
#ifndef __APPLE__
    (void)call;return linux_error(38);
#else
    if(call==85) {
        uint64_t clock=cpu->gpr[0],flags=cpu->gpr[1];
        if((clock!=0 && clock!=1 && clock!=7) || (flags&~(UINT64_C(2048)|524288)))return linux_error(22);
        /* the guest's CLOCK_MONOTONIC is Darwin's CLOCK_UPTIME_RAW, BOOTTIME CLOCK_MONOTONIC (compat.h) */
        unsigned i=0;while(i<IO_TIMERS && io_timers[i].refs)i++;
        if(i==IO_TIMERS)return linux_error(24);
        int kq=kqueue();if(kq<0)return linux_error(24);
        fcntl(kq,F_SETFD,FD_CLOEXEC);
        int slot=io_free_slot(0);if(slot<0) { close(kq);return linux_error(24); }
        io_timers[i]=(struct GuestTimer){1,(flags&2048)!=0,clock==0?CLOCK_REALTIME:clock==1?CLOCK_UPTIME_RAW:CLOCK_MONOTONIC,kq,0,0};
        IO(slot)=(GuestIO){IO_TIMER,(flags&524288)!=0,kq,NULL,NULL,&io_timers[i],NULL};
        return (uint64_t)slot;
    }
    if(io_slot(cpu->gpr[0])<0 || !IO(cpu->gpr[0]).kind)return linux_error(9);
    if(IO(cpu->gpr[0]).kind!=IO_TIMER)return linux_error(22);
    struct GuestTimer *t=IO(cpu->gpr[0]).timer;
    uint64_t old=call==86?cpu->gpr[3]:cpu->gpr[1],remaining;
    unsigned char *out=old?lookup(old,32,2):NULL;if(old && !out)return linux_error(14);
    if(call==86) {
        if(cpu->gpr[1]&~UINT64_C(3))return linux_error(22);                   /* ABSTIME, CANCEL_ON_SET */
        const unsigned char *in=lookup(cpu->gpr[2],32,4);if(!in)return linux_error(14);
        uint64_t times[2];
        for(unsigned k=0;k<2;k++) {
            uint64_t sec=le(in+k*16,8),ns=le(in+k*16+8,8);
            if((int64_t)sec<0 || ns>=1000000000u)return linux_error(22);
            times[k]=sec>UINT64_MAX/4/1000000000u?UINT64_MAX/4:sec*1000000000u+ns;  /* capped near 146 years */
        }
        (void)io_timer_due(t,1,&remaining);
        if(out)io_timer_pack(out,t->interval,remaining);
        t->interval=times[0];
        t->deadline=!times[1]?0:(cpu->gpr[1]&1)?times[1]:io_timer_now(t->clock)+times[1];
        io_timer_arm(t);io_epoch++;return 0;
    }
    if(!out)return linux_error(14);
    (void)io_timer_due(t,0,&remaining);io_timer_pack(out,t->interval,remaining);return 0;
#endif
}
static uint64_t io_call(uint64_t call) {
    if(io_slot(cpu->gpr[0])<0)return linux_error(9);
    GuestIO *f=&IO(cpu->gpr[0]);if(!f->kind)return linux_error(9);
    if(call==57) { io_release(f);fd_unbind(cpu->gpr[0]);return 0; }
    if(call==29 && cpu->gpr[1]==0x541b && (f->kind==IO_EOF || f->kind==IO_READ)) {   /* FIONREAD (Java's available()) */
        unsigned char *out=lookup(cpu->gpr[2],4,2);if(!out)return linux_error(14);
        fs_pack(out,0,f->kind==IO_READ?(uint64_t)f->pipe->used:0,4);return 0;
    }
    if(call==29 && cpu->gpr[1]==0x5421 && f->kind>=IO_READ && f->kind!=IO_INPUT && f->kind!=IO_EPOLL) {   /* FIONBIO: as F_SETFL O_NONBLOCK */
        const unsigned char *p=lookup(cpu->gpr[2],4,4);if(!p)return linux_error(14);
        unsigned on=le(p,4)!=0;
        if(f->kind==IO_SIGNAL)f->signals->nonblock=on;
        else if(f->kind==IO_TIMER)f->timer->nonblock=on;
        else if(f->kind==IO_READ)f->pipe->read_nonblock=on;
        else f->pipe->write_nonblock=on;                                      /* IO_WRITE, IO_EVENT (both ways) */
        io_epoch++;return 0;
    }
    if(call==29)return terminal_enabled || stdin_host?terminal_ioctl(f):linux_error(25);
    if(call==25) {
        if(cpu->gpr[1]==1)return f->cloexec;
        if(cpu->gpr[1]==2) { if(cpu->gpr[2]>1)return linux_error(22);f->cloexec=(unsigned)cpu->gpr[2];return 0; }
        if(cpu->gpr[1]==4 && f->kind==IO_EVENT) { f->pipe->write_nonblock=(cpu->gpr[2]&2048)!=0;io_epoch++;return 0; }
        if(cpu->gpr[1]==3 && f->kind==IO_EVENT)return 2|(f->pipe->write_nonblock?2048:0);
        if(cpu->gpr[1]==4 && (f->kind==IO_READ || f->kind==IO_WRITE)) {
            if(cpu->gpr[2]&(8192|65536))return linux_error(22); /* other flags ignored, as F_SETFL does */
            if(!(cpu->gpr[2]&2048) && !schedule_active)return linux_error(95);
            if(f->kind==IO_READ)f->pipe->read_nonblock=(cpu->gpr[2]&2048)!=0;else f->pipe->write_nonblock=(cpu->gpr[2]&2048)!=0;
            io_epoch++;return 0;
        }
        if(f->kind==IO_SIGNAL && (cpu->gpr[1]==3 || cpu->gpr[1]==4)) {
            if(cpu->gpr[1]==3)return 2|(f->signals->nonblock?2048:0);
            f->signals->nonblock=(cpu->gpr[2]&2048)!=0;return 0;
        }
        if(f->kind==IO_TIMER && (cpu->gpr[1]==3 || cpu->gpr[1]==4)) {
            if(cpu->gpr[1]==3)return 2|(f->timer->nonblock?2048:0);
            f->timer->nonblock=(cpu->gpr[2]&2048)!=0;return 0;
        }
        if(cpu->gpr[1]==3) {
            if(f->kind==IO_EPOLL)return 2;
            if(f->kind==IO_FILE) { int flags=fcntl(f->host,F_GETFL);if(flags<0)return fs_error();return (flags&O_ACCMODE)|((flags&O_APPEND)?1024:0)|131072; }
            return ((f->kind==IO_OUTPUT || f->kind==IO_WRITE)?1:0)|((f->kind==IO_READ || f->kind==IO_WRITE) && io_nonblocking(f)?2048:0);
        }
        return linux_error(95);
    }
    if(f->kind==IO_EPOLL)return linux_error(22);
    if(call==80) {
        unsigned char *p=lookup(cpu->gpr[1],128,2);if(!p)return linux_error(14);
        if(f->kind==IO_FILE) { struct stat st;if(fstat(f->host,&st)<0)return fs_error();fs_stat_pack(p,&st); }
        else if(f->kind==IO_TEXT) { memset(p,0,128);fs_pack(p,16,0100444,4);fs_pack(p,20,1,4);fs_pack(p,56,4096,4); } /* size 0, as procfs */
        else { memset(p,0,128);fs_pack(p,16,(f->kind==IO_READ || f->kind==IO_WRITE)?0010600:0020600,4);fs_pack(p,56,4096,4); }
        return 0;
    }
    if(call==62) {
        if(f->kind==IO_EOF)return 0;                                  /* stdin is /dev/null */
        if(f->kind==IO_TEXT) {
            struct GuestText *t=f->text;int64_t base=cpu->gpr[2]==0?0:cpu->gpr[2]==1?(int64_t)t->position:cpu->gpr[2]==2?(int64_t)t->size:-1;
            int64_t next=base+(int64_t)cpu->gpr[1];
            if(base<0 || next<0)return linux_error(22);
            t->position=(size_t)next;return (uint64_t)next;
        }
        if(f->kind!=IO_FILE)return linux_error(29);
        if(cpu->gpr[2]>2)return linux_error(22);
        off_t n=lseek(f->host,(off_t)(int64_t)cpu->gpr[1],(int)cpu->gpr[2]);return n<0?fs_error():(uint64_t)n;
    }
    if(call==67 && f->kind!=IO_TEXT)return linux_error(29);
    if(call!=63 && call!=64 && call!=67)return linux_error(95);
    if(f->kind==IO_FILE)return fs_transfer(f->host,call);
    if(f->kind==IO_SIGNAL) {
        /* Linux struct signalfd_siginfo (128 bytes) per pending signal in the mask. */
        if(call==64 || cpu->gpr[2]<128)return linux_error(22);
        unsigned capacity=cpu->gpr[2]/128>64?64:(unsigned)(cpu->gpr[2]/128);
        unsigned char *out=lookup(cpu->gpr[1],capacity*128,2);if(!out)return linux_error(14);
        unsigned n=0;uint64_t ready=io_signals_pending()&f->signals->mask;
        for(unsigned s=1;s<=64 && n<capacity;s++)if(ready&(UINT64_C(1)<<(s-1))) {
            unsigned char *r=out+n*128;memset(r,0,128);
            unsigned sender=io_signal_take(s);
            fs_pack(r,0,s,4);fs_pack(r,12,sender,4);fs_pack(r,16,guest_uid,4);       /* signo, pid, uid; SI_USER */
            n++;
        }
        if(n)return n*128;
        if(f->signals->nonblock || !schedule_active)return linux_error(11);
        return IO_BLOCK;                                                           /* woken when a signal arrives */
    }
    int writing=call==64;
    if(f->kind==IO_EVENT)return io_event_transfer(f,writing);
    if(f->kind==IO_TIMER)return writing?linux_error(22):io_timer_read(f->timer);
    if(f->kind==IO_TEXT) {
        if(writing)return linux_error(9);
        struct GuestText *t=f->text;
        size_t at=call==67?(size_t)cpu->gpr[3]:t->position,count=at<t->size?t->size-at:0;
        if((int64_t)cpu->gpr[3]<0 && call==67)return linux_error(22);
        if(count>cpu->gpr[2])count=(size_t)cpu->gpr[2];
        unsigned char *out=count?lookup(cpu->gpr[1],count,2):NULL;if(count && !out)return linux_error(14);
        if(count)memcpy(out,t->data+at,count);
        if(call!=67)t->position=at+count;
        return count;
    }
    if((writing && f->kind!=IO_WRITE && f->kind!=IO_OUTPUT) || (!writing && f->kind!=IO_READ && f->kind!=IO_EOF && f->kind!=IO_INPUT))return linux_error(9);
    uint64_t count=cpu->gpr[2];if(count>PIPE_CAP)return linux_error(22);
    unsigned char *p=count?lookup(cpu->gpr[1],count,writing?4:2):(unsigned char *)"";
    if(!p)return linux_error(14);if(!count)return 0;
    if(f->kind==IO_OUTPUT) { ssize_t n=write(f->host,p,(size_t)count);return n<0?net_error():(uint64_t)n; }
    if(f->kind==IO_EOF)return 0;
    if(f->kind==IO_INPUT && !terminal_enabled) { /* --stdin: host fd 0 is nonblocking; wait like a socket */
        ssize_t n=read(f->host,p,(size_t)count);
        if(n<0 && (errno==EAGAIN || errno==EINTR))return net_suspend_one(f->host,POLLIN,NET_FOREVER);
        net_wait.active=0;return n<0?net_error():(uint64_t)n;
    }
    if(f->kind==IO_INPUT)return terminal_read(p,(size_t)count);
    struct GuestPipe *pipe=f->pipe;
    if(writing)pipe->writes++;else pipe->reads++; /* activity for edge-triggered epoll */
    if(writing) {
        if(!pipe->readers)return linux_error(32); /* Signals are still unsupported. */
        size_t free_bytes=PIPE_CAP-pipe->used;
        if(!free_bytes || (count<=4096 && count>free_bytes))return io_would_block(f);
        if(count>free_bytes)count=free_bytes;
        memcpy(pipe->data+pipe->used,p,(size_t)count);pipe->used+=(size_t)count;io_pipe_changed(pipe);return count;
    }
    if(!pipe->used)return pipe->writers?io_would_block(f):0;
    if(count>pipe->used)count=pipe->used;
    memcpy(p,pipe->data,(size_t)count);pipe->used-=(size_t)count;
    memmove(pipe->data,pipe->data+count,pipe->used);io_pipe_changed(pipe);return count;
}
static void io_exec(void) { for(unsigned i=0;i<IO_END;i++)if(io_fds[i].cloexec) { io_release(&io_fds[i]);fd_unbind(io_guest[i]); } }
static int io_clone(GuestIO saved[IO_END]) {
    GuestIO child[IO_END]={0};
    for(unsigned i=0;i<IO_END;i++)if(io_copy(&child[i],&io_fds[i])<0) {
        for(unsigned j=0;j<i;j++)io_release(&child[j]);return -1;
    }
    memcpy(saved,io_fds,sizeof(io_fds));memcpy(io_fds,child,sizeof(io_fds));return 0;
}
static void io_restore(GuestIO saved[IO_END]) { io_cleanup();memcpy(io_fds,saved,sizeof(io_fds)); }
/* signalfd4(fd, mask, 8, SFD_NONBLOCK|SFD_CLOEXEC): a new descriptor, or a new
   mask for an existing one. SIGKILL and SIGSTOP are ignored in the mask. */
static uint64_t io_signalfd(void) {
    int64_t fd=(int64_t)cpu->gpr[0];uint64_t flags=cpu->gpr[3];
    if(cpu->gpr[2]!=8 || (flags&~(UINT64_C(2048)|524288)))return linux_error(22);
    unsigned char *in=lookup(cpu->gpr[1],8,4);if(!in)return linux_error(14);
    uint64_t mask=le(in,8)&~((UINT64_C(1)<<8)|(UINT64_C(1)<<18));
    if(fd!=-1) {
        if(fd<0 || !io_managed((uint64_t)fd) || IO(fd).kind!=IO_SIGNAL)return linux_error(22);
        IO(fd).signals->mask=mask;io_epoch++;return (uint64_t)fd;
    }
    unsigned i=0;while(i<IO_SIGNALFDS && io_signalfds[i].refs)i++;
    if(i==IO_SIGNALFDS)return linux_error(24);
    int slot=io_free_slot(0);if(slot<0)return linux_error(24);
    io_signalfds[i]=(struct GuestSignalfd){1,(flags&2048)!=0,mask};
    IO(slot)=(GuestIO){IO_SIGNAL,(flags&524288)!=0,-1,NULL,&io_signalfds[i],NULL,NULL};
    return (uint64_t)slot;
}
