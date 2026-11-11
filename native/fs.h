/* Root-relative filesystem adapter. Host identities never become guest fds. Guest
   symlinks are followed by fs_walk, which expands them itself and never lets the
   host kernel follow a link, so resolution cannot leave the guest root. */
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/mount.h>
#include <math.h>
#include <stddef.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#define FS_SLOTS GUEST_FD_LIMIT /* open files and directories per process */
/* Open files and directories of the current process (see the descriptor map in
   net.h); guest: slot -> guest descriptor; top: as the socket table's (net.h), which
   the EL1 handlers' opens raise too (hvf_el1_guest.c). */
/* ro: the file lives where writes are refused (a read-only root or bind mount).
   dev: it is a character device (fs_device), so reads and writes need no fstat:
   FS_DEV_NULL or FS_DEV_ZERO, plus FS_DEV_READ/FS_DEV_WRITE for its access mode
   (those two are served without the host), or 1 for another one. */
#define FS_DEV_NULL 2u
#define FS_DEV_ZERO 3u
#define FS_DEV_READ 4u
#define FS_DEV_WRITE 8u
/* sole: the host descriptor is the slot's alone (an image file opened read, never
   duplicated), so its close can keep it for the next open (fs_spare_put); transfers:
   detached reads and writes in flight, each on a duplicate of a slot's descriptor.
   pack: an image file served from the image's pack (pack.h), its fd a placeholder. */
#include "hvf_el1.h"
/* A slot's fields together (32 bytes): the first few hundred slots of a process share
   one host page, where an array per field put each field's first entries on a page
   of its own (four pages for a process holding a handful of files). */
typedef struct { int fd;unsigned char cloexec,ro,dev,sole;unsigned short guest;struct GuestDirectory *dir;HvfPackOpen *pack; } FsSlot;
typedef struct { unsigned top;int transfers;FsSlot slot[FS_SLOTS]; } FsTable;
static FsTable fs_table_first;
static _Thread_local FsTable *fs_table=&fs_table_first;
#define FS(i) (fs_table->slot[i])
#define fs_top (fs_table->top)
static int fs_claim(void) {
    for(unsigned i=0;i<FS_SLOTS;i++)if(!FS(i).fd) { if(i>=fs_top)fs_top=i+1;return (int)i; }
    return -1;
}
static void fs_table_copy(FsTable *to,const FsTable *from) {
    unsigned top=from->top,old=to->top>top?to->top:top;
    TABLE_COPY(to,from,slot,top,old);to->transfers=from->transfers;to->top=top;
}
/* What a process's own EL1 handlers may change (hvf_el1_guest.c), in one block mapped
   into them: each task's descriptor tables and mm lock word (scheduler.h), each
   process's count of record locks held, and the pool of pack open descriptions. */
typedef struct {
    uint32_t mm[PROCESS_LIMIT+1];
    unsigned short locks_held[PROCESS_LIMIT+2];
    uint32_t pack_hint;
    FsTable fs[PROCESS_LIMIT+3];                      /* by task, then the two outside schedule_tasks (scheduler.h) */
    FdTable fds[PROCESS_LIMIT+3];
    HvfPackOpen pack[HVF_EL1_PACK_OPENS];
    HvfMmCache mm_cache[PROCESS_LIMIT+1];             /* by mm lock index (hvf_el1.h) */
    uint32_t futex_waiters[HVF_EL1_FUTEX_SLOTS+1];    /* hvf_el1.h; the last: futexes in shared memory */
    HvfSpinner spinners[HVF_EL1_SPINNERS];            /* by worker (hvf_el1.h) */
    uint64_t futex_epoch[PROCESS_LIMIT+3];            /* each task's (ScheduledTask.futex_epoch), for EL1 */
    HvfSignals sig[PROCESS_LIMIT+3];                  /* each task's signal actions (task_sig), by task as fs */
} ProcBlock;
static ProcBlock *proc_block;
static size_t proc_block_bytes(void) { return (sizeof(ProcBlock)+16383)&~(size_t)16383; }
/* The region cache (hvf_el1.h) of the current process's memory; NULL when unscheduled. */
static HvfMmCache *mm_cache_of(void);                 /* scheduler.h */
static _Thread_local int mm_cache_busy;              /* unparking: its own munmaps change nothing more */
static uint64_t guest_munmap(uint64_t addr,uint64_t length);
/* A mapping change by the runtime over [addr, addr+bytes): parked regions there are
   unmapped for real, registered ones forgotten. */
static void mm_cache_range(uint64_t addr,uint64_t bytes) {
    HvfMmCache *m=mm_cache_busy?NULL:mm_cache_of();if(!m || !bytes)return;
    for(unsigned i=0;i<HVF_EL1_REGISTERED;i++)
        if(m->pages[i] && m->base[i]<addr+bytes && addr<m->base[i]+(uint64_t)m->pages[i]*4096)m->pages[i]=0;
    mm_cache_busy=1;
    for(unsigned i=0;i<m->parked;) {
        uint64_t base=m->park_base[i],size=(uint64_t)m->park_pages[i]*4096;
        if(!(base<addr+bytes && addr<base+size)) { i++;continue; }
        m->parked_pages-=m->park_pages[i];m->parked--;m->park_base[i]=m->park_base[m->parked];m->park_pages[i]=m->park_pages[m->parked];
        (void)guest_munmap(base,size);
    }
    mm_cache_busy=0;
}
static void mm_cache_register(uint64_t base,uint64_t bytes) {
    HvfMmCache *m=mm_cache_of();uint64_t pages=bytes/4096;
    if(!m || !pages || pages>HVF_EL1_REGION_PAGES)return;
    unsigned bucket=(unsigned)((base>>12)%(HVF_EL1_REGISTERED/4))*4,slot=bucket;  /* a free way, else the bucket's first */
    for(unsigned k=0;k<4;k++)if(!m->pages[bucket+k]) { slot=bucket+k;break; }
    m->pages[slot]=0;m->base[slot]=base;m->pages[slot]=(uint32_t)pages;
}

static void proc_block_init(void) {
    void *p=mmap(NULL,proc_block_bytes(),PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);   /* pages come as they are used */
    if(p==MAP_FAILED) { perror("dockiva: process tables");exit(125); }
    proc_block=p;
}
static int fs_slot(uint64_t guest) { return fd_slot(guest,FD_FILE); }
#define FS_FILE_LIMIT (16u*1024u*1024u)
static int fs_root=-1, fs_writable;
static int fs_lower=-1;                                        /* --lower: the image's files (overlay.h) */
static int fs_space=-1;                                        /* --statfs-from: the file system statfs reports, else the root's */
#include "pack.h"
/* Process-wide state every thread of a process shares (workers running its
   threads all point proc_now at the same record): the working directory, the
   guest identity, the umask and a vfork child's shared image. The identity makes
   every file appear owned by the process, so a switch (root to "redis", say) is
   coherent without per-file ownership; these are guest-visible numbers only, no
   host credential ever changes. uid and gid are the effective IDs (they own files);
   real and saved ones differ after a set-user-ID exec or seteuid. */
typedef struct { uint32_t uid,gid,ruid,rgid,suid,sgid; } GuestIds;
/* prctl's flags and name and the permitted capabilities (compat.h), each process's
   own: fork copies them with the rest of the record. Kept for the whole runtime
   they leaked between processes: once apt's sandbox child set no_new_privs, no
   later sudo in the container gained root. ponytail: Linux keeps all but dumpable
   per thread; here a process's threads share them. */
typedef struct { unsigned keepcaps,no_new_privs,dumpable,pdeathsig;uint32_t caps[2];char name[16]; } GuestPriv;
typedef struct { struct GuestCwd *cwd;GuestIds ids;unsigned umask;struct SharedImage *shared;GuestPriv priv; } ProcShared;
static ProcShared proc_first={.umask=0022,.priv={.dumpable=1,.caps={UINT32_MAX,0x1ffu}}}; /* root starts with capabilities 0..40 */
static _Thread_local ProcShared *proc_now=&proc_first;
#define fs_cwd (proc_now->cwd)
#define guest_ids (proc_now->ids)
#define guest_uid (proc_now->ids.uid)
#define guest_gid (proc_now->ids.gid)
#define fs_umask (proc_now->umask)
#define shared_image (proc_now->shared)
#define prctl_keepcaps (proc_now->priv.keepcaps)
#define prctl_no_new_privs (proc_now->priv.no_new_privs)
#define prctl_dumpable (proc_now->priv.dumpable)
#define prctl_pdeathsig (proc_now->priv.pdeathsig)
#define prctl_name (proc_now->priv.name)
#define cap_held (proc_now->priv.caps)
/* merged: the listing of a directory both image layers hold (overlay.h). */
struct OvList;static void ov_list_free(struct OvList *list);
typedef struct GuestDirectory { DIR *stream;unsigned refs,count;char path[1025];struct OvList *merged;int lower; } GuestDirectory; /* lower: opened there only */
static void fs_directory_release(GuestDirectory *directory) {
    if(directory && !--directory->refs) { if(directory->stream)closedir(directory->stream);ov_list_free(directory->merged);free(directory); }
}
/* ponytail: the byte counters are updated atomically (file writes run without the
   runtime lock) but checked without it, so concurrent writers can overshoot a write or
   storage limit by about one write each; the engine sets neither limit. */
static uint64_t fs_written;
static unsigned fs_created,fs_entries;
/* Atomic: openat (O_CREAT) also runs without the runtime lock. */
static void fs_count_created(void) { __atomic_fetch_add(&fs_created,1,__ATOMIC_RELAXED);__atomic_fetch_add(&fs_entries,1,__ATOMIC_RELAXED); }
#define FS_STORAGE_LIMIT (128u*1024u*1024u)
static uint64_t fs_storage_bytes;
/* Budgets for direct runtime use; the engine raises them (--file-limit-mib) and
   removes the storage and cumulative-write budgets (--storage-limit-mib 0), as
   Docker leaves disk use to the host. */
static uint64_t fs_file_limit=FS_FILE_LIMIT,fs_storage_limit=FS_STORAGE_LIMIT,fs_write_limit=64u*1024u*1024u; /* 0: unlimited */
/* With --storage-limit-mib, a run may also create at most 4096 files (8192 entries in all);
   without one (Docker's behaviour), only the host's disk bounds them. */
/* fsync and friends: by default an I/O barrier (F_BARRIERFSYNC), so writes reach the
   disk in order and a database survives power loss consistent, losing at most its
   last commits; --fsync full drains the drive's cache (F_FULLFSYNC, strictly durable,
   ~5x slower), --fsync fast is macOS's plain fsync (fastest, may reorder). A file
   system without the fcntl gets fsync. */
static int fs_fsync_mode;                                                    /* 0 barrier, 1 full, 2 fast */
static int fs_sync(int fd) {
#ifdef __APPLE__
    if(fs_fsync_mode!=2 && !fcntl(fd,fs_fsync_mode?F_FULLFSYNC:F_BARRIERFSYNC))return 0;
#endif
    return fsync(fd);
}
static int fs_count_full(void) { return fs_storage_limit && (fs_created>=4096 || fs_entries>=8192); }
/* Hardlinked files (BusyBox applets) share one inode; count their bytes once.
   Open addressing; capacity exceeds the 8192-entry scan budget. */
#define FS_INODE_SLOTS 16384u
static uint64_t fs_inode_seen[FS_INODE_SLOTS];
static int fs_inode_first(const struct stat *st) {
    uint64_t key=((uint64_t)st->st_ino^((uint64_t)st->st_dev<<40))|1;
    for(unsigned i=(unsigned)(key*UINT64_C(0x9E3779B97F4A7C15)>>50)%FS_INODE_SLOTS,n=0;n<FS_INODE_SLOTS;n++,i=(i+1)%FS_INODE_SLOTS) {
        if(fs_inode_seen[i]==key)return 0;
        if(!fs_inode_seen[i]) { fs_inode_seen[i]=key;return 1; }
    }
    return 1;
}
static int fs_scan_budget(int directory,unsigned depth) {
    if(depth>32) { errno=ELOOP;return -1; }
    int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(copy<0)return -1;
    DIR *stream=fdopendir(copy);if(!stream) { close(copy);return -1; }
    int result=0;struct dirent *entry;
    while((errno=0,entry=readdir(stream))!=NULL) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(++fs_entries>8192) { errno=ENOSPC;result=-1;break; }
        struct stat st;if(fstatat(directory,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<0) { result=-1;break; }
        if(S_ISREG(st.st_mode) && (st.st_nlink<2 || fs_inode_first(&st))) {
            if(st.st_size<0 || (uint64_t)st.st_size>fs_storage_limit-fs_storage_bytes) { errno=ENOSPC;result=-1;break; }
            __atomic_fetch_add(&fs_storage_bytes,(uint64_t)st.st_size,__ATOMIC_RELAXED);
        } else if(S_ISDIR(st.st_mode)) {
            int fd=openat(directory,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0) { result=-1;break; }
            result=fs_scan_budget(fd,depth+1);int saved=errno;close(fd);errno=saved;if(result)break;
        }
    }
    if(errno)result=-1;
    int saved=errno;closedir(stream);errno=saved;return result;
}
/* mem: a memfs directory (memfs.h; fd is then the root's, for nothing). */
typedef struct GuestCwd { int fd;unsigned refs;char path[1025];struct MemNode *mem; } GuestCwd;
static void mem_node_put_locked(struct MemNode *n);
static void fs_cwd_release(void) {
    if(fs_cwd && !--fs_cwd->refs) { if(fs_cwd->mem)mem_node_put_locked(fs_cwd->mem);close(fs_cwd->fd);free(fs_cwd); }
    fs_cwd=NULL;
}
static void fs_cleanup(void) {
    for(unsigned i=0;i<fs_top;i++) { fs_directory_release(FS(i).dir);if(!fs_pack_drop(i) && FS(i).fd)close(FS(i).fd-1); }
    fs_cwd_release();
    if(fs_root>=0) close(fs_root);
}
static int fs_host(uint64_t n) {
    int slot=fs_slot(n);if(slot<0 || !FS(slot).fd)return -1;
    if(FS(slot).pack)fs_pack_real((unsigned)slot);                    /* a call the pack cannot serve: the real file */
    return FS(slot).fd-1;
}
static uint64_t fs_error(void) {
    switch(errno) {
        case ENOENT:return linux_error(2);case ENOTDIR:return linux_error(20);
        case EISDIR:return linux_error(21);case EEXIST:return linux_error(17);
        case ELOOP:return linux_error(40);case ENAMETOOLONG:return linux_error(36);
        case ENOTEMPTY:return linux_error(39);case EROFS:return linux_error(30);
        case EFBIG:return linux_error(27);case ENOSPC:return linux_error(28);
        case EXDEV:return linux_error(18);                                          /* openat2 RESOLVE_BENEATH, rename across mounts */
        default:return net_error();
    }
}
/* A file call served with no lock at all (fs_call_detached, syscall.h): its path
   strings, its one buffer and the descriptors it resolves against were copied or
   duplicated under the process's mm lock, so sibling threads may close those
   descriptors, unmap the buffer or exec meanwhile. fs_string, fs_lookup and
   fs_memory read these copies instead of guest memory and the descriptor tables;
   an opened file is installed afterwards, under the lock (fs_install). */
static _Thread_local struct {
    int on;
    uint64_t at[2],dir[2];char path[2][1025];int base[2];char base_path[2][1025];   /* path arguments, and their base descriptors */
    uint64_t memory_at,memory_size;unsigned char *memory;                   /* the buffer argument */
    int read_only;                                                          /* the file's slot: fs_ro */
    int fd;uint64_t flags;int opened_ro,opened_device;struct GuestDirectory *directory; /* an opened file to install (fd -1: none) */
    HvfPackOpen *pack;                                                      /* and its pack description */
} fs_detached={.fd=-1};
static _Thread_local HvfPackOpen *fs_install_pack;                    /* the next fs_install's (pack.h) */
/* Guest memory a file call reads or writes: the copy while detached. */
static unsigned char *fs_memory(uint64_t address,uint64_t size,int mode) {
    if(!fs_detached.on)return lookup(address,size,mode);
    if(address>=fs_detached.memory_at && size<=fs_detached.memory_size && address-fs_detached.memory_at<=fs_detached.memory_size-size)return fs_detached.memory+(address-fs_detached.memory_at);
    for(unsigned i=0;i<2;i++)if(address==fs_detached.at[i] && fs_detached.path[i][0] && size<=strlen(fs_detached.path[i])+1)return (unsigned char *)fs_detached.path[i];
    return NULL;
}
/* Substitutes for a call's path arguments by guest address (memfs.h: a path that
   leaves memfs for the host, rewritten). */
static _Thread_local struct { uint64_t at[2];const char *path[2]; } fs_swap;
/* memfs mounts (memfs.h). fs_walk fails with this redirect when it reaches one: the
   mount's guest path and the rest of the path; memfs then serves the call. */
static unsigned mem_mount_count;
static pthread_mutex_t mem_lock=PTHREAD_MUTEX_INITIALIZER;   /* every memfs (memfs.h) */
/* The writable layer's new files in memory (upm.h): a memfs no guest path names,
   reached only through ov_walk's redirect (UPM_MARK) for a path missing on disk. */
static struct MemFs *upm_fs;
static uint64_t upm_drains;static _Thread_local uint64_t upm_walk_drains; /* write-backs done; the walk's start */
static _Thread_local int upm_lock_held;                     /* this thread holds mem_lock (memfs's probe walk) */
#define UPM_MARK "\x01"
#define mem_active (mem_mount_count || upm_fs)
static int upm_walk_hit(const char *canon);                 /* upm.h */
static int upm_maybe(const char *canon);
static int upm_has_files(const char *canon);
struct OvList;static int upm_list(const char *canon,struct OvList *list,size_t *capacity);
static void upm_moved(const char *from,const char *to);
static int64_t upm_create(const char *canon,uint64_t flags,uint64_t mode);
#define UPM_DISK INT64_MIN
#define MEM_HOST_PATH (-1001)                                /* memfs.h: a path the host resolves */
struct GuestInotify;static int64_t mem_inotify_watch(struct GuestInotify *in,uint64_t at,uint32_t mask,int follow);
static int mem_mount_at(const char *canon);static int mem_mount_inside(const char *canon);
static size_t mem_mount_line(unsigned i,char *out,size_t capacity,int mountinfo,unsigned id); /* a /proc/mounts line */
static _Thread_local struct { int pending;char path[5200]; } mem_redirect;
static int mem_redirect_set(const char *at,const char *rest) {
    while(*rest=='/')rest++;
    snprintf(mem_redirect.path,sizeof(mem_redirect.path),"%s%s%s",at,*rest?"/":"",rest);
    mem_redirect.pending=1;errno=EXDEV;return -1;
}
static int mem_redirect_take(void) { int was=mem_redirect.pending;mem_redirect.pending=0;return was; }
/* A guest path, read a page at a time (one lookup each), or a byte at a time where a
   segment ends within the page (the interpreter's ELF segments are not page-rounded). */
static int fs_string(uint64_t ptr,char path[1025]) {
    for(unsigned i=0;i<2;i++)if(fs_swap.path[i] && ptr==fs_swap.at[i]) { strcpy(path,fs_swap.path[i]);return 0; }
    if(fs_detached.on) {
        for(unsigned i=0;i<2;i++)if(ptr==fs_detached.at[i] && fs_detached.path[i][0]) { strcpy(path,fs_detached.path[i]);return 0; }
        errno=EFAULT;return -1;
    }
    for(unsigned i=0;i<=1024;) {
        if(ptr>UINT64_MAX-i) { errno=EFAULT;return -1; }
        uint64_t at=ptr+i,chunk=4096-(at&4095);if(chunk>1025-i)chunk=1025-i;
        unsigned char *p=lookup(at,chunk,4);
        if(!p) { chunk=1;p=lookup(at,1,4); }
        if(!p) { errno=EFAULT;return -1; }
        for(uint64_t k=0;k<chunk;k++,i++) {
            path[i]=(char)p[k];
            if(!p[k]) return i?0:(errno=ENOENT,-1);
            if(p[k]<32 || p[k]==127) { errno=EACCES;return -1; }
        }
    }
    errno=ENAMETOOLONG;return -1;
}
#define FS_LINK_LIMIT 40 /* Linux MAXSYMLINKS */
/* openat2's RESOLVE_IN_ROOT / RESOLVE_BENEATH for one call (fs_openat2): the walk treats
   root (a host directory descriptor, guest path at path) as "/" or refuses to leave it;
   RESOLVE_NO_SYMLINKS refuses every symlink. */
