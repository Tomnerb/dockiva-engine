/* inotify from macOS FSEvents. Every change a watch can see is a change to a host
   file: guest writes and edits made on the Mac (bind mounts, a dev server's sources)
   alike, so one FSEvents stream per inotify instance, on the watched directories (or
   a watched file's directory), covers both. Its callback, on a serial dispatch queue,
   turns each path and flag set into Linux inotify_event records in the instance's
   queue and writes a byte to the instance's host pipe, which blocking reads, poll,
   select and epoll wait on like a socket.
   FSEvents is loaded on first use (no link-time dependency) and coalesces: an event
   reports what happened to a path within ~50 ms, not each write, and renames are
   MOVED_FROM / MOVED_TO by whether the path still exists (no cookies). */
#define IO_INOTIFYS 32
#define INOTIFY_WATCHES 1024
#define INOTIFY_QUEUE (256u*1024u)
#define IN_MODIFY 0x2u
#define IN_ATTRIB 0x4u
#define IN_CLOSE_WRITE 0x8u
#define IN_MOVED_FROM 0x40u
#define IN_MOVED_TO 0x80u
#define IN_CREATE 0x100u
#define IN_DELETE 0x200u
#define IN_DELETE_SELF 0x400u
#define IN_MOVE_SELF 0x800u
#define IN_Q_OVERFLOW 0x4000u
#define IN_IGNORED 0x8000u
#define IN_ONLYDIR 0x01000000u
#define IN_DONT_FOLLOW 0x02000000u
#define IN_MASK_ADD 0x20000000u
#define IN_ISDIR 0x40000000u
#define IN_ONESHOT 0x80000000u
#define IN_ALL_EVENTS 0xfffu
struct GuestInotifyWatch { int wd,dir;uint32_t mask;char host[1025]; };
struct GuestInotify {
    unsigned refs,nonblock;int pipe_read,pipe_write;
    pthread_mutex_t lock;                 /* queue and watches, against the FSEvents callback */
    unsigned char *queue;size_t used;int signaled,overflowed;
    struct GuestInotifyWatch *watch;unsigned nwatch;int next_wd;
    void *stream;                         /* FSEventStreamRef */
    uint64_t seen[1024],seen_mtime[1024],seen_ctime[1024]; /* paths reported as existing, and their times (FSEvents flags accumulate) */
};
static struct GuestInotify *io_inotifies[IO_INOTIFYS];
static void inotify_retain(struct GuestInotify *in) { in->refs++; }
#ifdef __APPLE__
#include <dlfcn.h>
/* The CoreServices and CoreFoundation calls used, looked up at run time. */
typedef void (*inotify_callback_t)(const void *,void *,size_t,void *,const uint32_t *,const uint64_t *);
typedef struct { long version;void *info;void *retain,*release,*describe; } inotify_stream_context;
static struct {
    int loaded;
    void *(*create)(const void *,inotify_callback_t,inotify_stream_context *,const void *,uint64_t,double,uint32_t);
    void (*set_queue)(void *,void *);unsigned char (*start)(void *);void (*stop)(void *),(*invalidate)(void *),(*release)(void *);
    const void *(*array)(const void *,const void **,long,const void *);const void *(*string)(const void *,const char *,uint32_t);
    void (*cf_release)(const void *);const void *array_callbacks;
    void *(*queue_create)(const char *,void *);void (*sync)(void *,void *,void (*)(void *));void *queue;
} fse;
static int inotify_load(void) {
    if(fse.loaded)return fse.loaded>0;
    void *cs=dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_LAZY);
    void *cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",RTLD_LAZY);
    fse.loaded=-1;if(!cs || !cf)return 0;
    *(void **)&fse.create=dlsym(cs,"FSEventStreamCreate");*(void **)&fse.set_queue=dlsym(cs,"FSEventStreamSetDispatchQueue");
    *(void **)&fse.start=dlsym(cs,"FSEventStreamStart");*(void **)&fse.stop=dlsym(cs,"FSEventStreamStop");
    *(void **)&fse.invalidate=dlsym(cs,"FSEventStreamInvalidate");*(void **)&fse.release=dlsym(cs,"FSEventStreamRelease");
    *(void **)&fse.array=dlsym(cf,"CFArrayCreate");*(void **)&fse.string=dlsym(cf,"CFStringCreateWithCString");
    *(void **)&fse.cf_release=dlsym(cf,"CFRelease");fse.array_callbacks=dlsym(cf,"kCFTypeArrayCallBacks");
    *(void **)&fse.queue_create=dlsym(RTLD_DEFAULT,"dispatch_queue_create");*(void **)&fse.sync=dlsym(RTLD_DEFAULT,"dispatch_sync_f");
    if(!fse.create || !fse.set_queue || !fse.start || !fse.stop || !fse.invalidate || !fse.release || !fse.array || !fse.string ||
       !fse.cf_release || !fse.array_callbacks || !fse.queue_create || !fse.sync || !(fse.queue=fse.queue_create("dockiva.inotify",NULL)))return 0;
    fse.loaded=1;return 1;
}
/* Called with in->lock held. */
static void inotify_emit(struct GuestInotify *in,int wd,uint32_t mask,const char *name) {
    size_t name_size=name?(strlen(name)+1+15)/16*16:0,size=16+name_size;
    if(in->overflowed)return;
    if(in->used+size+16>INOTIFY_QUEUE) {                                        /* room for IN_Q_OVERFLOW stays */
        unsigned char *e=in->queue+in->used;memset(e,0,16);fs_pack(e,0,(uint64_t)(uint32_t)-1,4);fs_pack(e,4,IN_Q_OVERFLOW,4);
        in->used+=16;in->overflowed=1;return;
    }
    unsigned char *e=in->queue+in->used;memset(e,0,size);
    fs_pack(e,0,(uint64_t)(uint32_t)wd,4);fs_pack(e,4,mask,4);fs_pack(e,12,name_size,4);
    if(name)memcpy(e+16,name,strlen(name));
    in->used+=size;
}
static void inotify_drop(struct GuestInotify *in,unsigned index) {
    inotify_emit(in,in->watch[index].wd,IN_IGNORED,NULL);
    in->watch[index]=in->watch[--in->nwatch];
}
/* FSEvents repeats a path's earlier flags (created, renamed, modified) with later
   events: remember which paths were reported as existing, and their modification
   and change times, so a file is created once and a change is MODIFY (its contents)
   or ATTRIB (its mode, owner or attributes) by what actually changed. */
