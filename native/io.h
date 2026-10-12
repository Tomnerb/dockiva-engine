/* M20: bounded nonblocking pipes and duplicated regular/standard descriptors.
   Guest 0..2 and 96..127 belong here; existing filesystem/network ranges stay put. */
static int terminal_enabled,terminal_forwarded;
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
static void epoll_ref(int slot,int change);
static void epoll_forget_pipe(void *pipe,unsigned kind);
/* waiters: scheduler task slots blocked in epoll_wait on this pipe (woken by its next change). */
static struct GuestPipe { unsigned char *data;size_t used;unsigned readers,writers,read_nonblock,write_nonblock,reads,writes;uint64_t waiters; } io_pipes[PIPE_SLOTS];
static void schedule_wake_mask(uint64_t mask);
static void io_pipe_changed(struct GuestPipe *p) {
    io_epoch++;
    if(p->waiters) { uint64_t mask=p->waiters;p->waiters=0;schedule_wake_mask(mask); }
}
/* A signalfd's open description: the signals it reports, shared by dup and fork. */
static struct GuestSignalfd { unsigned refs,nonblock;uint64_t mask; } io_signalfds[IO_SIGNALFDS];
typedef struct { unsigned kind,cloexec;int host;struct GuestPipe *pipe;struct GuestSignalfd *signals; } GuestIO;
static uint64_t io_signals_pending(void);            /* the calling process's pending signals */
static unsigned io_signal_take(unsigned signal);     /* consume one; returns its sender's pid */
static uint64_t terminal_ioctl(GuestIO *fd);
/* The current process's I/O slots; guest: slot -> guest descriptor. */
typedef struct { GuestIO fds[IO_END];unsigned short guest[IO_END]; } IoTable;
static IoTable io_table_first={{[0]={IO_EOF,0,0,NULL,NULL},[1]={IO_OUTPUT,0,1,NULL,NULL},[2]={IO_OUTPUT,0,2,NULL,NULL}},{0,1,2}};
static _Thread_local IoTable *io_table=&io_table_first;
#define io_fds (io_table->fds)
#define io_guest (io_table->guest)
static int io_managed(uint64_t fd) { return io_slot(fd)>=0; }
static void io_release(GuestIO *f) {
    if(f->kind==IO_FILE)close(f->host);
    if(f->kind==IO_EPOLL)epoll_ref(f->host,-1);
    if(f->kind==IO_SIGNAL && !--f->signals->refs) { epoll_forget_pipe(f->signals,IO_SIGNAL);memset(f->signals,0,sizeof(*f->signals)); }
    if(f->kind==IO_READ || f->kind==IO_WRITE) {
        struct GuestPipe *p=f->pipe;io_pipe_changed(p);
        if(f->kind==IO_READ)p->readers--;else p->writers--;
        if((f->kind==IO_READ && !p->readers) || (f->kind==IO_WRITE && !p->writers))epoll_forget_pipe(p,f->kind);
        if(!p->readers && !p->writers) { free(p->data);memset(p,0,sizeof(*p)); }
    }
    memset(f,0,sizeof(*f));
}
static void io_cleanup(void) { for(unsigned i=0;i<IO_END;i++)io_release(&io_fds[i]); }
static int io_copy(GuestIO *out,const GuestIO *source) {
    *out=*source;
    if(source->kind==IO_EPOLL)epoll_ref(source->host,1);
    if(source->kind==IO_SIGNAL)source->signals->refs++;
    if(source->kind==IO_FILE) { out->host=dup(source->host);if(out->host<0) { memset(out,0,sizeof(*out));return -1; } }
    if(source->kind==IO_READ)source->pipe->readers++;
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
        case IO_FILE:return (int)(in|out);
        case IO_EOF:return (int)in;    /* reads return end of file at once */
        case IO_OUTPUT:return (int)out;
        case IO_READ:return (int)((f->pipe->used?in:0)|(f->pipe->writers?0:16));
        case IO_WRITE:return f->pipe->readers?(int)(f->pipe->used<PIPE_CAP?out:0):8;
        case IO_SIGNAL:return (io_signals_pending()&f->signals->mask)?(int)in:0;
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
    IO(r)=(GuestIO){IO_READ,cloexec,-1,p,NULL};IO(w)=(GuestIO){IO_WRITE,cloexec,-1,p,NULL};
    fs_pack(out,0,(unsigned)r,4);fs_pack(out,4,(unsigned)w,4);return 0;
}
static uint64_t io_call(uint64_t call) {
    if(io_slot(cpu->gpr[0])<0)return linux_error(9);
    GuestIO *f=&IO(cpu->gpr[0]);if(!f->kind)return linux_error(9);
    if(call==57) { io_release(f);fd_unbind(cpu->gpr[0]);return 0; }
    if(call==29)return terminal_enabled?terminal_ioctl(f):linux_error(25);
    if(call==25) {
        if(cpu->gpr[1]==1)return f->cloexec;
        if(cpu->gpr[1]==2) { if(cpu->gpr[2]>1)return linux_error(22);f->cloexec=(unsigned)cpu->gpr[2];return 0; }
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
        else { memset(p,0,128);fs_pack(p,16,(f->kind==IO_READ || f->kind==IO_WRITE)?0010600:0020600,4);fs_pack(p,56,4096,4); }
        return 0;
    }
    if(call==62) {
        if(f->kind!=IO_FILE)return linux_error(29);
        if(cpu->gpr[2]>2)return linux_error(22);
        off_t n=lseek(f->host,(off_t)(int64_t)cpu->gpr[1],(int)cpu->gpr[2]);return n<0?fs_error():(uint64_t)n;
    }
    if(call!=63 && call!=64)return linux_error(95);
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
    if((writing && f->kind!=IO_WRITE && f->kind!=IO_OUTPUT) || (!writing && f->kind!=IO_READ && f->kind!=IO_EOF && f->kind!=IO_INPUT))return linux_error(9);
    uint64_t count=cpu->gpr[2];if(count>PIPE_CAP)return linux_error(22);
    unsigned char *p=count?lookup(cpu->gpr[1],count,writing?4:2):(unsigned char *)"";
    if(!p)return linux_error(14);if(!count)return 0;
    if(f->kind==IO_OUTPUT) { ssize_t n=write(f->host,p,(size_t)count);return n<0?net_error():(uint64_t)n; }
    if(f->kind==IO_EOF)return 0;
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
    IO(slot)=(GuestIO){IO_SIGNAL,(flags&524288)!=0,-1,NULL,&io_signalfds[i]};
    return (uint64_t)slot;
}
