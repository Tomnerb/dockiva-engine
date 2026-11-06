/* The writable layer's new files in memory (--upper-memory). macOS's file system takes
   35-90 us to create a small file however the call is made (a Linux VM's ext4 about
   5), and npm install makes thousands: a regular file made where the walk found the
   path missing in both layers and the upper layer has its directory (OvLeaf.bare) is
   made in a memfs no guest path names, as a file of the same path below directories
   mirroring the upper ones (mode 0777: the walk to them already passed the host's
   checks). Every call on it is memfs's: ov_walk finds a path missing on disk, asks
   upm_walk_hit, and fails with a redirect (UPM_MARK and the path) that memfs follows
   (mem_resolve), as for a tmpfs reached through a symlink. It looks like the layer's
   host files: st_dev 0, owned by the caller, the host's statfs, no xattrs, inode
   numbers from 2^62 up (an inode changes once it is written back, as on overlayfs'
   copy-up). Listings merge it (ov_list), rmdir sees it, a directory rename moves it.

   Write-back: each file goes to its path in the upper directory (under a hidden name
   first, then renamed into place, so a host walk sees it whole or not at all) once it
   has been closed for UPM_AGE_NS, all closed ones at a sync request (dockivad, before
   docker exec, cp, commit, export, diff, healthchecks and a SIGKILL), and every one at
   the run's end, unless the layer is discarded with the container (--upper-discard:
   docker run --rm). A file made on disk meanwhile by another run (an exec) keeps the
   disk's version. ponytail: only regular files of existing upper directories; a
   crash of the engine loses what was not written back yet (at most UPM_AGE_NS of
   closed files, as a VM's page cache loses its dirty pages). */
#define UPM_AGE_NS UINT64_C(2000000000)
#define UPM_FILES_MAX 65536u
#define UPM_COUNTS 65536u
static uint32_t upm_counts[UPM_COUNTS];               /* pending files by path hash (a superset: a hit is looked up) */
static unsigned upm_files;                            /* under mem_lock */
static uint64_t upm_bytes_max;
static char upm_sync[1025];static int upm_discard,upm_stop;
static pthread_mutex_t upm_drain_lock=PTHREAD_MUTEX_INITIALIZER;   /* one write-back pass at a time */
static uint32_t upm_slot(const char *canon) { return ov_hash(canon)&(UPM_COUNTS-1); }
static void upm_count(const char *canon,int delta) { __atomic_add_fetch(&upm_counts[upm_slot(canon)],(uint32_t)delta,__ATOMIC_SEQ_CST); }
static int upm_maybe(const char *canon) { return upm_fs && __atomic_load_n(&upm_counts[upm_slot(canon)],__ATOMIC_SEQ_CST)!=0; }
static uint64_t upm_ns(const struct timespec *t) { return (uint64_t)t->tv_sec*1000000000+(uint64_t)t->tv_nsec; }

/* The hidden directory for an upper directory's guest path (made when make). mem_lock held. */
static MemNode *upm_dir(const char *canon,int make) {
    MemNode *cur=upm_fs->root;
    for(const char *p=canon;*p;) {
        while(*p=='/')p++;
        if(!*p)break;
        const char *q=p;while(*q && *q!='/')q++;
        size_t m=(size_t)(q-p);if(m>255)return NULL;
        char part[256];memcpy(part,p,m);part[m]=0;p=q;
        MemEntry *e=mem_find(cur,part);MemNode *n=e?e->node:NULL;
        if(n && !mem_is(n,MEM_DIR))return NULL;
        if(!n) {
            if(!make)return NULL;
            uint32_t uid=guest_uid,gid=guest_gid;
            if(!(n=mem_node_new(upm_fs,MEM_DIR|0777)))return NULL;
            n->uid=uid;n->gid=gid;
            if(!mem_link(cur,part,n)) { mem_node_free(n);return NULL; }
        }
        cur=n;
    }
    return cur;
}
static void upm_split(const char *canon,char dir[1025],const char **name) {
    const char *slash=strrchr(canon,'/');size_t d=slash?(size_t)(slash-canon):0;
    memcpy(dir,canon,d);dir[d]=0;if(!d)strcpy(dir,"/");
    *name=slash?slash+1:canon;
}
/* The pending file at canon (mem_lock held), or NULL. */
static MemEntry *upm_entry(const char *canon) {
    char dir[1025];const char *name;upm_split(canon,dir,&name);
    if(!*name)return NULL;
    MemNode *parent=upm_dir(dir,0);if(!parent)return NULL;
    MemEntry *e=mem_find(parent,name);
    return e && e->node && mem_is(e->node,MEM_REG)?e:NULL;
}
/* ov_walk: a leaf missing on disk below an upper directory. 1: a pending file (the
   redirect set, errno EXDEV); 0: missing; -1: a write-back moved one to disk since the
   walk began (the walk looks again). */
