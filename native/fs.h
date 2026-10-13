/* Root-relative filesystem adapter. Host identities never become guest fds. Guest
   symlinks are followed by fs_walk, which expands them itself and never lets the
   host kernel follow a link, so resolution cannot leave the guest root. */
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#define FS_SLOTS 1024u /* open files and directories per process */
/* Open files and directories of the current process (see the descriptor map in
   net.h); guest: slot -> guest descriptor. */
/* ro: the file lives where writes are refused (a read-only root or bind mount). */
typedef struct { int fd[FS_SLOTS];unsigned char cloexec[FS_SLOTS],ro[FS_SLOTS];struct GuestDirectory *dirs[FS_SLOTS];unsigned short guest[FS_SLOTS]; } FsTable;
static FsTable fs_table_first;
static _Thread_local FsTable *fs_table=&fs_table_first;
#define fs_fd (fs_table->fd)
#define fs_cloexec (fs_table->cloexec)
#define fs_dirs (fs_table->dirs)
#define fs_ro (fs_table->ro)
#define fs_guest (fs_table->guest)
static int fs_slot(uint64_t guest) { return fd_slot(guest,FD_FILE); }
#define FS_FILE_LIMIT (16u*1024u*1024u)
static int fs_root=-1, fs_writable;
/* Process-wide state every thread of a process shares (workers running its
   threads all point proc_now at the same record): the working directory, the
   guest identity, the umask and a vfork child's shared image. The identity makes
   every file appear owned by the process, so a switch (root to "redis", say) is
   coherent without per-file ownership; these are guest-visible numbers only, no
   host credential ever changes. */
typedef struct { struct GuestCwd *cwd;uint32_t uid,gid;unsigned umask;struct SharedImage *shared; } ProcShared;
static ProcShared proc_first={.umask=0022};
static _Thread_local ProcShared *proc_now=&proc_first;
#define fs_cwd (proc_now->cwd)
#define guest_uid (proc_now->uid)
#define guest_gid (proc_now->gid)
#define fs_umask (proc_now->umask)
#define shared_image (proc_now->shared)
typedef struct GuestDirectory { DIR *stream;unsigned refs,count;char path[1025]; } GuestDirectory;
static void fs_directory_release(GuestDirectory *directory) {
    if(directory && !--directory->refs) { if(directory->stream)closedir(directory->stream);free(directory); }
}
static uint64_t fs_written;
static unsigned fs_created,fs_entries;
#define FS_STORAGE_LIMIT (128u*1024u*1024u)
static uint64_t fs_storage_bytes;
/* Budgets for direct runtime use; the engine raises them (--file-limit-mib) and
   removes the storage and cumulative-write budgets (--storage-limit-mib 0), as
   Docker leaves disk use to the host. */