static uint64_t inotify_hash(const char *path) { uint64_t h=UINT64_C(1469598103934665603);while(*path)h=(h^(unsigned char)*path++)*UINT64_C(1099511628211);return h|1; }
static uint64_t inotify_ns(struct timespec t) { return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
/* Whether path was known; set >0 records it (with st's times), <0 forgets it. */
static int inotify_seen(struct GuestInotify *in,const char *path,int set,const struct stat *st,unsigned *slot_out) {
    uint64_t h=inotify_hash(path);unsigned slot=(unsigned)(h%1024);
    int was=in->seen[slot]==h;if(slot_out)*slot_out=slot;
    if(set>0) { in->seen[slot]=h;in->seen_mtime[slot]=inotify_ns(st->st_mtimespec);in->seen_ctime[slot]=inotify_ns(st->st_ctimespec); }
    else if(set<0 && was)in->seen[slot]=0;
    return was;
}
/* One watch's events for one FSEvents path; returns 1 when the watch is gone. */
static int inotify_match(struct GuestInotify *in,unsigned index,const char *path,uint32_t flags) {
    struct GuestInotifyWatch *w=&in->watch[index];
    size_t length=strlen(w->host);const char *name=NULL;
    int self=!strcmp(path,w->host);
    if(!self) {                                                                 /* a directory's direct child */
        if(!w->dir || strncmp(path,w->host,length) || path[length]!='/' || strchr(path+length+1,'/'))return 0;
        name=path+length+1;
    }
    struct stat st;int exists=lstat(path,&st)==0;
    uint32_t dir=(flags&0x20000u)?IN_ISDIR:0,created=flags&0x100u,removed=flags&0x200u,renamed=flags&0x800u,modified=flags&0x1000u;
    uint32_t attributes=flags&(0x400u|0x2000u|0x4000u|0x8000u);
    uint32_t events[6];unsigned count=0;int gone=0;
    if(self && (removed || renamed) && !exists) {                               /* the watched path itself went away */
        events[count++]=renamed && !removed?IN_MOVE_SELF:IN_DELETE_SELF;gone=1;
    } else if(name) {                                                           /* a directory's child */
        unsigned slot;int seen=inotify_seen(in,path,0,NULL,&slot);
        if(exists && seen) {                                                    /* a known file: compare its times */
            if(inotify_ns(st.st_mtimespec)!=in->seen_mtime[slot] && !dir) { events[count++]=IN_MODIFY;events[count++]=IN_CLOSE_WRITE; }
            else if(inotify_ns(st.st_ctimespec)!=in->seen_ctime[slot])events[count++]=IN_ATTRIB|dir;
        } else if(exists) {
            events[count++]=(renamed && !created?IN_MOVED_TO:IN_CREATE)|dir;
            if(modified && !dir && st.st_size) { events[count++]=IN_MODIFY;events[count++]=IN_CLOSE_WRITE; }
        } else {
            if(created && !seen) {                                              /* created and gone within one batch */
                events[count++]=IN_CREATE|dir;
                if(modified && !dir) { events[count++]=IN_MODIFY;events[count++]=IN_CLOSE_WRITE; }
            }
            if(removed)events[count++]=IN_DELETE|dir;
            else if(renamed)events[count++]=IN_MOVED_FROM|dir;
        }
    } else if(!w->dir) {                                                        /* the watched file itself */
        if(exists && (modified || renamed)) { events[count++]=IN_MODIFY;events[count++]=IN_CLOSE_WRITE; } /* an editor's save replaces it */
        if(exists && attributes && !created && !modified && !renamed)events[count++]=IN_ATTRIB;
    }
    int emitted=0;
    for(unsigned i=0;i<count;i++)if(w->mask&events[i]&IN_ALL_EVENTS) { inotify_emit(in,w->wd,events[i],name);emitted=1; }
    if(gone || (emitted && (w->mask&IN_ONESHOT))) { inotify_drop(in,index);return 1; }
    return 0;
}
static void inotify_events(const void *stream,void *info,size_t count,void *paths,const uint32_t *flags,const uint64_t *ids) {
    (void)stream;(void)ids;
    struct GuestInotify *in=info;char **path=paths;
    pthread_mutex_lock(&in->lock);
    for(size_t i=0;i<count;i++) {
        for(unsigned w=0;w<in->nwatch;)if(!inotify_match(in,w,path[i],flags[i]))w++;
        struct stat st;int exists=lstat(path[i],&st)==0;inotify_seen(in,path[i],exists?1:-1,&st,NULL);
    }
    if(in->used && !in->signaled) { char c=1;in->signaled=write(in->pipe_write,&c,1)==1; }
    pthread_mutex_unlock(&in->lock);
}
static void inotify_nothing(void *unused) { (void)unused; }
/* Replace the instance's stream with one on its current watches' directories. */
static void inotify_restream(struct GuestInotify *in) {
    if(in->stream) { fse.stop(in->stream);fse.invalidate(in->stream);fse.release(in->stream);in->stream=NULL;fse.sync(fse.queue,NULL,inotify_nothing); }
    if(!in->nwatch)return;
    const void **strings=calloc(in->nwatch,sizeof(void *));if(!strings)return;
    long n=0;
    for(unsigned i=0;i<in->nwatch;i++) {
        char directory[1025];strcpy(directory,in->watch[i].host);
        if(!in->watch[i].dir) { char *slash=strrchr(directory,'/');if(slash)*(slash==directory?slash+1:slash)=0; }
        if((strings[n]=fse.string(NULL,directory,0x08000100u)))n++;                 /* kCFStringEncodingUTF8 */
    }
    const void *array=fse.array(NULL,strings,n,fse.array_callbacks);
    for(long i=0;i<n;i++)fse.cf_release(strings[i]);
    free(strings);if(!array)return;
    inotify_stream_context context={0,in,NULL,NULL,NULL};
    /* since now; latency 50 ms; NoDefer | FileEvents */
    in->stream=fse.create(NULL,inotify_events,&context,array,UINT64_MAX,0.05,0x2u|0x10u);
    fse.cf_release(array);
    if(in->stream) { fse.set_queue(in->stream,fse.queue);if(!fse.start(in->stream)) { fse.invalidate(in->stream);fse.release(in->stream);in->stream=NULL; } }
}
static void inotify_release(struct GuestInotify *in) {
    if(--in->refs)return;
    if(in->stream) { fse.stop(in->stream);fse.invalidate(in->stream);fse.release(in->stream);fse.sync(fse.queue,NULL,inotify_nothing); }
    epoll_forget_socket(in->pipe_read);close(in->pipe_read);close(in->pipe_write);
    for(unsigned i=0;i<IO_INOTIFYS;i++)if(io_inotifies[i]==in)io_inotifies[i]=NULL;
    pthread_mutex_destroy(&in->lock);free(in->queue);free(in->watch);free(in);
}
/* inotify_init1 (26), inotify_add_watch (27), inotify_rm_watch (28). */
static uint64_t io_inotify(uint64_t call) {
    if(call==26) {
        uint64_t flags=cpu->gpr[0];if(flags&~(UINT64_C(2048)|524288))return linux_error(22);
        if(!inotify_load())return linux_error(38);
        unsigned slot=0;while(slot<IO_INOTIFYS && io_inotifies[slot])slot++;
        if(slot==IO_INOTIFYS)return linux_error(24);
        struct GuestInotify *in=calloc(1,sizeof(*in));int ends[2]={-1,-1};
        if(!in || !(in->queue=malloc(INOTIFY_QUEUE)) || !(in->watch=calloc(INOTIFY_WATCHES,sizeof(*in->watch))) || pipe(ends)) {
            if(in) { free(in->queue);free(in->watch);free(in); }
            return linux_error(24);
        }
        for(unsigned i=0;i<2;i++) { fcntl(ends[i],F_SETFL,O_NONBLOCK);fcntl(ends[i],F_SETFD,FD_CLOEXEC); }
        int fd=io_free_slot(0);if(fd<0) { close(ends[0]);close(ends[1]);free(in->queue);free(in->watch);free(in);return linux_error(24); }
        pthread_mutex_init(&in->lock,NULL);in->refs=1;in->nonblock=(flags&2048)!=0;in->pipe_read=ends[0];in->pipe_write=ends[1];
        io_inotifies[slot]=in;
        IO(fd)=(GuestIO){IO_INOTIFY,(flags&524288)!=0,ends[0],NULL,NULL,NULL,NULL,in};
        return (uint64_t)fd;
    }
    if(io_slot(cpu->gpr[0])<0 || !IO(cpu->gpr[0]).kind)return linux_error(9);
    if(IO(cpu->gpr[0]).kind!=IO_INOTIFY)return linux_error(22);
    struct GuestInotify *in=IO(cpu->gpr[0]).inotify;
    if(call==28) {
        int wd=(int)cpu->gpr[1];int found=0;
        pthread_mutex_lock(&in->lock);
        for(unsigned i=0;i<in->nwatch && !found;i++)if(in->watch[i].wd==wd) { inotify_drop(in,i);found=1; }
        if(found && !in->signaled) { char c=1;in->signaled=write(in->pipe_write,&c,1)==1; }
        pthread_mutex_unlock(&in->lock);
        if(!found)return linux_error(22);
        inotify_restream(in);return 0;
    }
    uint32_t mask=(uint32_t)cpu->gpr[2];
    if(!(mask&IN_ALL_EVENTS))return linux_error(22);
    char leaf[1025];int follow=!(mask&IN_DONT_FOLLOW);
    int parent=fs_lookup((uint64_t)-100,cpu->gpr[1],leaf,follow,NULL);                   /* relative to the working directory */
    if(parent<0)return fs_error();
    int fd=openat(parent,*leaf?leaf:".",O_EVTONLY|O_CLOEXEC|(follow?0:O_NOFOLLOW));int saved=errno;close(parent);
    if(fd<0) { errno=saved;return fs_error(); }
    struct stat st;char host[1025]={0};
    if(fstat(fd,&st)<0 || fcntl(fd,F_GETPATH,host)<0) { saved=errno;close(fd);errno=saved;return fs_error(); }
    close(fd);
    if((mask&IN_ONLYDIR) && !S_ISDIR(st.st_mode))return linux_error(20);
    pthread_mutex_lock(&in->lock);
    int wd=-1;
    for(unsigned i=0;i<in->nwatch;i++)if(!strcmp(in->watch[i].host,host)) {          /* one watch per file: its mask changes */
        in->watch[i].mask=(mask&IN_MASK_ADD)?in->watch[i].mask|mask:mask;wd=in->watch[i].wd;
    }
    int added=wd<0;
    if(added) {
        if(in->nwatch==INOTIFY_WATCHES) { pthread_mutex_unlock(&in->lock);return linux_error(28); }
        struct GuestInotifyWatch *w=&in->watch[in->nwatch++];
        w->wd=wd=++in->next_wd;w->dir=S_ISDIR(st.st_mode);w->mask=mask;strcpy(w->host,host);
    }
    pthread_mutex_unlock(&in->lock);
    if(added)inotify_restream(in);
    return (uint64_t)wd;
}
/* read: whole events, as many as fit (EINVAL when not even the first does). */
static uint64_t inotify_read(struct GuestInotify *in) {
    uint64_t count=cpu->gpr[2];
    pthread_mutex_lock(&in->lock);
    if(!in->used) {
        char drain[64];while(read(in->pipe_read,drain,sizeof(drain))>0) {}
        in->signaled=0;pthread_mutex_unlock(&in->lock);
        if(in->nonblock || !schedule_active)return linux_error(11);
        return net_suspend_one(in->pipe_read,POLLIN,NET_FOREVER);
    }
    size_t take=0;
    while(take<in->used) {
        size_t size=16+(size_t)le(in->queue+take+12,4);
        if(take+size>count)break;
        take+=size;
    }
    if(!take) { pthread_mutex_unlock(&in->lock);return linux_error(22); }
    unsigned char *out=lookup(cpu->gpr[1],take,2);
    if(!out) { pthread_mutex_unlock(&in->lock);return linux_error(14); }
    memcpy(out,in->queue,take);memmove(in->queue,in->queue+take,in->used-take);in->used-=take;
    if(!in->used) { in->overflowed=0;char drain[64];while(read(in->pipe_read,drain,sizeof(drain))>0) {} in->signaled=0; }
    pthread_mutex_unlock(&in->lock);
    net_wait.active=0;
    return take;
}
#else
static uint64_t io_inotify(uint64_t call) { (void)call;return linux_error(38); }
static uint64_t inotify_read(struct GuestInotify *in) { (void)in;return linux_error(9); }
static void inotify_release(struct GuestInotify *in) { (void)in; }
#endif