static int upm_walk_hit(const char *canon) {
    if(!upm_fs)return 0;
    if(upm_maybe(canon)) {
        int held=upm_lock_held;                                                /* memfs's probe walk holds it already */
        if(!held)pthread_mutex_lock(&mem_lock);
        int hit=upm_entry(canon)!=NULL;
        if(!held)pthread_mutex_unlock(&mem_lock);
        if(hit) { (void)mem_redirect_set(UPM_MARK,canon+1);return 1; }
    }
    return __atomic_load_n(&upm_drains,__ATOMIC_ACQUIRE)!=upm_walk_drains?-1:0;
}
/* A guest path's canonical form without asking the host (mem_path_call): absolute, or
   from a working directory or descriptor whose path is known, with no ".." and no
   reserved name. 0 when only the walk can tell. */
static int upm_lexical(uint64_t dir,const char *input,char out[1025]) {
    size_t n=strlen(input);if(!n || n>1024 || input[n-1]=='/' || fs_scope.root>=0)return 0;
    if(input[0]=='/')strcpy(out,"/");
    else if((int32_t)dir==-100) { if(!fs_cwd || !fs_cwd->path[0] || fs_cwd->mem)return 0;strcpy(out,fs_cwd->path); }
    else { int slot=fs_slot(dir);if(slot<0 || !fs_fd[slot] || !fs_dirs[slot] || !fs_dirs[slot]->path[0])return 0;strcpy(out,fs_dirs[slot]->path); }
    for(const char *p=input;*p;) {
        while(*p=='/')p++;
        if(!*p)break;
        const char *q=p;while(*q && *q!='/')q++;
        size_t m=(size_t)(q-p);
        if(m>255 || (m==2 && p[0]=='.' && p[1]=='.') || (m>=4 && !strncmp(p,OV_WH,4)))return 0;
        if(!(m==1 && p[0]=='.')) { char part[256];memcpy(part,p,m);part[m]=0;if(fs_canon_push(out,part)<0)return 0; }
        p=q;
    }
    return strcmp(out,"/")!=0;
}
/* Whether an upper directory holds pending files (rmdir, listings). */
static int upm_has_files(const char *canon) {
    if(!upm_fs)return 0;
    pthread_mutex_lock(&mem_lock);
    MemNode *d=upm_dir(canon,0);int any=0;
    for(uint32_t i=0;d && !any && i<d->d.count;i++)any=d->d.list[i]->node && mem_is(d->d.list[i]->node,MEM_REG);
    pthread_mutex_unlock(&mem_lock);
    return any;
}
/* ov_list: the pending files of directory canon, after the host's names. */
static int ov_list_add(OvList *list,size_t *capacity,uint64_t ino,unsigned char type,const char *name);
static int upm_list(const char *canon,OvList *list,size_t *capacity) {
    int r=0;
    pthread_mutex_lock(&mem_lock);
    MemNode *d=upm_dir(canon,0);
    for(uint32_t i=0;d && !r && i<d->d.count;i++) {
        MemEntry *e=d->d.list[i];
        if(e->node && mem_is(e->node,MEM_REG))r=ov_list_add(list,capacity,e->node->ino,8,e->name);
    }
    pthread_mutex_unlock(&mem_lock);
    return r;
}
/* A pending file no longer at canon (unlinked or renamed by memfs). mem_lock held. */
static void upm_gone(const char *canon) {
    if(!upm_fs || !canon[0] || upm_entry(canon))return;
    upm_count(canon,-1);if(upm_files)upm_files--;
    if(ov_gen)ov_bump(canon);
}
/* /dev/shm stays on disk: sem_open makes a semaphore under a temporary name and links
   it into place while it is still open, which a pending file cannot be (upm_cross:
   EXDEV, and Python's multiprocessing locks failed); and its files are few. */