static _Thread_local struct { int root;const char *path;unsigned resolve; } fs_scope={-1,"",0};
#define FS_WALK_ROOT (fs_scope.root>=0?fs_scope.root:fs_root)
static int fs_is_root(int fd) {
    struct stat a,b;return !fstat(fd,&a) && !fstat(FS_WALK_ROOT,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
}
/* Bind mounts (--bind HOST GUEST, --bind-ro): a guest path served by a host
   directory, or a single host file. fs_walk switches to the mount's descriptor
   when it reaches the guest path, and ".." at the top of a mounted directory
   returns to the guest parent, never the host's. Paths beneath a mount are
   resolved like the rest of the root: symlinks never reach the host kernel. */
#define FS_MOUNTS 32
/* plain: 0 not yet known, 1 when no directory on the way to the mount (in the root)
   is a symlink and no other mount lies above it, so a path beneath it resolves from
   the mount's descriptor in one host call (fs_walk_fast); 2 otherwise. */
static struct { char path[1025],name[256];int fd,directory,readonly,plain;dev_t dev;ino_t ino; } fs_mounts[FS_MOUNTS];
static unsigned fs_mount_count;
/* Where the last fs_walk resolved its leaf: 0 the container root, 1+i mount i,
   -1 unknown (a directory whose guest path is not known). */
static _Thread_local int fs_where;
static int fs_mount_index(const char *path) {
    for(unsigned i=0;*path && i<fs_mount_count;i++)if(!strcmp(fs_mounts[i].path,path))return (int)i;
    return -1;
}
static int fs_mount_of(const char *canon) {
    if(!*canon)return -1;
    int best=0;size_t longest=0;
    for(unsigned i=0;i<fs_mount_count;i++) {
        size_t n=strlen(fs_mounts[i].path);
        if(n>longest && !strncmp(canon,fs_mounts[i].path,n) && (canon[n]==0 || canon[n]=='/')) { best=(int)i+1;longest=n; }
    }
    return best;
}
static int fs_mount_plain(int i) {
    if(!fs_mounts[i].plain) {
        int plain=fs_root>=0;char prefix[1025];strcpy(prefix,fs_mounts[i].path);
        for(char *s=strrchr(prefix,'/');plain && s && s>prefix;s=strrchr(prefix,'/')) {
            *s=0;struct stat st;
            if(fs_mount_index(prefix)>=0 || (!fstatat(fs_root,prefix+1,&st,AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode)))plain=0;
            else if(fs_lower>=0 && fstatat(fs_root,prefix+1,&st,AT_SYMLINK_NOFOLLOW) && !fstatat(fs_lower,prefix+1,&st,AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))plain=0;
        }
        fs_mounts[i].plain=plain?1:2;
    }
    return fs_mounts[i].plain==1;
}
/* The mounted directory fd is the top of, or -1. */
static int fs_mount_root(int fd) {
    struct stat st;if(fstat(fd,&st))return -1;
    for(unsigned i=0;i<fs_mount_count;i++)if(fs_mounts[i].directory && fs_mounts[i].dev==st.st_dev && fs_mounts[i].ino==st.st_ino)return (int)i;
    return -1;
}
/* Whether the last fs_walk's leaf may be created, changed or removed. */
static int fs_path_writable(void) {
    if(fs_where>0)return !fs_mounts[fs_where-1].readonly;
    if(fs_where<0)
        for(unsigned i=0;i<fs_mount_count;i++)if(fs_mounts[i].readonly)return 0; /* it might lie in one */
    return fs_writable;
}
/* Add a bind mount before the guest starts; -1 with errno on a bad request. */
static int fs_add_mount(const char *host,const char *guest,int readonly) {
    size_t n=strlen(guest);
    if(fs_mount_count==FS_MOUNTS || guest[0]!='/' || n<2 || n>1024 || guest[n-1]=='/') { errno=EINVAL;return -1; }
    for(const char *p=guest+1;*p;) {                      /* clean absolute path: no empty, "." or ".." parts */
        const char *q=p;while(*q && *q!='/')q++;
        if(q==p || q-p>255 || (q-p==1 && p[0]=='.') || (q-p==2 && p[0]=='.' && p[1]=='.')) { errno=EINVAL;return -1; }
        for(const char *c=p;c<q;c++)if((unsigned char)*c<32 || *c==127) { errno=EINVAL;return -1; }
        p=*q?q+1:q;
    }
    if(fs_mount_index(guest)>=0) { errno=EEXIST;return -1; }
    struct stat st;int fd=-1;
    if(stat(host,&st))return -1;
    if(!S_ISSOCK(st.st_mode)) {                            /* (a socket cannot be opened) */
        if((fd=open(host,O_RDONLY|O_CLOEXEC))<0)return -1;
        if(fstat(fd,&st)) { int saved=errno;close(fd);errno=saved;return -1; }
    }
    char name[256]="";
    if(S_ISREG(st.st_mode) || S_ISSOCK(st.st_mode)) {      /* a file or a socket (an SSH agent's): its directory and name */
        if(fd>=0)close(fd);
        char real[PATH_MAX];if(!realpath(host,real))return -1;
        char *slash=strrchr(real,'/');if(!slash || strlen(slash+1)>255) { errno=EINVAL;return -1; }
        strcpy(name,slash+1);
        if(slash==real)slash[1]=0;else *slash=0;
        if((fd=open(real,O_RDONLY|O_DIRECTORY|O_CLOEXEC))<0)return -1;
    } else if(!S_ISDIR(st.st_mode)) { close(fd);errno=ENOTDIR;return -1; }
    fs_mounts[fs_mount_count].fd=fd;fs_mounts[fs_mount_count].directory=!name[0];fs_mounts[fs_mount_count].readonly=readonly;
    fs_mounts[fs_mount_count].dev=st.st_dev;fs_mounts[fs_mount_count].ino=st.st_ino;
    strcpy(fs_mounts[fs_mount_count].path,guest);strcpy(fs_mounts[fs_mount_count].name,name);
    fs_mount_count++;return 0;
}
/* Canonical guest paths ("" when unknown, e.g. after a stale cwd). */
static void fs_canon_pop(char *c) {
    char *s=strrchr(c,'/');if(!*c || !s)return;
    if(s==c)c[1]=0;else *s=0;
}
static int fs_canon_push(char *c,const char *part) {
    if(!*c)return 0;
    size_t n=strlen(c),m=strlen(part);if(n==1)n=0;
    if(n+m+1>1024) { errno=ENAMETOOLONG;return -1; }
    c[n]='/';memcpy(c+n+1,part,m+1);return 0;
}
/* Resolve input from base one component at a time with directory descriptors.
   Every host open uses O_NOFOLLOW; guest symlinks are read with readlinkat and
   spliced into the remaining path, absolute targets restarting at fs_root. ".."
   opens the real parent, except at fs_root where it stays (as at Linux "/").
   Directory descriptors only come from this walk, so ".." cannot leave the root.
   Returns the parent descriptor and leaf (never a symlink when follow is set),
   and the leaf's canonical guest path in canon when it is known. */
/* follow|FS_DIRECT: the caller passes the leaf only to host calls that refuse a symlink
   anywhere in it (FS_NOFOLLOW_AT, FS_NOFOLLOW_OPEN), so the fast path may return base
   itself (a copy) and a leaf of several components, "dir/dir/name", instead of opening
   the directories: a host open costs about 12 us, a stat about 1. */
/* The leaf's lstat as the last direct walk saw it (1), absent (2) or unknown (0), for
   fs_leaf_stat: callers that stat the leaf right after the walk need no second call. */
static _Thread_local struct { int known;struct stat st; } fs_walk_leaf;
#ifdef O_NOFOLLOW_ANY
#define FS_DIRECT 2
#define FS_NOFOLLOW_AT AT_SYMLINK_NOFOLLOW_ANY
#define FS_NOFOLLOW_OPEN O_NOFOLLOW_ANY
#define FS_ANY_AT AT_SYMLINK_NOFOLLOW_ANY                 /* unlinkat */
#define fs_renameat(a,b,c,d) renameatx_np(a,b,c,d,RENAME_NOFOLLOW_ANY)
/* fs_walk for a path with no ".." component: one host openat of its directories
   with O_NOFOLLOW_ANY, which refuses a symlink anywhere on the way, so the result
   stays beneath base exactly as the component walk would (direct: one fstatat of the
   whole path instead, the caller's call refusing symlinks itself). Returns -2 when
   the path needs that walk (a symlink, "..", an unusual form or error). */
static int fs_walk_fast(int base,const char *cur_start,const char *rest,int follow,int direct,char leaf[1025],char canon[1025]) {
    /* (canon receives the leaf's canonical path; it is always computed) */
    size_t n=strlen(rest);char trimmed[1025];
    if(n && rest[n-1]=='/') {                                   /* "dir/" (tar's mkdir -p): as the component walk, the leaf is "dir" */
        while(n && rest[n-1]=='/')n--;
        memcpy(trimmed,rest,n);trimmed[n]=0;rest=trimmed;
    }
    if(!n)return -2;
    for(const char *p=rest;*p;) {                               /* every component: no "..", at most 255 bytes */
        while(*p=='/')p++;
        const char *q=p;while(*q && *q!='/')q++;
        if(q-p>255 || (q-p==2 && p[0]=='.' && p[1]=='.'))return -2;
        p=q;
    }
    const char *slash=strrchr(rest,'/'),*name=slash?slash+1:rest;
    if(!strcmp(name,"."))return -2;
    char dirs[1025];size_t d=slash?(size_t)(slash-rest):0;
    memcpy(dirs,rest,d);dirs[d]=0;
    strcpy(canon,cur_start);
    for(char *p=dirs;*p;) {
        while(*p=='/')p++;
        char *q=p;while(*q && *q!='/')q++;
        char saved=*q;*q=0;
        int bad=*p && strcmp(p,".") && fs_canon_push(canon,p)<0;
        *q=saved;p=q;
        if(bad)return -1;
    }
    if(fs_canon_push(canon,name)<0)return -1;
    if(mem_mount_count && mem_mount_inside(canon))return -2;   /* into memfs: the component walk redirects */
    /* A path into (or across) a mount resolves from the mount's own descriptor when
       the way to it is plain (bind mounts: an app's files, a database's data); else
       it needs the component walk, even where the root has nothing at the mount's path. */
    char inside[1025];const char *relative=dirs;
    if(fs_mount_count && fs_mount_of(canon)!=fs_mount_of(cur_start)) {
        int to=fs_mount_of(canon);
        if(to<=0 || !fs_mounts[to-1].directory || !fs_mount_plain(to-1))return -2;
        const char *below=canon+strlen(fs_mounts[to-1].path),*end=strrchr(canon,'/');
        if(!*below || end<below)return -2;                     /* the mount itself */
        size_t m=(size_t)(end-below);memcpy(inside,below,m);inside[m]=0;
        base=fs_mounts[to-1].fd;relative=inside;
    }
    while(*relative=='/')relative++;
    if(direct) {
        /* A symlink on the way (ELOOP), here or swapped in before the caller's own call,
           is refused by the host; the component walk then follows it inside the root. */
        char path[1025];struct stat st;size_t r=strlen(relative),l=strlen(name);
        if(r+1+l>1024)return -2;
        memcpy(path,relative,r);if(r)path[r++]='/';memcpy(path+r,name,l+1);
        int known=1;
        if(!fstatat(base,path,&st,FS_NOFOLLOW_AT)) { if(follow && S_ISLNK(st.st_mode))return -2; fs_walk_leaf.st=st; }
        else if(errno==ENOTDIR)return -1;
        else if(errno!=ENOENT)return -2;
        else {
            known=2;
            if(*relative) {                                     /* the leaf is missing: are its directories there? */
                if(fstatat(base,relative,&st,FS_NOFOLLOW_AT))return errno==ENOENT || errno==ENOTDIR?-1:-2;
                if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
            }
        }
        int fd=dup(base);if(fd<0)return -1;
        strcpy(leaf,path);fs_walk_leaf.known=known;return fd;
    }
    int fd=*relative?openat(base,relative,O_RDONLY|O_DIRECTORY|O_NOFOLLOW_ANY|O_CLOEXEC):dup(base);
    if(fd<0)return errno==ENOENT || errno==ENOTDIR?-1:-2;
    struct stat st;
    if(follow && fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode)) { close(fd);return -2; }
    strcpy(leaf,name);return fd;
}
#else
#define FS_DIRECT 0
#define FS_NOFOLLOW_AT AT_SYMLINK_NOFOLLOW
#define FS_NOFOLLOW_OPEN O_NOFOLLOW
#define FS_ANY_AT 0
#define fs_renameat renameat
#endif
static int fs_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]);
static int ov_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]); /* overlay.h */
/* A guest directory by canonical path, as a descriptor (a mount's own, if it is one). */
static int fs_open_dir(const char *path) {
    char leaf[1025];int parent=fs_walk(fs_root,"/",path,1,leaf,NULL);if(parent<0)return -1;
    if(!strcmp(leaf,"."))return parent;
    int fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(parent);errno=saved;
    return fd;
}
/* lstat of the leaf fs_walk just returned with parent (FS_NOFOLLOW_AT), from the walk's
   own when it made one; the result is as fresh as a second call would be. */
static int fs_leaf_stat(int parent,const char *leaf,struct stat *st) {
    int known=fs_walk_leaf.known;fs_walk_leaf.known=0;
    if(known==1) { *st=fs_walk_leaf.st;return 0; }
    if(known==2) { errno=ENOENT;return -1; }
    return fstatat(parent,leaf,st,FS_NOFOLLOW_AT);
}
static int fs_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]) {
    if(fs_root<0) { errno=EACCES;return -1; }
    if(fs_lower>=0)return ov_walk(base,base_path,input,follow,leaf,canon);   /* image layers */
    int direct=follow&FS_DIRECT;follow&=1;(void)direct;
    fs_walk_leaf.known=0;
    char rest[4097],cur[1025];size_t length=strlen(input),pos=0;unsigned links=0;
    if(length>1024) { errno=ENAMETOOLONG;return -1; }
    memcpy(rest,input,length+1);
    if(rest[0]!='/') {
        if(base!=fs_root && !(!fs_detached.on && fs_cwd && base==fs_cwd->fd)) { /* those two are directories (detached: fs_cwd may be freed) */
            struct stat st;if(fstat(base,&st)<0)return -1;
            if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
        }
    } else if(fs_scope.resolve&8) { errno=EXDEV;return -1; }        /* RESOLVE_BENEATH: no absolute path */
    else base=FS_WALK_ROOT;
    strcpy(cur,rest[0]=='/'?(fs_scope.root>=0?fs_scope.path:"/"):base_path);
    if(mem_mount_count && *cur && mem_mount_inside(cur))return mem_redirect_set(cur,rest);
#ifdef O_NOFOLLOW_ANY
    {   /* The fast path stays in base's host directory tree: not across a mount. */
        char full[1025];int fast=fs_walk_fast(base,cur,rest,follow,direct,leaf,full);
        if(fast!=-2) {
            if(fast>=0) { fs_where=fs_mount_of(full);if(canon)strcpy(canon,full); }
            return fast;
        }
    }
#endif
    int fd=dup(base);if(fd<0)return -1;
    for(;;) {
        while(rest[pos]=='/')pos++;
        if(!rest[pos]) { strcpy(leaf,".");if(canon)strcpy(canon,cur);fs_where=fs_mount_of(cur);return fd; }
        size_t start=pos;while(rest[pos] && rest[pos]!='/')pos++;
        char part[256];size_t m=pos-start;
        if(m>255) { close(fd);errno=ENAMETOOLONG;return -1; }
        memcpy(part,rest+start,m);part[m]=0;
        size_t after=pos;while(rest[after]=='/')after++;
        int last=!rest[after];
        if(!strcmp(part,"."))continue;
        if(!strcmp(part,"..")) {
            if(fs_mount_count && fs_mount_root(fd)>=0) {   /* the top of a mount: back to its guest parent */
                if(!*cur)continue;                          /* position unknown: stay, as at "/" */
                fs_canon_pop(cur);
                int parent=fs_open_dir(cur);int saved=errno;close(fd);fd=parent;errno=saved;if(fd<0)return -1;
                continue;
            }
            if(fs_scope.resolve&8 && fs_is_root(fd)) { close(fd);errno=EXDEV;return -1; }
            if(!fs_is_root(fd)) {
                int next=openat(fd,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                int saved=errno;close(fd);fd=next;errno=saved;if(fd<0)return -1;
                fs_canon_pop(cur);
            }
            continue;
        }
        if(mem_mount_count && *cur) {                       /* memfs: redirect */
            char at[1025];strcpy(at,cur);
            if(!fs_canon_push(at,part) && mem_mount_at(at)>=0) { close(fd);return mem_redirect_set(at,rest+pos); }
        }
        if(fs_mount_count && *cur) {                        /* a mount at this path replaces what the root has */
            char at[1025];strcpy(at,cur);
            if(fs_canon_push(at,part)<0) { close(fd);return -1; }
            int m=fs_mount_index(at);
            if(m>=0) {
                if(!fs_mounts[m].directory && !last) { close(fd);errno=ENOTDIR;return -1; }
                close(fd);fd=dup(fs_mounts[m].fd);if(fd<0)return -1;
                strcpy(cur,at);
                if(last) {
                    strcpy(leaf,fs_mounts[m].directory?".":fs_mounts[m].name);
                    if(canon)strcpy(canon,at);
                    fs_where=m+1;return fd;
                }
                continue;
            }
        }
        struct stat st;
        if((!last || follow) && fstatat(fd,part,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode)) {
            if(++links>FS_LINK_LIMIT || (fs_scope.resolve&4)) { close(fd);errno=ELOOP;return -1; }
            char target[1025];ssize_t n=readlinkat(fd,part,target,1024);
            if(n<=0) { int saved=n<0?errno:ENOENT;close(fd);errno=saved;return -1; }
            target[n]=0;
            for(ssize_t i=0;i<n;i++)if((unsigned char)target[i]<32 || target[i]==127) { close(fd);errno=EACCES;return -1; }
            size_t tail=strlen(rest+pos);
            if((size_t)n+tail>4096) { close(fd);errno=ENAMETOOLONG;return -1; }
            memmove(rest+n,rest+pos,tail+1);memcpy(rest,target,(size_t)n);pos=0;
            if(target[0]=='/') {
                if(fs_scope.resolve&8) { close(fd);errno=EXDEV;return -1; }
                close(fd);fd=dup(FS_WALK_ROOT);if(fd<0)return -1;
                strcpy(cur,fs_scope.root>=0?fs_scope.path:"/");
            }
            continue;
        }
        if(last) {
            strcpy(leaf,part);
            if(canon) { strcpy(canon,cur);if(fs_canon_push(canon,part)<0) { close(fd);return -1; } }
            fs_where=fs_mount_of(cur);return fd;
        }
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int saved=errno;close(fd);fd=next;errno=saved;if(fd<0)return -1;
        if(fs_canon_push(cur,part)<0) { close(fd);return -1; }
    }
}
#include "overlay.h"
/* Minimal /proc: /proc/self/exe and /proc/<pid>/exe name each process's executable
   (canonical guest paths, indexed by pid; PROCESS_LIMIT+1 processes). Path lookups
   resolve them to that file; readlink returns the path. */
static char proc_exe[PROCESS_LIMIT+2][256];
/* Each process's argv, NUL-separated, for /proc/<pid>/cmdline, comm and stat (ps, pgrep). */
static char proc_cmdline[PROCESS_LIMIT+2][1024];static unsigned proc_cmdline_size[PROCESS_LIMIT+2];
static void proc_set_cmdline(uint64_t pid,int argc,const char **argv) {
    if(!pid || pid>PROCESS_LIMIT+1)return;
    unsigned size=0;
    for(int i=0;i<argc;i++) {
        size_t length=strlen(argv[i])+1;
        if(size+length>sizeof(proc_cmdline[0]))break;
        memcpy(proc_cmdline[pid]+size,argv[i],length);size+=(unsigned)length;
    }
    proc_cmdline_size[pid]=size;
}
/* The process name ps shows: argv[0]'s last component, at most 15 bytes, as Linux's comm. */
static const char *proc_comm(uint64_t pid) {
    static _Thread_local char name[16];
    const char *argv0=pid && pid<=PROCESS_LIMIT+1 && proc_cmdline_size[pid]?proc_cmdline[pid]:"guest";
    const char *slash=strrchr(argv0,'/');snprintf(name,sizeof(name),"%s",slash && slash[1]?slash+1:argv0);
    return name;
}
static _Thread_local uint64_t process_pid; /* defined with the process layer */
static uint64_t process_pid_now(void) { return process_pid; }
static uint64_t proc_split(const char *path,uint64_t *tid,const char **rest);static int64_t proc_resolve(uint64_t display);
static int fs_proc_exe(const char *path,char out[1025]) {
    uint64_t tid;const char *rest;uint64_t pid=proc_split(path,&tid,&rest);
    int64_t own=pid && !strcmp(rest,"exe")?proc_resolve(pid):0;  /* another run's processes: not at hand */
    if(own<=0 || own>PROCESS_LIMIT+1 || !proc_exe[own][0])return 0;
    strcpy(out,proc_exe[own]);return 1;
}
static int fs_lookup(uint64_t dir,uint64_t ptr,char leaf[1025],int follow,char canon[1025]) {
    char path[1025];if(fs_root<0) { errno=EACCES;return -1; }
    if(fs_string(ptr,path)<0) return -1;
    if(path[0]=='/' && !strncmp(path,"/proc/",6)) { char exe[1025];if(fs_proc_exe(path,exe))strcpy(path,exe); }
    int base=fs_root;const char *base_path="/";
    if(path[0]!='/') {
        if(fs_detached.on) { int i=ptr==fs_detached.at[1] && dir==fs_detached.dir[1];base=fs_detached.base[i];base_path=fs_detached.base_path[i]; }
        else if((int32_t)dir==-100) { base=fs_cwd?fs_cwd->fd:fs_root;base_path=fs_cwd?fs_cwd->path:"/"; }
        else {
            base=fs_host(dir);
            base_path=fs_slot(dir)>=0 && FS(fs_slot(dir)).dir?FS(fs_slot(dir)).dir->path:"";
        }
    }
    if(base<0) { errno=EBADF;return -1; }
    return fs_walk(base,base_path,path,follow,leaf,canon);
}
/* Parent directory and leaf; a final symlink is returned as the leaf itself (readlink,
   symlink, link, mknod). fs_lookup(..., 1, ...) follows it. */
static int fs_parent_nofollow(uint64_t dir,uint64_t ptr,char leaf[1025]) { return fs_lookup(dir,ptr,leaf,0,NULL); }
/* Minimal /dev: these guest paths open the matching host device, whose kernel
   semantics are already Linux's. Guests cannot otherwise obtain a character
   device (fs_open admits only regular files and directories), so a character
   device behind a guest fd is always one of these. The engine provides /dev. */
static const char *fs_device(const char *canon) {
    if(!strcmp(canon,"/dev/null"))return "/dev/null";
    if(!strcmp(canon,"/dev/zero"))return "/dev/zero";
    if(!strcmp(canon,"/dev/random") || !strcmp(canon,"/dev/urandom"))return "/dev/urandom";
    return NULL;
}
/* Whether host descriptor fd is a device: known per slot for the call's own fd (1-byte
   reads and writes, dd bs=1, paid an fstat each), else asked of the host. */
static int fs_is_device(int fd) {
    if(fs_detached.on)return 0;                                            /* only regular files detach */
    int slot=fs_slot(cpu->gpr[0]);if(slot>=0 && FS(slot).fd==fd+1)return FS(slot).dev;
    struct stat st;return !fstat(fd,&st) && S_ISCHR(st.st_mode);
}
static uint64_t fs_fchdir(void) {
    int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
    struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISDIR(st.st_mode))return linux_error(20);
    GuestCwd *next=calloc(1,sizeof(*next));if(!next)return linux_error(12);
    next->fd=dup(fd);if(next->fd<0) { free(next);return fs_error(); }next->refs=1;
    if(FS(fs_slot(cpu->gpr[0])).dir)strcpy(next->path,FS(fs_slot(cpu->gpr[0])).dir->path);
    fs_cwd_release();fs_cwd=next;return 0;
}
/* Working directories remain descriptor anchored and record the canonical
   (link-free) path. A renamed cwd is usable for relative I/O, but getcwd fails
   instead of returning an unverified stale pathname. */
