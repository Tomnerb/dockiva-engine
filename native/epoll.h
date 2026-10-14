/* Bounded Linux ARM64 epoll. Each registration keys the guest descriptor AND its
   open description. An epoll instance inherited across fork is shared, as on
   Linux. Under the interpreter's scheduler, epoll_wait never blocks the host: it
   suspends the task on its sockets, and pipe changes wake it.
   EPOLLET reports a watch when readiness newly appears, when the guest has read
   or written it since the last report, or when more bytes are pending. That may
   report more often than Linux (harmless to edge-triggered programs, which read
   until EAGAIN) but never misses an edge Linux would deliver, with one bound: an
   edge-triggered socket left holding unread data is rechecked every 10 ms, so
   epoll_wait can return 0 early in that case. */
#include <sys/ioctl.h>
#define EPOLL_SLOTS 256 /* epoll instances in the container (each PostgreSQL process has one) */
#define EPOLL_WATCHES 1024
#define EPOLL_RDHUP 0x2000u
#define EPOLL_EXCLUSIVE (1u<<28)
#define EPOLL_WAKEUP (1u<<29)
#define EPOLL_ONESHOT (1u<<30)
#define EPOLL_ET (1u<<31)
typedef struct {
    unsigned active,kind,events;uint64_t data,fd;
    int host;struct GuestPipe *pipe;
    unsigned disabled,last_ready,last_reads,last_writes;size_t last_avail; /* edge/oneshot state */
} EpollWatch;
/* high: no watch at or above this index is active (waits scan only below it).
   watch grows by doubling from 16 entries up to EPOLL_WATCHES. */
static struct GuestEpoll { unsigned refs,cursor,high,capacity;EpollWatch *watch; } epolls[EPOLL_SLOTS];
static int epoll_live(void) {
    for(unsigned i=0;i<EPOLL_SLOTS;i++)if(epolls[i].refs)return 1;
    return 0;
}
static void epoll_ref(int slot,int change) {
    struct GuestEpoll *e=&epolls[slot];
    if(change>0)e->refs++;else if(!--e->refs) { free(e->watch);memset(e,0,sizeof(*e)); }
}
static void epoll_forget_socket(int host) {
    for(unsigned i=0;i<EPOLL_SLOTS;i++)for(unsigned j=0;j<epolls[i].high;j++) {
        EpollWatch *w=&epolls[i].watch[j];
        if(w->active && !w->kind && w->host==host)w->active=0;
    }
}
static void epoll_forget_pipe(void *pipe,unsigned kind) {
    for(unsigned i=0;i<EPOLL_SLOTS;i++)for(unsigned j=0;j<epolls[i].high;j++) {
        EpollWatch *w=&epolls[i].watch[j];
        if(w->active && w->kind==kind && w->pipe==pipe)w->active=0;
    }
}
static int epoll_fd_valid(uint64_t fd) {
    return net_host(fd)>=0 || fs_host(fd)>=0 || (io_managed(fd) && IO(fd).kind);
}
/* An unlocked epoll_wait that found nothing leaves its socket wait here; the locked
   retry of that same call reuses it instead of polling every socket again. */