static int upm_on_disk(const char *canon) { return !strncmp(canon,"/dev/shm/",9); }
/* fs_open: a new regular file at canon (bare), made in memory. Its descriptor, -errno,
   or UPM_DISK (the host makes it: the budget is used, or a name clash). */
static int64_t upm_create(const char *canon,uint64_t flags,uint64_t mode) {
    char dir[1025];const char *name;upm_split(canon,dir,&name);
    if(!*name || strlen(name)>255 || !strncmp(name,OV_WH,4) || upm_on_disk(canon))return UPM_DISK;
    pthread_mutex_lock(&mem_lock);
    if(upm_files>=UPM_FILES_MAX || upm_fs->used>=upm_bytes_max) { pthread_mutex_unlock(&mem_lock);return UPM_DISK; }
    MemNode *parent=upm_dir(dir,1);
    if(parent && !parent->upper_only) {                                        /* the image lacks this directory: upm_create_fast may make files here */
        struct stat low;int lacks=fstatat(fs_lower,dir[1]?dir+1:".",&low,FS_NOFOLLOW_AT) && (errno==ENOENT || errno==ENOTDIR);
        parent->upper_only=lacks?1:2;
    }
    MemEntry *e=parent?mem_find(parent,name):NULL;
    if(e && e->node && mem_is(e->node,MEM_DIR) && !e->node->d.live && !e->node->refs)mem_unlink_entry(e);   /* a stale mirror of a removed directory */
    else if(e && e->node && !mem_is(e->node,MEM_REG))parent=NULL;
    if(!parent) { pthread_mutex_unlock(&mem_lock);return UPM_DISK; }
    MemPath w;memset(&w,0,sizeof(w));w.dir=parent;strcpy(w.name,name);snprintf(w.canon,sizeof(w.canon),"%s",canon);
    w.entry=mem_find(parent,name);w.node=w.entry?w.entry->node:NULL;
    int made=!w.node;
    if(made)upm_count(canon,1);                                                /* before it exists: walks look it up */
    int64_t r=mem_open(&w,flags,mode);
    if(made) { if(r>=0)upm_files++;else upm_count(canon,-1); }
    pthread_mutex_unlock(&mem_lock);
    return r==MEM_FIFO?-6:r;
}

/* mem_path_call: O_CREAT of a plain path in a mirrored directory the image lacks (so
   no image file can be at the name): two host stats say the directory is still there
   and the name free on disk, then the file is made in memory, with no walk and no
   runtime lock. UPM_DISK: the usual way. */