static uint64_t fs_file_limit=FS_FILE_LIMIT,fs_storage_limit=FS_STORAGE_LIMIT,fs_write_limit=64u*1024u*1024u; /* 0: unlimited */
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
            fs_storage_bytes+=(uint64_t)st.st_size;
        } else if(S_ISDIR(st.st_mode)) {
            int fd=openat(directory,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0) { result=-1;break; }
            result=fs_scan_budget(fd,depth+1);int saved=errno;close(fd);errno=saved;if(result)break;
        }
    }
    if(errno)result=-1;
    int saved=errno;closedir(stream);errno=saved;return result;
}
typedef struct GuestCwd { int fd;unsigned refs;char path[1025]; } GuestCwd;
static void fs_cwd_release(void) {
    if(fs_cwd && !--fs_cwd->refs) { close(fs_cwd->fd);free(fs_cwd); }
    fs_cwd=NULL;
}
static void fs_cleanup(void) {
    for(unsigned i=0;i<FS_SLOTS;i++) { fs_directory_release(fs_dirs[i]);if(fs_fd[i])close(fs_fd[i]-1); }
    fs_cwd_release();
    if(fs_root>=0) close(fs_root);
}
static int fs_host(uint64_t n) { int slot=fs_slot(n);return slot>=0 && fs_fd[slot]?fs_fd[slot]-1:-1; }
static uint64_t fs_error(void) {
    switch(errno) {
        case ENOENT:return linux_error(2);case ENOTDIR:return linux_error(20);
        case EISDIR:return linux_error(21);case EEXIST:return linux_error(17);
        case ELOOP:return linux_error(40);case ENAMETOOLONG:return linux_error(36);
        case ENOTEMPTY:return linux_error(39);case EROFS:return linux_error(30);
        case EFBIG:return linux_error(27);case ENOSPC:return linux_error(28);
        default:return net_error();
    }
}
static int fs_string(uint64_t ptr,char path[1025]) {
    for(unsigned i=0;i<=1024;i++) {
        if(ptr>UINT64_MAX-i) { errno=EFAULT;return -1; }
        unsigned char *p=lookup(ptr+i,1,4);
        if(!p) { errno=EFAULT;return -1; }
        path[i]=(char)*p;
        if(!*p) return i?0:(errno=ENOENT,-1);
        if(*p<32 || *p>=127 || *p=='\\' || *p==':') { errno=EACCES;return -1; }
    }
    errno=ENAMETOOLONG;return -1;
}
#define FS_LINK_LIMIT 40 /* Linux MAXSYMLINKS */
static int fs_is_root(int fd) {
    struct stat a,b;return !fstat(fd,&a) && !fstat(fs_root,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
}
/* Bind mounts (--bind HOST GUEST, --bind-ro): a guest path served by a host
   directory, or a single host file. fs_walk switches to the mount's descriptor
   when it reaches the guest path, and ".." at the top of a mounted directory
   returns to the guest parent, never the host's. Paths beneath a mount are
   resolved like the rest of the root: symlinks never reach the host kernel. */
#define FS_MOUNTS 32
static struct { char path[1025],name[256];int fd,directory,readonly;dev_t dev;ino_t ino; } fs_mounts[FS_MOUNTS];
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
        for(const char *c=p;c<q;c++)if(*c<32 || *c>=127 || *c=='\\' || *c==':') { errno=EINVAL;return -1; }
        p=*q?q+1:q;
    }
    if(fs_mount_index(guest)>=0) { errno=EEXIST;return -1; }
    int fd=open(host,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;if(fstat(fd,&st)) { int saved=errno;close(fd);errno=saved;return -1; }
    char name[256]="";
    if(S_ISREG(st.st_mode)) {                              /* a file: keep its directory and name */
        close(fd);
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
#ifdef O_NOFOLLOW_ANY
/* fs_walk for a path with no ".." component: one host openat of its directories
   with O_NOFOLLOW_ANY, which refuses a symlink anywhere on the way, so the result
   stays beneath base exactly as the component walk would. Returns -2 when the
   path needs that walk (a symlink, "..", an unusual form or error). */
static int fs_walk_fast(int base,const char *cur_start,const char *rest,int follow,char leaf[1025],char canon[1025]) {
    /* (canon receives the leaf's canonical path; it is always computed) */
    size_t n=strlen(rest);
    if(!n || rest[n-1]=='/')return -2;
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
    /* A path into (or across) a mount needs the component walk, even where the
       root has nothing at the mount's path. */
    if(fs_mount_count && fs_mount_of(canon)!=fs_mount_of(cur_start))return -2;
    const char *relative=dirs;while(*relative=='/')relative++;
    int fd=*relative?openat(base,relative,O_RDONLY|O_DIRECTORY|O_NOFOLLOW_ANY|O_CLOEXEC):dup(base);
    if(fd<0)return errno==ENOENT || errno==ENOTDIR?-1:-2;
    struct stat st;
    if(follow && fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode)) { close(fd);return -2; }
    strcpy(leaf,name);return fd;
}
#endif
static int fs_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]);
/* A guest directory by canonical path, as a descriptor (a mount's own, if it is one). */
static int fs_open_dir(const char *path) {
    char leaf[1025];int parent=fs_walk(fs_root,"/",path,1,leaf,NULL);if(parent<0)return -1;
    if(!strcmp(leaf,"."))return parent;
    int fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(parent);errno=saved;
    return fd;
}
static int fs_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]) {
    if(fs_root<0) { errno=EACCES;return -1; }
    char rest[4097],cur[1025];size_t length=strlen(input),pos=0;unsigned links=0;
    if(length>1024) { errno=ENAMETOOLONG;return -1; }
    memcpy(rest,input,length+1);
    if(rest[0]!='/') {
        struct stat st;if(fstat(base,&st)<0)return -1;
        if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
    } else base=fs_root;
    strcpy(cur,rest[0]=='/'?"/":base_path);
#ifdef O_NOFOLLOW_ANY
    {   /* The fast path stays in base's host directory tree: not across a mount. */
        char full[1025];int fast=fs_walk_fast(base,cur,rest,follow,leaf,full);
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
            if(!fs_is_root(fd)) {
                int next=openat(fd,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                int saved=errno;close(fd);fd=next;errno=saved;if(fd<0)return -1;
                fs_canon_pop(cur);
            }
            continue;
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
            if(++links>FS_LINK_LIMIT) { close(fd);errno=ELOOP;return -1; }
            char target[1025];ssize_t n=readlinkat(fd,part,target,1024);
            if(n<=0) { int saved=n<0?errno:ENOENT;close(fd);errno=saved;return -1; }
            target[n]=0;
            for(ssize_t i=0;i<n;i++)if((unsigned char)target[i]<32 || (unsigned char)target[i]>=127 || target[i]=='\\' || target[i]==':') { close(fd);errno=EACCES;return -1; }
            size_t tail=strlen(rest+pos);
            if((size_t)n+tail>4096) { close(fd);errno=ENAMETOOLONG;return -1; }
            memmove(rest+n,rest+pos,tail+1);memcpy(rest,target,(size_t)n);pos=0;
            if(target[0]=='/') {
                close(fd);fd=dup(fs_root);if(fd<0)return -1;
                strcpy(cur,"/");
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
/* Minimal /proc: /proc/self/exe and /proc/<pid>/exe name each process's executable
   (canonical guest paths, indexed by pid; PROCESS_LIMIT+1 processes). Path lookups
   resolve them to that file; readlink returns the path. */
static char proc_exe[PROCESS_LIMIT+2][256];
static _Thread_local uint64_t process_pid; /* defined with the process layer */
static int fs_proc_exe(const char *path,char out[1025]) {
    uint64_t pid=0;
    if(!strcmp(path,"/proc/self/exe"))pid=process_pid;
    else if(!strncmp(path,"/proc/",6)) {
        const char *p=path+6;while(*p>='0' && *p<='9' && pid<100)pid=pid*10+(uint64_t)(*p++-'0');
        if(p==path+6 || strcmp(p,"/exe"))pid=0;
    }
    if(!pid || pid>=18 || !proc_exe[pid][0])return 0;
    strcpy(out,proc_exe[pid]);return 1;
}
static int fs_lookup(uint64_t dir,uint64_t ptr,char leaf[1025],int follow,char canon[1025]) {
    char path[1025];if(fs_root<0) { errno=EACCES;return -1; }
    if(fs_string(ptr,path)<0) return -1;
    if(path[0]=='/' && !strncmp(path,"/proc/",6)) { char exe[1025];if(fs_proc_exe(path,exe))strcpy(path,exe); }
    int base=fs_root;const char *base_path="/";
    if(path[0]!='/') {
        if((int32_t)dir==-100) { base=fs_cwd?fs_cwd->fd:fs_root;base_path=fs_cwd?fs_cwd->path:"/"; }
        else {
            base=fs_host(dir);
            base_path=fs_slot(dir)>=0 && fs_dirs[fs_slot(dir)]?fs_dirs[fs_slot(dir)]->path:"";
        }
    }
    if(base<0) { errno=EBADF;return -1; }
    return fs_walk(base,base_path,path,follow,leaf,canon);
}
/* Parent directory and leaf, following every symlink including the last. */
static int fs_parent(uint64_t dir,uint64_t ptr,char leaf[1025]) { return fs_lookup(dir,ptr,leaf,1,NULL); }
/* As fs_parent, but a final symlink is returned as the leaf itself (lstat,
   readlink, unlink, rename, mkdir). */
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
static int fs_is_device(int fd) { struct stat st;return !fstat(fd,&st) && S_ISCHR(st.st_mode); }
static uint64_t fs_fchdir(void) {
    int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
    struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISDIR(st.st_mode))return linux_error(20);
    GuestCwd *next=calloc(1,sizeof(*next));if(!next)return linux_error(12);
    next->fd=dup(fd);if(next->fd<0) { free(next);return fs_error(); }next->refs=1;
    if(fs_dirs[fs_slot(cpu->gpr[0])])strcpy(next->path,fs_dirs[fs_slot(cpu->gpr[0])]->path);
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
static int fs_start_cwd(const char *path) {
    size_t length=strlen(path);
    if(fs_root<0 || path[0]!='/' || length>1024) { errno=EINVAL;return -1; }
    for(size_t i=0;i<length;i++)if((unsigned char)path[i]<32 || (unsigned char)path[i]>=127 || path[i]=='\\' || path[i]==':') { errno=EINVAL;return -1; }
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
        int fd=dup(fs_root);char copy[1025];strcpy(copy,path);char *p=copy;
        while(fd>=0 && *p) {
            while(*p=='/')p++;if(!*p)break;
            char *part=p;while(*p && *p!='/')p++;if(*p)*p++=0;
            int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;
        }
        struct stat a,b;int same=fd>=0 && !fstat(fd,&a) && !fstat(fs_cwd->fd,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
        if(fd>=0)close(fd);if(!same)return linux_error(2);
    }
    size_t size=strlen(path)+1;if(cpu->gpr[1]<size)return linux_error(34);
    unsigned char *out=lookup(cpu->gpr[0],size,2);if(!out)return linux_error(14);
    memcpy(out,path,size);return size;
}
/* Interpreter paths originate in an ELF header, not guest memory. */
static int fs_open_root_file(const char *path) {
    if(fs_root<0 || path[0]!='/') { errno=EACCES;return -1; }
    char copy[1025];size_t length=strlen(path);
    if(length>1024) { errno=ENAMETOOLONG;return -1; }
    for(size_t i=0;i<length;i++)if((unsigned char)path[i]<32 || (unsigned char)path[i]>=127 || path[i]=='\\' || path[i]==':') { errno=EACCES;return -1; }
    strcpy(copy,path);char leaf[1025];
    int parent=fs_walk(fs_root,"/",copy,1,leaf,NULL);if(parent<0)return -1;
    int fd=openat(parent,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    int saved=errno;close(parent);errno=saved;
    struct stat st;if(fd>=0 && (fstat(fd,&st)<0 || !S_ISREG(st.st_mode))) { close(fd);errno=EACCES;return -1; }
    return fd;
}
static uint64_t fs_open(void) {
    uint64_t flags=cpu->gpr[2];
    /* O_NOCTTY (256), O_NONBLOCK (2048) and O_NOATIME (262144) have no effect on regular
       files or directories; glibc's opendir passes O_NONBLOCK. */
    const uint64_t allowed=3|64|128|256|512|1024|2048|16384|32768|131072|262144|524288;
    if((flags & ~allowed) || (flags&3)==3) return linux_error(95);
    unsigned slot=0;while(slot<FS_SLOTS && fs_fd[slot]) slot++;
    if(slot==FS_SLOTS || fd_lowest(0)<0) return linux_error(24);
    /* Linux follows a final symlink unless O_NOFOLLOW (32768) or O_CREAT|O_EXCL. */
    int follow=!(flags&32768) && (flags&(64|128))!=(64|128);
    char leaf[1025],canon[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,follow,canon);if(parent<0) return fs_error();
    int options=(flags&3)==2?O_RDWR:(flags&3)==1?O_WRONLY:O_RDONLY;
    const char *device=fs_device(canon);
    if(device) { /* Writable even on a read-only root; O_CREAT/O_TRUNC are no-ops. */
        close(parent);
        if(flags&16384)return linux_error(20);
        int fd=open(device,options|O_CLOEXEC);if(fd<0)return fs_error();
        fs_fd[slot]=fd+1;fs_cloexec[slot]=(flags&524288)!=0;fs_ro[slot]=0;{ int guest=fd_lowest(0);fs_guest[slot]=(unsigned short)guest;fd_bind((uint64_t)guest,FD_FILE,slot);return (uint64_t)guest; }
    }
    int read_only=!fs_path_writable();
    if(read_only && (flags&(3|64|512|1024))) { close(parent);return linux_error(30); }
    options|=O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK;
    if(flags&64) options|=O_CREAT;
    if(flags&128) options|=O_EXCL;
    if(flags&512) options|=O_TRUNC;
    if(flags&1024) options|=O_APPEND;
    if(flags&16384) options|=O_DIRECTORY;
    if((flags&64) && (fs_created>=4096 || fs_entries>=8192)) { close(parent);return linux_error(28); }
    /* Validate the existing leaf before a possible truncate; only regular files/dirs.
       An unfollowed symlink reaches openat, which fails with ELOOP or EEXIST as on Linux. */
    struct stat before;
    if(fstatat(parent,leaf,&before,AT_SYMLINK_NOFOLLOW)==0 && !S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode) && !S_ISLNK(before.st_mode)) {
        close(parent);return linux_error(13);
    }
    int fd=openat(parent,leaf,options,(mode_t)(cpu->gpr[3]&0777&~fs_umask));int saved=errno;close(parent);errno=saved;
    if(fd<0) return fs_error();
    struct stat info;
    if(fstat(fd,&info)<0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || (uint64_t)info.st_size>fs_file_limit) {
        close(fd);return linux_error(13);
    }
    if(S_ISDIR(info.st_mode)) {
        GuestDirectory *directory=calloc(1,sizeof(*directory));if(!directory) { close(fd);return linux_error(12); }
        directory->refs=1;strcpy(directory->path,canon);
        fs_dirs[slot]=directory;
    }
    if(flags&64) { fs_created++;fs_entries++; }
    fs_fd[slot]=fd+1;fs_cloexec[slot]=(flags&524288)!=0;fs_ro[slot]=(unsigned char)read_only;{ int guest=fd_lowest(0);fs_guest[slot]=(unsigned short)guest;fd_bind((uint64_t)guest,FD_FILE,slot);return (uint64_t)guest; }
}
static void fs_pack(unsigned char *p,unsigned offset,uint64_t value,unsigned size) {
    for(unsigned i=0;i<size;i++) p[offset+i]=(unsigned char)(value>>(i*8));
}
static void fs_stat_pack(unsigned char *out,const struct stat *st) {
    memset(out,0,128);fs_pack(out,8,st->st_ino,8);
    fs_pack(out,16,(S_ISDIR(st->st_mode)?0040000:S_ISLNK(st->st_mode)?0120000:S_ISCHR(st->st_mode)?0020000:S_ISSOCK(st->st_mode)?0140000:S_ISFIFO(st->st_mode)?0010000:0100000)|(st->st_mode&0777),4);
    fs_pack(out,24,guest_uid,4);fs_pack(out,28,guest_gid,4);
    fs_pack(out,20,st->st_nlink?st->st_nlink:1,4);fs_pack(out,48,(uint64_t)st->st_size,8);
    fs_pack(out,56,4096,4);fs_pack(out,64,((uint64_t)st->st_size+511)/512,8);
    /* Linux arm64 st_atime, st_mtime and st_ctime, each seconds then nanoseconds. */
    const struct timespec *times[3]={&st->st_atimespec,&st->st_mtimespec,&st->st_ctimespec};
    for(unsigned i=0;i<3;i++) { fs_pack(out,72+i*16,(uint64_t)times[i]->tv_sec,8);fs_pack(out,80+i*16,(uint64_t)times[i]->tv_nsec,8); }
}
/* read/write at the file position, or at offset (pread/pwrite) when it is >= 0. */
static uint64_t fs_transfer_at(int fd,uint64_t call,int64_t offset) {
        uint64_t count=cpu->gpr[2]>1024*1024?1024*1024:cpu->gpr[2];          /* larger requests: a short transfer */
        unsigned char *buffer=count?lookup(cpu->gpr[1],count,call==63?2:4):(unsigned char *)"";
        if(!buffer) return linux_error(14);
        if(fs_is_device(fd)) { /* No file size, storage or write budget to account. */
            ssize_t result=offset>=0?(call==63?pread(fd,buffer,(size_t)count,offset):pwrite(fd,buffer,(size_t)count,offset))
                                    :(call==63?read(fd,buffer,(size_t)count):write(fd,buffer,(size_t)count));
            return result<0?fs_error():(uint64_t)result;
        }
        uint64_t previous_size=0,write_position=0;
        if(call==64) {
            int slot=fs_slot(cpu->gpr[0]);    /* a file slot knows its location; stdio files follow the root */
            if(slot>=0?fs_ro[slot]:!fs_writable) return linux_error(30);
            struct stat st;if(fstat(fd,&st)<0) return fs_error();
            off_t position=offset>=0?(off_t)offset:lseek(fd,0,SEEK_CUR);if(position<0) return fs_error();
            if(offset<0 && (fcntl(fd,F_GETFL)&O_APPEND)) position=st.st_size;
            if((uint64_t)position>fs_file_limit || count>fs_file_limit-(uint64_t)position || (fs_write_limit && count>fs_write_limit-fs_written)) return linux_error(27);
            previous_size=(uint64_t)st.st_size;write_position=(uint64_t)position;
            uint64_t end=write_position+count;
            if(fs_storage_limit && end>previous_size && end-previous_size>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        }
        ssize_t result=offset>=0?(call==63?pread(fd,buffer,(size_t)count,offset):pwrite(fd,buffer,(size_t)count,offset))
                                :(call==63?read(fd,buffer,(size_t)count):write(fd,buffer,(size_t)count));
        if(result<0) return fs_error();
        if(call==64) { fs_written+=(uint64_t)result;uint64_t end=write_position+(uint64_t)result;if(end>previous_size)fs_storage_bytes+=end-previous_size; }
        return (uint64_t)result;
}
static uint64_t fs_transfer(int fd,uint64_t call) { return fs_transfer_at(fd,call,-1); }
static uint64_t fs_call(uint64_t call) {
    if(call==166) { unsigned previous=fs_umask;fs_umask=(unsigned)cpu->gpr[0]&0777;return previous; }
    if(call==52 || call==53) {
        uint64_t mode=cpu->gpr[call==52?1:2];if(mode&~UINT64_C(07777))return linux_error(22); /* setuid/setgid/sticky allowed, as on Linux */
        int fd=fs_host(cpu->gpr[0]),owned=0;
        if(call==52 && fs_slot(cpu->gpr[0])>=0 && fs_ro[fs_slot(cpu->gpr[0])])return linux_error(30);
        if(call==53) {
            char leaf[1025];int parent=fs_parent(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0)return fs_error();
            if(!fs_path_writable()) { close(parent);return linux_error(30); }
            struct stat link;
            if(fstatat(parent,leaf,&link,AT_SYMLINK_NOFOLLOW)==0 && S_ISSOCK(link.st_mode)) { /* sockets cannot be opened */
                int result=fchmodat(parent,leaf,(mode_t)mode,AT_SYMLINK_NOFOLLOW);
                int saved=errno;close(parent);errno=saved;return result<0?fs_error():0;
            }
            fd=openat(parent,leaf,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
            int saved=errno;close(parent);errno=saved;if(fd<0)return fs_error();owned=1;
        } else if(fd<0)return linux_error(9);
        struct stat st;int result=fstat(fd,&st);
        if(!result && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { errno=EACCES;result=-1; }
        if(!result)result=fchmod(fd,(mode_t)mode);
        int saved=errno;if(owned)close(fd);errno=saved;return result<0?fs_error():0;
    }
    if(call==78) {
        if(!cpu->gpr[3] || cpu->gpr[3]>4096)return linux_error(22);
        unsigned char *out=lookup(cpu->gpr[2],cpu->gpr[3],2);if(!out)return linux_error(14);
        char path[1025],exe[1025];
        if(fs_string(cpu->gpr[1],path)==0 && fs_proc_exe(path,exe)) { /* readlink /proc/<pid>/exe */
            size_t n=strlen(exe);if(n>cpu->gpr[3])n=(size_t)cpu->gpr[3];memcpy(out,exe,n);return n;
        }
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0)return fs_error();
        /* Read the final link's text; ancestors are resolved inside the root. */
        ssize_t result=readlinkat(parent,leaf,(char *)out,(size_t)cpu->gpr[3]);
        int saved=errno;close(parent);errno=saved;return result<0?fs_error():(uint64_t)result;
    }
    if(call==36) { /* symlinkat(target, dirfd, path): the target is kept verbatim and resolved inside the root */
        char target[1025];if(fs_string(cpu->gpr[0],target)<0)return fs_error();
        if(fs_created>=4096 || fs_entries>=8192)return linux_error(28);
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[1],cpu->gpr[2],leaf);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        if(!strcmp(leaf,".")) { close(parent);return linux_error(17); }
        int result=symlinkat(target,parent,leaf);int saved=errno;close(parent);errno=saved;
        if(result<0)return fs_error();
        fs_created++;fs_entries++;return 0;
    }
    if(call==37) { /* linkat(olddirfd, old, newdirfd, new, flags): a hard link within one mount */
        uint64_t flags=cpu->gpr[4];if(flags&~UINT64_C(0x400))return linux_error(22);   /* AT_SYMLINK_FOLLOW */
        if(fs_created>=4096 || fs_entries>=8192)return linux_error(28);
        char from[1025];int source=fs_lookup(cpu->gpr[0],cpu->gpr[1],from,(flags&0x400)!=0,NULL);if(source<0)return fs_error();
        int where=fs_where;
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[2],cpu->gpr[3],leaf);if(parent<0) { uint64_t e=fs_error();close(source);return e; }
        uint64_t error=0;
        if(!fs_path_writable())error=linux_error(30);
        else if(where!=fs_where || where<0)error=linux_error(18);   /* EXDEV: never across a mount boundary */
        else if(!strcmp(from,".") || !strcmp(leaf,"."))error=linux_error(!strcmp(leaf,".")?17:1);
        else if(linkat(source,from,parent,leaf,0)<0)error=fs_error();
        close(parent);close(source);
        if(!error) { fs_created++;fs_entries++; }
        return error;
    }
    if(call==88) { /* utimensat(dirfd, path, times, flags); a NULL path acts on dirfd itself (futimens) */
        uint64_t flags=cpu->gpr[3];if(flags&~UINT64_C(0x100))return linux_error(22); /* AT_SYMLINK_NOFOLLOW */
        const unsigned char *in=cpu->gpr[2]?lookup(cpu->gpr[2],32,4):NULL;if(cpu->gpr[2] && !in)return linux_error(14);
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
            int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
            if(fs_ro[fs_slot(cpu->gpr[0])])return linux_error(30);
            return futimens(fd,times)<0?fs_error():0;
        }
        char leaf[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(flags&0x100),NULL);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        /* The leaf is never followed by the host: a final symlink is followed by fs_lookup already. */
        int result=utimensat(parent,leaf,times,AT_SYMLINK_NOFOLLOW);int saved=errno;close(parent);errno=saved;
        return result<0?fs_error():0;
    }
    if(call==45) { /* truncate(path, length): ftruncate of the resolved regular file */
        uint64_t length=cpu->gpr[1];
        if((int64_t)length<0)return linux_error(22);
        if(length>fs_file_limit)return linux_error(27);
        char leaf[1025];int parent=fs_parent((uint64_t)-100,cpu->gpr[0],leaf);if(parent<0)return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        int fd=openat(parent,leaf,O_WRONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);int saved=errno;close(parent);errno=saved;
        if(fd<0)return fs_error();
        struct stat st;if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode)) { saved=S_ISDIR(st.st_mode)?EISDIR:EINVAL;close(fd);errno=saved;return fs_error(); }
        uint64_t growth=length>(uint64_t)st.st_size?length-(uint64_t)st.st_size:0;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes) { close(fd);return linux_error(28); }
        int result=ftruncate(fd,(off_t)length);saved=errno;close(fd);errno=saved;
        if(result<0)return fs_error();
        fs_storage_bytes+=growth;return 0;
    }
    if(call==48 || call==439) { /* faccessat(2); synthetic uid 0; no-link namespace policy still applies. */
        if(cpu->gpr[2]&~UINT64_C(7) || (call==439 && (cpu->gpr[3]&~UINT64_C(0x300))))return linux_error(22); /* AT_EACCESS, AT_SYMLINK_NOFOLLOW */
        char leaf[1025];int parent=fs_parent(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0)return fs_error();
        int writable=fs_path_writable();
        struct stat st;int result=fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW);
        int saved=errno;close(parent);errno=saved;if(result<0)return fs_error();
        if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISSOCK(st.st_mode))return linux_error(13);
        if((cpu->gpr[2]&2) && !writable)return linux_error(30);
        if(guest_uid && (cpu->gpr[2]&~((unsigned)st.st_mode>>6)))return linux_error(13);
        if((cpu->gpr[2]&1) && S_ISREG(st.st_mode) && !(st.st_mode&0111))return linux_error(13);
        return 0;
    }
    if(call==82 || call==83) {
        int fd=fs_host(cpu->gpr[0]);if(fd<0)return linux_error(9);
        /* fsync is a valid stronger implementation of fdatasync. It is not a
           promise of durability across host power loss. */
        return fsync(fd)<0?fs_error():0;
    }
    if(call==79) {
        if(cpu->gpr[3]&~UINT64_C(256|2048|4096)) return linux_error(95);
        unsigned char *out=lookup(cpu->gpr[2],128,2);if(!out)return linux_error(14);
        unsigned char *first=lookup(cpu->gpr[1],1,4);if(!first)return linux_error(14);
        if(!*first) {
            if(!(cpu->gpr[3]&4096))return linux_error(2);
            int fd=(int32_t)cpu->gpr[0]==-100?(fs_cwd?fs_cwd->fd:fs_root):fs_host(cpu->gpr[0]);
            if(fd<0)return linux_error(9);
            struct stat st;if(fstat(fd,&st)<0)return fs_error();
            fs_stat_pack(out,&st);return 0;
        }
        /* AT_SYMLINK_NOFOLLOW (256) reports a final symlink itself. */
        char leaf[1025],canon[1025];int parent=fs_lookup(cpu->gpr[0],cpu->gpr[1],leaf,!(cpu->gpr[3]&256),canon);if(parent<0)return fs_error();
        struct stat st;const char *device=fs_device(canon);
        int result=device?stat(device,&st):fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW);
        int saved=errno;close(parent);errno=saved;if(result<0)return fs_error();
        if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && !S_ISSOCK(st.st_mode) && !device)return linux_error(13);
        fs_stat_pack(out,&st);return 0;
    }
    if(call==56) return fs_open();
    if(call==34) { /* Linux reports an existing path (including "/" or ".") before EROFS. */
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[0],cpu->gpr[1],leaf);
        if(parent>=0) {
            struct stat st;int present=!strcmp(leaf,".") || !fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW);close(parent);
            if(present)return linux_error(17);
        }
    }
    if(call==34 || call==35 || call==38) {
        if(call==34 && (fs_created>=4096 || fs_entries>=8192)) return linux_error(28);
        if(call==35 && cpu->gpr[2]!=0 && cpu->gpr[2]!=512) return linux_error(22);
        /* These act on a final symlink itself, never its target. */
        char leaf[1025];int parent=fs_parent_nofollow(cpu->gpr[0],cpu->gpr[1],leaf);if(parent<0) return fs_error();
        if(!fs_path_writable()) { close(parent);return linux_error(30); }
        if(!strcmp(leaf,".")) { close(parent);return linux_error(fs_where>0?16:13); } /* a mount point is busy */
        int result;
        if(call==34) { result=mkdirat(parent,leaf,(mode_t)(cpu->gpr[2]&0777&~fs_umask));if(!result) { fs_created++;fs_entries++; } }
        else if(call==35) result=unlinkat(parent,leaf,cpu->gpr[2]?AT_REMOVEDIR:0);
        else {
            char target[1025];int other=fs_parent_nofollow(cpu->gpr[2],cpu->gpr[3],target);
            if(other<0) { uint64_t e=fs_error();close(parent);return e; }
            if(!fs_path_writable()) { close(other);close(parent);return linux_error(30); }
            if(!strcmp(target,".")) { close(other);close(parent);return linux_error(fs_where>0?16:13); }
            result=renameat(parent,leaf,other,target);int saved=errno;close(other);errno=saved;
        }
        uint64_t e=result<0?fs_error():0;close(parent);return e;
    }
    int fd=fs_host(cpu->gpr[0]);if(fd<0) return linux_error(9);
    unsigned slot=(unsigned)fs_slot(cpu->gpr[0]);
    if(call==25) {
        if(cpu->gpr[1]==1)return fs_cloexec[slot];
        if(cpu->gpr[1]==2) { if(cpu->gpr[2]>1)return linux_error(22);fs_cloexec[slot]=(unsigned char)cpu->gpr[2];return 0; }
        if(cpu->gpr[1]==3) { int flags=fcntl(fd,F_GETFL);if(flags<0)return fs_error();
            return (unsigned)(flags&O_ACCMODE)|((flags&O_APPEND)?1024:0)|131072;
        }
        return linux_error(95);
    }
    if(call==61) {
        uint64_t count=cpu->gpr[2];if(count<24 || count>1024*1024)return linux_error(22);
        unsigned char *out=lookup(cpu->gpr[1],count,2);if(!out)return linux_error(14);
        if(!fs_dirs[slot]) {
            struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISDIR(st.st_mode))return linux_error(20);
            fs_dirs[slot]=calloc(1,sizeof(*fs_dirs[slot]));if(!fs_dirs[slot])return linux_error(12);fs_dirs[slot]->refs=1;
        }
        if(!fs_dirs[slot]->stream) {
            int copy=dup(fd);if(copy<0)return fs_error();
            fs_dirs[slot]->stream=fdopendir(copy);if(!fs_dirs[slot]->stream) { int saved=errno;close(copy);errno=saved;return fs_error(); }
        }
        uint64_t used=0;
        for(;;) {
            long position=telldir(fs_dirs[slot]->stream);errno=0;
            struct dirent *entry=readdir(fs_dirs[slot]->stream);
            if(!entry)return errno?(used?used:fs_error()):used;
            size_t length=strlen(entry->d_name);
            if(length>255)return linux_error(36);
            for(size_t i=0;i<length;i++)if((unsigned char)entry->d_name[i]<32 || (unsigned char)entry->d_name[i]>=127 || entry->d_name[i]=='\\' || entry->d_name[i]==':')return linux_error(13);
            unsigned reclen=(unsigned)((19+length+1+7)&~7u);
            if(reclen>count-used) { seekdir(fs_dirs[slot]->stream,position);return used?used:linux_error(22); }
            if(fs_dirs[slot]->count>=4096)return linux_error(75);
            unsigned char *record=out+used;memset(record,0,reclen);
            fs_pack(record,0,entry->d_ino,8);fs_pack(record,8,++fs_dirs[slot]->count,8);fs_pack(record,16,reclen,2);
            record[18]=entry->d_type==DT_DIR?4:entry->d_type==DT_REG?8:entry->d_type==DT_LNK?10:0;
            memcpy(record+19,entry->d_name,length);used+=reclen;
        }
    }
    if(call==57) { if(fs_dirs[slot]) { fs_directory_release(fs_dirs[slot]);fs_dirs[slot]=NULL; }fs_fd[slot]=0;fd_unbind(cpu->gpr[0]);return close(fd)<0?fs_error():0; }
    if(call==80) {
        unsigned char *out=lookup(cpu->gpr[1],128,2);if(!out) return linux_error(14);
        struct stat st;if(fstat(fd,&st)<0) return fs_error();
        fs_stat_pack(out,&st);return 0;
    }
    if(call==62) {
        if(fs_dirs[slot]) { if(cpu->gpr[1] || cpu->gpr[2])return linux_error(22);if(fs_dirs[slot]->stream)rewinddir(fs_dirs[slot]->stream);fs_dirs[slot]->count=0;return 0; }
        if(cpu->gpr[2]>2) return linux_error(22);
        off_t result=lseek(fd,(off_t)(int64_t)cpu->gpr[1],(int)cpu->gpr[2]);
        return result<0?fs_error():(uint64_t)result;
    }
    if(call==46) {
        if(fs_ro[slot]) return linux_error(30);
        if(cpu->gpr[1]>fs_file_limit || (fs_write_limit && cpu->gpr[1]>fs_write_limit-fs_written)) return linux_error(27);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();
        uint64_t growth=cpu->gpr[1]>(uint64_t)st.st_size?cpu->gpr[1]-(uint64_t)st.st_size:0;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        if(ftruncate(fd,(off_t)cpu->gpr[1])<0) return fs_error();
        fs_storage_bytes+=growth;fs_written+=cpu->gpr[1];return 0;
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
        return (cpu->gpr[3]&4) && fsync(fd)<0?fs_error():0;
    }
    if(call==47) { /* fallocate: mode 0 grows the file (zero-filled); other modes are unsupported */
        if(fs_ro[slot])return linux_error(30);
        if(cpu->gpr[1])return linux_error(95);
        uint64_t offset=cpu->gpr[2],length=cpu->gpr[3];
        if((int64_t)offset<0 || (int64_t)length<=0 || length>UINT64_MAX-offset)return linux_error(22);
        uint64_t end=offset+length;if(end>fs_file_limit)return linux_error(27);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();
        if(end<=(uint64_t)st.st_size)return 0;
        uint64_t growth=end-(uint64_t)st.st_size;
        if(fs_storage_limit && growth>fs_storage_limit-fs_storage_bytes)return linux_error(28);
        if(ftruncate(fd,(off_t)end)<0)return fs_error();
        fs_storage_bytes+=growth;return 0;
    }
    return linux_error(38);
}