static uint64_t fs_chdir(void) {
    char leaf[1025],canon[1025];
    int parent=fs_lookup((uint64_t)-100,cpu->gpr[0],leaf,1,canon);if(parent<0)return fs_error();
    int fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int saved=errno;close(parent);errno=saved;if(fd<0)return fs_error();
    GuestCwd *next=calloc(1,sizeof(*next));if(!next) { close(fd);return linux_error(12); }
    next->fd=fd;next->refs=1;strcpy(next->path,canon);
    fs_cwd_release();fs_cwd=next;return 0;
}
/* Initial working directory from --cwd (an image WorkingDir), resolved inside the root. */
static int mem_start_cwd(const char *path);
static int fs_start_cwd(const char *path) {
    { int m=path[0]=='/'?mem_start_cwd(path):0;if(m)return m>0?0:-1; }
    size_t length=strlen(path);
    if(fs_root<0 || path[0]!='/' || length>1024) { errno=EINVAL;return -1; }
    for(size_t i=0;i<length;i++)if((unsigned char)path[i]<32 || path[i]==127) { errno=EINVAL;return -1; }
    char leaf[1025],canon[1025];int parent=fs_walk(fs_root,"/",path,1,leaf,canon);if(parent<0)return -1;
    int fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int saved=errno;close(parent);errno=saved;if(fd<0)return -1;
    GuestCwd *next=calloc(1,sizeof(*next));if(!next) { close(fd);errno=ENOMEM;return -1; }
    next->fd=fd;next->refs=1;strcpy(next->path,canon);
    fs_cwd_release();fs_cwd=next;return 0;
}
static uint64_t fs_getcwd(void) {
    if(fs_root<0)return linux_error(13);
    const char *path=fs_cwd?fs_cwd->path:"/";
    if(fs_cwd) {
        if(!*path)return linux_error(2);
        /* Walk the path again (through bind mounts) and check it still names the cwd. */
        char leaf[1025],canon[1025];int parent=fs_walk(fs_root,"/",path,1|FS_DIRECT,leaf,canon);
        struct stat a,b;int same=parent>=0 && !fstatat(parent,leaf,&a,FS_NOFOLLOW_AT) && S_ISDIR(a.st_mode) && (fs_lower>=0 || (!fstat(fs_cwd->fd,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino)); /* layers: a path (overlay.h) */
        if(parent>=0)close(parent);if(!same)return linux_error(2);
    }
    size_t size=strlen(path)+1;if(cpu->gpr[1]<size)return linux_error(34);
    unsigned char *out=lookup(cpu->gpr[0],size,2);if(!out)return linux_error(14);
    memcpy(out,path,size);return size;
}
/* Programs and ELF interpreters by inode, one host descriptor each kept open: macOS's
   open costs 12-20 us, and every exec opens its program (a dynamic one its loader
   too); a stat (often known from the walk) and a dup cost 1-2. The loader only maps
   and preads them, so duplicates share no offset that matters. An inode stays the same
   file while a descriptor holds it. ponytail: 32 entries, round robin. */
#define EXEC_FDS 32u
static struct { dev_t dev;ino_t ino;int fd; } exec_fds[EXEC_FDS];static unsigned exec_fds_next;
static pthread_mutex_t exec_fds_lock=PTHREAD_MUTEX_INITIALIZER;
static int exec_open_at(int parent,const char *leaf,int flags,const struct stat *known) {
    struct stat st;int fd=-1;
    if(known)st=*known;else if(fstatat(parent,leaf,&st,FS_NOFOLLOW_AT))st.st_mode=0;
    if(S_ISREG(st.st_mode)) {
        pthread_mutex_lock(&exec_fds_lock);
        for(unsigned i=0;i<EXEC_FDS && fd<0;i++)if(exec_fds[i].fd>0 && exec_fds[i].ino==st.st_ino && exec_fds[i].dev==st.st_dev)fd=fcntl(exec_fds[i].fd-1,F_DUPFD_CLOEXEC,0);
        pthread_mutex_unlock(&exec_fds_lock);
        if(fd>=0)return fd;
    }
    if((fd=openat(parent,leaf,flags))<0 || !S_ISREG(st.st_mode))return fd;
    struct stat now;int keep;
    if(fstat(fd,&now) || now.st_ino!=st.st_ino || now.st_dev!=st.st_dev || (keep=fcntl(fd,F_DUPFD_CLOEXEC,0))<0)return fd;
    pthread_mutex_lock(&exec_fds_lock);
    unsigned at=exec_fds_next++%EXEC_FDS;int old=exec_fds[at].fd-1;
    exec_fds[at].dev=st.st_dev;exec_fds[at].ino=st.st_ino;exec_fds[at].fd=keep+1;
    pthread_mutex_unlock(&exec_fds_lock);
    if(old>=0)close(old);
    return fd;
}
/* Interpreter paths originate in an ELF header, not guest memory. */
static int fs_open_root_file(const char *path) {
    if(fs_root<0 || path[0]!='/') { errno=EACCES;return -1; }
    char copy[1025];size_t length=strlen(path);
    if(length>1024) { errno=ENAMETOOLONG;return -1; }
    for(size_t i=0;i<length;i++)if((unsigned char)path[i]<32 || path[i]==127) { errno=EACCES;return -1; }
    strcpy(copy,path);char leaf[1025];
    int parent=fs_walk(fs_root,"/",copy,1,leaf,NULL);if(parent<0)return -1;
    int fd=exec_open_at(parent,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,NULL);
    int saved=errno;close(parent);errno=saved;
    struct stat st;if(fd>=0 && (fstat(fd,&st)<0 || !S_ISREG(st.st_mode))) { close(fd);errno=EACCES;return -1; }
    return fd;
}
/* memfd_create(name, flags): an anonymous host file, unlinked at once (JITs such as
   Erlang's map it twice, writable and executable). MFD_CLOEXEC; MFD_ALLOW_SEALING is
   accepted and seals are not enforced. */
static uint64_t fs_memfd(void) {
    uint64_t flags=cpu->gpr[1];
    if(flags&~UINT64_C(3))return linux_error(22);
    char name[1025];if(fs_string(cpu->gpr[0],name)<0)return fs_error();
    int slot=fs_claim();
    if(slot<0 || fd_lowest(0)<0)return linux_error(24);
    char path[]="/tmp/dockiva-memfd-XXXXXX";int fd=mkstemp(path);if(fd<0)return fs_error();
    unlink(path);(void)fcntl(fd,F_SETFD,FD_CLOEXEC);
    FS(slot).fd=fd+1;FS(slot).cloexec=(flags&1)!=0;FS(slot).ro=0;FS(slot).dev=0;FS(slot).sole=0;FS(slot).pack=NULL;
    int guest=fd_lowest(0);FS(slot).guest=(unsigned short)guest;fd_bind((uint64_t)guest,FD_FILE,(unsigned)slot);return (uint64_t)guest;
}
static unsigned guest_cpus(void); /* scheduler.h */
static uint64_t io_text_open(const void *data,size_t size,int cloexec,int directory,const char *path); /* io.h: path, to read it again */
static int proc_text(const char *path,char *text,size_t capacity,size_t *out);
static const char *io_text_path(uint64_t fd);                              /* io.h */
static uint64_t io_fifo_open(dev_t dev,ino_t ino,uint64_t flags);          /* io.h */
static uint64_t guest_ppid_of(uint64_t pid);static unsigned guest_threads_of(uint64_t pid); /* syscall.h */
static int guest_process_exists(uint64_t pid);
static unsigned process_count; /* process.h: child slots in use (the highest, plus one); scans stop there */
static char guest_state_of(uint64_t pid);       /* syscall.h: R, S or T */
static uint64_t guest_group_of(uint64_t pid,int session); /* syscall.h: process group or session */
static void guest_signal_sets(uint64_t pid,uint64_t sets[4]);  /* syscall.h: pending, blocked, ignored, caught */
static void guest_ids_of(uint64_t pid,uint32_t uid[3],uint32_t gid[3]); /* syscall.h: real, effective, saved */
/* syscall.h: a process's threads (tid, R or S, CPU ns), at most capacity; the main thread's tid is the pid. */
static unsigned guest_thread_list(uint64_t pid,unsigned *tids,char *states,uint64_t (*cpu)[2],unsigned capacity);
static void proc_memory(uint64_t pid,int own_held,uint64_t *vsz,uint64_t *rss); /* syscall.h */
static uint64_t runtime_started_ns;             /* this run's start (CLOCK_REALTIME ns) */
#define PROC_TICK_NS UINT64_C(10000000)          /* USER_HZ: AT_CLKTCK is 100 */
static uint64_t proc_now_ns(void) { struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
/* Per pid: when the process was forked (0: as this run started) and its CPU time in
   ns (user, system), which the scheduler charges for every slice from its worker's
   host thread usage. proc_cpu_total keeps every process's, exited ones too, so the
   cpu lines of /proc/stat only grow (top works from their deltas). */
static uint64_t proc_started_ns[PROCESS_LIMIT+2],proc_cpu_ns[PROCESS_LIMIT+2][2],proc_cpu_total[2];
static void proc_start(uint64_t pid) {
    if(pid && pid<=PROCESS_LIMIT+1) { proc_started_ns[pid]=proc_now_ns();proc_cpu_ns[pid][0]=proc_cpu_ns[pid][1]=0; }
}
/* A host thread's CPU time in ns (user, system). */
#ifdef __APPLE__
typedef mach_port_t ProcThread;
static ProcThread proc_thread_self(void) { return pthread_mach_thread_np(pthread_self()); }
static void proc_cpu_read(ProcThread thread,uint64_t now[2]) {
    thread_basic_info_data_t info;mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;now[0]=now[1]=0;
    if(thread_info(thread,THREAD_BASIC_INFO,(thread_info_t)&info,&count)!=KERN_SUCCESS)return;
    now[0]=(uint64_t)info.user_time.seconds*1000000000u+(uint64_t)info.user_time.microseconds*1000u;
    now[1]=(uint64_t)info.system_time.seconds*1000000000u+(uint64_t)info.system_time.microseconds*1000u;
}
#else
typedef clockid_t ProcThread;
static ProcThread proc_thread_self(void) { clockid_t id;return pthread_getcpuclockid(pthread_self(),&id)?CLOCK_THREAD_CPUTIME_ID:id; }
static void proc_cpu_read(ProcThread thread,uint64_t now[2]) { struct timespec t;clock_gettime(thread,&t);now[0]=(uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;now[1]=0; }
#endif
/* The slice each worker runs now (pid 0: none), from mark on its host thread: a slice
   can run long (system calls served without the runtime lock do not end it), so its
   time so far counts too. Changed and read under the runtime lock. */
static struct { uint64_t pid,*thread,mark[2];ProcThread host; } proc_live[64]; /* WORKER_LIMIT */
static void proc_cpu_begin(unsigned worker_index,uint64_t pid,uint64_t *thread) {
    if(worker_index>=64)return;
    proc_live[worker_index].host=proc_thread_self();proc_cpu_read(proc_live[worker_index].host,proc_live[worker_index].mark);
    proc_live[worker_index].pid=pid;proc_live[worker_index].thread=thread;
}
static void proc_cpu_end(unsigned worker_index) {
    if(worker_index>=64 || !proc_live[worker_index].pid)return;
    uint64_t now[2],pid=proc_live[worker_index].pid,*thread=proc_live[worker_index].thread;proc_cpu_read(proc_live[worker_index].host,now);
    for(unsigned i=0;i<2;i++)if(now[i]>proc_live[worker_index].mark[i]) {
        uint64_t used=now[i]-proc_live[worker_index].mark[i];proc_cpu_total[i]+=used;
        if(pid<=PROCESS_LIMIT+1)proc_cpu_ns[pid][i]+=used;
        if(thread)thread[i]+=used;
    }
    proc_live[worker_index].pid=0;
}
/* Adds the running slices' time so far: of pid (0: every process), or of one thread. */
static void proc_cpu_live(uint64_t pid,const uint64_t *thread,uint64_t out[2]) {
    for(unsigned w=0;w<64;w++)if(proc_live[w].pid && (thread?proc_live[w].thread==thread:!pid || proc_live[w].pid==pid)) {
        uint64_t now[2];proc_cpu_read(proc_live[w].host,now);
        for(unsigned i=0;i<2;i++)if(now[i]>proc_live[w].mark[i])out[i]+=now[i]-proc_live[w].mark[i];
    }
}
/* Unscheduled, the one process is the whole run. */
static void proc_rusage(uint64_t out[2]) {
    struct rusage use;getrusage(RUSAGE_SELF,&use);
    out[0]=(uint64_t)use.ru_utime.tv_sec*1000000000u+(uint64_t)use.ru_utime.tv_usec*1000u;
    out[1]=(uint64_t)use.ru_stime.tv_sec*1000000000u+(uint64_t)use.ru_stime.tv_usec*1000u;
}
/* The container's processes as /proc shows them. An exec or healthcheck is a run of
   its own, so the main run publishes its process table (--proc-publish) and other runs
   read it (--proc-peer): their /proc lists their own processes under the pids getpid
   returns (/proc/self/exe, /proc/<getpid()>/stat) and the main run's as
   PROC_PEER_OFFSET+pid, so the two never collide. Read-only: a peer's processes cannot
   be signalled from another run.
   The table: per process "pid ppid uid size cmdline\n" (cmdline: size bytes), then
   "=boot boot_ns user_ticks system_ticks used_bytes load1 load5 load15 load_ns\n" and
   per process "=pid utime stime start_ticks vsz_kib rss_kib threads state\n" (older
   readers stop at the first '='). */
static const char *proc_publish_path,*proc_peer_path;
#define PROC_PEER_OFFSET 10000u /* above every pid of a run (PROCESS_LIMIT+1) */
/* Writes are coalesced: at most one per 20 ms (a build forks thousands of processes;
   each rewrite cost 0.4 ms of a fork). A change within that time is written by the
   scheduler's next pump (proc_publish_flush), so readers lag by at most about that.
   Usage (CPU, memory, states) is republished every 100 ms while it changes (busybox
   top's first frame is a 100 ms delta); a write costs about 0.15 ms. */
#define PROC_PUBLISH_NS UINT64_C(20000000)
#define PROC_USAGE_NS UINT64_C(100000000)
struct ProcPeer { uint64_t pid,ppid,utime,stime,start,vsz,rss;unsigned uid,size,threads;char state;char cmdline[1024]; };
static struct ProcPeer proc_peers[PROCESS_LIMIT+2];static unsigned proc_npeers;
static struct { uint64_t boot_ns,user,system,used,load_ns;double load[3]; } proc_main; /* the main run's totals */
static uint64_t proc_peer_loaded_ns;
static void proc_peer_load(void) {
    if(!proc_peer_path)return;
    uint64_t now=proc_now_ns();if(now-proc_peer_loaded_ns<PROC_PUBLISH_NS)return; /* one read per burst of /proc lookups */
    proc_peer_loaded_ns=now;proc_npeers=0;
    FILE *f=fopen(proc_peer_path,"rb");if(!f)return;
    struct ProcPeer *p;unsigned long long pid,ppid;unsigned uid,size;
    while(proc_npeers<PROCESS_LIMIT+2 && fscanf(f,"%llu %llu %u %u",&pid,&ppid,&uid,&size)==4 && fgetc(f)==' ') {
        p=&proc_peers[proc_npeers];if(size>sizeof(p->cmdline))break;
        if(fread(p->cmdline,1,size,f)!=size)break;
        memset(p,0,offsetof(struct ProcPeer,cmdline));
        p->pid=pid+PROC_PEER_OFFSET;p->ppid=ppid?ppid+PROC_PEER_OFFSET:0;p->uid=uid;p->size=size;p->threads=1;p->state='S';proc_npeers++;
        if(fgetc(f)!='\n')break;
    }
    char tag[24];
    while(fscanf(f," =%23s",tag)==1) {
        if(!strcmp(tag,"boot")) {
            unsigned long long v[5];double l[3];
            if(fscanf(f,"%llu %llu %llu %llu %lf %lf %lf %llu",&v[0],&v[1],&v[2],&v[3],&l[0],&l[1],&l[2],&v[4])==8) {
                proc_main.boot_ns=v[0];proc_main.user=v[1];proc_main.system=v[2];proc_main.used=v[3];proc_main.load_ns=v[4];
                memcpy(proc_main.load,l,sizeof(l));
            }
        } else {
            unsigned long long v[5];unsigned threads;char state;
            if(fscanf(f,"%llu %llu %llu %llu %llu %u %c",&v[0],&v[1],&v[2],&v[3],&v[4],&threads,&state)!=7)break;
            for(unsigned i=0;i<proc_npeers;i++)if(proc_peers[i].pid==strtoull(tag,NULL,10)+PROC_PEER_OFFSET) {
                p=&proc_peers[i];p->utime=v[0];p->stime=v[1];p->start=v[2];p->vsz=v[3]<<10;p->rss=v[4]<<10;p->threads=threads;p->state=state;
            }
        }
    }
    fclose(f);
}
/* The container's boot: the main run's start. */
static uint64_t proc_boot_ns(void) {
    if(proc_peer_path) { proc_peer_load();if(proc_main.boot_ns)return proc_main.boot_ns; }
    return runtime_started_ns;
}
/* CLOCK_BOOTTIME counts from the container's start, as in a Linux time namespace, so it
   agrees with /proc/uptime and process start times (procps derives elapsed time and
   %CPU from it). Host CLOCK_MONOTONIC, like BOOTTIME, includes sleep. */
static uint64_t proc_boottime_ns(void) {
    static uint64_t base;uint64_t now=clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if(!base) { uint64_t real=proc_now_ns(),boot=proc_boot_ns();base=now-(real>boot?real-boot:0); }
    return now-base;
}
static uint64_t proc_started(uint64_t pid) { return pid<=PROCESS_LIMIT+1 && proc_started_ns[pid]?proc_started_ns[pid]:runtime_started_ns; }
static void proc_cpu_of(uint64_t pid,uint64_t out[2]) {
    if(!schedule_active) { proc_rusage(out);return; }
    out[0]=proc_cpu_ns[pid][0];out[1]=proc_cpu_ns[pid][1];proc_cpu_live(pid,NULL,out);
}
/* Memory: the budget (docker run -m), or the host's memory if that is smaller; in use,
   what the container's runs have charged. */
static uint64_t proc_mem_total(void) {
#ifdef __APPLE__
    static uint64_t host;if(!host) { size_t size=sizeof(host);if(sysctlbyname("hw.memsize",&host,&size,NULL,0))host=UINT64_MAX; }
#else
    static uint64_t host;if(!host) { long pages=sysconf(_SC_PHYS_PAGES),page=sysconf(_SC_PAGESIZE);host=pages>0 && page>0?(uint64_t)pages*(uint64_t)page:UINT64_MAX; }
#endif
    return resource_limit<host?resource_limit:host;
}
static uint64_t proc_mem_used(void) {
    uint64_t used=resource_used;
    if(proc_peer_path) { proc_peer_load();used+=proc_main.used; }
    return used;
}
/* Processes in state R: this run's, and the main run's as last published. */
static unsigned proc_running(void) {
    unsigned running=0;
    for(uint64_t p=1;p<=process_count+1;p++)if(guest_process_exists(p))running+=guest_state_of(p)=='R';
    proc_peer_load();for(unsigned i=0;i<proc_npeers;i++)running+=proc_peers[i].state=='R';
    return running;
}
/* Load averages as Linux keeps them: runnable processes, decaying over 1, 5 and 15
   minutes, sampled every 100 ms (proc_publish_flush) and on reads. An exec run
   carries the main run's published averages forward. */
static double proc_load[3];static uint64_t proc_load_ns;
static void proc_load_sample(double out[3]) {
    static const double period[3]={60,300,900};
    uint64_t now=proc_now_ns();unsigned running=proc_running();
    if(proc_peer_path && proc_main.load_ns) { memcpy(proc_load,proc_main.load,sizeof(proc_load));proc_load_ns=proc_main.load_ns; }
    if(proc_load_ns && now>proc_load_ns)
        for(unsigned i=0;i<3;i++) { double e=exp(-(double)(now-proc_load_ns)/1e9/period[i]);proc_load[i]=proc_load[i]*e+running*(1-e); }
    proc_load_ns=now;
    if(out)memcpy(out,proc_load,sizeof(proc_load));
}
/* The container's CPU time in ticks (user, system): every process's, ever. */
static void proc_cpu_all(uint64_t out[2]) {
    uint64_t own[2]={proc_cpu_total[0],proc_cpu_total[1]};proc_cpu_live(0,NULL,own);if(!schedule_active)proc_rusage(own);
    out[0]=own[0]/PROC_TICK_NS;out[1]=own[1]/PROC_TICK_NS;
    if(proc_peer_path) { proc_peer_load();out[0]+=proc_main.user;out[1]+=proc_main.system; }
}
static int proc_publish_pending;static uint64_t proc_publish_last_ns,proc_tick_ns;
static void proc_publish_write(uint64_t ended,int own_held);
/* A process started, exec'd or ended (ended: its pid, whose task is gone but may still
   be the current one); the caller holds the current process's mm lock, if any. */
static void proc_publish(uint64_t ended) {
    if(!proc_publish_path)return;
    if(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-proc_publish_last_ns>=PROC_PUBLISH_NS)proc_publish_write(ended,1);
    else proc_publish_pending=1;
}
/* The scheduler's pump, busy or idle (no mm lock held). */
static void proc_publish_flush(void) {
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if(now-proc_tick_ns>=PROC_USAGE_NS) { proc_tick_ns=now;proc_load_sample(NULL);if(proc_publish_path)proc_publish_write(0,-1); }
    if(proc_publish_pending && now-proc_publish_last_ns>=PROC_PUBLISH_NS)proc_publish_write(0,0);
}
/* own_held: the current process's mm lock is held (1), or not (0); -1 as 0, but only
   rewrite when usage changed since the last write. */
static void proc_publish_write(uint64_t ended,int own_held) {
    static char usage[16384],last[16384];size_t length=0;
    uint64_t boot=runtime_started_ns,cpu[2];proc_cpu_all(cpu);
    length+=(size_t)snprintf(usage+length,sizeof(usage)-length,"%llu %llu %llu %llu\n",(unsigned long long)boot,
        (unsigned long long)cpu[0],(unsigned long long)cpu[1],(unsigned long long)resource_used);
    for(uint64_t pid=1;pid<=process_count+1 && length<sizeof(usage)-128;pid++)if(pid!=ended && guest_process_exists(pid)) {
        uint64_t vsz,rss,used[2],start=proc_started(pid);proc_memory(pid,own_held>0,&vsz,&rss);proc_cpu_of(pid,used);
        length+=(size_t)snprintf(usage+length,sizeof(usage)-length,"=%llu %llu %llu %llu %llu %llu %u %c\n",(unsigned long long)pid,
            (unsigned long long)(used[0]/PROC_TICK_NS),(unsigned long long)(used[1]/PROC_TICK_NS),(unsigned long long)(start>boot?(start-boot)/PROC_TICK_NS:0),
            (unsigned long long)(vsz>>10),(unsigned long long)(rss>>10),guest_threads_of(pid),guest_state_of(pid));
    }
    if(own_held<0 && !strcmp(usage,last))return;
    memcpy(last,usage,length+1);
    proc_publish_pending=0;proc_publish_last_ns=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    char temporary[1100];snprintf(temporary,sizeof(temporary),"%s.%d",proc_publish_path,(int)getpid());
    FILE *f=fopen(temporary,"wb");if(!f)return;
    for(uint64_t pid=1;pid<=process_count+1;pid++)if(pid!=ended && guest_process_exists(pid)) {
        uint32_t uid[3],gid[3];guest_ids_of(pid,uid,gid);
        fprintf(f,"%llu %llu %u %u ",(unsigned long long)pid,(unsigned long long)guest_ppid_of(pid),uid[1],proc_cmdline_size[pid]);
        fwrite(proc_cmdline[pid],1,proc_cmdline_size[pid],f);fputc('\n',f);
    }
    double load[3];proc_load_sample(load);
    char *rest=strchr(usage,'\n');*rest=0;  /* the totals line, then the processes' */
    fprintf(f,"=boot %s %.2f %.2f %.2f %llu\n%s",usage,load[0],load[1],load[2],(unsigned long long)proc_load_ns,rest+1);
    *rest='\n';
    if(fclose(f)==0)rename(temporary,proc_publish_path);else unlink(temporary);
}
/* A /proc pid: one of this run's (>0, its pid), a published one (<0, -(index+1)), or none (0). */
static int64_t proc_resolve(uint64_t display) {
    if(display<=PROCESS_LIMIT+1)return guest_process_exists(display)?(int64_t)display:0;
    if(!proc_peer_path)return 0;
    proc_peer_load();
    for(unsigned i=0;i<proc_npeers;i++)if(proc_peers[i].pid==display)return -(int64_t)i-1;
    return 0;
}
/* What /proc shows of a process (tid: one of its threads, or 0): times in ticks
   since boot, memory in bytes. */
typedef struct { uint64_t pid,ppid,pgrp,session,utime,stime,start,vsz,rss;unsigned threads,size;uint32_t uid[3],gid[3];char state;char comm[16];const char *cmdline; } ProcInfo; /* ids: real, effective, saved */
static int proc_info(uint64_t display,uint64_t tid,ProcInfo *info) {
    int64_t r=proc_resolve(display);if(!r)return 0;
    memset(info,0,sizeof(*info));info->pid=info->pgrp=info->session=display;
    if(r>0) {
        uint64_t own=(uint64_t)r,ppid=guest_ppid_of(own),boot=proc_boot_ns(),start=proc_started(own),used[2];
        info->ppid=ppid?ppid:0;info->pgrp=guest_group_of(own,0);info->session=guest_group_of(own,1);
        info->threads=guest_threads_of(own);info->state=guest_state_of(own);
        guest_ids_of(own,info->uid,info->gid);
        snprintf(info->comm,sizeof(info->comm),"%s",own==process_pid && *prctl_name?prctl_name:proc_comm(own));
        info->cmdline=proc_cmdline[own];info->size=proc_cmdline_size[own];
        proc_cpu_of(own,used);
        if(tid && tid!=display) {                                   /* a thread: its own state and CPU time */
            unsigned tids[256];char states[256];uint64_t cpu[256][2];unsigned n=guest_thread_list(own,tids,states,cpu,256);
            for(unsigned i=0;i<n;i++)if(tids[i]==tid) { info->state=states[i];used[0]=cpu[i][0];used[1]=cpu[i][1]; }
        }
        info->utime=used[0]/PROC_TICK_NS;info->stime=used[1]/PROC_TICK_NS;info->start=start>boot?(start-boot)/PROC_TICK_NS:0;
        proc_memory(own,1,&info->vsz,&info->rss);
    } else {
        struct ProcPeer *p=&proc_peers[-r-1];
        info->ppid=p->ppid;info->threads=p->threads;for(unsigned i=0;i<3;i++)info->uid[i]=info->gid[i]=p->uid;info->state=p->state;info->cmdline=p->cmdline;info->size=p->size;
        info->utime=p->utime;info->stime=p->stime;info->start=p->start;info->vsz=p->vsz;info->rss=p->rss;
        const char *argv0=p->size?p->cmdline:"guest",*slash=strrchr(argv0,'/');
        snprintf(info->comm,sizeof(info->comm),"%s",slash && slash[1]?slash+1:argv0);
    }
    if(tid)info->pid=tid;
    return 1;
}
/* Whether tid names a thread of the process with /proc pid display (its main thread
   is the pid itself; another run's processes show only that one). */
static int proc_thread_exists(uint64_t display,uint64_t tid) {
    if(tid==display)return 1;
    int64_t r=proc_resolve(display);if(r<=0)return 0;
    unsigned tids[256];char states[256];uint64_t cpu[256][2];unsigned n=guest_thread_list((uint64_t)r,tids,states,cpu,256);
    for(unsigned i=1;i<n;i++)if(tids[i]==tid)return 1;
    return 0;
}
/* A path in a process's /proc directory: /proc/self/REST, /proc/PID/REST or
   /proc/PID/task/TID/REST, REST empty for the directory itself. Returns the /proc pid
   (0: none of ours), the thread (0: the process) and REST. */
static uint64_t proc_split(const char *path,uint64_t *tid,const char **rest) {
    *tid=0;*rest="";
    if(strncmp(path,"/proc/",6))return 0;
    const char *p=path+6;uint64_t pid=0;
    if(!strncmp(p,"self",4) && (!p[4] || p[4]=='/')) { pid=process_pid;p+=4; }
    else {
        const char *digits=p;while(*p>='0' && *p<='9' && pid<100000)pid=pid*10+(uint64_t)(*p++-'0');
        if(p==digits || (*p && *p!='/') || !proc_resolve(pid))return 0;
    }
    if(*p=='/')p++;
    if(!strncmp(p,"task/",5) && p[5]>='0' && p[5]<='9') {
        const char *q=p+5;uint64_t t=0;while(*q>='0' && *q<='9' && t<10000000)t=t*10+(uint64_t)(*q++-'0');
        if((*q && *q!='/') || !proc_thread_exists(pid,t))return 0;
        *tid=t;p=*q?q+1:q;
    }
    *rest=p;return pid;
}
static const char *const fs_proc_root_files[]={"cpuinfo","loadavg","meminfo","mounts","stat","uptime","vmstat"};
static const char *const fs_proc_pid_files[]={"auxv","cgroup","cmdline","comm","maps","mountinfo","mounts","stat","statm","status"};
static size_t proc_maps(char *out,size_t capacity); /* mapping.h */
/* Kernel settings programs read (Elasticsearch's bootstrap checks, ClickHouse, Redis):
   Linux defaults, and somaxconn as the backlog this runtime grants (NET_BACKLOG_LIMIT). */
static const char *const fs_proc_sys[][2]={{"/proc/sys/vm/max_map_count","262144\n"},{"/proc/sys/vm/overcommit_memory","0\n"},
    {"/proc/sys/net/core/somaxconn","128\n"},{"/proc/sys/kernel/pid_max","4194304\n"},{"/proc/sys/kernel/ngroups_max","65536\n"}};
/* --x86-translator DIR: FEX in the guest at DIR (bin/FEX and bin/FEXServer, lib/ with
   their arm64 glibc and ld.so). The runtime then acts as binfmt_misc with FEX
   registered (x86_exec_args): an x86 ELF exec'd runs as "FEX PROGRAM ARGS", and FEX's
   own programs through DIR/lib's ld.so. /proc/sys/fs/binfmt_misc lists FEX's two
   entries, which is how FEX knows to hand an x86 execve back to the kernel rather
   than re-executing itself. */
static char x86_translator[256];
/* AT_EXECFD for startup, when >= 0: the program's descriptor that binfmt_misc's O flag
   gives its interpreter (FEX reads the program through it, and its argv in the
   preserve-argv0 layout); startup leaves the value's guest address in
   startup_execfd_slot. */
static int64_t startup_execfd=-1;static uint64_t startup_execfd_slot;
/* Whether fd holds an x86-64 or i386 ELF. */
static int x86_elf(int fd) {
    unsigned char h[20];
    if(pread(fd,h,sizeof(h),0)!=(ssize_t)sizeof(h) || memcmp(h,"\177ELF",4))return 0;
    unsigned machine=(unsigned)h[18]|(unsigned)h[19]<<8;
    return (h[4]==2 && machine==62) || (h[4]==1 && machine==3);
}
static const char *fs_proc_sys_value(const char *path) {
    for(unsigned i=0;i<sizeof(fs_proc_sys)/sizeof(*fs_proc_sys);i++)if(!strcmp(path,fs_proc_sys[i][0]))return fs_proc_sys[i][1];
    if(x86_translator[0] && (!strcmp(path,"/proc/sys/fs/binfmt_misc/FEX-x86_64") || !strcmp(path,"/proc/sys/fs/binfmt_misc/FEX-x86")))return "enabled\nflags: POCF\n";
    return NULL;
}
/* The command line binfmt_misc gives a program at the guest path exe (argv: n of them,
   argv[0] first), when it is x86 (x86 set) or one of FEX's own (under
   x86_translator): the translator's ld.so, its lib directory, then FEX with exe, its
   argv[0] and the other arguments (binfmt's preserve-argv0 layout, which FEX reads
   with AT_EXECFD set: the caller gives that), or the program itself named argv[0].
   The strings go to buffer from used; out gets the new argv. Returns its length, 0
   when exe is neither, -1 when the entries or strings would not fit. */
static int x86_exec_args(const char *exe,int x86,const char *const *argv,unsigned n,char *buffer,size_t *used,size_t limit,const char **out,unsigned capacity) {
    size_t dir=strlen(x86_translator);
    char ld[300],lib[300],fex[300];
    snprintf(ld,sizeof(ld),"%s/lib/ld-linux-aarch64.so.1",x86_translator);snprintf(lib,sizeof(lib),"%s/lib",x86_translator);snprintf(fex,sizeof(fex),"%s/bin/FEX",x86_translator);
    if(!x86_translator[0] || (!x86 && (strncmp(exe,x86_translator,dir) || exe[dir]!='/' || !strcmp(exe,ld))))return 0;
    const char *first=n?argv[0]:exe;
    const char *extra[8]={ld,"--library-path",lib,"--argv0",x86?fex:first,x86?fex:exe,exe,first};
    unsigned k=x86?8:6;
    if(n+k>capacity)return -1;
    for(unsigned i=0;i<k;i++) {
        size_t length=strlen(extra[i])+1;if(*used+length>limit)return -1;
        memcpy(buffer+*used,extra[i],length);out[i]=buffer+*used;*used+=length;
    }
    for(unsigned i=1;i<n;i++)out[k-1+i]=argv[i];
    return (int)(k-1+(n?n:1));
}
/* Go's asynchronous preemption (a SIGURG whose handler moves the interrupted thread
   into runtime.asyncPreempt) breaks under FEX: now and then a return from the edited
   signal frame resumes with a wrong stack or instruction pointer, and Go dies with
   "unknown caller pc", "fatal error: fault" or corrupt memory (on a Linux kernel
   too: FEX 2610 and 2611). An x86 program FEX runs therefore gets
   GODEBUG=asyncpreemptoff=1, added to a GODEBUG it has unless that already sets
   asyncpreemptoff to a value every Go applies (x86_godebug_int); Go then preempts at
   function calls only, and programs in other languages ignore it. The program and
   its children see it in their environment, even one its caller cleared (env -i), as
   do programs FEX runs by running itself again (a #! script whose interpreter is x86).
   Strings go to buffer at *used (limit bytes); env grows to capacity at most. */
/* Whether a GODEBUG value (up to ',' or the end) is one every Go applies: Go 1.26
   and later parse it with strconv.ParseInt (base 10, 32 bits), earlier ones with the
   runtime's Atoi32, which takes no '+'; a setting Go cannot parse is skipped
   ("asyncpreemptoff=true" leaves preemption on). So an optional '-', digits, and a
   value in int32's range. */
static int x86_godebug_int(const char *p) {
    int negative=*p=='-';p+=negative;
    const char *digits=p;uint64_t v=0;
    while(*p>='0' && *p<='9') { v=v*10+(uint64_t)(*p-'0');if(v>UINT64_C(2147483647)+(uint64_t)negative)return 0;p++; }
    return p>digits && (!*p || *p==',');
}
static void x86_godebug(const char **env,unsigned *envc,unsigned capacity,char *buffer,size_t *used,size_t limit) {
    static const char key[]="asyncpreemptoff=",fresh[]="GODEBUG=asyncpreemptoff=1";
    for(unsigned i=0;i<*envc;i++) {
        if(strncmp(env[i],"GODEBUG=",8))continue;
        const char *value=env[i]+8;
        for(const char *p=value;p;p=strchr(p,',')) {
            if(*p==',')p++;
            if(!strncmp(p,key,sizeof(key)-1) && x86_godebug_int(p+sizeof(key)-1))return; /* the program's own choice */
        }
        size_t length=strlen(env[i]),need=length+(value[0]?1:0)+sizeof("asyncpreemptoff=1");
        if(need>limit-*used)return;
        char *out=buffer+*used;
        memcpy(out,env[i],length);if(value[0])out[length++]=',';
        memcpy(out+length,"asyncpreemptoff=1",sizeof("asyncpreemptoff=1"));
        env[i]=out;*used+=need;return;
    }
    if(*envc>=capacity || sizeof(fresh)>limit-*used)return;
    char *out=buffer+*used;memcpy(out,fresh,sizeof(fresh));*used+=sizeof(fresh);
    env[(*envc)++]=out;
}
/* A path argument as /proc sees it: relative to a /proc working directory (busybox
   top chdirs there) or to one of our /proc directory descriptors (htop reads files
   relative to /proc/PID), made absolute, without trailing slashes. Other paths are
   returned as given (callers that fall through reread the guest's string). */
static int fs_proc_path(uint64_t dir,uint64_t ptr,char path[1025]) {
    if(fs_string(ptr,path))return -1;
    if(path[0] && path[0]!='/' && !fs_detached.on) {                 /* detached: not relative to a /proc directory */
        const char *base=(int32_t)dir==-100?(fs_cwd?fs_cwd->path:"/"):io_text_path(dir);
        if(base && (int32_t)dir!=-100 && syscall_unlocked) { syscall_abort=1;return -1; }  /* process tables need the runtime lock */
        if(base && !strncmp(base,"/proc",5) && (!base[5] || base[5]=='/')) {
            const char *relative=path;while(relative[0]=='.' && relative[1]=='/')relative+=2;
            char joined[1025];
            if((size_t)snprintf(joined,sizeof(joined),"%s/%s",base,strcmp(relative,".")?relative:"")<sizeof(joined))strcpy(path,joined);
        }
    }
    size_t n=strlen(path);
    if(!strncmp(path,"/proc/",6))while(n>6 && path[n-1]=='/')path[--n]=0;
    return 0;
}
/* What a /proc path is: 1 a directory, 2 a file, 0 not one of ours. */
static int fs_proc_kind(const char *path) {
    if(!strcmp(path,"/proc") || !strcmp(path,"/proc/"))return 1;
    for(unsigned i=0;i<sizeof(fs_proc_root_files)/sizeof(*fs_proc_root_files);i++)
        if(!strncmp(path,"/proc/",6) && !strcmp(path+6,fs_proc_root_files[i]))return 2;
    uint64_t tid;const char *rest;
    if(proc_split(path,&tid,&rest)) {
        if(!*rest || (!tid && (!strcmp(rest,"task") || !strcmp(rest,"fd"))))return 1;
        for(unsigned i=0;i<sizeof(fs_proc_pid_files)/sizeof(*fs_proc_pid_files);i++)if(!strcmp(rest,fs_proc_pid_files[i]))return 2;
    }
    return !strcmp(path,"/proc/net/if_inet6") || fs_proc_sys_value(path)?2:0;
}
/* A linux_dirent64 record. */
static size_t fs_dirent(char *out,size_t used,size_t capacity,uint64_t inode,const char *name,unsigned type) {
    size_t length=strlen(name),size=(19+length+1+7)&~(size_t)7;
    if(used+size>capacity)return used;
    unsigned char *r=(unsigned char *)out+used;memset(r,0,size);
    fs_pack(r,0,inode,8);fs_pack(r,8,used+size,8);fs_pack(r,16,size,2);r[18]=(unsigned char)type;memcpy(r+19,name,length);
    return used+size;
}
/* The text of one of our synthetic files, or 0. Generated again when a reader seeks
   back to the start, as procfs does (procps top and vmstat keep /proc/stat open). */
static int proc_text(const char *path,char *text,size_t capacity,size_t *out) {
    uint64_t tid=0;const char *rest="";uint64_t proc=proc_split(path,&tid,&rest);
    size_t length=0;int known=1;ProcInfo info;
    uint64_t now=proc_now_ns(),boot=proc_boot_ns(),up=now>boot?now-boot:0;
    if(proc && !strcmp(rest,"auxv")) {
        /* This process's auxiliary vector (CPU features: Go's x/sys/cpu reads it). */
        for(unsigned i=0;i<address_space->auxv_count;i++)fs_pack((unsigned char *)text,i*8,address_space->auxv[i],8);
        length=address_space->auxv_count*8;
    } else if(!strcmp(path,"/proc/meminfo") || !strcmp(path,"/proc/vmstat")) {
        /* The memory budget and what the container uses of it (free, top, vmstat). */
        uint64_t total=proc_mem_total(),used=proc_mem_used(),free=(used<total?total-used:0)>>10;total>>=10;used>>=10;
        if(path[6]=='m')length=(size_t)snprintf(text,capacity,"MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n"
            "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\nActive:         %llu kB\nInactive:              0 kB\n"
            "Active(anon):   %llu kB\nInactive(anon):        0 kB\nActive(file):          0 kB\nInactive(file):        0 kB\n"
            "Unevictable:           0 kB\nMlocked:               0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n"
            "Dirty:                 0 kB\nWriteback:             0 kB\nAnonPages:      %llu kB\nMapped:                0 kB\nShmem:                 0 kB\n"
            "KReclaimable:          0 kB\nSlab:                  0 kB\nSReclaimable:          0 kB\nSUnreclaim:            0 kB\n"
            "KernelStack:           0 kB\nPageTables:            0 kB\nCommitLimit:    %llu kB\nCommitted_AS:   %llu kB\n"
            "HugePages_Total:       0\nHugePages_Free:        0\nHugepagesize:       2048 kB\n",
            (unsigned long long)total,(unsigned long long)free,(unsigned long long)free,(unsigned long long)used,(unsigned long long)used,
            (unsigned long long)used,(unsigned long long)total,(unsigned long long)used);
        else length=(size_t)snprintf(text,capacity,"nr_free_pages %llu\nnr_inactive_anon 0\nnr_active_anon %llu\nnr_inactive_file 0\nnr_active_file 0\n"
            "nr_anon_pages %llu\nnr_mapped 0\nnr_file_pages 0\nnr_dirty 0\nnr_writeback 0\nnr_shmem 0\npgpgin 0\npgpgout 0\npswpin 0\npswpout 0\n"
            "pgalloc_normal 0\npgfree 0\npgfault 0\npgmajfault 0\n",(unsigned long long)(free/4),(unsigned long long)(used/4),(unsigned long long)(used/4));
    } else if(!strcmp(path,"/proc/cpuinfo")) {
        /* One entry per guest CPU (MongoDB counts them), features from AT_HWCAP. */
        static const char *const names[]={"fp","asimd","evtstrm","aes","pmull","sha1","sha2","crc32","atomics","fphp","asimdhp",
            "cpuid","asimdrdm","jscvt","fcma","lrcpc","dcpop","sha3","sm3","sm4","asimddp","sha512","sve","asimdfhm","dit",
            "uscat","ilrcpc","flagm","ssbs","sb","paca","pacg"};
        uint64_t hwcap=0;char features[512]="";
        for(unsigned i=0;i+1<address_space->auxv_count;i+=2)if(address_space->auxv[i]==16)hwcap=address_space->auxv[i+1];
        for(unsigned i=0;i<32;i++)if(hwcap&(UINT64_C(1)<<i)) { if(*features)strcat(features," ");strcat(features,names[i]); }
        /* ponytail: a 16 KiB pipe holds about 40 entries; larger containers list that many. */
        unsigned cpus=guest_cpus();
        for(unsigned i=0;i<cpus;i++) {
            char entry[768];
            int n=snprintf(entry,sizeof entry,"processor\t: %u\nBogoMIPS\t: 48.00\nFeatures\t: %s\nCPU implementer\t: 0x61\n"
                "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n",i,features);
            if(n<0 || length+(size_t)n>capacity)break;
            memcpy(text+length,entry,(size_t)n);length+=(size_t)n;
        }
    } else if(proc && !strcmp(rest,"stat") && proc_info(proc,tid,&info)) {
        /* ps, top, pgrep: CPU time from the host threads that ran it, memory from its backings. */
        length=(size_t)snprintf(text,capacity,"%llu (%s) %c %llu %llu %llu 0 -1 4194560 0 0 0 0 %llu %llu 0 0 20 0 %u 0 %llu %llu %llu"
            " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            (unsigned long long)info.pid,info.comm,info.state,(unsigned long long)info.ppid,(unsigned long long)info.pgrp,(unsigned long long)info.session,
            (unsigned long long)info.utime,(unsigned long long)info.stime,info.threads,(unsigned long long)info.start,
            (unsigned long long)info.vsz,(unsigned long long)(info.rss/4096));
    } else if(proc && !strcmp(rest,"statm") && proc_info(proc,tid,&info)) {
        /* Pages: size resident shared text lib data dt (RabbitMQ's memory monitor). */
        length=(size_t)snprintf(text,capacity,"%llu %llu 0 0 0 %llu 0\n",(unsigned long long)(info.vsz/4096),(unsigned long long)(info.rss/4096),(unsigned long long)(info.vsz/4096));
    } else if(proc && !strcmp(rest,"status") && proc_info(proc,tid,&info)) {
        unsigned long long vsz=info.vsz>>10,rss=info.rss>>10;char state=info.state;
        /* Signal sets as Linux shows them (the JDK's attach API requires SigCgt to show SIGQUIT caught). */
        uint64_t sets[4]={0,0,0,0};int64_t own=proc_resolve(proc);if(own>0)guest_signal_sets((uint64_t)own,sets);
        length=(size_t)snprintf(text,capacity,"Name:\t%s\nUmask:\t0022\nState:\t%c (%s)\nTgid:\t%llu\nNgid:\t0\nPid:\t%llu\nPPid:\t%llu\nTracerPid:\t0\n"
            "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t256\nGroups:\t\nVmPeak:\t%llu kB\nVmSize:\t%llu kB\nVmLck:\t0 kB\nVmPin:\t0 kB\n"
            "VmHWM:\t%llu kB\nVmRSS:\t%llu kB\nRssAnon:\t%llu kB\nRssFile:\t0 kB\nRssShmem:\t0 kB\nVmData:\t%llu kB\nVmStk:\t132 kB\nVmExe:\t0 kB\n"
            "VmLib:\t0 kB\nVmPTE:\t0 kB\nVmSwap:\t0 kB\nThreads:\t%u\nSigQ:\t0/63432\nSigPnd:\t%016llx\nShdPnd:\t%016llx\n"
            "SigBlk:\t%016llx\nSigIgn:\t%016llx\nSigCgt:\t%016llx\nvoluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
            info.comm,state,state=='S'?"sleeping":state=='T'?"stopped":"running",
            (unsigned long long)proc,(unsigned long long)info.pid,(unsigned long long)info.ppid,info.uid[0],info.uid[1],info.uid[2],info.uid[1],
            info.gid[0],info.gid[1],info.gid[2],info.gid[1],vsz,vsz,rss,rss,rss,vsz,info.threads,0ull,(unsigned long long)sets[0],
            (unsigned long long)sets[1],(unsigned long long)sets[2],(unsigned long long)sets[3]);
    } else if(!strcmp(path,"/proc/mounts") || (proc && (!strcmp(rest,"mounts") || !strcmp(rest,"mountinfo")))) {
        /* The container's mounts (Java's FileStore looks a path's mount point up here). */
        int mountinfo=strstr(path,"mountinfo")!=NULL;
        static const char *const base[][4]={{"overlay","/","overlay","rw,relatime"},{"proc","/proc","proc","rw,nosuid,nodev,noexec,relatime"},
            {"tmpfs","/dev","tmpfs","rw,nosuid,size=65536k,mode=755"},{"shm","/dev/shm","tmpfs","rw,nosuid,nodev,noexec,relatime,size=65536k"},
            {"sysfs","/sys","sysfs","ro,nosuid,nodev,noexec,relatime"}};
        for(unsigned i=0;i<5+fs_mount_count && length<capacity-1200;i++) {
            if(i==3 && mem_mount_count && mem_mount_at("/dev/shm")>=0)continue;   /* listed as a memfs mount below */
            char point[1100];const char *device=i<5?base[i][0]:"/dev/vda1",*type=i<5?base[i][2]:"ext4";
            const char *options=i<5?base[i][3]:fs_mounts[i-5].readonly?"ro,relatime":"rw,relatime";
            snprintf(point,sizeof point,"%s%s",i<5 || fs_mounts[i-5].path[0]=='/'?"":"/",i<5?base[i][1]:fs_mounts[i-5].path);
            int n=mountinfo?snprintf(text+length,capacity-length,"%u %u 0:%u / %s %s - %s %s %s\n",20+i,i?20:1,20+i,point,options,type,device,options)
                      :snprintf(text+length,capacity-length,"%s %s %s %s 0 0\n",device,point,type,options);
            if(n>0)length+=(size_t)n;
        }
        for(unsigned i=0;i<mem_mount_count && length<capacity-1200;i++)length+=mem_mount_line(i,text+length,capacity-length,mountinfo,25+fs_mount_count+i);
    } else if(proc && !strcmp(rest,"cmdline") && proc_info(proc,tid,&info)) {
        memcpy(text,info.cmdline,info.size);length=info.size;
    } else if(proc && !strcmp(rest,"comm") && proc_info(proc,tid,&info)) {
        length=(size_t)snprintf(text,capacity,"%s\n",info.comm);
    } else if(!strcmp(path,"/proc/stat") || !strcmp(path,"/proc/uptime") || !strcmp(path,"/proc/loadavg")) {
        /* Boot is the container's start. Busy time is what its processes used; the
           rest of every CPU's time since boot is idle (kept from going backwards). */
        unsigned cpus=guest_cpus(),processes=0;uint64_t busy[2];proc_cpu_all(busy);
        for(uint64_t p=1;p<=process_count+1;p++)processes+=guest_process_exists(p)!=0;
        proc_peer_load();processes+=proc_npeers;
        static uint64_t idle_last;uint64_t idle=up/PROC_TICK_NS*cpus;idle=idle>busy[0]+busy[1]?idle-busy[0]-busy[1]:0;
        if(idle<idle_last)idle=idle_last;
        idle_last=idle;
        if(path[6]=='u')length=(size_t)snprintf(text,capacity,"%llu.%02llu %llu.%02llu\n",(unsigned long long)(up/1000000000u),
            (unsigned long long)(up%1000000000u/PROC_TICK_NS),(unsigned long long)(idle/100),(unsigned long long)(idle%100));
        else if(path[6]=='l') {
            double load[3];proc_load_sample(load);
            length=(size_t)snprintf(text,capacity,"%.2f %.2f %.2f %u/%u %llu\n",load[0],load[1],load[2],proc_running(),processes,(unsigned long long)process_pid);
        } else {
            length=(size_t)snprintf(text,capacity,"cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",(unsigned long long)busy[0],(unsigned long long)busy[1],(unsigned long long)idle);
            for(unsigned c=0;c<cpus && length<capacity-200;c++)
                length+=(size_t)snprintf(text+length,capacity-length,"cpu%u %llu 0 %llu %llu 0 0 0 0 0 0\n",c,(unsigned long long)(busy[0]/cpus),
                    (unsigned long long)(busy[1]/cpus),(unsigned long long)(idle/cpus));
            length+=(size_t)snprintf(text+length,capacity-length,"intr 0\nctxt 0\nbtime %llu\nprocesses %u\nprocs_running %u\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n",
                (unsigned long long)(boot/1000000000u),processes,proc_running());
        }
    } else if(fs_proc_sys_value(path)) {
        length=(size_t)snprintf(text,capacity,"%s",fs_proc_sys_value(path));
    } else if(!strcmp(path,"/proc/net/if_inet6")) {
        /* IPv6 is up on lo (nginx's entrypoint then also listens on [::]). */
        length=(size_t)snprintf(text,capacity,"00000000000000000000000000000001 01 80 10 80       lo\n");
    } else if(proc && !strcmp(rest,"cgroup")) {
        length=(size_t)snprintf(text,capacity,"0::/\n");                       /* cgroup v2, the container's root */
    } else if(!strcmp(path,"/sys/devices/system/cpu/online") || !strcmp(path,"/sys/devices/system/cpu/possible")) {
        unsigned cpus=guest_cpus();
        length=(size_t)(cpus>1?snprintf(text,capacity,"0-%u\n",cpus-1):snprintf(text,capacity,"0\n"));
    } else known=0;
    *out=length;return known;
}
/* An opened host file becomes the lowest free guest descriptor (a detached call's,
   later, under the lock: fs_call_detached). */
/* Host descriptors of files a guest opened read-only (and plainly) and closed, kept for
   the next such open of the same file: macOS's open costs about 10 us and admits about
   100k a second machine-wide (nginx opens index.html per request; a build reopens
   each source it hashed, the go command its cache's entries), a kept one costs an
   lseek. A descriptor names the inode, so it reads what a fresh open would (a file
   replaced by a rename is another inode, never matched), and each kept one is no one
   else's (fs_sole), so its offset and flags are as a fresh open's. */
#define FS_SPARES 256
static struct FsSpare { dev_t dev;ino_t ino;int fd; } fs_spares[FS_SPARES];
static pthread_mutex_t fs_spare_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned fs_spare_at(dev_t dev,ino_t ino) { return (unsigned)(((uint64_t)ino*0x9E3779B97F4A7C15u+(uint64_t)dev)>>40)&(FS_SPARES-1); }
static int fs_spare_take(dev_t dev,ino_t ino) {
    int fd=-1;unsigned at=fs_spare_at(dev,ino);
    pthread_mutex_lock(&fs_spare_lock);
    for(unsigned i=0;i<8 && fd<0;i++) { struct FsSpare *e=&fs_spares[(at+i)&(FS_SPARES-1)];if(e->fd>0 && e->ino==ino && e->dev==dev) { fd=e->fd-1;e->fd=0; } }
    pthread_mutex_unlock(&fs_spare_lock);
    return fd;
}
#ifdef VMFREE_HVF
static void fs_spare_put(int fd) {
    struct stat st;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || fcntl(fd,F_GETFL)!=(O_RDONLY|O_NONBLOCK) || lseek(fd,0,SEEK_SET)) { close(fd);return; }
    unsigned at=fs_spare_at(st.st_dev,st.st_ino);int old=-1;
    pthread_mutex_lock(&fs_spare_lock);
    struct FsSpare *e=&fs_spares[at];
    for(unsigned i=0;i<8;i++)if(!fs_spares[(at+i)&(FS_SPARES-1)].fd) { e=&fs_spares[(at+i)&(FS_SPARES-1)];break; }
    old=e->fd-1;*e=(struct FsSpare){st.st_dev,st.st_ino,fd+1};
    pthread_mutex_unlock(&fs_spare_lock);
    if(old>=0)close(old);
}
#endif
static uint64_t fs_install(int fd,uint64_t flags,int read_only,int device,GuestDirectory *directory) {
    PackOpen *pack=fs_install_pack;fs_install_pack=NULL;
    if(fs_detached.on) { fs_detached.fd=fd;fs_detached.flags=flags;fs_detached.opened_ro=read_only;fs_detached.opened_device=device;fs_detached.directory=directory;fs_detached.pack=pack;return 0; }
    int slot=fs_claim(),guest=slot>=0?fd_lowest(0):-1;
    if(guest<0) { if(pack)pack_unref(pack);else close(fd);fs_directory_release(directory);return linux_error(24); }
    FS(slot).fd=fd+1;FS(slot).cloexec=(flags&524288)!=0;FS(slot).ro=(unsigned char)read_only;FS(slot).dev=(unsigned char)device;FS(slot).dir=directory;
    FS(slot).sole=!(flags&(3|64|512|1024|4096|65536)) && !device && !directory && !pack;FS(slot).pack=pack;   /* opened read-only, plainly */
    FS(slot).guest=(unsigned short)guest;fd_bind((uint64_t)guest,FD_FILE,(unsigned)slot);return (uint64_t)guest;
}
static uint64_t fs_open(void) {
    uint64_t flags=cpu->gpr[2];
    /* O_NOCTTY (256), O_NONBLOCK (2048) and O_NOATIME (262144) have no effect on regular
       files or directories; glibc's opendir passes O_NONBLOCK. O_DIRECT (65536) is a
       caching hint (MySQL and InnoDB ask for it); O_DSYNC (4096) and O_SYNC (1052672)
       map to Darwin's. */
    const uint64_t allowed=3|64|128|256|512|1024|2048|4096|16384|32768|65536|131072|262144|524288|1052672;
    /* Like Linux, ignore bits open(2) does not define (MySQL passes a mode there).
       O_PATH (a descriptor for *at calls, fstat and close; MariaDB walks directories
       with it) opens read-only. ponytail: Linux needs no read permission for O_PATH
       and can open a symlink itself (with O_NOFOLLOW); here those fail as plain opens.
       O_TMPFILE is unsupported (callers fall back to a named file). */
    if((flags&UINT64_C(0x400000)) || (flags&3)==3) return linux_error(95);
    if(flags&UINT64_C(0x200000))flags&=UINT64_C(16384|32768|524288);              /* O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC */
    flags&=allowed;
    if(!fs_detached.on) {                                  /* detached: checked under the lock, installed there */
        unsigned slot=0;while(slot<FS_SLOTS && FS(slot).fd) slot++;
        if(slot==FS_SLOTS || fd_lowest(0)<0) return linux_error(24);
    }
    char path[1025];int named=fs_proc_path(cpu->gpr[0],cpu->gpr[1],path)==0;
    if(syscall_unlocked && syscall_abort)return linux_error(11);     /* relative to a /proc directory: the locked path retries */
    uint64_t tid=0;const char *rest="";uint64_t proc=named?proc_split(path,&tid,&rest):0;
    if(named && (!strcmp(path,"/proc") || !strcmp(path,"/proc/") || (proc && (!*rest || (!tid && (!strcmp(rest,"task") || !strcmp(rest,"fd"))))))) {
        /* /proc, /proc/PID, its task directory and a thread's: the container's processes and our files (ps, pgrep, top, htop). */
        if(flags&(3|64|512|1024))return linux_error(flags&64?17:21);
        static _Thread_local Scratch room;const size_t list_bytes=131072;    /* 24-byte entries: GUEST_FD_LIMIT descriptors, PROCESS_LIMIT processes */
        char *list=scratch_room(&room,list_bytes),name[24];size_t used=0;if(!list)return linux_error(12);
        used=fs_dirent(list,used,list_bytes,1,".",4);used=fs_dirent(list,used,list_bytes,1,"..",4);
        if(!proc) {
            for(unsigned i=0;i<sizeof(fs_proc_root_files)/sizeof(*fs_proc_root_files);i++)used=fs_dirent(list,used,list_bytes,2+i,fs_proc_root_files[i],8);
            used=fs_dirent(list,used,list_bytes,20,"self",10);
            proc_peer_load();
            for(unsigned i=0;i<proc_npeers;i++) { snprintf(name,sizeof(name),"%llu",(unsigned long long)proc_peers[i].pid);used=fs_dirent(list,used,list_bytes,100+proc_peers[i].pid,name,4); }
            for(uint64_t p=1;p<=process_count+1;p++)if(guest_process_exists(p)) { snprintf(name,sizeof(name),"%llu",(unsigned long long)p);used=fs_dirent(list,used,list_bytes,100+p,name,4); }
        } else if(!strcmp(rest,"fd")) {          /* fd: this process's descriptors (another's are not at hand: none) */
            for(unsigned fd=0;proc==process_pid && fd<GUEST_FD_LIMIT;fd++)if(fd_kind[fd]) { snprintf(name,sizeof(name),"%u",fd);used=fs_dirent(list,used,list_bytes,3000000+fd,name,10); }
        } else if(*rest) {                                                     /* task: the threads */
            static unsigned tids[1024];static char states[1024];static uint64_t times[1024][2];
            int64_t own=proc_resolve(proc);unsigned n=own>0?guest_thread_list((uint64_t)own,tids,states,times,1024):0;
            if(own<0) { tids[0]=(unsigned)proc;n=1; }
            for(unsigned i=0;i<n;i++) { snprintf(name,sizeof(name),"%u",tids[i]);used=fs_dirent(list,used,list_bytes,2000000+tids[i],name,4); }
        } else {
            uint64_t base=1000*(tid?tid:proc);
            for(unsigned i=0;i<sizeof(fs_proc_pid_files)/sizeof(*fs_proc_pid_files);i++)used=fs_dirent(list,used,list_bytes,base+i,fs_proc_pid_files[i],8);
            used=fs_dirent(list,used,list_bytes,base+99,"exe",10);
            if(!tid) { used=fs_dirent(list,used,list_bytes,base+98,"task",4);used=fs_dirent(list,used,list_bytes,base+97,"fd",4); }
        }
        return io_text_open(list,used,(flags&524288)!=0,1,path);
    }
    if(proc && !strcmp(rest,"maps")) {
        /* This process's mappings (another process's address space is not at hand: empty). */
        if(flags&(3|64|512|1024|16384))return linux_error(13);
        size_t capacity=1<<20;char *text=malloc(capacity);if(!text)return linux_error(12);
        size_t length=proc==process_pid?proc_maps(text,capacity):0;
        uint64_t result=io_text_open(text,length,(flags&524288)!=0,0,NULL);free(text);return result;
    }
    if(named) {
        static _Thread_local char text[16384];size_t length=0;
        if(proc_text(path,text,sizeof(text),&length)) {
            /* Synthetic read-only text, seekable like procfs (Java's mount table reader seeks). */
            if(flags&(3|64|512|1024|16384))return linux_error(13);
            return io_text_open(text,length,(flags&524288)!=0,0,path);
        }
    }
    /* Linux follows a final symlink unless O_NOFOLLOW (32768) or O_CREAT|O_EXCL. */
    int follow=!(flags&32768) && (flags&(64|128))!=(64|128);
    char leaf[1025],canon[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,follow|FS_DIRECT,canon);if(parent<0) return fs_error();
    int options=(flags&3)==2?O_RDWR:(flags&3)==1?O_WRONLY:O_RDONLY;
    const char *device=fs_device(canon);
    if(device) { /* Writable even on a read-only root; O_CREAT/O_TRUNC are no-ops. */
        close(parent);
        if(flags&16384)return linux_error(20);
        unsigned dev=!strcmp(canon,"/dev/null")?FS_DEV_NULL:!strcmp(canon,"/dev/zero")?FS_DEV_ZERO:1;
        /* /dev/null and /dev/zero: a copy of one host descriptor each (their reads and
           writes never reach it), not a host open per call (Go opens /dev/null three
           times for each process it starts). */
        static int kept[2]={-1,-1};int *own=dev>1?&kept[dev==FS_DEV_ZERO]:NULL,fd=-1;
        if(own && __atomic_load_n(own,__ATOMIC_ACQUIRE)<0) {
            int made=open(device,O_RDWR|O_CLOEXEC),none=-1;
            if(made>=0 && !__atomic_compare_exchange_n(own,&none,made,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))close(made);
        }
        if(own && __atomic_load_n(own,__ATOMIC_ACQUIRE)>=0)fd=fcntl(__atomic_load_n(own,__ATOMIC_ACQUIRE),F_DUPFD_CLOEXEC,0);
        else fd=open(device,options|O_CLOEXEC);
        if(fd<0)return fs_error();
        if(dev>1)dev|=(options!=O_WRONLY?FS_DEV_READ:0)|(options!=O_RDONLY?FS_DEV_WRITE:0);
        return fs_install(fd,flags,0,(int)dev,NULL);
    }
    int read_only=!fs_path_writable();
    if(read_only && (flags&(3|64|512|1024))) { close(parent);return linux_error(30); }
    options|=FS_NOFOLLOW_OPEN|O_CLOEXEC|O_NONBLOCK;
    if(flags&64) options|=O_CREAT;
    if(flags&128) options|=O_EXCL;
    if(flags&512) options|=O_TRUNC;
    if(flags&1024) options|=O_APPEND;
    if(flags&4096) options|=O_DSYNC;
    if((flags&1052672)==1052672) options|=O_SYNC;
    if(flags&16384) options|=O_DIRECTORY;
    OvLeaf layers=ov_leaf;int made=0;                       /* the root's image layers (overlay.h) */
    if(fs_lower>=0 && layers.canon[0]) {
        if((flags&(64|128))==(64|128) && layers.layer!=2) { close(parent);return linux_error(17); }
        made=layers.layer==2 && (flags&64);
        if((made || (flags&(3|512))) && ov_prepare(&layers,&parent,leaf,made?OV_CREATE|OV_DIRECT:OV_MODIFY)) { uint64_t e=fs_error();close(parent);return e; }
        if(layers.layer==1) { read_only=2;options&=~(O_CREAT|O_EXCL); }   /* a lower file, read: never written through */
    }
    if((flags&64) && fs_count_full()) { close(parent);return linux_error(28); }
    /* Validate the existing leaf before a possible truncate; only regular files/dirs.
       An unfollowed symlink reaches openat, which fails with ELOOP or EEXIST as on Linux. */
    struct stat before;int exists=fs_leaf_stat(parent,leaf,&before)==0;
    if(!exists && errno==ENOENT && !(flags&64)) { close(parent);return linux_error(2); }   /* missing, not made: no host open (gcc tries every include directory) */
    if(exists) {
        if(S_ISFIFO(before.st_mode)) { /* a named pipe: the container's shared pipe */
            close(parent);
            if((flags&(64|128))==(64|128))return linux_error(17);
            if(syscall_unlocked) { syscall_abort=1;return linux_error(11); }             /* pipe tables need the lock */
            return io_fifo_open(before.st_dev,before.st_ino,flags);
        }
        if(!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode) && !S_ISLNK(before.st_mode)) { close(parent);return linux_error(13); }
        if(S_ISLNK(before.st_mode) && (flags&16384) && !(flags&64)) { close(parent);return linux_error(20); } /* as Linux, not ELOOP */
    }
    if(exists && !(flags&(3|64|512|1024|4096|16384|65536)) && S_ISREG(before.st_mode) && (uint64_t)before.st_size<=fs_file_limit) {
        const PackEntry *packed=read_only==2?pack_find(&before,canon):NULL;   /* in the image's pack: no host open */
        PackOpen *o=packed?pack_open_new(packed,&before):NULL;
        if(o) { close(parent);fs_install_pack=o;return fs_install(pack_placeholder,flags,2,0,NULL); }
        int kept=fs_spare_take(before.st_dev,before.st_ino);   /* a file read again (the same inode): a kept descriptor */
        if(kept>=0) { close(parent);return fs_install(kept,flags,2,0,NULL); }
    }
    /* A leaf the walk found missing (bare): made exclusively, it is a new empty
       regular file and needs no fstat; one made meanwhile by another process is
       opened the usual way. */
    int fresh=made && layers.bare && (options&O_CREAT);mode_t mode=(mode_t)(cpu->gpr[3]&0777&~fs_umask);
    if(fresh && upm_fs && !fs_where && !(flags&(16384|0x200000|0x400000))) {   /* a new file of the writable layer: in memory (upm.h) */
        if(syscall_unlocked) { close(parent);syscall_abort=1;return linux_error(11); }   /* memfs descriptors: the locked path */
        int64_t r=upm_create(layers.canon,flags,cpu->gpr[3]);
        if(r!=UPM_DISK) { close(parent);if(r>=0)fs_count_created();return r<0?linux_error((unsigned)-r):(uint64_t)r; }
    }
    int fd=openat(parent,leaf,options|(fresh?O_EXCL:0),mode);
    if(fd<0 && fresh && errno==EEXIST && !(flags&128)) { fresh=0;fd=openat(parent,leaf,options,mode); }
    if(fd<0) { uint64_t e=fs_error();close(parent);return e; }
    struct stat info;
    if(!fresh && (fstat(fd,&info)<0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || (uint64_t)info.st_size>fs_file_limit)) {
        close(fd);close(parent);return linux_error(13);
    }
    GuestDirectory *directory=NULL;
    if(!fresh && S_ISDIR(info.st_mode)) {
        directory=calloc(1,sizeof(*directory));if(!directory) { close(fd);close(parent);return linux_error(12); }
        directory->refs=1;strcpy(directory->path,canon);directory->lower=layers.layer==1 && layers.canon[0];
    }
    if(flags&64) { fs_count_created(); }
    if(made)ov_created(&layers,parent,leaf,0);
    close(parent);
    return fs_install(fd,flags,read_only,0,directory);
}
static void fs_pack(unsigned char *p,unsigned offset,uint64_t value,unsigned size) {
    for(unsigned i=0;i<size;i++) p[offset+i]=(unsigned char)(value>>(i*8));
}
static void fs_stat_pack(unsigned char *out,const struct stat *st) {
    memset(out,0,128);fs_pack(out,8,st->st_ino,8);
    fs_pack(out,16,(S_ISDIR(st->st_mode)?0040000:S_ISLNK(st->st_mode)?0120000:S_ISCHR(st->st_mode)?0020000:S_ISSOCK(st->st_mode)?0140000:S_ISFIFO(st->st_mode)?0010000:0100000)|(st->st_mode&07777),4); /* with setuid, setgid and sticky */
    fs_pack(out,24,guest_uid,4);fs_pack(out,28,guest_gid,4);
    fs_pack(out,20,st->st_nlink?st->st_nlink:1,4);fs_pack(out,48,(uint64_t)st->st_size,8);
    fs_pack(out,56,4096,4);fs_pack(out,64,((uint64_t)st->st_size+511)/512,8);
    /* Linux arm64 st_atime, st_mtime and st_ctime, each seconds then nanoseconds. */
    const struct timespec *times[3]={&st->st_atimespec,&st->st_mtimespec,&st->st_ctimespec};
    for(unsigned i=0;i<3;i++) { fs_pack(out,72+i*16,(uint64_t)times[i]->tv_sec,8);fs_pack(out,80+i*16,(uint64_t)times[i]->tv_nsec,8); }
}
/* One read/write of at most 1 MiB at the file position, or at offset (pread/pwrite) when it is >= 0. */
/* mapping.h: a write to a file also updates its live MAP_SHARED mappings (as Linux's
   page cache does; bbolt reads its pages through one and writes them with pwrite). */
static unsigned file_shares_live;
static void file_share_written(int fd,uint64_t at,const unsigned char *data,size_t bytes);
static uint64_t fs_transfer_chunk(int fd,uint64_t call,int64_t offset) {
        uint64_t count=cpu->gpr[2]>1024*1024?1024*1024:cpu->gpr[2];
        unsigned char *buffer=count?fs_memory(cpu->gpr[1],count,call==63?2:4):(unsigned char *)"";
        if(!buffer) return linux_error(14);
        unsigned dev=(unsigned)fs_is_device(fd);
        if(dev>1) { /* /dev/null and /dev/zero: their whole behaviour, without a host call */
            if(!(dev&(call==63?FS_DEV_READ:FS_DEV_WRITE)))return linux_error(9);
            if(call==64)return count;
            if((dev&3)==FS_DEV_ZERO)memset(buffer,0,(size_t)count);
            return (dev&3)==FS_DEV_ZERO?count:0;
        }
        if(dev) { /* No file size, storage or write budget to account. */
            ssize_t result=offset>=0?(call==63?pread(fd,buffer,(size_t)count,offset):pwrite(fd,buffer,(size_t)count,offset))
                                    :(call==63?read(fd,buffer,(size_t)count):write(fd,buffer,(size_t)count));
            return result<0?fs_error():(uint64_t)result;
        }
        uint64_t previous_size=0,write_position=0;
        if(call==64) {
            int slot=fs_detached.on?-1:fs_slot(cpu->gpr[0]); /* a file slot knows its location; stdio files follow the root */
            if(fs_detached.on?fs_detached.read_only:slot>=0?FS(slot).ro:!fs_writable) return linux_error(30);
            /* The size only for an append or the storage budget (fstat was a third of a
               small write's host time); the engine sets no storage budget. */
            int append=offset<0 && (fcntl(fd,F_GETFL)&O_APPEND);
            /* An append's position matters only to a 1 TiB file limit and to mappings of
               the file: dockivad's runs (Docker: no limit) skip that fstat, which 12 nginx
               workers logging to one file queue on. */
            int unsized=append && !fs_storage_limit && fs_file_limit>=(UINT64_C(1)<<40) && !__atomic_load_n(&file_shares_live,__ATOMIC_RELAXED);
            struct stat st;st.st_size=0;if((append || fs_storage_limit) && !unsized && fstat(fd,&st)<0) return fs_error();
            off_t position=offset>=0?(off_t)offset:append?st.st_size:lseek(fd,0,SEEK_CUR);if(position<0) return fs_error();
            if((uint64_t)position>fs_file_limit || count>fs_file_limit-(uint64_t)position || (fs_write_limit && count>fs_write_limit-fs_written)) return linux_error(27);
            previous_size=(uint64_t)st.st_size;write_position=(uint64_t)position;
            uint64_t end=write_position+count;
            if(fs_storage_limit && end>previous_size && end-previous_size>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        }
        ssize_t result=offset>=0?(call==63?pread(fd,buffer,(size_t)count,offset):pwrite(fd,buffer,(size_t)count,offset))
                                :(call==63?read(fd,buffer,(size_t)count):write(fd,buffer,(size_t)count));
        if(result<0) return fs_error();
        if(call==64) { /* atomic: file writes also run without the runtime lock */
            if(__atomic_load_n(&file_shares_live,__ATOMIC_RELAXED))file_share_written(fd,write_position,buffer,(size_t)result);
            __atomic_fetch_add(&fs_written,(uint64_t)result,__ATOMIC_RELAXED);uint64_t end=write_position+(uint64_t)result;
            if(fs_storage_limit && end>previous_size)__atomic_fetch_add(&fs_storage_bytes,end-previous_size,__ATOMIC_RELAXED);
        }
        return (uint64_t)result;
}
/* Regular files never transfer short on Linux (InnoDB warns when they do): larger
   requests move 1 MiB at a time. Devices take one chunk, as they may block. */
static uint64_t fs_transfer_at(int fd,uint64_t call,int64_t offset) {
    uint64_t base=cpu->gpr[1],count=cpu->gpr[2],done=0;
    for(;;) {
        cpu->gpr[1]=base+done;cpu->gpr[2]=count-done;
        uint64_t result=fs_transfer_chunk(fd,call,offset<0?offset:offset+(int64_t)done);
        if((int64_t)result<0) { if(!done)done=result;break; }
        done+=result;
        if(!result || result<1024*1024 || done==count || fs_is_device(fd))break;
    }
    cpu->gpr[1]=base;cpu->gpr[2]=count;return done;
}
static uint64_t fs_transfer(int fd,uint64_t call) { return fs_transfer_at(fd,call,-1); }
/* sendfile and copy_file_range between regular files (libuv's fs.copyFile, coreutils
   cp): through the host, at most 1 MiB per call (callers loop, as on Linux), with
   write(2)'s limits and accounting. in_at, out_at: guest offsets to use and update,
   or 0 for the descriptors' positions. */
static uint64_t fs_copy_between(uint64_t in_guest,uint64_t out_guest,uint64_t in_at,uint64_t out_at,uint64_t count) {
    int in=fs_host(in_guest),out=fs_host(out_guest),in_slot=fs_slot(in_guest),out_slot=fs_slot(out_guest);
    /* An open pipe or socket: EINVAL, as Linux, on which callers (Go's io.Copy) read and write. */
    if(in<0 || out<0)return in_guest<GUEST_FD_LIMIT && out_guest<GUEST_FD_LIMIT && fd_kind[in_guest] && fd_kind[out_guest]?linux_error(22):linux_error(9);
    if(FS(in_slot).dir || FS(out_slot).dir)return linux_error(21);
    if(FS(out_slot).ro)return linux_error(30);
    unsigned char *pin=in_at?lookup(in_at,8,6):NULL,*pout=out_at?lookup(out_at,8,6):NULL;
    if((in_at && !pin) || (out_at && !pout))return linux_error(14);
    off_t from=pin?(off_t)le(pin,8):lseek(in,0,SEEK_CUR);if(from<0)return pin?linux_error(22):fs_error();
    struct stat st;if(fstat(out,&st)<0)return fs_error();
    int append=(fcntl(out,F_GETFL)&O_APPEND)!=0;
    off_t to=pout?(off_t)le(pout,8):append?st.st_size:lseek(out,0,SEEK_CUR);if(to<0)return pout?linux_error(22):fs_error();
    if(count>1024*1024)count=1024*1024;
    if(!count)return 0;
    if((uint64_t)to>fs_file_limit || count>fs_file_limit-(uint64_t)to || (fs_write_limit && count>fs_write_limit-fs_written))return linux_error(27);
    if(fs_storage_limit && (uint64_t)to+count>(uint64_t)st.st_size && (uint64_t)to+count-(uint64_t)st.st_size>fs_storage_limit-fs_storage_bytes)return linux_error(28);
    unsigned char *buffer=malloc((size_t)count);if(!buffer)return linux_error(12);
    ssize_t got=pread(in,buffer,(size_t)count,from),put=got>0?pwrite(out,buffer,(size_t)got,to):got;
    if(put>0 && __atomic_load_n(&file_shares_live,__ATOMIC_RELAXED))file_share_written(out,(uint64_t)to,buffer,(size_t)put);
    int saved=errno;free(buffer);errno=saved;
    if(got<0 || put<0)return fs_error();
    __atomic_fetch_add(&fs_written,(uint64_t)put,__ATOMIC_RELAXED);
    if((uint64_t)(to+put)>(uint64_t)st.st_size)__atomic_fetch_add(&fs_storage_bytes,(uint64_t)(to+put)-(uint64_t)st.st_size,__ATOMIC_RELAXED);
    if(pin)fs_pack(pin,0,(uint64_t)(from+put),8);else lseek(in,from+put,SEEK_SET);
    if(pout)fs_pack(pout,0,(uint64_t)(to+put),8);else if(!append)lseek(out,to+put,SEEK_SET);
    return (uint64_t)put;
}
static uint64_t io_epoch; /* io.h: changes wake blocked tasks */
/* POSIX record locks (fcntl F_GETLK/F_SETLK/F_SETLKW, and the OFD variants as the
   same), kept here: every guest process shares this host process, whose own locks
   would never conflict. end is exclusive; INT64_MAX reaches the end of the file. */
#define FS_LOCKS 1024
static struct FsLock { dev_t dev;ino_t ino;uint64_t owner;int64_t start,end;unsigned type,used,pid; } fs_locks[FS_LOCKS];
/* flock(2) locks live in the same table under owners with this bit (the open file
   description); they never conflict with fcntl locks, as on Linux. */
#define FS_FLOCK_OWNER (UINT64_C(1)<<63)
static int fs_lock_conflict(dev_t dev,ino_t ino,uint64_t owner,int64_t start,int64_t end,unsigned type) {
    for(int i=0;i<FS_LOCKS;i++) {
        struct FsLock *l=&fs_locks[i];
        if(l->used && l->dev==dev && l->ino==ino && l->owner!=owner && ((l->owner^owner)&FS_FLOCK_OWNER)==0 && l->start<end && start<l->end && (type==1 || l->type==1))return i;
    }
    return -1;
}
/* Live locks, in all and by the process that took them: a close in a process holding
   none has none to release (it runs without the runtime lock). The go command locks
   its build cache files, then its compilers close thousands of files. Changed under
   the runtime lock and the taking process's mm lock, which its own closes hold. */
static int fs_locks_taken;
#define fs_locks_held (proc_block->locks_held)
static void fs_lock_drop(int i) {
    fs_locks[i].used=0;__atomic_fetch_sub(&fs_locks_taken,1,__ATOMIC_RELEASE);
    __atomic_fetch_sub(&fs_locks_held[fs_locks[i].pid],1,__ATOMIC_RELEASE);
}
static int fs_lock_add(dev_t dev,ino_t ino,uint64_t owner,int64_t start,int64_t end,unsigned type) {
    for(int i=0;i<FS_LOCKS;i++)if(!fs_locks[i].used) {
        unsigned pid=(unsigned)process_pid_now();
        fs_locks[i]=(struct FsLock){dev,ino,owner,start,end,type,1,pid};
        __atomic_fetch_add(&fs_locks_taken,1,__ATOMIC_RELEASE);__atomic_fetch_add(&fs_locks_held[pid],1,__ATOMIC_RELEASE);
        return 0;
    }
    return -1;
}
/* Replace the owner's locks over [start,end) with type (2: unlock), splitting partial ones. */
static uint64_t fs_lock_set(dev_t dev,ino_t ino,uint64_t owner,int64_t start,int64_t end,unsigned type) {
    for(int i=0;i<FS_LOCKS;i++) {
        struct FsLock l=fs_locks[i];
        if(!l.used || l.dev!=dev || l.ino!=ino || l.owner!=owner || l.end<=start || end<=l.start)continue;
        fs_lock_drop(i);
        if(l.start<start && fs_lock_add(dev,ino,owner,l.start,start,l.type))return linux_error(37);
        if(end<l.end && fs_lock_add(dev,ino,owner,end,l.end,l.type))return linux_error(37);
    }
    io_epoch++; /* waiters (F_SETLKW) retry */
    return type==2 || !fs_lock_add(dev,ino,owner,start,end,type)?0:linux_error(37); /* ENOLCK */
}
/* A process closed a descriptor of the file (or exited, dev and ino 0): POSIX drops its locks. */
static void fs_locks_release(dev_t dev,ino_t ino,uint64_t owner) {
    for(int i=0;i<FS_LOCKS;i++)if(fs_locks[i].used && fs_locks[i].owner==owner && ((!dev && !ino) || (fs_locks[i].dev==dev && fs_locks[i].ino==ino))) { fs_lock_drop(i);io_epoch++; }
}
static uint64_t fs_fcntl_lock(int fd,uint64_t command) {
    int get=command==5 || command==36,wait=command==7 || command==38;
    unsigned char *p=lookup(cpu->gpr[2],32,get?2:4);if(!p || (get && !lookup(cpu->gpr[2],32,4)))return linux_error(14);
    unsigned type=(unsigned)le(p,2),whence=(unsigned)le(p+2,2);int64_t start=(int64_t)le(p+8,8),length=(int64_t)le(p+16,8);
    if(type>2 || whence>2)return linux_error(22);
    struct stat st;if(fstat(fd,&st)<0)return fs_error();
    int64_t base=whence==1?(int64_t)lseek(fd,0,SEEK_CUR):whence==2?(int64_t)st.st_size:0;
    if(base<0)return fs_error();
    int64_t s=base+start,e=length>0?s+length:length==0?INT64_MAX:s;
    if(length<0)s+=length;
    if(s<0 || e<s)return linux_error(22);
    uint64_t owner=process_pid_now();
    int conflict=fs_lock_conflict(st.st_dev,st.st_ino,owner,s,e,type==2?0:type);
    if(get) {
        if(conflict<0 || type==2) { fs_pack(p,0,2,2);return 0; }
        struct FsLock *l=&fs_locks[conflict];
        fs_pack(p,0,l->type,2);fs_pack(p,2,0,2);fs_pack(p,8,(uint64_t)l->start,8);
        fs_pack(p,16,l->end==INT64_MAX?0:(uint64_t)(l->end-l->start),8);fs_pack(p,24,(uint32_t)l->owner,4);
        return 0;
    }
    if(type!=2 && conflict>=0)return wait && schedule_active?IO_BLOCK:linux_error(11);
    return fs_lock_set(st.st_dev,st.st_ino,owner,s,e,type);
}
/* flock(fd, LOCK_SH 1 | LOCK_EX 2 | LOCK_UN 8, LOCK_NB 4): a whole-file lock held by
   the open file description (its host descriptor here). */
static uint64_t fs_flock(int fd) {
    uint64_t operation=cpu->gpr[1],kind=operation&~UINT64_C(4);
    if(operation&~UINT64_C(15) || (kind!=1 && kind!=2 && kind!=8))return linux_error(22);
    struct stat st;if(fstat(fd,&st)<0)return fs_error();
    uint64_t owner=FS_FLOCK_OWNER|(uint64_t)fd;unsigned type=kind==1?0:kind==2?1:2;
    if(type!=2 && fs_lock_conflict(st.st_dev,st.st_ino,owner,0,INT64_MAX,type)>=0)
        return (operation&4) || !schedule_active?linux_error(11):IO_BLOCK;
    return fs_lock_set(st.st_dev,st.st_ino,owner,0,INT64_MAX,type);
}
static uint64_t fs_call(uint64_t call) {
    if(call==166) { unsigned previous=fs_umask;fs_umask=(unsigned)cpu->gpr[0]&0777;return previous; }
    if(call==52 || call==53 || call==452) {
        uint64_t mode=cpu->gpr[call==52?1:2]&UINT64_C(07777); /* Linux ignores other bits (libuv passes st_mode); setuid/setgid/sticky allowed */
        uint64_t flags=call==452?cpu->gpr[3]:0;if(flags&~UINT64_C(0x1100))return linux_error(22);
        /* fchmodat2(fd, "", mode, AT_EMPTY_PATH) changes the descriptor's file, as fchmod:
           systemd's fchmod_opath (sysusers on its new /etc/.#group* file). ponytail: with
           AT_FDCWD it is EBADF (Linux changes the working directory); none seen asks. */
        if(flags&0x1000) { const unsigned char *first=fs_memory(cpu->gpr[1],1,4);if(first && !*first)call=52; }
        int fd=fs_host(cpu->gpr[0]),own=-1;
        if(call==52 && fs_slot(cpu->gpr[0])>=0 && FS(fs_slot(cpu->gpr[0])).ro && (fd=own=ov_fd_upper(fs_slot(cpu->gpr[0])))<0)return linux_error(30);
        if(call!=52) {
            /* By path, never opening the file (an open costs ~12 us and needs read permission).
               fchmodat2 (452): AT_SYMLINK_NOFOLLOW on a symlink is EOPNOTSUPP, as Linux; glibc
               and Go call it first. */
            char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,(flags&0x100?0:1)|FS_DIRECT,NULL);if(parent<0)return fs_error();
            if(!fs_path_writable()) { close(parent);return linux_error(30); }
            if(flags&0x100) { struct stat link;if(!fstatat(parent,leaf,&link,FS_NOFOLLOW_AT) && S_ISLNK(link.st_mode)) { close(parent);return linux_error(95); } } /* before a copy-up */
            { OvLeaf w=ov_leaf;if(ov_prepare(&w,&parent,leaf,OV_MODIFY)) { uint64_t e=fs_error();close(parent);return e; } }
            struct stat st;int result=fs_leaf_stat(parent,leaf,&st);
            if(!result && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISSOCK(st.st_mode)) { errno=EACCES;result=-1; }
            if(!result)result=fchmodat(parent,leaf,(mode_t)mode,FS_NOFOLLOW_AT);
            int saved=errno;close(parent);errno=saved;return result<0?fs_error():0;
        } else if(fd<0)return linux_error(9);
        struct stat st;int result=fstat(fd,&st);
        if(!result && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { errno=EACCES;result=-1; }
        if(!result)result=fchmod(fd,(mode_t)mode);
        if(own>=0) { int saved=errno;close(own);errno=saved; }
        return result<0?fs_error():0;
    }
    if(call==78) {
        if(!cpu->gpr[3] || (int64_t)(int32_t)cpu->gpr[3]<=0)return linux_error(22);
        if(cpu->gpr[3]>4096)cpu->gpr[3]=4096;                     /* any buffer size, as on Linux (libc++ passes more) */
        unsigned char *out=fs_memory(cpu->gpr[2],cpu->gpr[3],2);if(!out)return linux_error(14);
        char path[1025],exe[1025];
        if(fs_proc_path(cpu->gpr[0],cpu->gpr[1],path)==0 && fs_proc_exe(path,exe)) { /* readlink /proc/<pid>/exe */
            size_t n=strlen(exe);if(n>cpu->gpr[3])n=(size_t)cpu->gpr[3];memcpy(out,exe,n);return n;
        }
        if(!strcmp(path,"/proc/self") || !strcmp(path,"/proc/thread-self")) {          /* the caller's pid */
            char pid[24];size_t n=(size_t)snprintf(pid,sizeof pid,"%llu",(unsigned long long)process_pid);
            if(n>cpu->gpr[3])n=(size_t)cpu->gpr[3];memcpy(out,pid,n);return n;
        }
        if(fs_proc_kind(path))return linux_error(22);  /* our /proc files and directories are not links (glibc's realpath asks) */
        /* Most paths asked are not links (gcc's realpath asks of every component of every
           header): the direct walk's lstat answers them with no directory opened. */
        char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,FS_DIRECT,NULL);if(parent<0)return fs_error();
        struct stat st;int known=fs_leaf_stat(parent,leaf,&st),leaf_errno=errno;close(parent);
        if(known<0) { errno=leaf_errno;return fs_error(); }
        if(!S_ISLNK(st.st_mode))return linux_error(22);
        parent=fs_parent_nofollow(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0)return fs_error();
        /* Read the final link's text; ancestors are resolved inside the root. */
        ssize_t result=readlinkat(parent,leaf,(char *)out,(size_t)cpu->gpr[3]);
        int saved=errno;close(parent);errno=saved;return result<0?fs_error():(uint64_t)result;
    }
    if(call==36) { /* symlinkat(target, dirfd, path): the target is kept verbatim and resolved inside the root */
        char target[1025];if(fs_string(cpu->gpr[0],target)<0)return fs_error();
        if(fs_count_full())return linux_error(28);
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[1],cpu->gpr[2],leaf);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        if(!strcmp(leaf,".")) { close(parent);return linux_error(17); }
        OvLeaf w=ov_leaf;if(ov_prepare(&w,&parent,leaf,OV_CREATE)) { uint64_t e=fs_error();close(parent);return e; }
        int result=symlinkat(target,parent,leaf);int saved=errno;
        if(!result)ov_created(&w,parent,leaf,0);
        close(parent);errno=saved;
        if(result<0)return fs_error();
        fs_count_created();return 0;
    }
    if(call==37) { /* linkat(olddirfd, old, newdirfd, new, flags): a hard link within one mount */
        uint64_t flags=cpu->gpr[4];if(flags&~UINT64_C(0x400))return linux_error(22);   /* AT_SYMLINK_FOLLOW */
        if(fs_count_full())return linux_error(28);
        char from[1025];int source=fs_lookup(cpu->gpr[0],cpu->gpr[1],from,(flags&0x400)!=0,NULL);if(source<0)return fs_error();
        int where=fs_where;OvLeaf old=ov_leaf;
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[2],cpu->gpr[3],leaf);if(parent<0) { uint64_t e=fs_error();close(source);return e; }
        uint64_t error=0;OvLeaf w=ov_leaf;
        if(!fs_path_writable())error=linux_error(30);
        else if(where!=fs_where || where<0)error=linux_error(18);   /* EXDEV: never across a mount boundary */
        else if(!strcmp(from,".") || !strcmp(leaf,"."))error=linux_error(!strcmp(leaf,".")?17:1);
        else if(old.layer==2)error=linux_error(2);
        else if(ov_prepare(&w,&parent,leaf,OV_CREATE) || ov_prepare(&old,&source,from,OV_MODIFY))error=fs_error();  /* image layers: a lower file is copied up */
        else if(linkat(source,from,parent,leaf,0)<0)error=fs_error();
        else ov_created(&w,parent,leaf,0);
        close(parent);close(source);
        if(!error) { fs_count_created(); }
        return error;
    }
    if(call==88) { /* utimensat(dirfd, path, times, flags); a NULL path acts on dirfd itself (futimens) */
        uint64_t flags=cpu->gpr[3];if(flags&~UINT64_C(0x100))return linux_error(22); /* AT_SYMLINK_NOFOLLOW */
        const unsigned char *in=cpu->gpr[2]?fs_memory(cpu->gpr[2],32,4):NULL;if(cpu->gpr[2] && !in)return linux_error(14);
        struct timespec times[2];
        for(unsigned i=0;i<2;i++) {                    /* Linux UTIME_NOW/UTIME_OMIT differ from the host's */
            uint64_t sec=in?le(in+i*16,8):0,nsec=in?le(in+i*16+8,8):(UINT64_C(1)<<30)-1;
            times[i].tv_sec=(time_t)sec;
            if(nsec==(UINT64_C(1)<<30)-1)times[i].tv_nsec=UTIME_NOW;
            else if(nsec==(UINT64_C(1)<<30)-2)times[i].tv_nsec=UTIME_OMIT;
            else if(nsec<1000000000)times[i].tv_nsec=(long)nsec;
            else return linux_error(22);
        }
        if(!cpu->gpr[1]) {
            int fd=fs_host(cpu->gpr[0]),own=-1;if(fd<0)return linux_error(9);
            if(FS(fs_slot(cpu->gpr[0])).ro && (fd=own=ov_fd_upper(fs_slot(cpu->gpr[0])))<0)return linux_error(30);
            int result=futimens(fd,times);if(own>=0) { int saved=errno;close(own);errno=saved; }
            return result<0?fs_error():0;
        }
        char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(flags&0x100)|FS_DIRECT,NULL);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        { OvLeaf w=ov_leaf;if(ov_prepare(&w,&parent,leaf,OV_MODIFY)) { uint64_t e=fs_error();close(parent);return e; } }
        /* The leaf is never followed by the host: a final symlink is followed by fs_lookup already. */
        int result=utimensat(parent,leaf,times,FS_NOFOLLOW_AT);int saved=errno;close(parent);errno=saved;
        return result<0?fs_error():0;
    }
    if(call==45) { /* truncate(path, length): ftruncate of the resolved regular file */
        uint64_t length=cpu->gpr[1];
        if((int64_t)length<0)return linux_error(22);
        if(length>fs_file_limit)return linux_error(27);
        char leaf[1025];int parent=fs_lookup((uint64_t)-100,cpu->gpr[0],leaf,1|FS_DIRECT,NULL);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        { OvLeaf w=ov_leaf;if(ov_prepare(&w,&parent,leaf,OV_MODIFY)) { uint64_t e=fs_error();close(parent);return e; } }
        int fd=openat(parent,leaf,O_WRONLY|FS_NOFOLLOW_OPEN|O_NONBLOCK|O_CLOEXEC);int saved=errno;close(parent);errno=saved;
        if(fd<0)return fs_error();
        struct stat st;if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode)) { saved=S_ISDIR(st.st_mode)?EISDIR:EINVAL;close(fd);errno=saved;return fs_error(); }
        uint64_t growth=length>(uint64_t)st.st_size?length-(uint64_t)st.st_size:0;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes) { close(fd);return linux_error(28); }
        int result=ftruncate(fd,(off_t)length);saved=errno;close(fd);errno=saved;
        if(result<0)return fs_error();
        __atomic_fetch_add(&fs_storage_bytes,growth,__ATOMIC_RELAXED);return 0;
    }
    if(call==48 || call==439) { /* faccessat(2); synthetic uid 0; no-link namespace policy still applies. */
        { char proc_path[1025];if(fs_proc_path(cpu->gpr[0],cpu->gpr[1],proc_path)==0 && fs_proc_kind(proc_path))return (cpu->gpr[2]&2)?linux_error(13):0; }
        if(cpu->gpr[2]&~UINT64_C(7) || (call==439 && (cpu->gpr[3]&~UINT64_C(0x300))))return linux_error(22); /* AT_EACCESS, AT_SYMLINK_NOFOLLOW */
        char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,1|FS_DIRECT,NULL);if(parent<0)return fs_error();
        int writable=fs_path_writable();
        struct stat st;int result=fs_leaf_stat(parent,leaf,&st);
        int saved=errno;close(parent);errno=saved;if(result<0)return fs_error();
        if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode))return linux_error(13);
        if((cpu->gpr[2]&2) && !writable)return linux_error(30);
        if(guest_uid && (cpu->gpr[2]&~((unsigned)st.st_mode>>6)))return linux_error(13);
        if((cpu->gpr[2]&1) && S_ISREG(st.st_mode) && !(st.st_mode&0111))return linux_error(13);
        return 0;
    }
    if(call==32) { int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);return fs_flock(fd); }
    if(call==82 || call==83) {
        int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
        return fs_sync(fd)<0?fs_error():0;                                 /* fdatasync as fsync */
    }
    if(call==79) {
        if(cpu->gpr[3]&~(UINT64_C(256)|2048|4096)) return linux_error(95);
        unsigned char *out=fs_memory(cpu->gpr[2],128,2);if(!out)return linux_error(14);
        unsigned char *first=fs_memory(cpu->gpr[1],1,4);if(!first)return linux_error(14);
        if(!*first) {
            if(!(cpu->gpr[3]&4096))return linux_error(2);
            int fd=(int32_t)cpu->gpr[0]==-100?(fs_cwd?fs_cwd->fd:fs_root):fs_host(cpu->gpr[0]);
            if(fd<0)return linux_error(9);
            struct stat st;if(fstat(fd,&st)<0)return fs_error();
            fs_stat_pack(out,&st);return 0;
        }
        {   /* /proc paths of ours: directories and empty read-only files, as procfs shows them */
            char proc_path[1025];int kind=fs_proc_path(cpu->gpr[0],cpu->gpr[1],proc_path)==0?fs_proc_kind(proc_path):0;
            if(syscall_unlocked && syscall_abort)return linux_error(11);   /* relative to a /proc directory: the locked path retries */
            char exe[1025];
            if(!kind && (cpu->gpr[3]&256) && fs_proc_path(cpu->gpr[0],cpu->gpr[1],proc_path)==0 && fs_proc_exe(proc_path,exe)) {
                /* lstat /proc/<pid>/exe: the link itself (libstdc++'s read_symlink, which FEXServer uses, checks) */
                struct stat st;memset(&st,0,sizeof(st));st.st_mode=S_IFLNK|0777;st.st_nlink=1;st.st_size=(off_t)strlen(exe);
                fs_stat_pack(out,&st);return 0;
            }
            if(kind) {
                struct stat st;memset(&st,0,sizeof(st));st.st_mode=kind==1?S_IFDIR|0555:S_IFREG|0444;st.st_nlink=kind==1?2:1;st.st_blksize=4096;
                fs_stat_pack(out,&st);
                /* Owned by the process's user (ps's USER column). */
                uint64_t tid;const char *rest;uint64_t pid=proc_split(proc_path,&tid,&rest);ProcInfo info;
                if(pid && proc_info(pid,0,&info)) { fs_pack(out,24,info.uid[1],4);fs_pack(out,28,info.gid[1],4); }
                return 0;
            }
        }
        /* AT_SYMLINK_NOFOLLOW (256) reports a final symlink itself. */
        char leaf[1025],canon[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(cpu->gpr[3]&256)|FS_DIRECT,canon);if(parent<0)return fs_error();
        struct stat st;const char *device=fs_device(canon);
        int result=device?stat(device,&st):fs_leaf_stat(parent,leaf,&st);
        int saved=errno;close(parent);errno=saved;if(result<0)return fs_error();
        if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && !S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode) && !device)return linux_error(13);
        fs_stat_pack(out,&st);return 0;
    }
    if(call==56) return fs_open();
    if(call==437) {
        /* openat2(dirfd, path, how, size): openat with how's flags and mode, and RESOLVE_*
           scoping the walk (FEX opens its x86 root's files with RESOLVE_IN_ROOT).
           ponytail: NO_XDEV and NO_MAGICLINKS are accepted unenforced (bind mounts and
           /proc/self/fd are this runtime's only crossings and magic links). */
        uint64_t size=cpu->gpr[3];if(size<24 || size>4096)return linux_error(22);
        const unsigned char *how=lookup(cpu->gpr[2],size,4);if(!how)return linux_error(14);
        for(uint64_t i=24;i<size;i++)if(how[i])return linux_error(7);           /* E2BIG: fields not known here */
        uint64_t flags=le(how,8),mode=le(how+8,8),resolve=le(how+16,8);
        if((resolve&~UINT64_C(0x3f)) || (flags>>32) || mode>07777 || (mode && !(flags&UINT64_C(0x400040))) || (resolve&0x18)==0x18)return linux_error(22);
        if(resolve&0x20)return linux_error(11);                 /* RESOLVE_CACHED: no cache to answer from; callers retry */
        if(resolve&0x18) {
            int dir=(int32_t)cpu->gpr[0]==-100?(fs_cwd?fs_cwd->fd:fs_root):fs_host(cpu->gpr[0]);if(dir<0)return linux_error(9);
            fs_scope.root=dir;fs_scope.path=(int32_t)cpu->gpr[0]==-100?(fs_cwd?fs_cwd->path:"/"):fs_slot(cpu->gpr[0])>=0 && FS(fs_slot(cpu->gpr[0])).dir?FS(fs_slot(cpu->gpr[0])).dir->path:"";
        }
        fs_scope.resolve=(unsigned)resolve;
        uint64_t keep2=cpu->gpr[2],keep3=cpu->gpr[3];cpu->gpr[2]=flags;cpu->gpr[3]=mode;
        uint64_t result=fs_open();
        cpu->gpr[2]=keep2;cpu->gpr[3]=keep3;fs_scope.root=-1;fs_scope.path="";fs_scope.resolve=0;
        return result;
    }
    if(call==33) { /* mknodat: FIFOs (mkfifo) and regular files; devices need privileges we do not grant */
        unsigned type=(unsigned)(cpu->gpr[2]&0170000);mode_t mode=(mode_t)(cpu->gpr[2]&0777&~fs_umask);
        if(type!=0010000 && type!=0100000 && type)return linux_error(1);
        if(fs_count_full())return linux_error(28);
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        OvLeaf w=ov_leaf;if(ov_prepare(&w,&parent,leaf,OV_CREATE)) { uint64_t e=fs_error();close(parent);return e; }
        int result;
        if(type==0010000)result=mkfifoat(parent,leaf,mode);
        else { int fd=openat(parent,leaf,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,mode);result=fd<0?-1:close(fd); }
        uint64_t e=result<0?fs_error():0;if(!e)ov_created(&w,parent,leaf,0);close(parent);if(!e)fs_count_created();return e;
    }
    if(call==34 || call==35 || call==38) {
        if(call==35 && cpu->gpr[2]!=0 && cpu->gpr[2]!=512) return linux_error(22);
        /* These act on a final symlink itself, never its target. */
        char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,FS_DIRECT,NULL);if(parent<0) return fs_error();
        if(call==34) { /* Linux reports an existing path (including "/" or ".") before EROFS (tar's mkdir -p: mostly). */
            struct stat st;
            if(!strcmp(leaf,".") || !fs_leaf_stat(parent,leaf,&st)) { close(parent);return linux_error(17); }
            if(fs_count_full()) { close(parent);return linux_error(28); }
            if(strchr(leaf,'/') && !(fs_lower>=0 && ov_leaf.canon[0])) {  /* mkdirat refuses no symlink on the way: the real parent */
                close(parent);parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,0,NULL);if(parent<0) return fs_error();
            }
        }
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        if(!strcmp(leaf,".")) { close(parent);return linux_error(fs_where>0?16:13); } /* a mount point is busy */
        int result;OvLeaf w=ov_leaf;int layers=fs_lower>=0 && w.canon[0]; /* the root's image layers (overlay.h) */
        if(call==34) {
            if(ov_prepare(&w,&parent,leaf,OV_CREATE)) { uint64_t e=fs_error();close(parent);return e; }
            result=mkdirat(parent,leaf,(mode_t)(cpu->gpr[2]&0777&~fs_umask));if(!result) { fs_count_created();ov_created(&w,parent,leaf,1); }
        }
        else if(call==35) result=layers?ov_unlink(&w,parent,leaf,cpu->gpr[2]!=0):unlinkat(parent,leaf,(cpu->gpr[2]?AT_REMOVEDIR:0)|FS_ANY_AT);
        else {
            char target[1025];int other=fs_lookup(cpu->gpr[2],cpu->gpr[3],target,FS_DIRECT,NULL);
            if(other<0) { uint64_t e=fs_error();close(parent);return e; }
            if(!fs_path_writable()) { close(other);close(parent);return linux_error(30); }
            if(!strcmp(target,".")) { close(other);close(parent);return linux_error(fs_where>0?16:13); }
            OvLeaf to=ov_leaf;struct stat moving;
            int directory=upm_fs && layers && to.canon[0] && !fstatat(parent,leaf,&moving,FS_NOFOLLOW_AT) && S_ISDIR(moving.st_mode);
            if(directory && upm_has_files(to.canon)) { result=-1;errno=ENOTEMPTY; }        /* its pending files (upm.h) are entries */
            else if(layers || (fs_lower>=0 && to.canon[0]))result=layers && to.canon[0]?ov_rename(&w,parent,leaf,&to,other,target):(errno=EXDEV,-1);
            else result=fs_renameat(parent,leaf,other,target);
            if(directory && !result)upm_moved(w.canon,to.canon);
            int saved=errno;close(other);errno=saved;
        }
        uint64_t e=result<0?fs_error():0;
        /* unlink of a directory: Darwin says EPERM, Linux EISDIR (glibc's remove(), and so
           Java's File.delete, then retries with rmdir). */
        if(call==35 && !cpu->gpr[2] && result<0 && errno==EPERM) { struct stat st;if(!fstatat(parent,leaf,&st,FS_NOFOLLOW_AT) && S_ISDIR(st.st_mode))e=linux_error(21); }
        close(parent);return e;
    }
    int fd=fs_host(cpu->gpr[0]);if(fd<0) return linux_error(9);
    unsigned slot=(unsigned)fs_slot(cpu->gpr[0]);
    if(call==25) {
        if(cpu->gpr[1]==1)return FS(slot).cloexec;
        if(cpu->gpr[1]==2) { if(cpu->gpr[2]>1)return linux_error(22);FS(slot).cloexec=(unsigned char)cpu->gpr[2];fd_mirror(cpu->gpr[0]);return 0; }
        if(cpu->gpr[1]==3) { int flags=fcntl(fd,F_GETFL);if(flags<0)return fs_error();
            return (unsigned)(flags&O_ACCMODE)|((flags&O_APPEND)?1024:0)|131072;
        }
        if(cpu->gpr[1]==4) { /* F_SETFL: only O_APPEND changes for files (O_NONBLOCK has no effect on them) */
            int flags=fcntl(fd,F_GETFL),append=(cpu->gpr[2]&1024)?O_APPEND:0;if(flags<0)return fs_error();
            if((flags&O_APPEND)!=append && fd_mirror_append)__atomic_store_n(fd_mirror_append,1,__ATOMIC_SEQ_CST); /* before it changes */
            return fcntl(fd,F_SETFL,(flags&~O_APPEND)|append)<0?fs_error():0;
        }
        if((cpu->gpr[1]>=5 && cpu->gpr[1]<=7) || (cpu->gpr[1]>=36 && cpu->gpr[1]<=38))return fs_fcntl_lock(fd,cpu->gpr[1]);
        if(cpu->gpr[1]==1033 || cpu->gpr[1]==1034)return 0; /* F_ADD_SEALS, F_GET_SEALS: accepted, not enforced */
        return linux_error(95);
    }
    if(call==61) {
        uint64_t count=cpu->gpr[2];if(count<24 || count>1024*1024)return linux_error(22);
        unsigned char *out=lookup(cpu->gpr[1],count,2);if(!out)return linux_error(14);
        if(!FS(slot).dir) {
            struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISDIR(st.st_mode))return linux_error(20);
            FS(slot).dir=calloc(1,sizeof(*FS(slot).dir));if(!FS(slot).dir)return linux_error(12);FS(slot).dir->refs=1;
        }
        GuestDirectory *dir=FS(slot).dir;
        /* The root's image layers (overlay.h): a directory both hold lists them merged. */
        int layers=fs_lower>=0 && dir->path[0] && !(fs_mount_count && fs_mount_of(dir->path));
        if(!dir->stream && !dir->merged) {
            struct stat below;   /* only a directory both layers hold merges: a lower-only one (most of an image's) streams as it is */
            if(layers && !dir->lower && ((!fstatat(fs_lower,dir->path[1]?dir->path+1:".",&below,FS_NOFOLLOW_AT) && S_ISDIR(below.st_mode)) || upm_has_files(dir->path)))dir->merged=ov_list(dir->path);
            if(!dir->merged) {
                int copy=dup(fd);if(copy<0)return fs_error();
                dir->stream=fdopendir(copy);if(!dir->stream) { int saved=errno;close(copy);errno=saved;return fs_error(); }
            }
        }
        uint64_t used=0;
        for(;;) {
            long position=0;const char *name;uint64_t ino;unsigned type;
            if(dir->merged) {
                if(dir->merged->at>=dir->merged->size)return used;
                const char *at=dir->merged->data+dir->merged->at;memcpy(&ino,at,8);type=(unsigned char)at[8];name=at+9;
            } else {
                position=telldir(dir->stream);errno=0;
                struct dirent *entry=readdir(dir->stream);
                if(!entry)return errno?(used?used:fs_error()):used;
                name=entry->d_name;ino=entry->d_ino;type=entry->d_type;
                if(layers && ov_hidden(name))continue;
            }
            size_t length=strlen(name);
            if(length>255)return linux_error(36);
            for(size_t i=0;i<length;i++)if((unsigned char)name[i]<32 || name[i]==127)return linux_error(13);
            unsigned reclen=(unsigned)((19+length+1+7)&~7u);
            if(reclen>count-used) { if(!dir->merged)seekdir(dir->stream,position);return used?used:linux_error(22); }
            if(dir->count>=4096)return linux_error(75);
            if(dir->merged)dir->merged->at+=10+length;
            unsigned char *record=out+used;memset(record,0,reclen);
            fs_pack(record,0,ino,8);fs_pack(record,8,++dir->count,8);fs_pack(record,16,reclen,2);
            record[18]=type==DT_DIR?4:type==DT_REG?8:type==DT_LNK?10:0;
            memcpy(record+19,name,length);used+=reclen;
        }
    }
    if(call==57) { struct stat closing;if(!fstat(fd,&closing)) { fs_locks_release(closing.st_dev,closing.st_ino,process_pid_now());fs_locks_release(closing.st_dev,closing.st_ino,FS_FLOCK_OWNER|(uint64_t)fd); }
        if(FS(slot).dir) { fs_directory_release(FS(slot).dir);FS(slot).dir=NULL; }fs_pack_drop(slot);FS(slot).fd=0;fd_unbind(cpu->gpr[0]);return close(fd)<0?fs_error():0; }
    if(call==80) {
        unsigned char *out=lookup(cpu->gpr[1],128,2);if(!out) return linux_error(14);
        struct stat st;if(fstat(fd,&st)<0) return fs_error();
        fs_stat_pack(out,&st);return 0;
    }
    if(call==62) {
        if(FS(slot).dir) { if(cpu->gpr[1] || cpu->gpr[2])return linux_error(22);if(FS(slot).dir->stream)rewinddir(FS(slot).dir->stream);ov_list_free(FS(slot).dir->merged);FS(slot).dir->merged=NULL;FS(slot).dir->count=0;return 0; }
        if(cpu->gpr[2]>2) return linux_error(22);
        off_t result=lseek(fd,(off_t)(int64_t)cpu->gpr[1],(int)cpu->gpr[2]);
        return result<0?fs_error():(uint64_t)result;
    }
    if(call==46) {
        if(FS(slot).ro) return linux_error(30);
        if(cpu->gpr[1]>fs_file_limit || (fs_write_limit && cpu->gpr[1]>fs_write_limit-fs_written)) return linux_error(27);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();
        uint64_t growth=cpu->gpr[1]>(uint64_t)st.st_size?cpu->gpr[1]-(uint64_t)st.st_size:0;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        if(ftruncate(fd,(off_t)cpu->gpr[1])<0) return fs_error();
        __atomic_fetch_add(&fs_storage_bytes,growth,__ATOMIC_RELAXED);__atomic_fetch_add(&fs_written,cpu->gpr[1],__ATOMIC_RELAXED);return 0;
    }
    if(call==63 || call==64)return fs_transfer(fd,call);
    if(call==67 || call==68) { /* pread64/pwrite64 */
        if((int64_t)cpu->gpr[3]<0)return linux_error(22);
        return fs_transfer_at(fd,call==67?63:64,(int64_t)cpu->gpr[3]);
    }
    if(call==69 || call==70 || call==286 || call==287) { /* preadv/pwritev(2): one host call per buffer, stopping at a short one */
        if(call>=286 && cpu->gpr[5])return linux_error(95);                    /* RWF_* flags unsupported */
        if(call>=286 && (int64_t)cpu->gpr[3]==-1) {                              /* offset -1: the file position */
            off_t at=lseek(fd,0,SEEK_CUR);if(at<0)return fs_error();
            uint64_t saved=cpu->gpr[3];cpu->gpr[3]=(uint64_t)at;uint64_t n=fs_call(call==286?69:70);cpu->gpr[3]=saved;
            if((int64_t)n>0 && lseek(fd,at+(off_t)n,SEEK_SET)<0)return fs_error();
            return n;
        }
        if(call>=286)call-=217;                                                  /* 286 -> 69, 287 -> 70 */
        uint64_t count=cpu->gpr[2],offset=cpu->gpr[3],done=0,old1=cpu->gpr[1],old2=cpu->gpr[2];
        if((int64_t)offset<0 || count>1024)return linux_error(22);
        unsigned char *table=count?lookup(old1,count*16,4):NULL;if(count && !table)return linux_error(14);
        uint64_t result=0;
        for(uint64_t i=0;i<count;i++) {
            uint64_t length=le(table+i*16+8,8);if(!length)continue;
            cpu->gpr[1]=le(table+i*16,8);cpu->gpr[2]=length>1024*1024?1024*1024:length;
            uint64_t n=fs_transfer_at(fd,call==69?63:64,(int64_t)(offset+done));
            if((int64_t)n<0) { result=done?done:n;break; }
            done+=n;result=done;if(n<length)break;
        }
        cpu->gpr[1]=old1;cpu->gpr[2]=old2;return result;
    }
    if(call==84) { /* sync_file_range: SYNC_FILE_RANGE_WAIT_AFTER (4) syncs; the write-back hint alone is a no-op */
        if(cpu->gpr[3]&~UINT64_C(7))return linux_error(22);
        return (cpu->gpr[3]&4) && fs_sync(fd)<0?fs_error():0;
    }
    if(call==47) { /* fallocate: mode 0 grows the file (zero-filled); other modes are unsupported */
        if(FS(slot).ro)return linux_error(30);
        if(cpu->gpr[1])return linux_error(95);
        uint64_t offset=cpu->gpr[2],length=cpu->gpr[3];
        if((int64_t)offset<0 || (int64_t)length<=0 || length>UINT64_MAX-offset)return linux_error(22);
        uint64_t end=offset+length;if(end>fs_file_limit)return linux_error(27);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();
        if(end<=(uint64_t)st.st_size)return 0;
        uint64_t growth=end-(uint64_t)st.st_size;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        if(ftruncate(fd,(off_t)end)<0)return fs_error();
        __atomic_fetch_add(&fs_storage_bytes,growth,__ATOMIC_RELAXED);return 0;
    }
    return linux_error(38);
}