static int64_t upm_create_fast(const char *canon,uint64_t flags,uint64_t mode) {
    if((flags&(16384|0x200000|0x400000)) || fs_root<0 || !fs_writable || fs_device(canon) || (fs_mount_count && fs_mount_of(canon)) || upm_on_disk(canon))return UPM_DISK;
    char dir[1025];const char *name;upm_split(canon,dir,&name);
    if(!*name || strlen(name)>255 || !strncmp(name,OV_WH,4))return UPM_DISK;
    pthread_mutex_lock(&mem_lock);
    MemNode *parent=upm_dir(dir,0);
    int ok=parent && parent->upper_only==1 && upm_files<UPM_FILES_MAX && upm_fs->used<upm_bytes_max;
    pthread_mutex_unlock(&mem_lock);
    struct stat st;
    if(!ok || fstatat(fs_root,dir[1]?dir+1:".",&st,FS_NOFOLLOW_AT) || !S_ISDIR(st.st_mode))return UPM_DISK;
    if(!fstatat(fs_root,canon+1,&st,FS_NOFOLLOW_AT) || errno!=ENOENT || fs_count_full())return UPM_DISK;   /* there on disk (or a symlink on the way) */
    pthread_mutex_lock(&mem_lock);
    if(!(parent=upm_dir(dir,0)) || parent->upper_only!=1) { pthread_mutex_unlock(&mem_lock);return UPM_DISK; }
    MemPath w;memset(&w,0,sizeof(w));w.dir=parent;strcpy(w.name,name);snprintf(w.canon,sizeof(w.canon),"%s",canon);
    w.entry=mem_find(parent,name);w.node=w.entry?w.entry->node:NULL;
    if(w.node && !mem_is(w.node,MEM_REG)) { pthread_mutex_unlock(&mem_lock);return UPM_DISK; }
    int made=!w.node;
    if(made)upm_count(canon,1);
    int64_t r=mem_open(&w,flags,mode);
    if(made) { if(r>=0)upm_files++;else upm_count(canon,-1); }
    pthread_mutex_unlock(&mem_lock);
    if(r>=0 && made) { fs_count_created();if(ov_gen)ov_bump(canon); }
    return r==MEM_FIFO?-6:r;
}

/* ---- Write-back ---- */
static _Thread_local unsigned char *upm_buffer;
#define UPM_CHUNK (1u<<20)
/* Write pending file n (at canon; mem_lock held, released meanwhile, held again on
   return) to the upper directory and forget it. Unless force (the run's end), only
   if nothing holds it open and it did not change meanwhile. 1: done. */
static int upm_write_back(MemNode *n,const char *canon,int force) {
    static unsigned serial;
    if(!force && n->refs)return 0;
    if(!upm_buffer && (upm_buffer=mmap(NULL,UPM_CHUNK,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0))==MAP_FAILED) { upm_buffer=NULL;return 0; }
    n->refs++;                                                                 /* pinned while mem_lock is released */
    uint64_t size=n->size;uint32_t mode=n->mode;struct timespec mtime=n->mtime,ctime=n->ctime,atime=n->atime;
    char dir[1025],temp[64];const char *name;upm_split(canon,dir,&name);
    snprintf(temp,sizeof temp,OV_WH ".dkv.%d.%u",(int)getpid(),__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED));
    pthread_mutex_unlock(&mem_lock);
    /* All from the directory, opened refusing a symlink anywhere on the way (another
       run, an exec, may have swapped one in): nothing lands outside the root. */
    int parent=dir[1]?openat(fs_root,dir+1,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC):dup(fs_root);
    int fd=parent>=0?openat(parent,temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600):-1,ok=fd>=0;
    for(uint64_t at=0;ok && at<size;) {
        pthread_mutex_lock(&mem_lock);
        uint64_t got=mem_read_at(n,at,upm_buffer,size-at<UPM_CHUNK?size-at:UPM_CHUNK);
        pthread_mutex_unlock(&mem_lock);
        if(!got)break;                                                         /* truncated meanwhile: seen below */
        for(uint64_t done=0;ok && done<got;) { ssize_t k=pwrite(fd,upm_buffer+done,(size_t)(got-done),(off_t)(at+done));if(k<=0)ok=0;else done+=(uint64_t)k; }
        at+=got;
    }
    struct timespec times[2]={atime,mtime};
    if(ok && (fchmod(fd,(mode_t)(mode&07777)) || futimens(fd,times)))ok=0;
    if(fd>=0 && close(fd))ok=0;
    pthread_mutex_lock(&mem_lock);
    MemEntry *now=upm_entry(canon);int done=0;
    int same=now && now->node==n && n->nlink==1 && n->size==size && n->mode==mode && upm_ns(&n->mtime)==upm_ns(&mtime) && upm_ns(&n->ctime)==upm_ns(&ctime) && (force || n->refs==1);
    if(ok && same) {
        int placed=!renameatx_np(parent,temp,parent,name,RENAME_EXCL);
        if(placed || errno==EEXIST) {                                          /* EEXIST: made on disk meanwhile; that one stays */
            if(!placed)(void)unlinkat(parent,temp,0);
            mem_unlink_entry(now);upm_count(canon,-1);if(upm_files)upm_files--;
            __atomic_add_fetch(&upm_drains,1,__ATOMIC_ACQ_REL);if(ov_gen)ov_bump(canon);
            done=1;
        } else (void)unlinkat(parent,temp,0);
    } else if(fd>=0)(void)unlinkat(parent,temp,0);
    if(parent>=0)close(parent);
    mem_node_put(n);                                                           /* the pin (frees it once forgotten) */
    return done;
}
/* memfs's rename or link between a pending file and the host (mem_lock held; released
   here). MEM_PASS-3 to redo the call (the host's now), else its result. */
