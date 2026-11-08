/* Image layers (--lower DIR), as Docker's overlay2 driver has them: the container's
   files are its root directory (the upper layer, fs_root) over the image's unpacked
   files (the lower layer, fs_lower), which every container of the image shares and
   none ever writes, so they stay warm in the host's cache from one container to the
   next. An upper entry hides the lower one; in an upper directory ".wh.NAME" hides
   NAME below (a whiteout) and ".wh..wh..opq" the whole lower directory (opaque) - the
   layer tar format, so the engine reads the upper layer as a layer. Names starting
   ".wh." are reserved: the guest neither sees nor makes them (ENOENT). Changing a lower file
   first copies it up (an APFS clone: instant, sharing blocks); changing anything in a
   lower directory first creates the directory, and those above it, in the upper layer
   with the lower one's mode and times. Host calls that change anything get upper
   descriptors only (ov_prepare, ov_unlink, ov_rename): a lower descriptor reaches
   reads alone. Paths resolve by canonical guest path (directories keep theirs).
   ponytail: so a working directory renamed while in use keeps its old path (Linux
   follows the inode); lower directories cannot be renamed at all (EXDEV, as overlayfs
   without redirect_dir), so only directories made in the container are affected. */
#ifdef __APPLE__
#include <sys/clonefile.h>
#define ov_rename_excl(a,b,c,d) renameatx_np(a,b,c,d,RENAME_EXCL)
#else
#define CLONE_NOFOLLOW 1
static int clonefileat(int a,const char *b,int c,const char *d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e;errno=ENOTSUP;return -1; }
#define ov_rename_excl(a,b,c,d) (linkat(a,b,c,d,0)?-1:unlinkat(a,b,0))
#endif
#define OV_WH ".wh."
#define OV_OPQ ".wh..wh..opq"
/* The last walk's leaf: layer 0 upper, 1 lower, 2 neither; canon its guest path, ""
   outside the layers (a bind mount); whiteout: ov_prepare found ".wh.NAME" for it;
   bare: the fast walk found it in neither layer and its directory in the upper one,
   so no whiteout names it (one is made only for a lower entry, and the lower layer
   never changes): ov_prepare and ov_rename make no host call to learn so. */
typedef struct { int layer,whiteout,bare;char canon[1025]; } OvLeaf;
static _Thread_local OvLeaf ov_leaf;
/* What ov_walk_fast found for a path in the lower layer or in neither, by canonical
   guest path, so a repeated lookup (nginx's index.html per request, a compiler's
   headers, node's module search) makes no host call: macOS serializes path lookups
   machine-wide (about 100k opens and 1M stats a second, whatever the thread count).
   The lower layer never changes; the upper one only through ov_prepare, ov_unlink and
   ov_rename (each marks its call, ov_changed counts it when the call ends) and
   dockivad, which count changes in --layers-gen, counters every run of the container
   maps: [0] any change; [1] an entry made where the upper layer has its directory, or
   an upper file renamed (a build's outputs, directories and cache entries, npm's
   packages: hundreds a second), which changes the answer for
   its own path and for paths below it, in [2+bucket of its path] too. An entry holds
   for the counts it was made under: [0] and its path's bucket, and [1] for a missing
   directory. Upper files are not kept: their stat changes with every write. Removing
   an upper file with nothing below it changes no entry. Entries are seqlocked: no lock
   is taken. */