static _Thread_local struct { int valid;uint64_t pc,fd;unsigned count,limit; } epoll_prepared;
static _Thread_local struct pollfd epoll_wait_fds[EPOLL_WATCHES];
/* Block on the sockets in epoll_wait_fds; pipes record the task as a waiter. */
static uint64_t epoll_suspend(struct GuestEpoll *e,unsigned high,unsigned count,unsigned limit) {
    uint64_t result=net_suspend(epoll_wait_fds,count,limit);
    if(result==IO_BLOCK && !syscall_unlocked)
        for(unsigned i=0;i<high;i++) {
            EpollWatch *w=&e->watch[i];
            if(w->active && (w->kind==IO_READ || w->kind==IO_WRITE || w->kind==IO_EVENT))w->pipe->waiters|=UINT64_C(1)<<(process_pid-1);
        }
    return result==IO_BLOCK?IO_BLOCK:0; /* deadline passed: no events */
}
static uint64_t guest_epoll(uint64_t call) {
    if(call==20) {
        unsigned flags=(unsigned)cpu->gpr[0];if(flags&~524288u)return linux_error(22);
        unsigned i=0;while(i<EPOLL_SLOTS && epolls[i].refs)i++;
        if(i==EPOLL_SLOTS)return linux_error(24);
        int fd=io_free_slot(0);if(fd<0)return linux_error(24);
        EpollWatch *watch=calloc(16,sizeof(EpollWatch));if(!watch) { fd_unbind((uint64_t)fd);return linux_error(12); }
        epolls[i]=(struct GuestEpoll){1,0,0,16,watch};IO(fd)=(GuestIO){IO_EPOLL,!!flags,(int)i,NULL,NULL,NULL,NULL,NULL};return (uint64_t)fd;
    }
    uint64_t fd=cpu->gpr[0];
    if(!epoll_fd_valid(fd))return linux_error(9);
    if(!io_managed(fd) || IO(fd).kind!=IO_EPOLL)return linux_error(22);
    struct GuestEpoll *e=&epolls[IO(fd).host];
    if(call==21) {
        unsigned op=(unsigned)cpu->gpr[1];uint64_t target=cpu->gpr[2];
        if(!epoll_fd_valid(target))return linux_error(9);
        if(target==fd || op<1 || op>3)return linux_error(22);
        EpollWatch key={0};key.fd=target;key.host=net_host(target);if(key.host<0)key.host=io_poll_host(target);
        if(key.host<0) {
            if(!io_managed(target))return linux_error(1);
            GuestIO *f=&IO(target);
            if(f->kind!=IO_READ && f->kind!=IO_WRITE && f->kind!=IO_SIGNAL && f->kind!=IO_EVENT)return linux_error(f->kind==IO_EPOLL?95:1);
            key.kind=f->kind;key.pipe=f->kind==IO_SIGNAL?(struct GuestPipe *)(void *)f->signals:f->pipe;
        }
        int found=-1,free_slot=-1;
        for(unsigned i=0;i<e->capacity;i++) {
            EpollWatch *w=&e->watch[i];
            if(!w->active) { if(free_slot<0)free_slot=(int)i;continue; }
            if(w->fd==target && w->kind==key.kind && w->host==key.host && w->pipe==key.pipe)found=(int)i;
        }
        if(op==1 && found>=0)return linux_error(17);
        if(op!=1 && found<0)return linux_error(2);
        if(op==2) { e->watch[found].active=0;return 0; }
        const unsigned char *event=lookup(cpu->gpr[3],16,4);if(!event)return linux_error(14);
        key.events=(unsigned)le(event,4);key.data=le(event+8,8);
        /* PRI, IN, OUT, RDHUP, ET, ONESHOT; ERR/HUP are always delivered.
           EXCLUSIVE and WAKEUP need no action with a single waiter per wakeup. */
        if(key.events&~(31u|EPOLL_RDHUP|EPOLL_EXCLUSIVE|EPOLL_WAKEUP|EPOLL_ONESHOT|EPOLL_ET))return linux_error(22);
        if(op==3 && (key.events&EPOLL_EXCLUSIVE))return linux_error(22);
        if(op==1 && free_slot<0) {                                               /* grow the watch list */
            if(e->capacity>=EPOLL_WATCHES)return linux_error(28);
            unsigned next=e->capacity*2>EPOLL_WATCHES?EPOLL_WATCHES:e->capacity*2;
            EpollWatch *grown=calloc(next,sizeof(EpollWatch));if(!grown)return linux_error(12);
            memcpy(grown,e->watch,e->capacity*sizeof(EpollWatch));free(e->watch);
            e->watch=grown;free_slot=(int)e->capacity;e->capacity=next;
        }
        unsigned slot=(unsigned)(op==1?free_slot:found);key.active=1;e->watch[slot]=key;
        if(slot>=e->high)e->high=slot+1;
        /* A thread blocked in epoll_wait polls what it saw when it blocked; Linux applies
           the change to waits in progress (Erlang adds sockets while its poll thread
           waits). This process's waiting threads look again. */
        if(schedule_active)schedule_wake_mask(UINT64_C(1)<<(process_pid-1));
        return 0;
    }
    int maxevents=(int32_t)cpu->gpr[2],timeout=(int32_t)cpu->gpr[3];
    if(maxevents<=0 || maxevents>INT32_MAX/16)return linux_error(22);
    if(cpu->gpr[4])return linux_error(95); /* Atomic signal-mask changes need guest signal support. */
    unsigned capacity=(unsigned)maxevents;if(capacity>EPOLL_WATCHES)capacity=EPOLL_WATCHES;
    int scheduled=schedule_active;
    static _Thread_local struct pollfd polls[EPOLL_WATCHES];
    static _Thread_local unsigned level[EPOLL_WATCHES],deliver[EPOLL_WATCHES],reads[EPOLL_WATCHES],writes[EPOLL_WATCHES];static _Thread_local size_t avail[EPOLL_WATCHES];
    while(e->high && !e->watch[e->high-1].active)e->high--;
    unsigned high=e->high;
    if(epoll_prepared.valid && !syscall_unlocked && epoll_prepared.pc==cpu->pc && epoll_prepared.fd==fd) {
        /* Sockets were just polled without the lock; recheck what the lock guards
           (pipes, pending signals) and, if nothing is ready, block on that wait. */
        epoll_prepared.valid=0;int ready=0;
        for(unsigned i=0;i<high;i++) {
            EpollWatch *w=&e->watch[i];if(!w->active || !w->kind || w->disabled)continue;
            if(w->kind==IO_SIGNAL)ready|=(io_signals_pending()&((struct GuestSignalfd *)(void *)w->pipe)->mask)!=0;
            else if(w->kind==IO_READ)ready|=w->pipe->used || pipe_hung_up(w->pipe);
            else if(w->kind==IO_EVENT)ready|=w->pipe->used || (w->events&4);
            else ready|=PIPE_CAP-w->pipe->used>=4096 || !w->pipe->readers;
        }
        if(!ready)return epoll_suspend(e,high,epoll_prepared.count,epoll_prepared.limit);
    }
    epoll_prepared.valid=0;
    for(int pass=0;;pass++) {
        for(unsigned i=0;i<high;i++) {
            EpollWatch *w=&e->watch[i];polls[i]=(struct pollfd){-1,0,0};
            if(!w->active || w->kind)continue;
            polls[i].fd=w->host;
            if(w->events&(1|EPOLL_RDHUP))polls[i].events|=POLLIN;
            if(w->events&2)polls[i].events|=POLLPRI;
            if(w->events&4)polls[i].events|=POLLOUT;
        }
        /* Unscheduled callers may block in the host poll on the second pass. */
        int wait_ms=pass && !scheduled?timeout:0;
        if(poll(polls,high,wait_ms)<0)return net_error();
        unsigned any=0,unread_edge=0;
        for(unsigned i=0;i<high;i++) {
            EpollWatch *w=&e->watch[i];level[i]=deliver[i]=0;avail[i]=0;reads[i]=writes[i]=0;
            if(!w->active)continue;
            if(!w->kind) {
                short re=polls[i].revents;
                if(re&POLLNVAL)return linux_error(9);
                level[i]=((re&POLLIN)?1:0)|((re&POLLPRI)?2:0)|((re&POLLOUT)?4:0)|((re&POLLERR)?8:0)|((re&POLLHUP)?16:0);
                /* Byte counts, activity counters and peer-close probing serve only
                   edge-triggered or EPOLLRDHUP watches; level-triggered ones skip them. */
                int edge=(w->events&EPOLL_ET)!=0,rdhup=(w->events&EPOLL_RDHUP)!=0;
                int pending=0;if((edge || rdhup) && (re&POLLIN) && ioctl(w->host,FIONREAD,&pending)==0 && pending>0)avail[i]=(size_t)pending;
                if(rdhup || edge) {
                    if(re&POLLHUP)level[i]|=EPOLL_RDHUP;
                    else if((re&POLLIN) && !avail[i]) { char c;if(recv(w->host,&c,1,MSG_PEEK|MSG_DONTWAIT)==0)level[i]|=EPOLL_RDHUP; } /* peer closed */
                }
                if(edge) {
                    struct NetDescription *d=net_host_description(w->host);
                    if(d) { reads[i]=d->reads;writes[i]=d->writes; }
                }
            } else if(w->kind==IO_SIGNAL) {
                if(io_signals_pending()&((struct GuestSignalfd *)(void *)w->pipe)->mask)level[i]|=1;
            } else if(w->kind==IO_EVENT) {
                level[i]|=(w->pipe->used?1u:0u)|4u;
                avail[i]=w->pipe->used;reads[i]=w->pipe->reads;writes[i]=w->pipe->writes;
            } else if(w->kind==IO_READ) {
                if(w->pipe->used)level[i]|=1;
                if(pipe_hung_up(w->pipe))level[i]|=16;
                avail[i]=w->pipe->used;reads[i]=w->pipe->reads;writes[i]=w->pipe->writes;
            } else {
                if(PIPE_CAP-w->pipe->used>=4096)level[i]|=4;
                if(!w->pipe->readers)level[i]|=8;
                reads[i]=w->pipe->reads;writes[i]=w->pipe->writes;
            }
            unsigned wanted=level[i]&((w->events&(7|EPOLL_RDHUP))|24);
            if(w->disabled)wanted=0;
            else if(w->events&EPOLL_ET) {
                unsigned fresh=wanted&~w->last_ready;
                if((wanted&1) && (reads[i]!=w->last_reads || avail[i]>w->last_avail))fresh|=1;
                if((wanted&4) && writes[i]!=w->last_writes)fresh|=4;
                if((wanted&(8|16|EPOLL_RDHUP)) && (reads[i]!=w->last_reads || writes[i]!=w->last_writes))fresh|=wanted&(8|16|EPOLL_RDHUP);
                if((wanted&1) && !(fresh&1) && !w->kind)unread_edge=1;
                wanted=fresh?wanted:0;
            }
            deliver[i]=wanted;if(wanted)any=1;
            /* Forget readiness that went away, so its return is a new edge. */
            w->last_ready&=level[i];if(avail[i]<w->last_avail)w->last_avail=avail[i];
        }
        if(any || !timeout)break;
        if(!scheduled) { if(pass)break;continue; }
        /* Suspend on the watched sockets (a host fd watched twice is simply polled
           twice), leaving out conditions that are already true but already reported. */
        unsigned count=0;
        for(unsigned i=0;i<high;i++) {
            EpollWatch *w=&e->watch[i];if(polls[i].fd<0 || w->disabled)continue;
            short events=polls[i].events;
            if(w->events&EPOLL_ET)events&=(short)~((level[i]&1?POLLIN:0)|(level[i]&4?POLLOUT:0));
            if(events)epoll_wait_fds[count++]=(struct pollfd){polls[i].fd,events,0};
        }
        unsigned limit=timeout<0?NET_FOREVER:(unsigned)timeout;
        if(unread_edge && limit>10)limit=10;
        if(syscall_unlocked) { epoll_prepared.valid=1;epoll_prepared.pc=cpu->pc;epoll_prepared.fd=fd;epoll_prepared.count=count;epoll_prepared.limit=limit; }
        return epoll_suspend(e,high,count,limit);
    }
    if(scheduled)net_wait.active=0;
    if(trace) {
        fprintf(stderr,"epoll_wait timeout=%d:",timeout);
        for(unsigned i=0;i<high;i++)if(e->watch[i].active)fprintf(stderr," fd%" PRIu64 "(ev=%#x level=%#x deliver=%#x)",e->watch[i].fd,e->watch[i].events,level[i],deliver[i]);
        fputc('\n',stderr);
    }
    if(syscall_unlocked) {        /* claim the whole output first: an aborted call must not consume events */
        unsigned ready=0;for(unsigned i=0;i<high;i++)ready+=e->watch[i].active && deliver[i];
        if(ready>capacity)ready=capacity;
        if(ready && !lookup(cpu->gpr[1],(uint64_t)ready*16,2)) { syscall_abort=1;return IO_BLOCK; }
    }
    unsigned count=0,start=e->cursor;
    for(unsigned n=0;n<high && count<capacity;n++) {
        unsigned i=(start+n)%high;EpollWatch *w=&e->watch[i];
        if(!w->active || !deliver[i])continue;
        uint64_t address=cpu->gpr[1]+(uint64_t)count*16;
        unsigned char *out=address>=cpu->gpr[1]?lookup(address,16,2):NULL;
        if(!out)return count?count:linux_error(14);
        memset(out,0,16);fs_pack(out,0,deliver[i],4);fs_pack(out,8,w->data,8);
        count++;e->cursor=(i+1)%high;
        /* Delivered: record the edge baseline; ONESHOT waits for EPOLL_CTL_MOD. */
        w->last_ready=level[i];w->last_reads=reads[i];w->last_writes=writes[i];w->last_avail=avail[i];
        if(w->events&EPOLL_ONESHOT)w->disabled=1;
    }
    return count;
}