static int64_t upm_cross(uint64_t call,MemPath *w,const int *res) {
    if(syscall_unlocked) { pthread_mutex_unlock(&mem_lock);return MEM_PASS-1; }  /* walks and write-backs: under the runtime lock */
    uint64_t *x=cpu->gpr;uint64_t flags=call==276?x[4]:0;int64_t r=-18;
    if(res[0]==1) {                                                            /* from a pending file */
        MemNode *n=w[0].node;char from[1025];snprintf(from,sizeof from,"%s",w[0].canon);
        if(!n || !from[0]) { pthread_mutex_unlock(&mem_lock);return -2; }
        if(call!=37 && !(flags&2)) {                                           /* rename to a path missing on disk: moved in memory */
            n->refs++;pthread_mutex_unlock(&mem_lock);
            char leaf[1025],to[1025];int bare=0,parent=fs_lookup(x[2],x[3],leaf,FS_DIRECT,to);
            if(parent>=0) { bare=ov_leaf.layer==2 && ov_leaf.bare && !fs_where && to[0];close(parent); }
            int redirected=mem_redirect_take();
            pthread_mutex_lock(&mem_lock);
            MemEntry *e=upm_entry(from);
            if(bare && !redirected && e && e->node==n) {
                char dir[1025];const char *name;upm_split(to,dir,&name);
                MemNode *into=*name && strlen(name)<256 && strncmp(name,OV_WH,4)?upm_dir(dir,1):NULL;
                MemEntry *clash=into?mem_find(into,name):NULL;
                if(into && !(clash && clash->node)) {
                    upm_count(to,1);
                    if(!(r=mem_move(e,into,name))) {
                        n->ctime=into->mtime=into->ctime=mem_now();upm_count(from,-1);
                        if(ov_gen) { ov_bump(from);ov_bump(to); }
                        mem_node_put(n);pthread_mutex_unlock(&mem_lock);return 0;
                    }
                    upm_count(to,-1);
                }
            }
            mem_node_put(n);
            if(!(e=upm_entry(from)) || e->node!=n) { pthread_mutex_unlock(&mem_lock);return MEM_PASS-3; }   /* changed meanwhile: again */
        }
        r=n->refs?-18:upm_write_back(n,from,0)?MEM_PASS-3:-18;                /* open: EXDEV, as across file systems */
        pthread_mutex_unlock(&mem_lock);return r;
    }
    /* to a pending file, from the host */
    MemNode *t=w[1].node;
    if(!t || call==37 || (flags&1)) { pthread_mutex_unlock(&mem_lock);return t?-17:-2; }
    char at[1025];snprintf(at,sizeof at,"%s",w[1].canon);
    if(flags&2)r=t->refs?-18:upm_write_back(t,at,0)?MEM_PASS-3:-18;          /* RENAME_EXCHANGE: both on disk */
    else if(w[1].entry) { mem_unlink_entry(w[1].entry);upm_gone(at);r=MEM_PASS-3; }   /* replaced: the host's rename puts the file there */
    pthread_mutex_unlock(&mem_lock);return r;
}
/* A directory renamed on disk: its pending files move with it. */
static void upm_rekey(MemNode *d,char path[1025],int delta) {
    size_t length=strlen(path);d->upper_only=0;                               /* a new path: whether the image has it, again */
    for(uint32_t i=0;i<d->d.count;i++) {
        MemEntry *e=d->d.list[i];if(!e->node)continue;
        if(length+1+strlen(e->name)>1024)continue;
        snprintf(path+length,1025-length,"%s%s",length>1?"/":"",e->name);
        if(mem_is(e->node,MEM_REG))upm_count(path,delta);
        else if(mem_is(e->node,MEM_DIR))upm_rekey(e->node,path,delta);
        path[length]=0;
    }
}
static void upm_moved(const char *from,const char *to) {
    if(!upm_fs)return;
    pthread_mutex_lock(&mem_lock);
    char dir[1025],path[1025];const char *name;upm_split(from,dir,&name);
    MemNode *parent=upm_dir(dir,0);MemEntry *e=parent && *name?mem_find(parent,name):NULL;
    if(e && e->node && mem_is(e->node,MEM_DIR)) {
        snprintf(path,sizeof path,"%s",from);upm_rekey(e->node,path,-1);
        char dir2[1025];const char *name2;upm_split(to,dir2,&name2);
        MemNode *into=*name2?upm_dir(dir2,1):NULL;MemEntry *old=into?mem_find(into,name2):NULL;
        if(old && old->node && mem_is(old->node,MEM_DIR) && !old->node->d.live)mem_unlink_entry(old);   /* an empty mirror there */
        MemNode *moved=e->node;int ok=into && !(old && old->node) && !mem_move(e,into,name2);
        snprintf(path,sizeof path,"%s",ok?to:from);upm_rekey(moved,path,1);   /* not moved (no memory): still found where it was */
    }
    pthread_mutex_unlock(&mem_lock);
}
/* One write-back pass: the pending files (all if force, else those closed and older
   than age), a batch at a time. Returns how many were written. */