#define OV_CACHE_SLOTS 32768
#define OV_CACHE_PATH 192
#define OV_BUCKETS 4096
typedef struct { uint64_t gen,made,bucket; } OvStamp;
typedef struct { uint64_t seq;OvStamp at;unsigned char layer,lower,follow;char path[OV_CACHE_PATH];struct stat st; } OvCached;
static uint64_t *ov_gen;static OvCached *ov_cache;
typedef struct OvLink OvLink;static OvLink *ov_links;   /* ov_link_get */
#define OV_LINK_SLOTS 4096
#define OV_LINK_KEY 256
#define OV_LINK_PATHS 24
struct OvLink { uint64_t seq,gen,sum;unsigned mounts;unsigned short from,length,count;unsigned char follow;unsigned short bucket[OV_LINK_PATHS];char key[OV_LINK_KEY],to[OV_LINK_KEY]; };
static _Thread_local int ov_dirty;
static _Thread_local char ov_made[1025];                              /* the call's file made (ov_prepare's fast way) */
static uint32_t ov_hash(const char *path) { uint32_t h=2166136261u;for(const char *p=path;*p;p++)h=(h^(unsigned char)*p)*16777619u;return h; }
static uint64_t *ov_bucket(uint32_t h) { return &ov_gen[2+((h>>16)&(OV_BUCKETS-1))]; }
static void ov_bump(const char *path) { __atomic_fetch_add(&ov_gen[1],1,__ATOMIC_RELEASE);__atomic_fetch_add(ov_bucket(ov_hash(path)),1,__ATOMIC_RELEASE); }   /* a change at path alone, after it */
static void ov_changed(void) {
    if(ov_made[0]) {
        if(ov_gen && !ov_dirty)ov_bump(ov_made);
        ov_made[0]=0;
    }
    if(ov_dirty) { ov_dirty=0;if(ov_gen)__atomic_fetch_add(ov_gen,1,__ATOMIC_RELEASE); }
}
static int ov_cache_open(const char *file) {
    size_t size=(2+OV_BUCKETS)*sizeof(uint64_t);
    int fd=open(file,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    struct stat st;void *p=fstat(fd,&st) || (st.st_size<(off_t)size && ftruncate(fd,(off_t)size))?MAP_FAILED:mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(p==MAP_FAILED)return -1;
    void *cache=mmap(NULL,(OV_CACHE_SLOTS*sizeof(OvCached)+16383)&~(size_t)16383,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);   /* page-aligned: EL1 code reads it (hvf_el1_setup) */
    if(cache==MAP_FAILED)return -1;
    ov_cache=cache;
    ov_links=calloc(OV_LINK_SLOTS,sizeof(OvLink));                     /* none: every symlinked path walks */
    ov_gen=p;return 0;
}
/* The counts a lookup of path starts under: read before any host call (ov_cache_put). */
static void ov_stamp(const char *path,OvStamp *at) {
    at->gen=__atomic_load_n(&ov_gen[0],__ATOMIC_ACQUIRE);at->made=__atomic_load_n(&ov_gen[1],__ATOMIC_ACQUIRE);
    at->bucket=__atomic_load_n(ov_bucket(ov_hash(path)),__ATOMIC_ACQUIRE);
}
/* A path's two slots (a std library's files outnumber a direct-mapped table's). */
static OvCached *ov_slot(const char *path,int follow,int way) { return &ov_cache[((ov_hash(path)*2u+(unsigned)follow)^(unsigned)(way*2))&(OV_CACHE_SLOTS-1)]; }
static int ov_cache_read(OvCached *e,const char *path,int follow,OvCached *out) {
    uint64_t s=__atomic_load_n(&e->seq,__ATOMIC_ACQUIRE);
    if(!s || (s&1))return 0;
    memcpy(out,e,sizeof(*out));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&e->seq,__ATOMIC_RELAXED)==s && out->follow==follow && !strncmp(out->path,path,OV_CACHE_PATH);
}
static int ov_cache_get(const char *path,int follow,const OvStamp *at,OvCached *out) {
    for(int way=0;way<2;way++)if(ov_cache_read(ov_slot(path,follow,way),path,follow,out))
        return out->at.gen==at->gen && out->at.bucket==at->bucket && (out->layer!=3 || out->at.made==at->made);
    return 0;
}
static void ov_cache_put(const char *path,int follow,const OvStamp *at,int layer,int lower,const struct stat *st) {
    size_t n=strlen(path);if(n>=OV_CACHE_PATH)return;
    OvCached *e=ov_slot(path,follow,0),*other=ov_slot(path,follow,1),copy;   /* its own slot, else an empty one, else by the hash */
    if(!ov_cache_read(e,path,follow,&copy) && (ov_cache_read(other,path,follow,&copy) ||
       (__atomic_load_n(&e->seq,__ATOMIC_RELAXED) && (!__atomic_load_n(&other->seq,__ATOMIC_RELAXED) || ov_hash(path)>>31))))e=other;
    uint64_t s=__atomic_load_n(&e->seq,__ATOMIC_RELAXED);
    if((s&1) || !__atomic_compare_exchange_n(&e->seq,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;   /* another writer has it */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->at=*at;e->layer=(unsigned char)layer;e->lower=(unsigned char)lower;e->follow=(unsigned char)follow;memcpy(e->path,path,n+1);
    if(st)e->st=*st;
    __atomic_store_n(&e->seq,s+2,__ATOMIC_RELEASE);
}
/* Where the component walk took a path with symlinks on the way (by its start
   directory, the path and follow), so the next lookup is ov_walk_fast of the result:
   the walk opens every directory (30 to 60 us; Debian's /lib and /bin are links, so
   is every node_modules/.bin entry). An entry holds while [0] and the buckets of every
   path the walk passed hold: a link or a directory on the way changes by a rename or
   a lower entry's removal ([0]), or a removal and a new entry there (its bucket; an
   upper symlink's removal counts in its bucket too, ov_unlink). Seqlocked as ov_cache. */
typedef struct { uint64_t gen,sum;unsigned mounts,count;int ok;unsigned short bucket[OV_LINK_PATHS]; } OvLinkWalk;
static unsigned ov_link_mounts(void) { return fs_mount_count*1024u+mem_mount_count; }
static OvLink *ov_link_slot(const char *from,const char *path,int follow) { return &ov_links[(ov_hash(from)*31u^ov_hash(path)^(unsigned)follow)&(OV_LINK_SLOTS-1)]; }
/* A path the walk passes: its bucket's count now, before any host call about it. */
static void ov_link_pass(OvLinkWalk *k,const char *path) {
    if(k->count==OV_LINK_PATHS) { k->ok=0;return; }
    unsigned b=(ov_hash(path)>>16)&(OV_BUCKETS-1);k->bucket[k->count++]=(unsigned short)b;
    k->sum+=__atomic_load_n(&ov_gen[2+b],__ATOMIC_ACQUIRE);
}
static int ov_link_get(const char *from,const char *path,int follow,char to[1025]) {
    if(!ov_links)return 0;
    OvLink *e=ov_link_slot(from,path,follow),c;size_t f=strlen(from),n=f+1+strlen(path);
    if(n>=OV_LINK_KEY)return 0;
    uint64_t s=__atomic_load_n(&e->seq,__ATOMIC_ACQUIRE);
    if(!s || (s&1))return 0;
    memcpy(&c,e,sizeof(c));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(__atomic_load_n(&e->seq,__ATOMIC_RELAXED)!=s || c.follow!=follow || c.from!=f || c.length!=n || c.count>OV_LINK_PATHS ||
       memcmp(c.key,from,f+1) || memcmp(c.key+f+1,path,n-f) || c.mounts!=ov_link_mounts() || c.gen!=__atomic_load_n(&ov_gen[0],__ATOMIC_ACQUIRE))return 0;
    uint64_t sum=0;for(unsigned i=0;i<c.count;i++)sum+=__atomic_load_n(&ov_gen[2+c.bucket[i]],__ATOMIC_ACQUIRE);
    if(sum!=c.sum)return 0;
    c.to[OV_LINK_KEY-1]=0;strcpy(to,c.to);return 1;
}
static void ov_link_put(const char *from,const char *path,int follow,const OvLinkWalk *k,const char *to) {
    size_t f=strlen(from),n=f+1+strlen(path),t=strlen(to);
    if(!ov_links || !k->ok || n>=OV_LINK_KEY || t>=OV_LINK_KEY)return;
    OvLink *e=ov_link_slot(from,path,follow);
    uint64_t s=__atomic_load_n(&e->seq,__ATOMIC_RELAXED);
    if((s&1) || !__atomic_compare_exchange_n(&e->seq,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->gen=k->gen;e->sum=k->sum;e->mounts=k->mounts;e->count=(unsigned short)k->count;memcpy(e->bucket,k->bucket,k->count*sizeof(*k->bucket));
    e->from=(unsigned short)f;e->length=(unsigned short)n;e->follow=(unsigned char)follow;
    memcpy(e->key,from,f+1);memcpy(e->key+f+1,path,n-f);memcpy(e->to,to,t+1);
    __atomic_store_n(&e->seq,s+2,__ATOMIC_RELEASE);
}
static int ov_hidden(const char *name) { return !strncmp(name,OV_WH,4); }
static int ov_marker(int dir,const char *name) {                  /* ".wh.NAME" (name NULL: the opaque marker) exists */
    char wh[300];struct stat st;
    if(!name)strcpy(wh,OV_OPQ);else if(snprintf(wh,sizeof wh,OV_WH "%s",name)>255)return 0;
    return !fstatat(dir,wh,&st,AT_SYMLINK_NOFOLLOW);
}
static int ov_mark(int dir,const char *name) {                    /* make ".wh.NAME", or the opaque marker */
    char wh[300];
    if(!name)strcpy(wh,OV_OPQ);else if(snprintf(wh,sizeof wh,OV_WH "%s",name)>255) { errno=ENAMETOOLONG;return -1; } /* ponytail: APFS names stop at 255 bytes */
    int fd=openat(dir,wh,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    close(fd);return 0;
}
static void ov_unmark(int dir,const char *name) { char wh[300];if(snprintf(wh,sizeof wh,OV_WH "%s",name)<=255)unlinkat(dir,wh,0); }
static void ov_close2(int a,int b) { int saved=errno;if(a>=0)close(a);if(b>=0)close(b);errno=saved; }
static int ov_subdir(int dir,const char *name) { return openat(dir,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); }
/* A directory stream from its start (a dup shares the descriptor's position). */
static DIR *ov_opendir(int dir) {
    int copy=dup(dir);DIR *s=copy>=0 && lseek(copy,0,SEEK_SET)==0?fdopendir(copy):NULL;
    if(!s)ov_close2(copy,-1);
    return s;
}
/* The next path component of *p (advanced past it) into part; 0 at the end. */
static int ov_part(const char **p,char part[256]) {
    while(**p=='/')(*p)++;
    const char *q=*p;while(*q && *q!='/')q++;
    if(q==*p || q-*p>255)return 0;
    memcpy(part,*p,(size_t)(q-*p));part[q-*p]=0;*p=q;return 1;
}
/* A canonical guest directory's upper and lower descriptors (-1: none there, or the
   lower one hidden), or, inside a bind mount (*mount its index), the mount's alone. */
static int ov_pair(const char *canon,int *upper,int *lower,int *mount) {
    int m=fs_mount_count?fs_mount_of(canon):0;const char *p=canon;char part[256];
    *mount=-1;*lower=-1;
    if(m>0) {
        if(!fs_mounts[m-1].directory) { errno=ENOTDIR;return -1; }
        *mount=m-1;p+=strlen(fs_mounts[m-1].path);*upper=dup(fs_mounts[m-1].fd);if(*upper<0)return -1;
    } else {
        /* Most directories are in one layer only: a path the lower layer lacks is
           the upper one's, found in one call (and the reverse needs the walk). */
        struct stat st;
        if(canon[1] && fstatat(fs_lower,canon+1,&st,FS_NOFOLLOW_AT) && (errno==ENOENT || errno==ENOTDIR)) {
            *upper=openat(fs_root,canon+1,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC);
            if(*upper>=0 || errno!=ELOOP)return *upper<0?-1:0;
        }
        *upper=dup(fs_root);*lower=dup(fs_lower);
        if(*upper<0 || *lower<0) { ov_close2(*upper,*lower);return -1; }
    }
    while(ov_part(&p,part)) {
        int u=*upper>=0?ov_subdir(*upper,part):-1;
        if(u<0 && *upper>=0 && errno!=ENOENT) { ov_close2(*upper,*lower);return -1; }   /* a file or link hides what is below */
        int l=-1;
        if(*lower>=0 && !(u>=0?ov_marker(u,NULL):*upper>=0 && ov_marker(*upper,part)))l=ov_subdir(*lower,part);
        ov_close2(*upper,*lower);*upper=u;*lower=l;
        if(u<0 && l<0) { errno=ENOENT;return -1; }
    }
    return 0;
}
/* Mode bits as the guest set them; macOS refuses setgid on a file whose group the
   user is not in (a home under /tmp is group wheel): such a file takes the user's. */
static int ov_chmod(int dir,const char *name,mode_t mode) {
    if(!fchmodat(dir,name,mode,AT_SYMLINK_NOFOLLOW))return 0;
    if(errno!=EPERM || !(mode&S_ISGID) || fchownat(dir,name,(uid_t)-1,getegid(),AT_SYMLINK_NOFOLLOW))return -1;
    return fchmodat(dir,name,mode,AT_SYMLINK_NOFOLLOW);
}
/* A canonical guest directory of the root's layers in the upper layer: it and those
   above it are made from the lower ones where missing. Returns its descriptor. */
static int ov_upper(const char *canon) {
    if(canon[1]) { int d=openat(fs_root,canon+1,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC);if(d>=0)return d; } /* there already */
    int u=dup(fs_root),l=dup(fs_lower);const char *p=canon;char part[256];
    if(u<0 || l<0) { ov_close2(u,l);return -1; }
    while(ov_part(&p,part)) {
        int nu=ov_subdir(u,part),nl=l>=0?ov_subdir(l,part):-1;
        if(nu<0 && errno==ENOENT) {
            struct stat st;
            if(nl<0 || fstat(nl,&st) || ov_marker(u,part)) { ov_close2(u,l);ov_close2(nl,-1);errno=ENOENT;return -1; }
            int made=!mkdirat(u,part,0700);
            if(!made && errno!=EEXIST) { ov_close2(u,l);ov_close2(nl,-1);return -1; }
            if(made) {
                struct timespec times[2]={st.st_atimespec,st.st_mtimespec};
                (void)ov_chmod(u,part,st.st_mode&07777);(void)utimensat(u,part,times,AT_SYMLINK_NOFOLLOW);
                ov_dirty=1;                                            /* a lower directory now shows from above (fchmod of its descriptor): ov_changed */
            }
            nu=ov_subdir(u,part);
        }
        if(nu<0) { ov_close2(u,l);ov_close2(nl,-1);return -1; }
        if(nl>=0 && ov_marker(nu,NULL)) { close(nl);nl=-1; }        /* an opaque directory: nothing lower is seen */
        ov_close2(u,l);u=nu;l=nl;
    }
    ov_close2(l,-1);return u;
}
/* Split a canonical leaf path "/a/b/name" into its directory "/a/b" and name. */
static void ov_split(const char *canon,char dir[1025],char name[256]) {
    const char *slash=strrchr(canon,'/');size_t n=(size_t)(slash-canon);
    memcpy(dir,canon,n);dir[n?n:1]=0;if(!n)dir[0]='/';
    snprintf(name,256,"%s",slash+1);
}
/* The lower directory of a visible lower leaf's parent (no link on the way: it is canonical). */
static int ov_lower_dir(const char *dir) {
    return dir[1]?openat(fs_lower,dir+1,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC):dup(fs_lower);
}
/* Copy lower file name (in lower directory l) up into upper directory u: a clone made
   aside and renamed in unless another process copied it first. */
static int ov_copy_file(int u,int l,const char *name) {
    static unsigned serial;char tmp[64];struct stat st;
    snprintf(tmp,sizeof tmp,OV_WH OV_WH "cp%d.%u",(int)getpid(),__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED));
    if(fstatat(l,name,&st,AT_SYMLINK_NOFOLLOW))return -1;
    if(S_ISDIR(st.st_mode)) { errno=EISDIR;return -1; }
    if(clonefileat(l,name,u,tmp,CLONE_NOFOLLOW)) {                   /* another volume: copy */
        if(S_ISLNK(st.st_mode)) {
            char target[1025];ssize_t n=readlinkat(l,name,target,1024);if(n<0)return -1;
            target[n]=0;if(symlinkat(target,u,tmp))return -1;
        } else {
            int in=openat(l,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(in<0)return -1;
            int out=openat(u,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
            if(out<0) { ov_close2(in,-1);return -1; }
            char buffer[65536];ssize_t got;int bad=0;
            while((got=read(in,buffer,sizeof buffer))>0)if(write(out,buffer,(size_t)got)!=got) { bad=1;break; }
            struct timespec times[2]={st.st_atimespec,st.st_mtimespec};
            if(got<0 || bad || futimens(out,times)) { ov_close2(in,out);unlinkat(u,tmp,0);return -1; }
            ov_close2(in,out);
        }
    }
    if(!S_ISLNK(st.st_mode) && ov_chmod(u,tmp,st.st_mode&07777)) { unlinkat(u,tmp,0);return -1; }   /* a clone drops setuid and setgid */
    if(ov_rename_excl(u,tmp,u,name)) { int saved=errno;unlinkat(u,tmp,0);errno=saved;return errno==EEXIST?0:-1; }
    return 0;
}
/* Before a host call changes the walk's leaf: replace parent with the leaf's upper
   directory (made where missing) and leaf with its name. OV_CREATE: the leaf is to be
   made (EEXIST if the guest sees one); OV_MODIFY: a lower leaf is copied up first (a
   missing one is left for the call to report). Returns -1 with errno, 0 otherwise. */
#define OV_CREATE 0
#define OV_MODIFY 1
#define OV_DIRECT 2   /* with OV_CREATE: the caller passes leaf only to host calls refusing symlinks on the way (FS_NOFOLLOW_*) */
/* A canonical directory the upper layer already has, as the path of dir/name below
   fs_root, for host calls that refuse symlinks on the way: one stat (about 2 us)
   instead of ov_upper's open (about 10). 0 when it is not there (ov_upper makes it). */
static int ov_upper_known(const char *dir,const char *name,char path[1025],int bare) {
    struct stat st;size_t d=strlen(dir+1),n=strlen(name);
    if(!FS_DIRECT || d+1+n>1024 || (d && !bare && (fstatat(fs_root,dir+1,&st,FS_NOFOLLOW_AT) || !S_ISDIR(st.st_mode))))return 0;
    memcpy(path,dir+1,d);if(d)path[d++]='/';memcpy(path+d,name,n+1);return 1;
}
/* ov_marker of name in canonical directory dir, by path below fs_root. */
static int ov_marker_at(const char *dir,const char *name) {
    char wh[1400];struct stat st;
    if(snprintf(wh,sizeof wh,"%s%s" OV_WH "%s",dir+1,dir[1]?"/":"",name)>=(int)sizeof wh || strlen(name)>251)return 1; /* too long: the caller's slow way decides */
    return !fstatat(fs_root,wh,&st,FS_NOFOLLOW_AT);
}
static int ov_prepare(OvLeaf *w,int *parent,char leaf[1025],int how) {
    int direct=how&OV_DIRECT;how&=~OV_DIRECT;
    w->whiteout=0;
    if(fs_lower<0 || !w->canon[0])return 0;                          /* no layers, or a bind mount */
    if(how==OV_CREATE && w->layer!=2) { errno=EEXIST;return -1; }
    if(w->layer==0 || (how==OV_MODIFY && w->layer==2))return 0;      /* no layer changes (Go touches its cache files' times) */
    char dir[1025],name[256];
    if(!strcmp(w->canon,"/")) { strcpy(dir,"/");strcpy(name,"."); }
    else ov_split(w->canon,dir,name);
    char path[1025];
    if(how==OV_CREATE && strcmp(name,".") && ov_upper_known(dir,name,path,w->bare) && (w->bare || !ov_marker_at(dir,name))) {
        /* Most entries (files, a build's directories, npm's packages and links) are made
           where the upper layer has the directory: the upper layer changes at this path
           alone (ov_changed), and the lookups cached elsewhere hold. */
        if(ov_made[0])ov_dirty=1;else strcpy(ov_made,w->canon);
        int u=direct?dup(fs_root):ov_upper(dir);if(u<0)return -1;     /* direct: by path; else the directory, opened link-free */
        close(*parent);*parent=u;strcpy(leaf,direct?path:name);fs_walk_leaf.known=direct && w->bare?2:0;return 0;   /* bare: still missing */
    }
    ov_dirty=1;                                                        /* the upper layer may change: ov_changed */
    if(how==OV_MODIFY) {                                             /* a lower leaf */
        struct stat st;int l=ov_lower_dir(dir);if(l<0)return -1;
        int r=fstatat(l,strcmp(name,".")?name:".",&st,AT_SYMLINK_NOFOLLOW);
        if(!r && S_ISDIR(st.st_mode)) { int d=ov_upper(w->canon);r=d<0?-1:0;ov_close2(d,-1); }
        else if(!r) { int u=ov_upper(dir);r=u<0?-1:ov_copy_file(u,l,name);ov_close2(u,-1); }
        ov_close2(l,-1);if(r)return -1;
    }
    int u=ov_upper(dir);if(u<0)return -1;
    if(how==OV_CREATE)w->whiteout=ov_marker(u,name);
    close(*parent);*parent=u;strcpy(leaf,name);
    fs_walk_leaf.known=0;w->layer=how==OV_MODIFY?0:w->layer;return 0;
}
/* After a leaf ov_prepare (OV_CREATE) made: its whiteout goes, and a directory made
   where a lower one was deleted hides that one's files (opaque). */
static void ov_created(OvLeaf *w,int parent,const char *leaf,int directory) {
    if(fs_lower<0 || !w->canon[0] || !w->whiteout)return;
    if(directory) { int d=ov_subdir(parent,leaf);if(d>=0) { (void)ov_mark(d,NULL);close(d); } }
    ov_unmark(parent,leaf);
}
/* Whether a merged directory holds nothing the guest sees; clean: then remove its
   markers from the upper one, so the host can remove or replace it. */
static int ov_empty(const char *canon,int clean) {
    int u,l,m;if(ov_pair(canon,&u,&l,&m))return 0;
    int empty=1;struct dirent *e;
    if(u>=0) {
        DIR *s=ov_opendir(u);
        if(!s)empty=0;
        else { while((e=readdir(s))!=NULL)if(strcmp(e->d_name,".") && strcmp(e->d_name,"..") && !ov_hidden(e->d_name))empty=0;closedir(s); }
    }
    if(empty && l>=0) {
        DIR *s=ov_opendir(l);
        if(!s)empty=0;
        else { while(empty && (e=readdir(s))!=NULL)if(strcmp(e->d_name,".") && strcmp(e->d_name,"..") && !(u>=0 && ov_marker(u,e->d_name)))empty=0;closedir(s); }
    }
    if(empty && clean && u>=0) {
        DIR *s=ov_opendir(u);
        if(s) { while((e=readdir(s))!=NULL)if(ov_hidden(e->d_name))unlinkat(u,e->d_name,0);closedir(s); }
    }
    ov_close2(u,l);return empty;
}
/* Whether the lower layer shows a canonical leaf (a whiteout made for it must stay).
   ov_lower_absent: it has nothing at that path at all (nor below it). */
static _Thread_local int ov_lower_absent;
static int ov_lower_has(const char *canon,struct stat *st) {
    /* Most leaves (a container's own files) are not in the image at all: one stat says
       so, where the walk opens every directory on the way (a symlink there: the walk). */
    ov_lower_absent=0;
    if(canon[1] && fstatat(fs_lower,canon+1,st,FS_NOFOLLOW_AT) && (errno==ENOENT || errno==ENOTDIR)) { ov_lower_absent=1;return 0; }
    char dir[1025],name[256];ov_split(canon,dir,name);
    int u,l,m;if(ov_pair(dir,&u,&l,&m))return 0;
    int has=l>=0 && !(u>=0 && ov_marker(u,name)) && !fstatat(l,name,st,AT_SYMLINK_NOFOLLOW);
    ov_close2(u,l);return has;
}
/* unlinkat of the walk's leaf (w) in the root's layers: the upper entry goes and a
   whiteout hides a lower one. */
static int ov_unlink(OvLeaf *w,int parent,const char *leaf,int directory) {
    struct stat st,low;
    if(w->layer==2 || fstatat(parent,leaf,&st,FS_NOFOLLOW_AT)) { errno=ENOENT;return -1; }
    if(!strcmp(w->canon,"/")) { errno=EBUSY;return -1; }
    if(directory && !S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
    if(!directory && S_ISDIR(st.st_mode)) { errno=EPERM;return -1; }   /* the caller reports EISDIR, as Linux */
    if(directory && upm_fs && upm_has_files(w->canon)) { errno=ENOTEMPTY;return -1; }   /* files in memory (upm.h) */
    char dir[1025],name[256];ov_split(w->canon,dir,name);
    int lower=ov_lower_has(w->canon,&low);
    if(lower || directory)ov_dirty=1;                                  /* the upper layer's change shows (ov_changed) */
    else if(S_ISLNK(st.st_mode)) { if(ov_made[0])ov_dirty=1;else strcpy(ov_made,w->canon); }   /* ov_link_get: its bucket */
    /* A directory the lower layer lacks holds no markers (they are made only for lower
       entries): the host's rmdir says whether it is empty, without reading it. */
    int quick=directory && ov_lower_absent && w->layer==0;
    if(directory && !quick && !ov_empty(w->canon,w->layer==0)) { errno=ENOTEMPTY;return -1; }
    int u=lower?ov_upper(dir):-1;
    if(lower && (u<0 || ov_mark(u,name))) { ov_close2(u,-1);return -1; }   /* first: the name never shows the lower entry */
    if(w->layer==0) {
        int up=u>=0?u:parent;const char *at=u>=0?name:leaf;
        int r=unlinkat(up,at,(directory?AT_REMOVEDIR:0)|FS_ANY_AT);
        if(r && quick && errno==ENOTEMPTY)r=ov_empty(w->canon,1)?unlinkat(up,at,AT_REMOVEDIR|FS_ANY_AT):(errno=ENOTEMPTY,-1);   /* hidden names only: the slow way */
        if(r) { int saved=errno;if(lower)ov_unmark(u,name);ov_close2(u,-1);errno=saved;return -1; }
    }
    ov_close2(u,-1);return 0;
}
/* renameat in the root's layers: from (w, its parent and leaf) to (to, parent, leaf).
   A lower file is copied up first; a directory with lower files cannot move (EXDEV, as
   overlayfs without redirect_dir: mv copies it). */
static int ov_rename(OvLeaf *w,int parent,char leaf[1025],OvLeaf *to,int other,char target[1025]) {
    struct stat st,dst,low;
    if(w->layer==2 || fstatat(parent,leaf,&st,FS_NOFOLLOW_AT)) { errno=ENOENT;return -1; }
    if(!strcmp(w->canon,to->canon))return 0;
    if(!strcmp(w->canon,"/") || !strcmp(to->canon,"/")) { errno=EBUSY;return -1; }
    size_t n=strlen(w->canon);
    if(S_ISDIR(st.st_mode) && !strncmp(to->canon,w->canon,n) && to->canon[n]=='/') { errno=EINVAL;return -1; }
    char sdir[1025],sname[256],ddir[1025],dname[256];ov_split(w->canon,sdir,sname);ov_split(to->canon,ddir,dname);
    if(S_ISDIR(st.st_mode)) { int u,l,m;if(ov_pair(w->canon,&u,&l,&m))return -1;ov_close2(u,l);if(l>=0) { errno=EXDEV;return -1; } }
    if(to->layer!=2) {                                               /* replacing: the types must agree, a directory be empty */
        if(fstatat(other,target,&dst,FS_NOFOLLOW_AT))return -1;
        if(S_ISDIR(dst.st_mode) && !S_ISDIR(st.st_mode)) { errno=EISDIR;return -1; }
        if(!S_ISDIR(dst.st_mode) && S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
        if(S_ISDIR(dst.st_mode) && !ov_empty(to->canon,to->layer==0)) { errno=ENOTEMPTY;return -1; }
    }
    int lower=ov_lower_has(w->canon,&low),below=!to->bare && ov_lower_has(to->canon,&low) && S_ISDIR(low.st_mode);
    OvLeaf from=*w;int sp=dup(parent);char sl[1025];strcpy(sl,leaf);
    if(sp<0 || ov_prepare(&from,&sp,sl,OV_MODIFY)) { ov_close2(sp,-1);return -1; }   /* the source, copied up */
    close(sp);
    /* Nothing below either name and both directories in the upper layer (a file the
       container made, renamed into place): the rename alone, by path. */
    char spath[1025],dpath[1025];
    if(!lower && !below && ov_upper_known(sdir,sname,spath,0) && ov_upper_known(ddir,dname,dpath,to->bare) && (to->bare || !ov_marker_at(ddir,dname))) {
        int r=fs_renameat(fs_root,spath,fs_root,dpath);
        /* A file (npm's and Go's write-then-rename): its two paths change alone, as a
           file made (ov_changed); a directory moves what is below it too. */
        if(!r && !S_ISDIR(st.st_mode) && ov_gen) { ov_bump(w->canon);ov_bump(to->canon); }
        else if(!r)ov_dirty=1;
        return r;
    }
    ov_dirty=1;                                                        /* the upper layer changes: ov_changed */
    if((sp=ov_upper(sdir))<0)return -1;
    int dp=ov_upper(ddir);if(dp<0) { ov_close2(sp,-1);return -1; }
    int had=ov_marker(dp,dname);
    if(lower && ov_mark(sp,sname)) { ov_close2(sp,dp);return -1; }
    if(fs_renameat(sp,sname,dp,dname)) { int saved=errno;if(lower)ov_unmark(sp,sname);ov_close2(sp,dp);errno=saved;return -1; }
    if(had)ov_unmark(dp,dname);
    if(S_ISDIR(st.st_mode) && (below || had)) { int d=ov_subdir(dp,dname);if(d>=0) { (void)ov_mark(d,NULL);close(d); } }
    ov_close2(sp,dp);return 0;
}
/* ov_walk for a path with no ".." (canonical but for symlinks, which the host refuses
   with ELOOP): a few host stats decide the layer. Returns -2 when the component walk
   must decide (a symlink, a whiteout or opaque directory on the way, a mount). */
static int ov_walk_fast(const char *start,const char *rest,int follow,int direct,char leaf[1025],char canon[1025]) {
    size_t n=strlen(rest);char trimmed[1025];
    if(n && rest[n-1]=='/') { while(n && rest[n-1]=='/')n--;memcpy(trimmed,rest,n);trimmed[n]=0;rest=trimmed; }
    if(!n)return -2;
    for(const char *p=rest;*p;) {
        while(*p=='/')p++;
        const char *q=p;while(*q && *q!='/')q++;
        if(q-p>255 || (q-p==2 && p[0]=='.' && p[1]=='.') || (q-p>=4 && ov_hidden(p)))return -2;
        p=q;
    }
    const char *slash=strrchr(rest,'/'),*name=slash?slash+1:rest;
    if(!strcmp(name,"."))return -2;
    char full[1025];strcpy(full,rest[0]=='/'?"/":start);
    for(const char *p=rest;;) {
        char part[256];if(!ov_part(&p,part))break;
        if(strcmp(part,".") && fs_canon_push(full,part)<0)return -1;
    }
    if(!strcmp(full,"/"))return -2;
    if(mem_mount_count && mem_mount_inside(full))return -2;
    int to=fs_mount_count?fs_mount_of(full):0;
    if(to) {   /* in a bind mount: from its descriptor when the way to it is plain (as fs_walk_fast) */
        if(to<0 || !fs_mounts[to-1].directory || !fs_mount_plain(to-1))return -2;
        const char *below=full+strlen(fs_mounts[to-1].path),*end=strrchr(full,'/');
        if(!*below || end<below)return -2;                           /* the mount itself */
        char inside[1025],path[1025];size_t m=(size_t)(end-below);memcpy(inside,below,m);inside[m]=0;
        const char *relative=inside;while(*relative=='/')relative++;
        int base=fs_mounts[to-1].fd,fd;struct stat st;
        if(direct) {
            size_t r=strlen(relative),l=strlen(end+1);if(r+1+l>1024)return -2;
            memcpy(path,relative,r);if(r)path[r++]='/';memcpy(path+r,end+1,l+1);
            int known=1;
            if(!fstatat(base,path,&st,FS_NOFOLLOW_AT)) { if(follow && S_ISLNK(st.st_mode))return -2;fs_walk_leaf.st=st; }
            else if(errno==ENOTDIR)return -1;
            else if(errno!=ENOENT)return -2;
            else {
                known=2;
                if(*relative) {
                    if(fstatat(base,relative,&st,FS_NOFOLLOW_AT))return errno==ENOENT || errno==ENOTDIR?-1:-2;
                    if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
                }
            }
            if((fd=dup(base))<0)return -1;
            strcpy(leaf,path);fs_walk_leaf.known=known;
        } else {
            fd=*relative?openat(base,relative,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC):dup(base);
            if(fd<0)return errno==ENOENT || errno==ENOTDIR?-1:-2;
            if(follow && !fstatat(fd,end+1,&st,AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode)) { close(fd);return -2; }
            strcpy(leaf,end+1);
        }
        fs_where=to;if(canon)strcpy(canon,full);return fd;
    }
    const char *rel=full+1,*last=strrchr(full,'/')+1;
    char dirs[1025];size_t d=(size_t)(last-1-rel);if(last-1<rel)d=0;
    memcpy(dirs,rel,d);dirs[d]=0;
    struct stat st,low_probe;int layer=2,top=0,hide=0,base;size_t k=0;    /* k: bytes of dirs the upper layer has */
    OvStamp at;if(ov_gen)ov_stamp(full,&at);
    OvCached hit;
    if(ov_gen && ov_cache_get(full,follow,&at,&hit)) {
        if(hit.layer==3) { errno=hit.lower?ENOTDIR:ENOENT;return -1; }   /* its directory is missing too */
        layer=hit.layer;st=hit.st;base=hit.lower?fs_lower:fs_root;goto found;
    }
    {   /* the first component: most paths of an image have nothing above them */
        char first[256];const char *p=rel;ov_part(&p,first);
        if(fstatat(fs_root,first,&st,AT_SYMLINK_NOFOLLOW)) {
            if(errno!=ENOENT || ov_marker(fs_root,first))return -2;
            top=1;
        } else if(S_ISLNK(st.st_mode) && (*p || follow))return -2;
    }
    if(!top) {
        if(!fstatat(fs_root,rel,&st,FS_NOFOLLOW_AT)) { if(follow && S_ISLNK(st.st_mode))return -2;layer=0;k=d; }
        else if(errno==ENOTDIR)return -1;
        else if(errno!=ENOENT)return -2;
        else {
            /* How far the upper layer goes: most often all the way (a file being made
               in the container's own directory) or the first component alone. */
            if(!d)return -2;
            if(!fstatat(fs_root,dirs,&st,FS_NOFOLLOW_AT) && S_ISDIR(st.st_mode))k=d;
            else for(const char *p=dirs;;) {
                char part[256];if(!ov_part(&p,part))break;
                size_t m=(size_t)(p-dirs);char prefix[1025];memcpy(prefix,dirs,m);prefix[m]=0;
                if(fstatat(fs_root,prefix,&st,FS_NOFOLLOW_AT)) { if(errno!=ENOENT)return -2;break; }
                if(!S_ISDIR(st.st_mode))return -2;
                k=m;
            }
            hide=1;
        }
    }
    if(layer==2) {
        if(!fstatat(fs_lower,rel,&st,FS_NOFOLLOW_AT)) { if(follow && S_ISLNK(st.st_mode))return -2;layer=1; }
        else if(errno==ELOOP)return -2;
        else if(errno!=ENOENT && errno!=ENOTDIR)return -2;
    }
    /* A lower entry beneath upper directories shows unless one of them is opaque or
       holds a whiteout for the next name (then the component walk decides). */
    for(const char *p=dirs;layer==1 && hide && (size_t)(p-dirs)<k;) {
        char part[256];if(!ov_part(&p,part))break;
        size_t m=(size_t)(p-dirs);const char *q=rel+m;char after[256],probe[1100];
        if(!ov_part(&q,after))break;
        snprintf(probe,sizeof probe,"%.*s/" OV_OPQ,(int)m,dirs);if(!fstatat(fs_root,probe,&low_probe,AT_SYMLINK_NOFOLLOW))return -2;
        if(snprintf(probe,sizeof probe,"%.*s/" OV_WH "%s",(int)m,dirs,after)<(int)sizeof probe && !fstatat(fs_root,probe,&low_probe,AT_SYMLINK_NOFOLLOW))return -2;
    }
    if(layer==2) {                                                   /* missing: does its directory exist? */
        if(d && k<d) {
            struct stat low;int notdir=-1;                           /* -1: the directory is there */
            if(fstatat(fs_lower,dirs,&low,FS_NOFOLLOW_AT)) { if(errno!=ENOENT && errno!=ENOTDIR)return -2;notdir=errno==ENOTDIR; }
            else if(!S_ISDIR(low.st_mode))notdir=1;
            if(notdir>=0) {
                if(ov_gen)ov_cache_put(full,follow,&at,3,notdir,NULL);
                errno=notdir?ENOTDIR:ENOENT;return -1;
            }
            base=fs_lower;
        } else base=fs_root;
    } else base=layer?fs_lower:fs_root;
    if(ov_gen && layer && !(layer==2 && upm_maybe(full)))ov_cache_put(full,follow,&at,layer,base==fs_lower,layer==1?&st:NULL);
  found:;
    if(layer==2 && base==fs_root && upm_fs) { int pending=upm_walk_hit(full);if(pending>0)return -1;if(pending<0)return -2; }   /* in memory (upm.h): redirected */
    int fd;
    if(direct) { fd=dup(base);if(fd<0)return -1;strcpy(leaf,rel);fs_walk_leaf.known=layer==2?2:1;if(layer!=2)fs_walk_leaf.st=st; }
    else {
        fd=d?openat(base,dirs,O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC):dup(base);
        if(fd<0)return errno==ENOENT || errno==ENOTDIR?-1:-2;
        strcpy(leaf,last);
    }
    ov_leaf.layer=layer;ov_leaf.bare=layer==2 && base==fs_root;strcpy(ov_leaf.canon,full);fs_where=0;if(canon)strcpy(canon,full);
    return fd;
}
/* Whether rest names its start directory itself: "." parts and slashes only. */
static int ov_dot(const char *rest) {
    int dots=0;
    for(const char *p=rest;*p;) {
        while(*p=='/')p++;
        if(!*p)break;
        if(p[0]!='.' || (p[1] && p[1]!='/'))return 0;
        dots=1;p++;
    }
    return dots;
}
/* fs_walk over the layers (see fs_walk): the path from base_path's canonical guest
   path, component by component, with each directory's upper and lower descriptors. */
static int ov_walk(int base,const char *base_path,const char *input,int follow,char leaf[1025],char canon[1025]) {
    int direct=follow&FS_DIRECT;follow&=1;
    fs_walk_leaf.known=0;ov_leaf.layer=2;ov_leaf.bare=0;ov_leaf.canon[0]=0;
    if(upm_fs)upm_walk_drains=__atomic_load_n(&upm_drains,__ATOMIC_ACQUIRE);   /* a write-back meanwhile: the walk redoes */
    size_t length=strlen(input);if(length>1024) { errno=ENAMETOOLONG;return -1; }
    const char *floor=fs_scope.root>=0?fs_scope.path:"/";char rest[4097],cur[1025];
    if(input[0]=='/') { if(fs_scope.resolve&8) { errno=EXDEV;return -1; } strcpy(cur,floor); }
    else {
        if(base!=fs_root && !(!fs_detached.on && fs_cwd && base==fs_cwd->fd)) {
            struct stat st;if(fstat(base,&st)<0)return -1;
            if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR;return -1; }
        }
        snprintf(cur,sizeof cur,"%s",base_path);
    }
    if(!*cur || !*floor) { errno=ENOENT;return -1; }                    /* a directory whose path is unknown */
    memcpy(rest,input,length+1);
    if(mem_mount_count && mem_mount_inside(cur))return mem_redirect_set(cur,rest);
    OvLinkWalk passed={0};char from[1025];
    if(fs_scope.root<0) {
        char to[1025];int fast;
        if(ov_link_get(cur,rest,follow,to) && (fast=ov_walk_fast("/",to,follow,direct,leaf,canon))!=-2)return fast;
        if((fast=ov_walk_fast(cur,rest,follow,direct,leaf,canon))!=-2)return fast;
        if(ov_gen && ov_links) { passed.ok=1;passed.gen=__atomic_load_n(&ov_gen[0],__ATOMIC_ACQUIRE);passed.mounts=ov_link_mounts();strcpy(from,cur); }
    }
    /* "." (or "./", ".//."): the directory itself, from the descriptor the call came
       with, not two directory opens below (ov_pair). Go's os.Getwd stats "." for each
       path its tools make absolute: the assembler 45 times a run, the compiler more.
       The upper layer's copy when it has one (overlayfs reports that one), else base
       (the lower's). The leaf stays ".", so rmdir and unlink refuse it as before. */
    if(input[0]!='/' && ov_dot(rest) && fs_scope.root<0 && !(fs_mount_count && fs_mount_of(cur))) {
        struct stat up,own;int fd=-1,layer=1;
        if(!fstatat(fs_root,cur[1]?cur+1:".",&up,FS_NOFOLLOW_AT) && S_ISDIR(up.st_mode)) {
            layer=0;
            fd=!fstat(base,&own) && own.st_ino==up.st_ino && own.st_dev==up.st_dev?dup(base):openat(fs_root,cur[1]?cur+1:".",O_RDONLY|O_DIRECTORY|FS_NOFOLLOW_OPEN|O_CLOEXEC);
        } else if(errno==ENOENT || errno==ENOTDIR)fd=dup(base);
        if(fd>=0) {
            strcpy(leaf,".");if(canon)strcpy(canon,cur);
            fs_where=0;ov_leaf.layer=layer;strcpy(ov_leaf.canon,cur);
            return fd;
        }
    }
    int u,l,mount;unsigned links=0;size_t pos=0;
    if(ov_pair(cur,&u,&l,&mount))return -1;
    for(;;) {
        while(rest[pos]=='/')pos++;
        if(!rest[pos]) {                                             /* the path names cur itself */
            strcpy(leaf,".");if(canon)strcpy(canon,cur);
            fs_where=mount>=0?mount+1:0;
            if(mount<0) { ov_leaf.layer=u>=0?0:1;strcpy(ov_leaf.canon,cur); }
            int fd=u>=0?u:l;ov_close2(fd==u?l:u,-1);return fd;
        }
        size_t start=pos;while(rest[pos] && rest[pos]!='/')pos++;
        char part[256];size_t m=pos-start;
        if(m>255) { ov_close2(u,l);errno=ENAMETOOLONG;return -1; }
        memcpy(part,rest+start,m);part[m]=0;
        size_t after=pos;while(rest[after]=='/')after++;
        int last=!rest[after];
        if(!strcmp(part,"."))continue;
        if(!strcmp(part,"..")) {
            if(!strcmp(cur,floor)) { if(fs_scope.resolve&8) { ov_close2(u,l);errno=EXDEV;return -1; } continue; }
            fs_canon_pop(cur);ov_close2(u,l);
            if(ov_pair(cur,&u,&l,&mount))return -1;
            continue;
        }
        char at[1025];strcpy(at,cur);if(fs_canon_push(at,part)<0) { ov_close2(u,l);return -1; }
        if(passed.ok)ov_link_pass(&passed,at);
        if(mem_mount_count && mem_mount_at(at)>=0) { ov_close2(u,l);return mem_redirect_set(at,rest+pos); }
        if(fs_mount_count) {                                         /* a mount at this path replaces what the root has */
            int i=fs_mount_index(at);
            if(i>=0) {
                if(!fs_mounts[i].directory && !last) { ov_close2(u,l);errno=ENOTDIR;return -1; }
                ov_close2(u,l);u=dup(fs_mounts[i].fd);l=-1;mount=i;if(u<0)return -1;
                strcpy(cur,at);
                if(last) { strcpy(leaf,fs_mounts[i].directory?".":fs_mounts[i].name);if(canon)strcpy(canon,at);fs_where=i+1;return u; }
                continue;
            }
        }
        if(mount<0 && ov_hidden(part)) { ov_close2(u,l);errno=ENOENT;return -1; }   /* reserved: no such name */
        struct stat st;int layer=-1;
        {
            if(u>=0 && !fstatat(u,part,&st,AT_SYMLINK_NOFOLLOW))layer=0;
            else if(u>=0 && errno!=ENOENT) { ov_close2(u,l);return -1; }
            else if(l>=0 && !(u>=0 && ov_marker(u,part)) && !fstatat(l,part,&st,AT_SYMLINK_NOFOLLOW))layer=1;
        }
        if(layer>=0 && (!last || follow) && S_ISLNK(st.st_mode)) {
            if(++links>FS_LINK_LIMIT || (fs_scope.resolve&4)) { ov_close2(u,l);errno=ELOOP;return -1; }
            char target[1025];ssize_t n=readlinkat(layer?l:u,part,target,1024);
            if(n<=0) { int saved=n<0?errno:ENOENT;ov_close2(u,l);errno=saved;return -1; }
            target[n]=0;
            for(ssize_t i=0;i<n;i++)if((unsigned char)target[i]<32 || target[i]==127) { ov_close2(u,l);errno=EACCES;return -1; }
            size_t tail=strlen(rest+pos);
            if((size_t)n+tail>4096) { ov_close2(u,l);errno=ENAMETOOLONG;return -1; }
            memmove(rest+n,rest+pos,tail+1);memcpy(rest,target,(size_t)n);pos=0;
            if(target[0]=='/') {
                if(fs_scope.resolve&8) { ov_close2(u,l);errno=EXDEV;return -1; }
                ov_close2(u,l);strcpy(cur,floor);
                if(ov_pair(cur,&u,&l,&mount))return -1;
            }
            continue;
        }
        if(last && layer<0 && mount<0 && u>=0 && upm_fs) {                /* missing on disk: in memory? (upm.h) */
            int pending=upm_walk_hit(at);
            if(pending>0) { ov_close2(u,l);return -1; }
            if(pending<0 && !fstatat(u,part,&st,AT_SYMLINK_NOFOLLOW))layer=0;     /* written back meanwhile */
        }
        if(last) {
            strcpy(leaf,part);if(canon)strcpy(canon,at);
            fs_where=mount>=0?mount+1:0;
            int fd=layer==1?l:u>=0?u:l;ov_close2(fd==u?l:u,-1);
            if(mount<0) { ov_leaf.layer=layer<0?2:layer;strcpy(ov_leaf.canon,at); }
            if(mount<0 && links && passed.ok)ov_link_put(from,input,follow,&passed,at);
            return fd;
        }
        if(layer<0) { ov_close2(u,l);errno=ENOENT;return -1; }
        if(!S_ISDIR(st.st_mode)) { ov_close2(u,l);errno=ENOTDIR;return -1; }
        int nu=-1,nl=-1;
        if(layer==0) {
            nu=ov_subdir(u,part);
            if(nu<0) { ov_close2(u,l);return -1; }
            if(mount<0 && l>=0 && !ov_marker(nu,NULL))nl=ov_subdir(l,part);
        } else if((nl=ov_subdir(l,part))<0) { ov_close2(u,l);return -1; }
        ov_close2(u,l);u=nu;l=nl;strcpy(cur,at);
    }
}
/* A directory's merged listing (getdents of a directory in the root's layers): upper
   names (not the reserved ones), then lower names neither shown above nor whited out.
   Records are d_ino (8), d_type (1), the name and its NUL. NULL when one layer serves. */
typedef struct OvList { char *data;size_t size,at; } OvList;
static int ov_name_compare(const void *a,const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }
static int ov_list_add(OvList *list,size_t *capacity,uint64_t ino,unsigned char type,const char *name) {
    size_t length=strlen(name);
    if(list->size+10+length>*capacity) {
        size_t grow=*capacity?*capacity*2:8192;while(grow<list->size+10+length)grow*=2;
        char *next=malloc(grow);if(!next)return -1;                  /* (malloc is the charged one: resources.h) */
        if(list->size)memcpy(next,list->data,list->size);free(list->data);list->data=next;*capacity=grow;
    }
    memcpy(list->data+list->size,&ino,8);list->data[list->size+8]=(char)type;
    memcpy(list->data+list->size+9,name,length+1);list->size+=10+length;return 0;
}
static OvList *ov_list(const char *canon) {
    int u,l,m;if(ov_pair(canon,&u,&l,&m))return NULL;
    int pending=upm_fs && upm_has_files(canon);                       /* files in memory (upm.h) list after the upper's */
    if(u<0 || (l<0 && !pending)) { ov_close2(u,l);return NULL; }
    OvList *list=calloc(1,sizeof(*list)),hidden={0};size_t capacity=0,hidden_capacity=0,names=0;char **seen=NULL;
    int ok=list!=NULL;struct dirent *e;
    DIR *s=ok?ov_opendir(u):NULL;
    if(!s)ok=0;
    while(ok && (e=readdir(s))!=NULL) {                               /* upper names, and whiteouts' names into hidden */
        if(!ov_hidden(e->d_name))ok=!ov_list_add(list,&capacity,e->d_ino,e->d_type,e->d_name);
        if(ok && strcmp(e->d_name,OV_OPQ))ok=!ov_list_add(&hidden,&hidden_capacity,0,0,ov_hidden(e->d_name)?e->d_name+4:e->d_name);
        names++;
    }
    if(s)closedir(s);
    if(ok && names && !(seen=malloc(names*sizeof(*seen))))ok=0;
    names=0;
    for(size_t at=0;ok && at<hidden.size;at+=10+strlen(hidden.data+at+9))seen[names++]=hidden.data+at+9;
    if(ok)qsort(seen,names,sizeof(*seen),ov_name_compare);
    s=ok && l>=0?ov_opendir(l):NULL;
    if(!s && l>=0)ok=0;
    while(ok && s && (e=readdir(s))!=NULL) {
        const char *key=e->d_name;
        if(!names || !bsearch(&key,seen,names,sizeof(*seen),ov_name_compare))ok=!ov_list_add(list,&capacity,e->d_ino,e->d_type,e->d_name);
    }
    if(s)closedir(s);
    if(ok && pending)ok=upm_list(canon,list,&capacity)==0;
    free(seen);free(hidden.data);ov_close2(u,l);
    if(!ok) { if(list)free(list->data);free(list);return NULL; }
    return list;
}
static void ov_list_free(struct OvList *list) { if(list) { free(list->data);free(list); } }
/* fchmod or futimens of a descriptor opened from the lower layer (fs_ro 2): a
   directory is copied up by its path and changed there. ponytail: a lower file says
   EROFS (Linux copies it up; its descriptor then still reads the lower file). */
static int ov_fd_upper(int slot) {
    if(FS(slot).ro!=2 || !FS(slot).dir || !FS(slot).dir->path[0] || !fs_writable) { errno=EROFS;return -1; }
    return ov_upper(FS(slot).dir->path);
}