typedef struct { MemNode *node;char canon[1025]; } UpmItem;
static unsigned upm_collect(MemNode *d,char path[1025],UpmItem *items,unsigned room,unsigned n,int force,uint64_t age,uint64_t now) {
    size_t length=strlen(path);
    for(uint32_t i=0;n<room && i<d->d.count;i++) {
        MemEntry *e=d->d.list[i];if(!e->node || length+1+strlen(e->name)>1024)continue;
        snprintf(path+length,1025-length,"%s%s",length>1?"/":"",e->name);
        if(mem_is(e->node,MEM_DIR))n=upm_collect(e->node,path,items,room,n,force,age,now);
        else if(mem_is(e->node,MEM_REG) && (force || (!e->node->refs && now-(upm_ns(&e->node->mtime)>upm_ns(&e->node->ctime)?upm_ns(&e->node->mtime):upm_ns(&e->node->ctime))>=age))) {
            items[n].node=e->node;e->node->refs++;strcpy(items[n].canon,path);n++;
        }
        path[length]=0;
    }
    return n;
}
static unsigned upm_pass(int force,uint64_t age) {
    if(!upm_fs || fs_root<0)return 0;
    enum { BATCH=256 };
    static UpmItem *items;
    if(!items && (items=mmap(NULL,BATCH*sizeof(UpmItem),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0))==MAP_FAILED) { items=NULL;return 0; }
    unsigned written=0;
    pthread_mutex_lock(&upm_drain_lock);
    for(;;) {
        pthread_mutex_lock(&mem_lock);
        char path[1025]="/";struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
        unsigned n=upm_files?upm_collect(upm_fs->root,path,items,BATCH,0,force,age,upm_ns(&now)):0,done=0;
        for(unsigned i=0;i<n;i++) {
            MemNode *node=items[i].node;node->refs--;                          /* the collection's pin; write_back pins its own */
            if(!node->nlink) { if(!node->refs)mem_node_free(node);continue; }    /* unlinked meanwhile */
            if(upm_write_back(node,items[i].canon,force))done++;
        }
        pthread_mutex_unlock(&mem_lock);
        written+=done;
        if(n<BATCH || !done)break;
    }
    pthread_mutex_unlock(&upm_drain_lock);
    return written;
}
/* The background writer: aged files every quarter second, everything closed on a sync
   request (the file dockivad makes, removed once done), and early when memory fills.
   A request wakes it at once (kqueue: an entry made in the request's directory). It was
   a 20 ms sleep at utility QoS, which macOS stretches: requests waited 4-163 ms, docker
   exec took ~135 ms more than without the memory layer and docker kill up to ~400 ms.
   It waits at user-initiated QoS, as someone waits on a request, and passes over aged
   files at utility. */
static void *upm_writer(void *unused) {
    (void)unused;
    char dir[1025];snprintf(dir,sizeof dir,"%s",upm_sync);
    char *slash=strrchr(dir,'/');if(slash)*slash=0;
    int kq=kqueue(),fd=slash?open(dir,O_EVTONLY|O_CLOEXEC):-1;
    struct kevent change;EV_SET(&change,fd,EVFILT_VNODE,EV_ADD|EV_CLEAR,NOTE_WRITE,0,NULL);
    if(kq>=0 && (fd<0 || kevent(kq,&change,1,NULL,0,NULL)<0)) { close(kq);kq=-1; }   /* then a 20 ms poll */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t aged=upm_ns(&now)+250000000;
    while(!__atomic_load_n(&upm_stop,__ATOMIC_ACQUIRE)) {
        if(!access(upm_sync,F_OK)) { (void)upm_pass(0,0);(void)unlink(upm_sync);continue; }   /* one made before the watch too */
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(upm_ns(&now)>=aged) {
            aged=upm_ns(&now)+250000000;
            if(__atomic_load_n(&upm_files,__ATOMIC_RELAXED)) {
                pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
                (void)upm_pass(0,upm_fs->used>upm_bytes_max/2?0:UPM_AGE_NS);
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED,0);
            }
            continue;
        }
        uint64_t left=aged-upm_ns(&now);
        struct timespec wait={(time_t)(left/1000000000),(long)(left%1000000000)};
        struct kevent event;
        if(kq<0 || kevent(kq,NULL,0,&event,1,&wait)<0)usleep(20000);
    }
    if(kq>=0)close(kq);
    if(fd>=0)close(fd);
    return NULL;
}
/* --upper-memory SYNC: start; the root and its layers are open. */
static void upm_start(void) {
    if(!upm_sync[0] || fs_lower<0 || fs_root<0 || upm_fs)return;
    MemFs *fs=calloc(1,sizeof(*fs));if(!fs)return;
    fs->index=MEM_MOUNTS;fs->path[0]=0;fs->length=0;                          /* named by no guest path: mem_dir_path gives the layer's */
    upm_fs=fs;
    if(!(fs->root=mem_node_new(fs,MEM_DIR|0777))) { upm_fs=NULL;free(fs);return; }
    fs->root->refs=1;fs->root->nlink=2;
    upm_bytes_max=resource_limit/4;if(!upm_bytes_max || upm_bytes_max>(UINT64_C(512)<<20))upm_bytes_max=UINT64_C(512)<<20;
    pthread_t t;if(!pthread_create(&t,NULL,upm_writer,NULL))pthread_detach(t);
}
/* The run's end: every pending file to disk (none when the layer goes with the container). */
static void upm_finish(void) {
    if(!upm_fs)return;
    __atomic_store_n(&upm_stop,1,__ATOMIC_RELEASE);
    if(!upm_discard)(void)upm_pass(1,0);
}
