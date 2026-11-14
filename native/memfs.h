/* memfs: a tmpfs the runtime keeps itself (--tmpfs GUEST[:size=,mode=,uid=,gid=,noexec,ro]).
   Every file operation on it is a few memory accesses under one mutex, with no host
   system call: an APFS create costs 50-60 us, an open and close 12 us, which made
   scratch-heavy work (cp -a, tar, compilers' temporary files) several times slower
   than a Linux VM's ext4. Shared by every process of the run (they live in this host
   process); not by docker exec runs, which are host processes of their own.
   Linux semantics: directories with stable getdents offsets, sparse page-backed files
   charged to the memory budget (resource_charge, as tmpfs pages are to the cgroup),
   symlinks, hard links, rename (RENAME_NOREPLACE, RENAME_EXCHANGE), unlink while open,
   O_TMPFILE, owners, modes (checked), timestamps, xattrs, FIFOs and record locks.
   Paths: a guest path that lexically enters a mount point (no ".." on the host part)
   is resolved here; one that reaches it through a host symlink or ".." is found by
   fs_walk, which then fails with a redirect (mem_redirect), and the call is redone
   here. A memfs symlink to a host path, or ".." out of a mount, hands the call back to
   the host code with the path rewritten (fs_swap).
   Open files are I/O descriptors (IO_MEM) sharing a MemOpen description, so dup,
   fork, exec's close-on-exec, SCM_RIGHTS and close_range work as for pipes. mmap
   copies, as for host files (mapping.h): private mappings take a copy, shared ones a
   file share flushed back on msync and the last unmap. exec runs a host copy. */
#define MEM_MOUNTS 16
#define MEM_PAGE 4096u
#define MEM_LEAF 512u                                  /* pages per page-table leaf: 2 MiB */
#define MEM_FILE_MAX (UINT64_C(1)<<44)                  /* 16 TiB */
#define MEM_PASS (-4)                                   /* mem_route: not a memfs call */
typedef struct MemNode MemNode;
typedef struct MemFs { MemNode *root;uint64_t limit,used,inodes,inode_limit;unsigned index,noexec,readonly;char path[1025];size_t length; } MemFs;
typedef struct MemEntry { MemNode *dir,*node;struct MemEntry *next;uint64_t offset;uint32_t hash;char name[]; } MemEntry; /* node NULL: removed */
typedef struct { unsigned char *page[MEM_LEAF]; } MemLeaf;
typedef struct MemXattr { struct MemXattr *next;size_t size;char *name;unsigned char value[]; } MemXattr;
struct MemNode {
    uint32_t mode,uid,gid,nlink;      /* nlink: names (a directory: 2 + subdirectories) */
    uint32_t refs;                    /* open descriptions, working directories, shared mappings */
    unsigned char upper_only;         /* upm.h: a mirror of an upper directory the lower layer lacks (1), holds (2), not looked at (0) */
    unsigned char whiteouts;          /* upm.h: a mirror whose upper directory may hold whiteouts naming pending files (upm_move) */
    uint64_t ino,size;
    struct timespec atime,mtime,ctime;
    MemFs *fs;MemNode *parent;MemEntry *entry; /* a directory's parent and own name (NULL: the root, or removed) */
    MemXattr *xattrs;
    union {
        struct { MemEntry **list;uint32_t count,capacity,live;uint64_t next; } d; /* entries by offset; removed ones until compacted */
        struct { MemLeaf **leaf;uint64_t leaves,pages; } f;
        char *target;
    };
};
/* flags: open(2)'s access mode, O_APPEND, O_NONBLOCK, O_PATH; dir and name: where it was opened (held), for inotify. */
typedef struct MemOpen { MemNode *node,*dir;uint64_t position;unsigned refs,flags,written;char *name; } MemOpen;
static MemFs *mem_mounts[MEM_MOUNTS];
/* ponytail: one lock for every memfs (lookups, data copies); per-inode locks if threads of a build contend on it. */
static MemEntry **mem_table;static size_t mem_table_size,mem_table_used;
static uint64_t mem_next_ino=2;
static _Thread_local int mem_inside;                   /* the host code is serving this call: do not route it again */
#define MEM_DEV(fs) (UINT64_C(0x30)+(fs)->index)        /* st_dev: an anonymous device, as tmpfs's 0:N */
#pragma push_macro("malloc")
#pragma push_macro("free")
#undef malloc
#undef free
/* File pages bypass resource_malloc's header (4 KiB pages would round up); charged here. */
static unsigned char *mem_page_alloc(void) { return malloc(MEM_PAGE); }
static void mem_page_free(unsigned char *p) { free(p); }
#pragma pop_macro("malloc")
#pragma pop_macro("free")
static struct timespec mem_now(void) { struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return t; }
static int mem_is(const MemNode *n,uint32_t type) { return (n->mode&0170000)==type; }
#define MEM_DIR 0040000u
#define MEM_REG 0100000u
#define MEM_LNK 0120000u
/* rwx (4, 2, 1) for the caller: root passes, except execute on a file with no x bit.
   An upper directory's mirror (upm.h) is 0777 whatever the directory's mode: changing
   its entries is the host directory's to allow, as for the files on disk beside them
   (a user made files in a 0555 directory, and renamed ones not yet written back). */
static void mem_dir_path(const MemNode *n,char out[1025]);
static int mem_may(const MemNode *n,unsigned want) {
    int layer=n->fs==upm_fs;
    if(!guest_uid && !(layer && !fs_dac_can()))return !(want&1) || mem_is(n,MEM_DIR) || (n->mode&0111);   /* (the layer's files: root holding CAP_DAC_OVERRIDE, fs.h) */
    if(layer && (want&2) && mem_is(n,MEM_DIR)) {
        char path[1025];struct stat st;mem_dir_path(n,path);
        if(path[0] && !fstatat(fs_root,path[1]?path+1:".",&st,FS_NOFOLLOW_AT) && (st.st_mode&0300)!=0300)return 0;
    }
    unsigned bits=guest_uid==n->uid || layer?(n->mode>>6)&7:guest_gid==n->gid?(n->mode>>3)&7:n->mode&7;   /* upm.h: owned as host files are */
    return (bits&want)==want;
}
/* Removing or renaming node out of dir: the sticky bit keeps others' files. */
static int mem_sticky_ok(const MemNode *dir,const MemNode *node) {
    return !(dir->mode&01000) || !guest_uid || guest_uid==dir->uid || guest_uid==node->uid || dir->fs==upm_fs;
}
static MemNode *mem_node_new(MemFs *fs,uint32_t mode) {
    if(fs->inode_limit && fs->inodes>=fs->inode_limit)return NULL;
    MemNode *n=calloc(1,sizeof(*n));if(!n)return NULL;
    n->mode=mode;n->uid=guest_uid;n->gid=guest_gid;n->fs=fs;n->ino=mem_next_ino++;
    if(fs==upm_fs)n->ino|=UINT64_C(1)<<62;                                       /* upm.h: beside host inode numbers, under the same st_dev */
    n->atime=n->mtime=n->ctime=mem_now();fs->inodes++;
    if(mem_is(n,MEM_DIR)) { n->nlink=2;n->d.next=3; }
    return n;
}
static void mem_pages_free(MemNode *n,uint64_t first);
static void mem_notify(MemNode *watched,uint32_t mask,const char *name,uint32_t cookie);
#define IN_ACCESS 0x1u
#define IN_CLOSE_NOWRITE 0x10u
#define IN_OPEN 0x20u
static void mem_node_free(MemNode *n) {
    if(mem_is(n,MEM_REG)) { mem_pages_free(n,0);free(n->f.leaf); }
    else if(mem_is(n,MEM_DIR)) { for(uint32_t i=0;i<n->d.count;i++)free(n->d.list[i]);free(n->d.list); }
    else if(mem_is(n,MEM_LNK))free(n->target);
    for(MemXattr *x=n->xattrs,*next;x;x=next) { next=x->next;free(x->name);free(x); }
    mem_notify(n,IN_IGNORED,NULL,0);                                            /* its watches go */
    n->fs->inodes--;free(n);
}
static void mem_node_put(MemNode *n) { if(n && !--n->refs && !n->nlink)mem_node_free(n); }
/* inotify: events on memfs watches (inotify.h keeps them; mem_lock held). IN_ISDIR
   in mask when the subject is a directory; cookie pairs MOVED_FROM with MOVED_TO. */
static uint32_t mem_cookie;
static void mem_notify(MemNode *watched,uint32_t mask,const char *name,uint32_t cookie) {
    if(!watched || !__atomic_load_n(&mem_watching,__ATOMIC_RELAXED))return;
    for(unsigned i=0;i<IO_INOTIFYS;i++) {
        struct GuestInotify *in=io_inotifies[i];if(!in)continue;
        pthread_mutex_lock(&in->lock);int any=0;
        for(unsigned k=0;k<in->nwatch;) {
            struct GuestInotifyWatch *w=&in->watch[k];
            if(w->mem!=watched || (mask!=IN_IGNORED && !(w->mask&mask&IN_ALL_EVENTS))) { k++;continue; }
            if(mask==IN_IGNORED) { inotify_drop(in,k);any=1;continue; }            /* the node is gone */
            size_t at=in->used;inotify_emit(in,w->wd,mask,name);any=1;
            if(cookie && in->used>at)fs_pack(in->queue+at,8,cookie,4);
            if(w->mask&IN_ONESHOT) { inotify_drop(in,k);continue; }
            k++;
        }
        if(any && !in->signaled) { char c=1;in->signaled=write(in->pipe_write,&c,1)==1; }
        pthread_mutex_unlock(&in->lock);
    }
}
/* An event on node and, under name, on the directory it is in. */
static void mem_notify_child(MemNode *dir,const char *name,MemNode *node,uint32_t mask) {
    if(!__atomic_load_n(&mem_watching,__ATOMIC_RELAXED))return;
    uint32_t isdir=mem_is(node,MEM_DIR)?IN_ISDIR:0;
    mem_notify(node,mask|isdir,NULL,0);
    if(dir && name && *name)mem_notify(dir,mask|isdir,name,0);
}
static void mem_node_put_locked(MemNode *n) { pthread_mutex_lock(&mem_lock);mem_node_put(n);pthread_mutex_unlock(&mem_lock); }
/* Names: one hash table over every directory, keyed by (directory, name). */
static uint32_t mem_hash(const MemNode *dir,const char *name) {
    uint64_t h=UINT64_C(1469598103934665603)^((uint64_t)(uintptr_t)dir*UINT64_C(0x9E3779B97F4A7C15));
    while(*name)h=(h^(unsigned char)*name++)*UINT64_C(1099511628211);
    return (uint32_t)(h^(h>>32));
}
static MemEntry *mem_find(const MemNode *dir,const char *name) {
    if(!mem_table_size)return NULL;
    uint32_t h=mem_hash(dir,name);
    for(MemEntry *e=mem_table[h&(mem_table_size-1)];e;e=e->next)if(e->hash==h && e->dir==dir && !strcmp(e->name,name))return e;
    return NULL;
}
static int mem_table_grow(void) {
    size_t size=mem_table_size?mem_table_size*2:4096;
    MemEntry **table=calloc(size,sizeof(*table));if(!table)return 0;
    for(size_t i=0;i<mem_table_size;i++)for(MemEntry *e=mem_table[i],*next;e;e=next) { next=e->next;e->next=table[e->hash&(size-1)];table[e->hash&(size-1)]=e; }
    free(mem_table);mem_table=table;mem_table_size=size;return 1;
}
/* Add name -> node to dir (it must not be there). 0 when out of memory. */
static MemEntry *mem_link(MemNode *dir,const char *name,MemNode *node) {
    if(mem_table_used>=mem_table_size && !mem_table_grow())return NULL;
    if(dir->d.count==dir->d.capacity) {
        if(dir->d.live<dir->d.count/2) {                                        /* compact removed entries */
            uint32_t k=0;for(uint32_t i=0;i<dir->d.count;i++) { if(dir->d.list[i]->node)dir->d.list[k++]=dir->d.list[i];else free(dir->d.list[i]); }
            dir->d.count=k;
        } else {
            uint32_t capacity=dir->d.capacity?dir->d.capacity*2:8;
            MemEntry **list=calloc(capacity,sizeof(*list));if(!list)return NULL;
            if(dir->d.count)memcpy(list,dir->d.list,dir->d.count*sizeof(*list));
            free(dir->d.list);dir->d.list=list;dir->d.capacity=capacity;
        }
    }
    size_t length=strlen(name);MemEntry *e=calloc(1,sizeof(*e)+length+1);if(!e)return NULL;
    e->dir=dir;e->node=node;e->hash=mem_hash(dir,name);e->offset=dir->d.next++;memcpy(e->name,name,length+1);
    e->next=mem_table[e->hash&(mem_table_size-1)];mem_table[e->hash&(mem_table_size-1)]=e;mem_table_used++;
    dir->d.list[dir->d.count++]=e;dir->d.live++;
    if(mem_is(node,MEM_DIR)) { node->parent=dir;node->entry=e;dir->nlink++; } else node->nlink++;
    return e;
}
/* Take e out of its directory; the node goes when nothing else holds it. */
static void mem_unlink_entry(MemEntry *e) {
    MemNode *dir=e->dir,*node=e->node;
    for(MemEntry **p=&mem_table[e->hash&(mem_table_size-1)];*p;p=&(*p)->next)if(*p==e) { *p=e->next;break; }
    mem_table_used--;e->node=NULL;dir->d.live--;
    if(mem_is(node,MEM_DIR)) { node->nlink=0;node->entry=NULL;dir->nlink--; } else node->nlink--;
    if(!node->nlink && !node->refs)mem_node_free(node);
}
/* The first entry with an offset above position (binary search over the ordered list). */
static uint32_t mem_dir_index(const MemNode *dir,uint64_t position) {
    uint32_t low=0,high=dir->d.count;
    while(low<high) { uint32_t mid=(low+high)/2;if(dir->d.list[mid]->offset<=position)low=mid+1;else high=mid; }
    return low;
}
/* Pages: a two-level table, sparse (absent pages read as zeros). */
static unsigned char *mem_page_at(const MemNode *n,uint64_t index) {
    uint64_t l=index/MEM_LEAF;return l<n->f.leaves && n->f.leaf[l]?n->f.leaf[l]->page[index%MEM_LEAF]:NULL;
}
/* A new page at index (zeroed unless the caller fills it whole), charged to the
   memory budget and the mount's size. */
static unsigned char *mem_page_new_as(MemNode *n,uint64_t index,int whole) {
    uint64_t l=index/MEM_LEAF;
    if(l>=n->f.leaves) {
        uint64_t leaves=n->f.leaves?n->f.leaves:4;while(leaves<=l)leaves*=2;
        MemLeaf **table=calloc((size_t)leaves,sizeof(*table));if(!table)return NULL;
        if(n->f.leaves)memcpy(table,n->f.leaf,(size_t)n->f.leaves*sizeof(*table));
        free(n->f.leaf);n->f.leaf=table;n->f.leaves=leaves;
    }
    if(!n->f.leaf[l] && !(n->f.leaf[l]=calloc(1,sizeof(MemLeaf))))return NULL;
    MemFs *fs=n->fs;
    if((fs->limit && fs->used+MEM_PAGE>fs->limit) || !resource_charge(MEM_PAGE))return NULL;
    unsigned char *p=mem_page_alloc();if(!p) { resource_uncharge(MEM_PAGE);return NULL; }
    if(!whole)memset(p,0,MEM_PAGE);
    fs->used+=MEM_PAGE;n->f.pages++;
    return n->f.leaf[l]->page[index%MEM_LEAF]=p;
}
static unsigned char *mem_page_new(MemNode *n,uint64_t index) { return mem_page_new_as(n,index,0); }
static void mem_pages_free(MemNode *n,uint64_t first) {
    for(uint64_t l=first/MEM_LEAF;l<n->f.leaves;l++) {
        MemLeaf *leaf=n->f.leaf[l];if(!leaf)continue;
        for(unsigned i=l==first/MEM_LEAF?(unsigned)(first%MEM_LEAF):0;i<MEM_LEAF;i++)if(leaf->page[i]) {
            mem_page_free(leaf->page[i]);leaf->page[i]=NULL;resource_uncharge(MEM_PAGE);n->fs->used-=MEM_PAGE;n->f.pages--;
        }
        if(l>first/MEM_LEAF || first%MEM_LEAF==0) { free(leaf);n->f.leaf[l]=NULL; }
    }
}
static uint64_t mem_read_at(MemNode *n,uint64_t at,unsigned char *out,uint64_t count) {
    if(at>=n->size)return 0;
    if(count>n->size-at)count=n->size-at;
    for(uint64_t done=0;done<count;) {
        uint64_t pos=at+done,offset=pos%MEM_PAGE,take=MEM_PAGE-offset<count-done?MEM_PAGE-offset:count-done;
        const unsigned char *p=mem_page_at(n,pos/MEM_PAGE);
        if(p)memcpy(out+done,p+offset,(size_t)take);else memset(out+done,0,(size_t)take);
        done+=take;
    }
    return count;
}
static void file_share_written_at(uint64_t dev,uint64_t ino,uint64_t at,const unsigned char *data,size_t bytes); /* mapping.h */
/* Bytes written (short when the mount or the memory budget is full), or -errno. */
static int64_t mem_write_at(MemNode *n,uint64_t at,const unsigned char *data,uint64_t count) {
    if(at>=MEM_FILE_MAX)return count?-27:0;
    if(count>MEM_FILE_MAX-at)count=MEM_FILE_MAX-at;
    uint64_t done=0;
    while(done<count) {
        uint64_t pos=at+done,offset=pos%MEM_PAGE,take=MEM_PAGE-offset<count-done?MEM_PAGE-offset:count-done;
        unsigned char *p=mem_page_at(n,pos/MEM_PAGE);
        if(!p && !(p=mem_page_new_as(n,pos/MEM_PAGE,take==MEM_PAGE)))break;
        memcpy(p+offset,data+done,(size_t)take);done+=take;
    }
    if(!done && count)return -28;                                               /* ENOSPC */
    if(at+done>n->size)n->size=at+done;
    n->mtime=n->ctime=mem_now();
    return (int64_t)done;
}
/* New size: pages past it go, the rest of its last page is zeroed. */
static int mem_resize(MemNode *n,uint64_t size) {
    if(size>MEM_FILE_MAX)return -27;
    if(size<n->size) {
        mem_pages_free(n,(size+MEM_PAGE-1)/MEM_PAGE);
        unsigned char *p=size%MEM_PAGE?mem_page_at(n,size/MEM_PAGE):NULL;if(p)memset(p+size%MEM_PAGE,0,MEM_PAGE-size%MEM_PAGE);
    }
    n->size=size;n->mtime=n->ctime=mem_now();return 0;
}
/* Mounts. mem_mount_count and the lookups fs_walk uses are declared in fs.h. */
static int mem_mount_at(const char *canon) {
    for(unsigned i=0;i<mem_mount_count;i++)if(!strcmp(mem_mounts[i]->path,canon))return (int)i;
    return -1;
}
static int mem_mount_inside(const char *canon) {
    for(unsigned i=0;i<mem_mount_count;i++) { size_t n=mem_mounts[i]->length;if(!strncmp(canon,mem_mounts[i]->path,n) && (!canon[n] || canon[n]=='/'))return 1; }
    return 0;
}
/* Whether a mount lies at or below canon ("/" holds them all). */
static int mem_mount_below(const char *canon) {
    size_t n=strlen(canon);if(n==1)return 1;
    for(unsigned i=0;i<mem_mount_count;i++)if(!strncmp(mem_mounts[i]->path,canon,n) && (!mem_mounts[i]->path[n] || mem_mounts[i]->path[n]=='/'))return 1;
    return 0;
}
static uint64_t mem_size_option(const char *v,int *ok) {
    char *end;errno=0;unsigned long long n=strtoull(v,&end,10);*ok=!errno && end!=v;
    if(*end=='%') { end++;n=(unsigned long long)((double)n/100*(double)resource_limit); }
    else if(*end=='k' || *end=='K') { end++;n<<=10; } else if(*end=='m' || *end=='M') { end++;n<<=20; } else if(*end=='g' || *end=='G') { end++;n<<=30; }
    if(*end)*ok=0;
    return n;
}
/* --tmpfs GUEST[:OPTIONS]: Docker's tmpfs options (size, mode, uid, gid, nr_inodes,
   exec/noexec, ro/rw; nosuid, nodev and others are accepted). */
static int mem_add_mount(const char *spec) {
    char path[1025];const char *colon=strchr(spec,':');size_t n=colon?(size_t)(colon-spec):strlen(spec);
    if(mem_mount_count==MEM_MOUNTS || n<2 || n>1024 || spec[0]!='/') { errno=EINVAL;return -1; }
    memcpy(path,spec,n);path[n]=0;while(n>1 && path[n-1]=='/')path[--n]=0;
    for(const char *p=path+1;*p;) {                                             /* clean: no empty, "." or ".." parts */
        const char *q=p;while(*q && *q!='/')q++;
        if(q==p || q-p>255 || (q-p==1 && p[0]=='.') || (q-p==2 && p[0]=='.' && p[1]=='.')) { errno=EINVAL;return -1; }
        p=*q?q+1:q;
    }
    if(mem_mount_at(path)>=0) { errno=EEXIST;return -1; }
    MemFs *fs=calloc(1,sizeof(*fs));if(!fs) { errno=ENOMEM;return -1; }
    fs->index=mem_mount_count;strcpy(fs->path,path);fs->length=n;
    uint32_t mode=01777,uid=0,gid=0;
    for(const char *o=colon?colon+1:"";*o;) {
        char option[64];size_t k=strcspn(o,",");if(k>=sizeof(option)) { free(fs);errno=EINVAL;return -1; }
        memcpy(option,o,k);option[k]=0;o+=k;if(*o)o++;
        char *value=strchr(option,'=');if(value)*value++=0;int ok=1;
        if(!strcmp(option,"size") && value)fs->limit=(mem_size_option(value,&ok)+MEM_PAGE-1)/MEM_PAGE*MEM_PAGE;
        else if(!strcmp(option,"nr_inodes") && value)fs->inode_limit=mem_size_option(value,&ok);
        else if(!strcmp(option,"mode") && value) { char *end;mode=(uint32_t)strtoul(value,&end,8);ok=!*end && mode<=07777; }
        else if(!strcmp(option,"uid") && value)uid=(uint32_t)strtoul(value,NULL,10);
        else if(!strcmp(option,"gid") && value)gid=(uint32_t)strtoul(value,NULL,10);
        else if(!strcmp(option,"noexec"))fs->noexec=1;
        else if(!strcmp(option,"exec"))fs->noexec=0;
        else if(!strcmp(option,"ro"))fs->readonly=1;
        else if(!strcmp(option,"rw"))fs->readonly=0;
        if(!ok) { free(fs);errno=EINVAL;return -1; }
    }
    uint32_t keep_uid=guest_uid,keep_gid=guest_gid;guest_uid=uid;guest_gid=gid;
    fs->root=mem_node_new(fs,MEM_DIR|mode);guest_uid=keep_uid;guest_gid=keep_gid;
    if(!fs->root) { free(fs);errno=ENOMEM;return -1; }
    fs->root->refs=1;fs->root->nlink=2;                                        /* the mount holds it */
    mem_mounts[mem_mount_count++]=fs;return 0;
}
static MemOpen *mem_fd(uint64_t fd) { return io_managed(fd) && IO(fd).kind==IO_MEM?IO(fd).mem:NULL; }
static void mem_open_put(MemOpen *o) {
    pthread_mutex_lock(&mem_lock);
    if(!--o->refs) {
        if(!(o->flags&0x200000))mem_notify_child(o->dir,o->name,o->node,(o->flags&3)?IN_CLOSE_WRITE:IN_CLOSE_NOWRITE);
        mem_node_put(o->node);mem_node_put(o->dir);free(o->name);free(o);
    }
    pthread_mutex_unlock(&mem_lock);
}
static void mem_open_get(MemOpen *o) { pthread_mutex_lock(&mem_lock);o->refs++;pthread_mutex_unlock(&mem_lock); }
/* Resolution. */
typedef struct {
    MemNode *dir;                     /* the directory holding name; NULL when the leaf is not a name ("/", ".", "..") */
    MemNode *node;                    /* the leaf, or NULL when missing */
    MemEntry *entry;                  /* dir's entry for name, when the leaf exists */
    char name[256];int slash;         /* the last component; whether the path ended in '/' */
    char canon[1025];                 /* the leaf's canonical guest path ("" if unknown) */
} MemPath;
/* fs_walk's redirect (fs.h): it reached a memfs mount; path is that mount's guest path and the rest. */
/* The guest path of a memfs directory: its mount's and its names up to the root ("" when removed). */
static void mem_dir_path(const MemNode *n,char out[1025]) {
    char tail[1025];size_t used=0;tail[0]=0;
    for(;n && n!=n->fs->root;n=n->parent) {
        if(!n->entry) { out[0]=0;return; }
        size_t m=strlen(n->entry->name);if(used+m+1>=sizeof(tail)-n->fs->length) { out[0]=0;return; }
        memmove(tail+m+1,tail,used+1);tail[0]='/';memcpy(tail+1,n->entry->name,m);used+=m+1;
    }
    snprintf(out,1025,"%s%s",n?n->fs->path:"",tail);
}
/* Rewritten host paths (a memfs symlink's target, ".." out of a mount), one per path argument. */
static _Thread_local char mem_rewrite[2][1025];
/* Resolve input from dir under mem_lock (follow: a final symlink; probe: ask fs_walk
   where a host path leads). Returns 1 for a memfs leaf (w); 0 for the host, with the
   path unchanged or (*rewritten) the absolute host path in rewrite; or -errno. */
static int mem_resolve(uint64_t dir,const char *input,int follow,int probe,MemPath *w,int *rewritten,char rewrite[1025]) {
    char rest[4097],canon[1025];MemNode *cur=NULL;unsigned links=0;int spliced=0;
    *rewritten=0;
    size_t length=strlen(input);if(length>1024)return -36;
    if(!length)return -2;
    memcpy(rest,input,length+1);
    if(rest[0]=='/')strcpy(canon,"/");
    else if((int32_t)dir==-100) { if(fs_cwd && fs_cwd->mem)cur=fs_cwd->mem;strcpy(canon,fs_cwd?fs_cwd->path:"/"); }
    else {
        MemOpen *o=mem_fd(dir);
        if(o) { if(!mem_is(o->node,MEM_DIR))return -20;cur=o->node;mem_dir_path(cur,canon); }
        else {
            int slot=fs_slot(dir);
            if(slot<0 || !FS(slot).fd)return dir<GUEST_FD_LIMIT && fd_kind[dir]?-20:-9;
            if(FS(slot).dir && FS(slot).dir->path[0])strcpy(canon,FS(slot).dir->path);else canon[0]=0;  /* unknown: the host walk decides */
        }
    }
    size_t pos=0;
again:
    for(;;) {
        while(rest[pos]=='/')pos++;
        if(!rest[pos]) {                                                     /* the path names cur itself */
            if(!cur)goto host;
            w->dir=NULL;w->entry=NULL;w->node=cur;w->name[0]=0;                  /* "": a mount point (EBUSY) */w->slash=pos>0 && rest[pos-1]=='/';strcpy(w->canon,canon);return 1;
        }
        size_t start=pos;while(rest[pos] && rest[pos]!='/')pos++;
        size_t m=pos-start;if(m>255)return -36;
        char part[256];memcpy(part,rest+start,m);part[m]=0;
        size_t after=pos;while(rest[after]=='/')after++;
        int last=!rest[after],slash=last && after>pos;
        if(!cur) {                                                           /* on the host, lexically */
            if(!strcmp(part,"."))continue;
            if(!strcmp(part,"..") || !*canon) { pos=start;goto host; }       /* only the host walk knows where ".." leads */
            if(fs_canon_push(canon,part)<0)return -36;
            int i=mem_mount_at(canon);
            if(i>=0) { cur=mem_mounts[i]->root;continue; }
            if(!mem_mount_below(canon)) { fs_canon_pop(canon);pos=start;goto host; }
            continue;
        }
        if(!mem_is(cur,MEM_DIR))return -20;
        if(!mem_may(cur,1))return -13;
        if(!strcmp(part,".")) { if(last) { w->dir=NULL;w->entry=NULL;w->node=cur;strcpy(w->name,".");w->slash=slash;strcpy(w->canon,canon);return 1; } continue; }
        if(!strcmp(part,"..")) {
            if(cur==cur->fs->root) {                                         /* out of the mount, to its guest parent */
                strcpy(canon,cur->fs->path);fs_canon_pop(canon);cur=NULL;spliced=1;
                if(last) { pos=after;goto host; }
                continue;
            }
            if(!cur->parent)return -2;                                       /* removed */
            cur=cur->parent;fs_canon_pop(canon);
            if(last) { w->dir=NULL;w->entry=NULL;w->node=cur;strcpy(w->name,"..");w->slash=slash;strcpy(w->canon,canon);return 1; }
            continue;
        }
        if(!cur->nlink)return -2;                                            /* a removed directory holds nothing */
        MemEntry *e=mem_find(cur,part);MemNode *child=e?e->node:NULL;
        if(child && mem_is(child,MEM_LNK) && (!last || follow || slash)) {
            if(++links>FS_LINK_LIMIT || (fs_scope.resolve&4))return -40;
            size_t n=strlen(child->target),tail=strlen(rest+pos);
            if(n+tail>4096)return -36;
            memmove(rest+n,rest+pos,tail+1);memcpy(rest,child->target,n);pos=0;
            if(child->target[0]=='/') { cur=NULL;strcpy(canon,"/");spliced=1; }
            continue;
        }
        if(!last) {
            if(!child)return -2;
            if(!mem_is(child,MEM_DIR))return -20;
            cur=child;if(fs_canon_push(canon,part)<0)return -36;
            continue;
        }
        w->dir=cur;w->entry=e;w->node=child;strcpy(w->name,part);w->slash=slash;
        strcpy(w->canon,canon);if(fs_canon_push(w->canon,part)<0)return -36;
        return 1;
    }
host: {  /* the host serves the rest: from canon, at rest+pos */
        if(!spliced && !probe)return 0;
        char full[1025];full[0]=0;
        if(*canon) {
            size_t c=strlen(canon),r=strlen(rest+pos);if(c+1+r>1024)return -36;
            memcpy(full,canon,c);if(r && c>1)full[c++]='/';
            memcpy(full+c,rest+pos,r+1);
        }
        if(probe) {                                                           /* where the host walk leads */
            char leaf[1025];mem_redirect.pending=0;int fd;
            upm_lock_held=1;                                                  /* ov_walk's look into upm.h: mem_lock is ours */
            if(*full)fd=fs_walk(fs_root,"/",full,follow,leaf,NULL);
            else { int base=(int32_t)dir==-100?(fs_cwd?fs_cwd->fd:fs_root):fs_host(dir);fd=base<0?-1:fs_walk(base,"",input,follow,leaf,NULL); }
            upm_lock_held=0;
            if(fd>=0)close(fd);
            if(mem_redirect_take()) {                                         /* into a mount: the rest is ours */
                const char *p=mem_redirect.path;size_t best=0;int i=-1;
                if(p[0]==UPM_MARK[0] && upm_fs) {                                 /* the writable layer's file in memory (upm.h) */
                    if(++links>FS_LINK_LIMIT)return -40;
                    strcpy(canon,"/");cur=upm_fs->root;snprintf(rest,sizeof(rest),"%s",p+1);pos=0;spliced=1;
                    goto again;
                }
                for(unsigned k=0;k<mem_mount_count;k++) { size_t n=mem_mounts[k]->length;if(n>best && !strncmp(p,mem_mounts[k]->path,n) && (!p[n] || p[n]=='/')) { best=n;i=(int)k; } }
                if(i<0 || ++links>FS_LINK_LIMIT)return -40;
                strcpy(canon,mem_mounts[i]->path);cur=mem_mounts[i]->root;snprintf(rest,sizeof(rest),"%s",p+best);pos=0;spliced=1;
                goto again;
            }
        }
        if(!spliced || !*full)return 0;
        strcpy(rewrite,full);*rewritten=1;return 0;
    }
}
/* Linux struct stat (arm64, 128 bytes) and struct statfs (120). */
static uint64_t mem_size_of(const MemNode *n) { return mem_is(n,MEM_REG)?n->size:mem_is(n,MEM_DIR)?20*(uint64_t)(n->d.live+2):mem_is(n,MEM_LNK)?strlen(n->target):0; }
static void mem_stat_pack(unsigned char *out,const MemNode *n) {
    memset(out,0,128);int layer=n->fs==upm_fs;                                     /* upm.h: as the layer's host files (fs_stat_pack) */
    fs_pack(out,0,layer?0:MEM_DEV(n->fs),8);fs_pack(out,8,n->ino,8);fs_pack(out,16,n->mode,4);fs_pack(out,20,n->nlink,4);
    fs_pack(out,24,layer?guest_uid:n->uid,4);fs_pack(out,28,layer?guest_gid:n->gid,4);fs_pack(out,48,mem_size_of(n),8);fs_pack(out,56,MEM_PAGE,4);
    fs_pack(out,64,mem_is(n,MEM_REG)?n->f.pages*(MEM_PAGE/512):0,8);
    const struct timespec *t[3]={&n->atime,&n->mtime,&n->ctime};
    for(unsigned i=0;i<3;i++) { fs_pack(out,72+i*16,(uint64_t)t[i]->tv_sec,8);fs_pack(out,80+i*16,(uint64_t)t[i]->tv_nsec,8); }
}
static uint64_t proc_mem_total(void);
static void mem_statfs_pack(unsigned char *out,const MemFs *fs) {
    struct statfs host;
    if(fs==upm_fs && fs_root>=0 && !fstatfs(fs_space>=0?fs_space:fs_root,&host)) { /* upm.h: the layer's, as the host's files say (syscall.h) */
        memset(out,0,120);
        fs_pack(out,0,0x794c7630,8);fs_pack(out,8,host.f_bsize,8);fs_pack(out,16,host.f_blocks,8);
        fs_pack(out,24,host.f_bfree,8);fs_pack(out,32,host.f_bavail,8);fs_pack(out,40,host.f_files,8);
        fs_pack(out,48,host.f_ffree,8);fs_pack(out,64,255,8);fs_pack(out,72,host.f_bsize,8);return;
    }
    uint64_t total=fs->limit?fs->limit:proc_mem_total()/2,blocks=total/MEM_PAGE,used=fs->used/MEM_PAGE,free_blocks=blocks>used?blocks-used:0;
    uint64_t budget=resource_limit>resource_used?(resource_limit-resource_used)/MEM_PAGE:0;if(free_blocks>budget)free_blocks=budget;
    uint64_t files=fs->inode_limit?fs->inode_limit:blocks/2+1;
    memset(out,0,120);
    fs_pack(out,0,0x01021994,8);fs_pack(out,8,MEM_PAGE,8);fs_pack(out,16,blocks,8);fs_pack(out,24,free_blocks,8);fs_pack(out,32,free_blocks,8);
    fs_pack(out,40,files,8);fs_pack(out,48,files>fs->inodes?files-fs->inodes:0,8);fs_pack(out,56,MEM_DEV(fs),4);
    fs_pack(out,64,255,8);fs_pack(out,72,MEM_PAGE,8);fs_pack(out,80,(fs->readonly?1:0)|2|4|(fs->noexec?8:0)|1024,8); /* ST_RDONLY, NOSUID, NODEV, NOEXEC, RELATIME */
}
/* A new description of n as the lowest free descriptor (fd: one io_free_slot claimed). */
static int64_t mem_install(int fd,MemNode *n,unsigned flags,int cloexec,MemNode *dir,const char *name) {
    MemOpen *o=calloc(1,sizeof(*o));if(!o) { fd_unbind((uint64_t)fd);return -12; }
    o->node=n;o->refs=1;o->flags=flags;n->refs++;
    if(dir && name && *name && (o->name=malloc(strlen(name)+1))) { strcpy(o->name,name);o->dir=dir;dir->refs++; }
    if(!(flags&0x200000))mem_notify_child(dir,name,n,IN_OPEN);
    IO(fd)=(GuestIO){.kind=IO_MEM,.cloexec=(unsigned)(cloexec!=0),.host=-1,.mem=o};
    return fd;
}
#define MEM_FIFO (-1000)                                /* mem_open: a FIFO, which the caller opens as a pipe */
static MemNode *mem_create(MemPath *w,uint32_t mode,int *error) {
    MemNode *dir=w->dir;
    if(w->node || !dir) { *error=-17;return NULL; }
    if(!dir->nlink) { *error=-2;return NULL; }
    if(!mem_may(dir,3)) { *error=-13;return NULL; }
    if(dir->fs->readonly) { *error=-30;return NULL; }
    MemNode *n=mem_node_new(dir->fs,mode);if(!n) { *error=-28;return NULL; }
    if(dir->mode&02000) { n->gid=dir->gid;if(mem_is(n,MEM_DIR))n->mode|=02000; }  /* set-group-ID directories pass their group on */
    if(!mem_link(dir,w->name,n)) { mem_node_free(n);*error=-12;return NULL; }
    dir->mtime=dir->ctime=n->mtime;w->node=n;
    mem_notify(dir,IN_CREATE|(mem_is(n,MEM_DIR)?IN_ISDIR:0),w->name,0);
    return n;
}
static int64_t mem_open(MemPath *w,uint64_t flags,uint64_t mode) {
    unsigned acc=(unsigned)(flags&3);int path=(flags&0x200000)!=0,create=(flags&64)!=0;
    if(path)flags&=UINT64_C(0x200000|16384|32768|524288);
    if(acc==3 && !path)return -22;
    MemNode *n=w->node;
    if(n && !path && (flags&0x400000)) {                                       /* O_TMPFILE: an unnamed file in n */
        if(!mem_is(n,MEM_DIR))return -20;
        if(!acc)return -22;
        if(!n->nlink)return -2;
        if(!mem_may(n,3))return -13;
        if(n->fs->readonly)return -30;
        int fd=io_free_slot(0);if(fd<0)return -24;
        MemNode *f=mem_node_new(n->fs,MEM_REG|((uint32_t)mode&07777&~fs_umask));if(!f) { fd_unbind((uint64_t)fd);return -28; }
        if(n->mode&02000)f->gid=n->gid;
        int64_t r=mem_install(fd,f,(unsigned)(flags&(3|1024|2048)),(flags&524288)!=0,NULL,NULL);if(r<0)mem_node_free(f);
        return r;
    }
    if(!n && (flags&0x400000))return -2;
    if(n) {
        if(create && (flags&128))return -17;
        if(mem_is(n,MEM_LNK) && !path)return -40;                               /* O_NOFOLLOW on a symlink */
        if(((flags&16384) || w->slash) && !mem_is(n,MEM_DIR))return -20;
        if(!path) {
            if(mem_is(n,MEM_DIR) && (acc || create || (flags&512)))return -21;
            if((n->mode&0170000)==0140000)return -6;                             /* a socket: ENXIO */
            if((acc!=1 && !mem_may(n,4)) || (acc && !mem_may(n,2)) || ((flags&512) && !mem_may(n,2)))return -13;
            if(n->fs->readonly && (acc || (flags&512)))return -30;
            if((n->mode&0170000)==0010000)return MEM_FIFO;
        }
    } else if(!create || path)return -2;
    else if(w->slash)return -21;                                               /* "name/" names a directory */
    int fd=io_free_slot(0);if(fd<0)return -24;
    if(!n) { int error;if(!(n=mem_create(w,MEM_REG|((uint32_t)mode&07777&~fs_umask),&error))) { fd_unbind((uint64_t)fd);return error; } }
    else if((flags&512) && mem_is(n,MEM_REG) && !path) { mem_resize(n,0);mem_notify_child(w->dir,w->name,n,IN_MODIFY); }
    return mem_install(fd,n,(unsigned)(flags&(3|1024|2048|0x200000)),(flags&524288)!=0,w->dir,w->name);
}
/* The record of one directory entry for getdents64; 0 when it does not fit. */
static size_t mem_dirent(unsigned char *out,size_t room,uint64_t ino,uint64_t offset,const char *name,unsigned type) {
    size_t length=strlen(name),size=(19+length+1+7)&~(size_t)7;
    if(size>room)return 0;
    memset(out,0,size);fs_pack(out,0,ino,8);fs_pack(out,8,offset,8);fs_pack(out,16,size,2);out[18]=(unsigned char)type;memcpy(out+19,name,length);
    return size;
}
static unsigned mem_dtype(const MemNode *n) { unsigned t=n->mode&0170000;return t==MEM_DIR?4:t==MEM_REG?8:t==MEM_LNK?10:t==0010000?1:t==0140000?12:0; }
static int64_t mem_getdents(MemOpen *o,unsigned char *out,size_t count) {
    MemNode *d=o->node;
    if(!mem_is(d,MEM_DIR))return -20;
    if(!d->nlink)return -2;
    size_t used=0,size;
    if(o->position<1) { if(!(size=mem_dirent(out,count,d->ino,1,".",4)))return -22;used+=size;o->position=1; }
    if(o->position<2) { if(!(size=mem_dirent(out+used,count-used,d->parent?d->parent->ino:d->ino,2,"..",4)))return used?(int64_t)used:-22;used+=size;o->position=2; }
    for(uint32_t i=mem_dir_index(d,o->position);i<d->d.count;i++) {
        MemEntry *e=d->d.list[i];if(!e->node)continue;
        if(!(size=mem_dirent(out+used,count-used,e->node->ino,e->offset,e->name,mem_dtype(e->node))))return used?(int64_t)used:-22;
        used+=size;o->position=e->offset;
    }
    return (int64_t)used;
}
static int mem_writable_dir(MemPath *w,MemNode *node) {
    if(!mem_may(w->dir,3))return -13;
    if(!mem_sticky_ok(w->dir,node))return -1;
    if(w->dir->fs->readonly)return -30;
    return 0;
}
static int mem_unlink(MemPath *w,uint64_t flags) {
    MemNode *n=w->node;if(!n)return -2;
    int rmdir=(flags&0x200)!=0;
    if(!w->dir) {                                                              /* "/", "." or ".." */
        if(!rmdir)return -21;
        return !strcmp(w->name,".")?-22:!strcmp(w->name,"..")?-39:-16;
    }
    if(rmdir) { if(!mem_is(n,MEM_DIR))return -20;if(n->d.live)return -39; }
    else { if(mem_is(n,MEM_DIR))return -21;if(w->slash)return -20; }
    int e=mem_writable_dir(w,n);if(e)return e;
    struct timespec now=mem_now();w->dir->mtime=w->dir->ctime=now;n->ctime=now;
    uint32_t isdir=mem_is(n,MEM_DIR)?IN_ISDIR:0;
    mem_notify(w->dir,IN_DELETE|isdir,w->name,0);
    if(isdir || n->nlink==1) { mem_notify(n,IN_DELETE_SELF|isdir,NULL,0);mem_notify(n,IN_IGNORED,NULL,0); }
    else mem_notify(n,IN_ATTRIB,NULL,0);
    mem_unlink_entry(w->entry);return 0;
}
/* Whether dir is node or lies inside it. */
static int mem_inside_of(const MemNode *dir,const MemNode *node) { for(;dir;dir=dir->parent)if(dir==node)return 1;return 0; }
/* Move entry e (in its directory) to name in dir, replacing nothing. */
static int mem_move(MemEntry *e,MemNode *dir,const char *name) {
    MemNode *node=e->node,*from=e->dir;
    if(!mem_link(dir,name,node))return -12;                                    /* a directory's parent and entry now dir's */
    for(MemEntry **p=&mem_table[e->hash&(mem_table_size-1)];*p;p=&(*p)->next)if(*p==e) { *p=e->next;break; }
    mem_table_used--;e->node=NULL;from->d.live--;
    if(mem_is(node,MEM_DIR))from->nlink--;else node->nlink--;
    return 0;
}
static int mem_rename(MemPath *a,MemPath *b,uint64_t flags) {
    if(flags&~UINT64_C(3) || flags==3)return -22;                              /* RENAME_NOREPLACE, RENAME_EXCHANGE; no WHITEOUT */
    MemNode *n=a->node,*t=b->node;
    if(!n)return -2;
    if(!a->dir || !b->dir)return -16;                                          /* ".", "..", a mount point: EBUSY */
    if(n->fs!=b->dir->fs)return -18;
    if((a->slash || b->slash) && !mem_is(n,MEM_DIR))return -20;
    int e=mem_writable_dir(a,n);if(e)return e;
    if(t && (e=mem_writable_dir(b,t)))return e;
    if(!t && (!mem_may(b->dir,3) || !b->dir->nlink))return !b->dir->nlink?-2:-13;
    struct timespec now=mem_now();
    if(flags&2) {                                                              /* RENAME_EXCHANGE */
        if(!t)return -2;
        if(n==t)return 0;
        if((mem_is(n,MEM_DIR) && mem_inside_of(b->dir,n)) || (mem_is(t,MEM_DIR) && mem_inside_of(a->dir,t)))return -22;
        MemEntry *x=a->entry,*y=b->entry;x->node=t;y->node=n;
        uint32_t c1=++mem_cookie,c2=++mem_cookie,dn=mem_is(n,MEM_DIR)?IN_ISDIR:0,dt=mem_is(t,MEM_DIR)?IN_ISDIR:0;
        mem_notify(a->dir,IN_MOVED_FROM|dn,a->name,c1);mem_notify(b->dir,IN_MOVED_TO|dn,b->name,c1);
        mem_notify(b->dir,IN_MOVED_FROM|dt,b->name,c2);mem_notify(a->dir,IN_MOVED_TO|dt,a->name,c2);
        mem_notify(n,IN_MOVE_SELF|dn,NULL,0);mem_notify(t,IN_MOVE_SELF|dt,NULL,0);
        if(mem_is(n,MEM_DIR)) { n->parent=b->dir;n->entry=y;a->dir->nlink--;b->dir->nlink++; }
        if(mem_is(t,MEM_DIR)) { t->parent=a->dir;t->entry=x;b->dir->nlink--;a->dir->nlink++; }
        a->dir->mtime=a->dir->ctime=b->dir->mtime=b->dir->ctime=n->ctime=t->ctime=now;return 0;
    }
    if(t==n)return 0;                                                          /* two names of one file */
    if(t && (flags&1))return -17;
    if(mem_is(n,MEM_DIR)) {
        if(t && !mem_is(t,MEM_DIR))return -20;
        if(t && t->d.live)return -39;
        if(mem_inside_of(b->dir,n))return -22;
    } else if(t && mem_is(t,MEM_DIR))return -21;
    if(t) {
        t->ctime=now;
        if(mem_is(t,MEM_DIR) || t->nlink==1) { mem_notify(t,IN_DELETE_SELF|(mem_is(t,MEM_DIR)?IN_ISDIR:0),NULL,0);mem_notify(t,IN_IGNORED,NULL,0); }
        mem_unlink_entry(b->entry);
    }
    uint32_t cookie=++mem_cookie,isdir=mem_is(n,MEM_DIR)?IN_ISDIR:0;
    mem_notify(a->dir,IN_MOVED_FROM|isdir,a->name,cookie);
    if((e=mem_move(a->entry,b->dir,b->name)))return e;
    mem_notify(b->dir,IN_MOVED_TO|isdir,b->name,cookie);mem_notify(n,IN_MOVE_SELF|isdir,NULL,0);
    a->dir->mtime=a->dir->ctime=b->dir->mtime=b->dir->ctime=n->ctime=now;return 0;
}
static int mem_hardlink(MemNode *n,MemPath *b) {
    if(mem_is(n,MEM_DIR))return -1;
    if(!b->dir || b->node)return -17;
    if(n->fs!=b->dir->fs)return -18;
    if(!b->dir->nlink)return -2;
    if(!mem_may(b->dir,3))return -13;
    if(n->fs->readonly)return -30;
    if(!mem_link(b->dir,b->name,n))return -12;
    struct timespec now=mem_now();n->ctime=b->dir->mtime=b->dir->ctime=now;
    mem_notify(b->dir,IN_CREATE,b->name,0);mem_notify(n,IN_ATTRIB,NULL,0);return 0;
}
static int mem_symlink(const char *target,MemPath *w) {
    size_t n=strlen(target);if(!n)return -2;
    int error;MemNode *l=mem_create(w,MEM_LNK|0777,&error);if(!l)return error;
    if(!(l->target=malloc(n+1))) { mem_unlink_entry(w->entry?w->entry:mem_find(w->dir,w->name));return -12; }
    memcpy(l->target,target,n+1);return 0;
}
static int mem_chmod(MemNode *n,uint64_t mode) {
    if(guest_uid && guest_uid!=n->uid)return -1;
    if(n->fs->readonly)return -30;
    if(guest_uid && !mem_is(n,MEM_DIR) && n->gid!=guest_gid)mode&=~UINT64_C(02000);
    n->mode=(n->mode&0170000)|(uint32_t)(mode&07777);n->ctime=mem_now();return 0;
}
static int mem_chown(MemNode *n,uint32_t uid,uint32_t gid) {
    if(n->fs->readonly)return -30;
    if(guest_uid) {
        if(uid!=UINT32_MAX && uid!=n->uid)return -1;
        if(gid!=UINT32_MAX && gid!=n->gid && (guest_uid!=n->uid || gid!=guest_gid))return -1;
    }
    if(uid!=UINT32_MAX)n->uid=uid;
    if(gid!=UINT32_MAX)n->gid=gid;
    if(guest_uid && !mem_is(n,MEM_DIR))n->mode&=~06000u;
    n->ctime=mem_now();return 0;
}
/* utimensat's times (NULL: now), Linux UTIME_NOW and UTIME_OMIT. */
static int mem_utimens(MemNode *n,const unsigned char *in) {
    uint64_t ns[2];int now_only=1;
    for(unsigned i=0;i<2;i++) {
        ns[i]=in?le(in+i*16+8,8):(UINT64_C(1)<<30)-1;
        if(ns[i]!=(UINT64_C(1)<<30)-1 && ns[i]!=(UINT64_C(1)<<30)-2) { if(ns[i]>=1000000000)return -22;now_only=0; }
    }
    if(ns[0]==(UINT64_C(1)<<30)-2 && ns[1]==(UINT64_C(1)<<30)-2)return 0;
    if(guest_uid && guest_uid!=n->uid && (!now_only || !mem_may(n,2)))return now_only?-13:-1;
    if(n->fs->readonly)return -30;
    struct timespec now=mem_now(),*t[2]={&n->atime,&n->mtime};
    for(unsigned i=0;i<2;i++) {
        if(ns[i]==(UINT64_C(1)<<30)-2)continue;
        if(ns[i]==(UINT64_C(1)<<30)-1)*t[i]=now;else { t[i]->tv_sec=(time_t)le(in+i*16,8);t[i]->tv_nsec=(long)ns[i]; }
    }
    n->ctime=now;return 0;
}
static int mem_access(MemNode *n,uint64_t mode) {
    if((mode&2) && n->fs->readonly && !mem_is(n,MEM_LNK))return -30;
    if(guest_uid?!mem_may(n,(unsigned)(mode&7)):(mode&1) && !mem_may(n,1))return -13;
    return 0;
}
/* Extended attributes, any namespace: name and value as given. */
static MemXattr **mem_xattr_find(MemNode *n,const char *name) {
    MemXattr **x=&n->xattrs;while(*x && strcmp((*x)->name,name))x=&(*x)->next;return x;
}
static int64_t mem_xattr(MemNode *n,uint64_t call,const char *name,uint64_t value,uint64_t size,uint64_t flags) {
    unsigned op=(unsigned)((call-5)/3);                                         /* 0 set, 1 get, 2 list, 3 remove */
    if(n->fs==upm_fs)return -95;                                                /* upm.h: as the layer's host files */
    if(op!=2 && (!*name || strlen(name)>255))return -34;
    if(op==2) {
        size_t total=0;for(MemXattr *x=n->xattrs;x;x=x->next)total+=strlen(x->name)+1;
        if(!size)return (int64_t)total;
        if(total>size)return -34;
        unsigned char *out=total?lookup(value,total,2):NULL;if(total && !out)return -14;
        for(MemXattr *x=n->xattrs;x;x=x->next) { size_t k=strlen(x->name)+1;memcpy(out,x->name,k);out+=k; }
        return (int64_t)total;
    }
    MemXattr **at=mem_xattr_find(n,name);
    if(op==1) {
        if(!*at)return -61;                                                    /* ENODATA */
        if(!size)return (int64_t)(*at)->size;
        if((*at)->size>size)return -34;
        unsigned char *out=(*at)->size?lookup(value,(*at)->size,2):NULL;if((*at)->size && !out)return -14;
        if((*at)->size)memcpy(out,(*at)->value,(*at)->size);return (int64_t)(*at)->size;
    }
    if(n->fs->readonly)return -30;
    if(guest_uid && guest_uid!=n->uid)return -1;
    if(op==3) { if(!*at)return -61;MemXattr *x=*at;*at=x->next;free(x->name);free(x);n->ctime=mem_now();return 0; }
    if(flags&~UINT64_C(3) || size>65536)return flags&~UINT64_C(3)?-22:-7;
    if((flags&1) && *at)return -17;                                            /* XATTR_CREATE */
    if((flags&2) && !*at)return -61;                                           /* XATTR_REPLACE */
    const unsigned char *in=size?lookup(value,size,4):NULL;if(size && !in)return -14;
    MemXattr *x=calloc(1,sizeof(*x)+size);char *copy=malloc(strlen(name)+1);
    if(!x || !copy) { free(x);free(copy);return -12; }
    strcpy(copy,name);x->name=copy;x->size=size;if(size)memcpy(x->value,in,size);
    if(*at) { MemXattr *old=*at;x->next=old->next;free(old->name);free(old); }
    *at=x;n->ctime=mem_now();return 0;
}
/* Descriptor calls. */
static uint64_t mem_result(int64_t r) { return r<0?linux_error((unsigned)-r):(uint64_t)r; }
/* read/write (at < 0: at the file position) through guest memory, 1 MiB at a time. */
static uint64_t mem_transfer(MemOpen *o,int writing,uint64_t address,uint64_t count,int64_t at) {
    unsigned acc=o->flags&3;
    if((o->flags&0x200000) || (writing?!acc:acc==1))return linux_error(9);
    MemNode *n=o->node;
    if(mem_is(n,MEM_DIR))return linux_error(21);
    uint64_t done=0;
    while(done<count) {
        uint64_t chunk=count-done>1024*1024?1024*1024:count-done;
        unsigned char *p=lookup(address+done,chunk,writing?4:2);
        if(!p) { if(!done)return linux_error(14);break; }
        pthread_mutex_lock(&mem_lock);
        uint64_t pos=at>=0?(uint64_t)at+done:o->position;
        if(writing && (o->flags&1024))pos=n->size;                               /* O_APPEND: even pwrite, as Linux */
        int64_t r=writing?mem_write_at(n,pos,p,chunk):(int64_t)mem_read_at(n,pos,p,chunk);
        if(!writing && r>0 && (n->atime.tv_sec<n->mtime.tv_sec || n->atime.tv_sec<n->ctime.tv_sec || n->atime.tv_sec+86400<mem_now().tv_sec))
            n->atime=mem_now();                                                  /* relatime */
        if(r>0 && at<0)o->position=pos+(uint64_t)r;
        if(r>0)mem_notify_child(o->dir,o->name,n,writing?IN_MODIFY:IN_ACCESS);
        uint64_t dev=MEM_DEV(n->fs),ino=n->ino;
        pthread_mutex_unlock(&mem_lock);
        if(r<0) { if(!done)return linux_error((unsigned)-r);break; }
        if(writing && r>0 && __atomic_load_n(&file_shares_live,__ATOMIC_RELAXED))file_share_written_at(dev,ino,pos,p,(size_t)r);
        done+=(uint64_t)r;
        if((uint64_t)r<chunk)break;
    }
    return done;
}
static uint64_t mem_vector(MemOpen *o,int writing,uint64_t table,uint64_t count,int64_t at) {
    if(count>1024)return linux_error(22);
    const unsigned char *t=count?lookup(table,count*16,4):NULL;if(count && !t)return linux_error(14);
    uint64_t done=0;
    for(uint64_t i=0;i<count;i++) {
        uint64_t length=le(t+i*16+8,8);if(!length)continue;
        uint64_t r=mem_transfer(o,writing,le(t+i*16,8),length,at>=0?at+(int64_t)done:-1);
        if((int64_t)r<0)return done?done:r;
        done+=r;if(r<length)break;
    }
    return done;
}
static int64_t mem_lseek(MemOpen *o,int64_t offset,uint64_t whence) {
    MemNode *n=o->node;
    if(o->flags&0x200000)return -9;
    if(mem_is(n,MEM_DIR)) {                                                     /* telldir/seekdir cookies */
        if(whence==0 && offset>=0)return (int64_t)(o->position=(uint64_t)offset);
        if(whence==1 && !offset)return (int64_t)o->position;
        return -22;
    }
    int64_t base=whence==0?0:whence==1?(int64_t)o->position:(int64_t)n->size,next;
    if(whence==3 || whence==4) {                                                /* SEEK_DATA, SEEK_HOLE */
        if(offset<0 || (uint64_t)offset>=n->size)return -6;
        uint64_t at=(uint64_t)offset;
        while(at<n->size && (mem_page_at(n,at/MEM_PAGE)!=NULL)!=(whence==3))at=(at/MEM_PAGE+1)*MEM_PAGE;
        if(at>=n->size) { if(whence==3)return -6;at=n->size; }
        if(at<(uint64_t)offset)at=(uint64_t)offset;
        return (int64_t)(o->position=at);
    }
    if(whence>2)return -22;
    if(__builtin_add_overflow(base,offset,&next) || next<0)return -22;
    return (int64_t)(o->position=(uint64_t)next);
}
static int64_t mem_fallocate(MemOpen *o,uint64_t mode,uint64_t offset,uint64_t length) {
    MemNode *n=o->node;
    if((o->flags&0x200000) || !(o->flags&3))return -9;
    if(!mem_is(n,MEM_REG))return mem_is(n,MEM_DIR)?-21:-19;
    if((int64_t)offset<0 || (int64_t)length<=0)return -22;
    if(offset+length>MEM_FILE_MAX || offset+length<offset)return -27;
    if(mode==3) {                                                                /* PUNCH_HOLE | KEEP_SIZE */
        uint64_t end=offset+length<n->size?offset+length:n->size;
        for(uint64_t at=offset;at<end;) {
            uint64_t index=at/MEM_PAGE,top=(index+1)*MEM_PAGE<end?(index+1)*MEM_PAGE:end;unsigned char *p=mem_page_at(n,index);
            if(p) {
                if(at==index*MEM_PAGE && top==(index+1)*MEM_PAGE) { mem_page_free(p);n->f.leaf[index/MEM_LEAF]->page[index%MEM_LEAF]=NULL;resource_uncharge(MEM_PAGE);n->fs->used-=MEM_PAGE;n->f.pages--; }
                else memset(p+at%MEM_PAGE,0,(size_t)(top-at));
            }
            at=top;
        }
        n->mtime=n->ctime=mem_now();return 0;
    }
    if(mode>1)return -95;
    for(uint64_t index=offset/MEM_PAGE;index<=(offset+length-1)/MEM_PAGE;index++)
        if(!mem_page_at(n,index) && !mem_page_new(n,index))return -28;
    if(!mode && offset+length>n->size) { n->size=offset+length;n->mtime=n->ctime=mem_now(); }
    return 0;
}
/* fcntl record locks (F_GETLK/F_SETLK/F_SETLKW and the OFD variants) in fs.h's table. */
static uint64_t mem_fcntl_lock(MemOpen *o,uint64_t command) {
    int get=command==5 || command==36,wait=command==7 || command==38;
    unsigned char *p=lookup(cpu->gpr[2],32,get?2:4);if(!p || (get && !lookup(cpu->gpr[2],32,4)))return linux_error(14);
    unsigned type=(unsigned)le(p,2),whence=(unsigned)le(p+2,2);int64_t start=(int64_t)le(p+8,8),length=(int64_t)le(p+16,8);
    if(type>2 || whence>2)return linux_error(22);
    if(!get && ((type==0 && (o->flags&3)==1) || (type==1 && !(o->flags&3))))return linux_error(9);
    pthread_mutex_lock(&mem_lock);
    uint64_t dev=MEM_DEV(o->node->fs),ino=o->node->ino;int64_t base=whence==1?(int64_t)o->position:whence==2?(int64_t)o->node->size:0;
    pthread_mutex_unlock(&mem_lock);
    int64_t s=base+start,e=length>0?s+length:length==0?INT64_MAX:s;
    if(length<0)s+=length;
    if(s<0 || e<s)return linux_error(22);
    uint64_t owner=process_pid_now();
    int conflict=fs_lock_conflict((dev_t)dev,(ino_t)ino,owner,s,e,type==2?0:type);
    if(get) {
        if(conflict<0 || type==2) { fs_pack(p,0,2,2);return 0; }
        struct FsLock *l=&fs_locks[conflict];
        fs_pack(p,0,l->type,2);fs_pack(p,2,0,2);fs_pack(p,8,(uint64_t)l->start,8);
        fs_pack(p,16,l->end==INT64_MAX?0:(uint64_t)(l->end-l->start),8);fs_pack(p,24,(uint32_t)l->owner,4);
        return 0;
    }
    if(type!=2 && conflict>=0)return wait && schedule_active?IO_BLOCK:linux_error(11);
    return fs_lock_set((dev_t)dev,(ino_t)ino,owner,s,e,type);
}
static uint64_t mem_flock_owner(const MemOpen *o) { return FS_FLOCK_OWNER|((uint64_t)(uintptr_t)o>>4); }
static uint64_t mem_flock(MemOpen *o,uint64_t operation) {
    uint64_t kind=operation&~UINT64_C(4);
    if(operation&~UINT64_C(15) || (kind!=1 && kind!=2 && kind!=8))return linux_error(22);
    dev_t dev=(dev_t)MEM_DEV(o->node->fs);ino_t ino=(ino_t)o->node->ino;unsigned type=kind==1?0:kind==2?1:2;
    if(type!=2 && fs_lock_conflict(dev,ino,mem_flock_owner(o),0,INT64_MAX,type)>=0)
        return (operation&4) || !schedule_active?linux_error(11):IO_BLOCK;
    return fs_lock_set(dev,ino,mem_flock_owner(o),0,INT64_MAX,type);
}
/* The guest descriptor's description is going (io_release): its flock goes with the last reference. */
static void mem_release(MemOpen *o) {
    if(o->refs==1 && __atomic_load_n(&fs_locks_taken,__ATOMIC_ACQUIRE))fs_locks_release((dev_t)MEM_DEV(o->node->fs),(ino_t)o->node->ino,mem_flock_owner(o));
    mem_open_put(o);
}
/* sendfile (out, in, &offset) and copy_file_range (in, &in_at, out, &out_at): between
   memfs and host files, through a buffer, at most 1 MiB per call (callers loop). Others: EINVAL. */
static uint64_t mem_copy(uint64_t in_fd,uint64_t in_at,uint64_t out_fd,uint64_t out_at,uint64_t count) {
    MemOpen *in=mem_fd(in_fd),*out=mem_fd(out_fd);
    int in_host=in?-1:fs_host(in_fd),out_host=out?-1:fs_host(out_fd);
    if((!in && in_host<0) || (!out && out_host<0))return in_fd<GUEST_FD_LIMIT && out_fd<GUEST_FD_LIMIT && fd_kind[in_fd] && fd_kind[out_fd]?linux_error(22):linux_error(9);
    if((in_host>=0 && FS(fs_slot(in_fd)).dir) || (out_host>=0 && FS(fs_slot(out_fd)).dir))return linux_error(21);
    if((in && (in->flags&0x200000 || (in->flags&3)==1)) || (out && (out->flags&0x200000 || !(out->flags&3))))return linux_error(9);
    if((in && !mem_is(in->node,MEM_REG)) || (out && !mem_is(out->node,MEM_REG)))return linux_error(in && mem_is(in->node,MEM_DIR)?21:22);
    if(out_host>=0 && FS(fs_slot(out_fd)).ro)return linux_error(30);
    unsigned char *pin=in_at?lookup(in_at,8,6):NULL,*pout=out_at?lookup(out_at,8,6):NULL;
    if((in_at && !pin) || (out_at && !pout))return linux_error(14);
    if(count>1024*1024)count=1024*1024;
    if(!count)return 0;
    static _Thread_local unsigned char *buffer;                                 /* per worker, kept: sendfile runs per file */
    if(!buffer && !(buffer=malloc(1024*1024)))return linux_error(12);
    int64_t from=pin?(int64_t)le(pin,8):-1,got;
    if(in) { pthread_mutex_lock(&mem_lock);if(from<0)from=(int64_t)in->position;got=(int64_t)mem_read_at(in->node,(uint64_t)from,buffer,count);if(!pin)in->position+=(uint64_t)got;pthread_mutex_unlock(&mem_lock); }
    else { if(from<0 && (from=lseek(in_host,0,SEEK_CUR))<0)return fs_error();
           got=pread(in_host,buffer,(size_t)count,from);if(got<0)return fs_error();if(!pin)lseek(in_host,from+got,SEEK_SET); }
    int64_t put=0;
    if(got>0) {
        if(out) {
            pthread_mutex_lock(&mem_lock);
            uint64_t to=pout?le(pout,8):(out->flags&1024)?out->node->size:out->position;
            put=mem_write_at(out->node,to,buffer,(uint64_t)got);
            if(put>0 && pout)fs_pack(pout,0,to+(uint64_t)put,8);else if(put>0)out->position=to+(uint64_t)put;
            uint64_t dev=MEM_DEV(out->node->fs),ino=out->node->ino;pthread_mutex_unlock(&mem_lock);
            if(put>0 && __atomic_load_n(&file_shares_live,__ATOMIC_RELAXED))file_share_written_at(dev,ino,to,buffer,(size_t)put);
        } else {
            int append=(fcntl(out_host,F_GETFL)&O_APPEND)!=0;
            off_t to=pout?(off_t)le(pout,8):append?lseek(out_host,0,SEEK_END):lseek(out_host,0,SEEK_CUR);
            put=to<0?-1:pwrite(out_host,buffer,(size_t)got,to);
            if(put<0)put=(int64_t)fs_error();
            else {
                if(__atomic_load_n(&file_shares_live,__ATOMIC_RELAXED))file_share_written(out_host,(uint64_t)to,buffer,(size_t)put);
                __atomic_fetch_add(&fs_written,(uint64_t)put,__ATOMIC_RELAXED);
                if(pout)fs_pack(pout,0,(uint64_t)(to+put),8);else lseek(out_host,to+put,SEEK_SET);
            }
        }
    }
    if(pin && got>0 && put>0)fs_pack(pin,0,(uint64_t)from+(uint64_t)put,8);
    if(put<0)return (uint64_t)put;
    return got<=0?0:(uint64_t)put;
}
/* For mapping.h: a memfs regular file's identity, size and node (held by the caller's file share). */
static int mem_map_file(uint64_t guestfd,uint64_t *dev,uint64_t *ino,uint64_t *size,int *writable,MemNode **node) {
    MemOpen *o=mem_fd(guestfd);if(!o)return 0;
    pthread_mutex_lock(&mem_lock);
    MemNode *n=o->node;int regular=mem_is(n,MEM_REG) && !(o->flags&0x200000);
    if(regular) { *dev=MEM_DEV(n->fs);*ino=n->ino;*size=n->size;*writable=(o->flags&3)==2;if(node) { *node=n;n->refs++; } }
    pthread_mutex_unlock(&mem_lock);
    return regular?1:-1;
}
static void mem_map_read(MemNode *n,unsigned char *out,size_t count,uint64_t offset) {
    pthread_mutex_lock(&mem_lock);uint64_t got=mem_read_at(n,offset,out,count);pthread_mutex_unlock(&mem_lock);
    if(got<count)memset(out+got,0,count-(size_t)got);
}
static void mem_map_write(MemNode *n,const unsigned char *data,size_t count) {   /* a shared mapping's flush: the file's bytes only */
    pthread_mutex_lock(&mem_lock);
    if(count>n->size)count=(size_t)n->size;
    for(size_t done=0;done<count;done+=MEM_PAGE) {                              /* only pages that differ: holes stay holes */
        size_t take=count-done<MEM_PAGE?count-done:MEM_PAGE;unsigned char *p=mem_page_at(n,done/MEM_PAGE);
        int changed=0;
        if(p) { if(memcmp(p,data+done,take)) { memcpy(p,data+done,take);changed=1; } }
        else { size_t k=0;while(k<take && !data[done+k])k++;if(k<take && (p=mem_page_new(n,done/MEM_PAGE))) { memcpy(p,data+done,take);changed=1; } }
        if(changed)n->mtime=n->ctime=mem_now();
    }
    pthread_mutex_unlock(&mem_lock);
}
/* A working directory in memfs (its descriptor is the root's, never used for lookups). */
static GuestCwd *mem_cwd_new(MemNode *n,int64_t *error) {
    if(!mem_is(n,MEM_DIR)) { *error=-20;return NULL; }
    if(!mem_may(n,1)) { *error=-13;return NULL; }
    GuestCwd *next=calloc(1,sizeof(*next));if(!next) { *error=-12;return NULL; }
    next->fd=fs_root>=0?dup(fs_root):-1;next->refs=1;next->mem=n;n->refs++;mem_dir_path(n,next->path);
    return next;
}
static int mem_abort(void) { syscall_abort=1;cpu->gpr[0]=linux_error(11);return -3; } /* needs the runtime lock: the locked path redoes it */
static int mem_answer(uint64_t r) { if(r==IO_BLOCK) { cpu->pc-=4;return -2; }cpu->gpr[0]=r;return -3; }
static int mem_fd_call(uint64_t call,MemOpen *o) {
    uint64_t *x=cpu->gpr,fd=x[0],r=0;MemNode *n=o->node;
    switch(call) {
    case 63:case 64: return mem_answer(mem_transfer(o,call==64,x[1],x[2],-1));
    case 67:case 68: return mem_answer((int64_t)x[3]<0?linux_error(22):mem_transfer(o,call==68,x[1],x[2],(int64_t)x[3]));
    case 65:case 66: return mem_answer(mem_vector(o,call==66,x[1],x[2],-1));
    case 69:case 70: return mem_answer((int64_t)x[3]<0?linux_error(22):mem_vector(o,call==70,x[1],x[2],(int64_t)x[3]));
    case 286:case 287:
        if(x[5])return mem_answer(linux_error(95));
        if((int64_t)x[3]<-1)return mem_answer(linux_error(22));
        return mem_answer(mem_vector(o,call==287,x[1],x[2],(int64_t)x[3]));
    case 80: case 61: case 44: {
        uint64_t size=call==80?128:call==44?120:x[2];
        if(call==61 && size>1024*1024)size=1024*1024;
        unsigned char *out=lookup(x[1],size,2);if(!out)return mem_answer(linux_error(14));
        pthread_mutex_lock(&mem_lock);
        if(call==80)mem_stat_pack(out,n);else if(call==44)mem_statfs_pack(out,n->fs);
        else r=(o->flags&0x200000)?linux_error(9):mem_result(mem_getdents(o,out,(size_t)size));
        pthread_mutex_unlock(&mem_lock);
        return mem_answer(r);
    }
    case 57:
        if(__atomic_load_n(&fs_locks_taken,__ATOMIC_ACQUIRE)) {
            if(syscall_unlocked)return mem_abort();
            fs_locks_release((dev_t)MEM_DEV(n->fs),(ino_t)n->ino,process_pid_now());
        }
        io_release(&IO(fd));fd_unbind(fd);return mem_answer(0);
    case 25: {
        uint64_t command=x[1];
        if(command==0 || command==1030)return MEM_PASS;                     /* F_DUPFD: guest_dup */
        if(command==1)return mem_answer(IO(fd).cloexec);
        if(command==2) { if(x[2]>1)return mem_answer(linux_error(22));IO(fd).cloexec=(unsigned)x[2];return mem_answer(0); }
        if(command==3)return mem_answer((o->flags&(3|1024|2048|0x200000))|131072);
        if(command==4) {
            if(o->flags&0x200000)return mem_answer(linux_error(9));
            pthread_mutex_lock(&mem_lock);o->flags=(o->flags&~(1024u|2048u))|(unsigned)(x[2]&(1024|2048));pthread_mutex_unlock(&mem_lock);
            return mem_answer(0);
        }
        if((command>=5 && command<=7) || (command>=36 && command<=38)) {
            if(syscall_unlocked)return mem_abort();
            if(o->flags&0x200000)return mem_answer(linux_error(9));
            return mem_answer(mem_fcntl_lock(o,command));
        }
        if(command==1025)return mem_answer(2);                               /* F_GETLEASE: none */
        return mem_answer(linux_error(22));
    }
    case 32: if(syscall_unlocked)return mem_abort();return mem_answer(mem_flock(o,x[1]));
    case 29:
        if(x[1]==0x5451 || x[1]==0x5450) { IO(fd).cloexec=x[1]==0x5451;return mem_answer(0); }
        if(x[1]==0x5421) {
            const unsigned char *p=lookup(x[2],4,4);if(!p)return mem_answer(linux_error(14));
            pthread_mutex_lock(&mem_lock);o->flags=le(p,4)?o->flags|2048u:o->flags&~2048u;pthread_mutex_unlock(&mem_lock);
            return mem_answer(0);
        }
        if(x[1]==0x541b && mem_is(n,MEM_REG)) {                               /* FIONREAD */
            unsigned char *p=lookup(x[2],4,2);if(!p)return mem_answer(linux_error(14));
            pthread_mutex_lock(&mem_lock);fs_pack(p,0,n->size>o->position?n->size-o->position:0,4);pthread_mutex_unlock(&mem_lock);
            return mem_answer(0);
        }
        return mem_answer(linux_error(25));
    case 50: {
        int64_t error=0;pthread_mutex_lock(&mem_lock);GuestCwd *next=mem_cwd_new(n,&error);pthread_mutex_unlock(&mem_lock);
        if(!next)return mem_answer(linux_error((unsigned)-error));
        fs_cwd_release();fs_cwd=next;return mem_answer(0);
    }
    case 82:case 83:case 267: return mem_answer((o->flags&0x200000)?linux_error(9):0);
    case 84: return mem_answer(x[3]&~UINT64_C(7)?linux_error(22):0);
    case 213: return mem_answer(mem_is(n,MEM_REG)?0:linux_error(22));
    case 223: return mem_answer((int32_t)x[3]<0 || (int32_t)x[3]>5 || (int64_t)x[2]<0?linux_error(22):0);
    }
    pthread_mutex_lock(&mem_lock);
    if(call==62)r=mem_result(mem_lseek(o,(int64_t)x[1],x[2]));
    else if(call==46) {
        if(o->flags&0x200000)r=linux_error(9);
        else if(!(o->flags&3) || !mem_is(n,MEM_REG) || (int64_t)x[1]<0)r=linux_error(22);
        else r=mem_result(mem_resize(n,x[1]));
    }
    else if(call==47)r=mem_result(mem_fallocate(o,x[1],x[2],x[3]));
    else if(call==52)r=(o->flags&0x200000)?linux_error(9):mem_result(mem_chmod(n,x[1]&07777));
    else if(call==55)r=mem_result(mem_chown(n,(uint32_t)x[1],(uint32_t)x[2]));
    else r=linux_error(38);
    if((call==52 || call==55) && !r)mem_notify_child(o->dir,o->name,n,IN_ATTRIB);
    if((call==46 || call==47) && !r)mem_notify_child(o->dir,o->name,n,IN_MODIFY);
    pthread_mutex_unlock(&mem_lock);
    return mem_answer(r);
}
/* Path arguments: (directory descriptor, path), whether a final symlink is followed,
   and whether an empty path names the descriptor (AT_EMPTY_PATH). */
typedef struct { uint64_t dir,at;int follow,empty; } MemArg;
static int64_t guest_fd_path(const char *path);static int guest_syscall_dispatch(void); /* syscall.h */
static int64_t mem_path_op(uint64_t call,MemPath *w,const int *res,uint64_t *fifo);
static int64_t upm_cross(uint64_t call,MemPath *w,const int *res);   /* upm.h */
static MemEntry *upm_entry(const char *canon);static int upm_lexical(uint64_t dir,const char *input,char out[1025]);
static int64_t upm_create_fast(const char *canon,uint64_t flags,uint64_t mode);
static void upm_gone(const char *canon);
static int mem_path_call(uint64_t call) {
    uint64_t *x=cpu->gpr;MemArg a[2];unsigned n=1;
    switch(call) {
    case 56: a[0]=(MemArg){x[0],x[1],!(x[2]&32768) && (x[2]&192)!=192,0};break;
    case 437: {
        if(x[3]<24 || x[3]>4096)return MEM_PASS;
        const unsigned char *how=lookup(x[2],24,4);if(!how)return MEM_PASS;
        uint64_t flags=le(how,8);a[0]=(MemArg){x[0],x[1],!(flags&32768) && (flags&192)!=192,0};break;
    }
    case 79: if(x[3]&~(UINT64_C(256)|2048|4096))return MEM_PASS;a[0]=(MemArg){x[0],x[1],!(x[3]&256),(x[3]&4096)!=0};break;
    case 48: a[0]=(MemArg){x[0],x[1],1,0};break;
    case 439: a[0]=(MemArg){x[0],x[1],!(x[3]&256),(x[3]&4096)!=0};break;
    case 78: case 34: case 35: case 33: a[0]=(MemArg){x[0],x[1],0,0};break;
    case 38: case 276: a[0]=(MemArg){x[0],x[1],0,0};a[1]=(MemArg){x[2],x[3],0,0};n=2;break;
    case 36: a[0]=(MemArg){x[1],x[2],0,0};break;
    case 37: a[0]=(MemArg){x[0],x[1],(x[4]&0x400)!=0,(x[4]&0x1000)!=0};a[1]=(MemArg){x[2],x[3],0,0};n=2;break;
    case 88:
        if(!x[1]) {                                                         /* futimens */
            MemOpen *o=mem_fd(x[0]);if(!o)return MEM_PASS;
            const unsigned char *in=x[2]?lookup(x[2],32,4):NULL;if(x[2] && !in)return mem_answer(linux_error(14));
            if(x[3])return mem_answer(linux_error(22));
            pthread_mutex_lock(&mem_lock);uint64_t r=mem_result(mem_utimens(o->node,in));if(!r)mem_notify_child(o->dir,o->name,o->node,IN_ATTRIB);pthread_mutex_unlock(&mem_lock);
            return mem_answer(r);
        }
        a[0]=(MemArg){x[0],x[1],!(x[3]&256),(x[3]&4096)!=0};break;
    case 53: a[0]=(MemArg){x[0],x[1],1,0};break;
    case 452: if(x[3]&~UINT64_C(0x1100))return MEM_PASS;a[0]=(MemArg){x[0],x[1],!(x[3]&256),(x[3]&4096)!=0};break;
    case 54: a[0]=(MemArg){x[0],x[1],!(x[4]&256),(x[4]&4096)!=0};break;
    case 45: case 49: case 43: case 5: case 8: case 11: case 14: a[0]=(MemArg){(uint64_t)-100,x[0],1,0};break;
    case 6: case 9: case 12: case 15: a[0]=(MemArg){(uint64_t)-100,x[0],0,0};break;
    case 7: case 10: case 13: case 16: a[0]=(MemArg){x[0],0,0,1};break;           /* f*xattr: the descriptor */
    default: return MEM_PASS;
    }
    char text[2][1025];MemPath w[2];int res[2]={0,0},rewritten[2]={0,0};
    for(unsigned i=0;i<n;i++) {
        const unsigned char *first=a[i].at?lookup(a[i].at,1,4):NULL;
        if(a[i].empty && (!a[i].at || (first && !*first))) {                    /* the descriptor itself */
            if(!mem_fd(a[i].dir))return MEM_PASS;
            text[i][0]=0;continue;
        }
        if(fs_string(a[i].at,text[i])<0)return MEM_PASS;                      /* the host reports it */
        int64_t fd=call==37 && !i && a[i].follow?guest_fd_path(text[i]):-1;      /* linkat of /proc/self/fd/N */
        if(fd>=0 && fd<GUEST_FD_LIMIT && mem_fd((uint64_t)fd)) { a[i].dir=(uint64_t)fd;a[i].empty=1;text[i][0]=0; }
    }
    int probe=0;unsigned scope=call==437?(unsigned)(le(lookup(x[2],24,4)+16,8)&4):0; /* RESOLVE_NO_SYMLINKS */
    int descriptor=0;for(unsigned i=0;i<n;i++)descriptor|=!text[i][0];          /* a memfs descriptor itself (upm.h's files too) */
    /* upm.h: a path naming a pending file plainly (no "..": its directories are the
       host's real ones) is that file, with no host walk; others take the walk's redirect. */
    unsigned pending=0,plain=0;char pend[2][1025];
    for(unsigned i=0;upm_fs && i<n;i++)if(text[i][0] && upm_lexical(a[i].dir,text[i],pend[i])) { plain|=1u<<i;if(upm_maybe(pend[i]))pending|=1u<<i; }
    if(upm_fs && call==56 && !pending && (plain&1) && (x[2]&64) && !mem_mount_inside(pend[0])) {   /* a new file in a mirrored directory */
        int64_t r=upm_create_fast(pend[0],x[2],x[3]);
        if(r!=UPM_DISK)return mem_answer(mem_result(r));
    }
    for(unsigned attempt=0;;attempt++) {
        if(!mem_mount_count && !probe && !descriptor && !pending) {               /* no mount to name lexically: the host first */
            if(syscall_unlocked)return MEM_PASS;
            goto host;
        }
        pthread_mutex_lock(&mem_lock);fs_scope.resolve=scope;
        int any=0,again=0,error=0;
        for(unsigned i=0;i<n && !error;i++) {
            MemEntry *held=(pending&(1u<<i))?upm_entry(pend[i]):NULL;
            if(!text[i][0]) {                                                    /* AT_EMPTY_PATH on a memfs descriptor */
                MemOpen *o=mem_fd(a[i].dir);w[i].dir=NULL;w[i].entry=NULL;w[i].node=o->node;w[i].name[0]=0;w[i].slash=0;w[i].canon[0]=0;res[i]=1;
            } else if(held) {                                                    /* a pending file (upm.h) */
                w[i].dir=held->dir;w[i].entry=held;w[i].node=held->node;snprintf(w[i].name,sizeof w[i].name,"%s",held->name);w[i].slash=0;strcpy(w[i].canon,pend[i]);res[i]=1;
            } else if((res[i]=mem_resolve(a[i].dir,text[i],a[i].follow,probe,&w[i],&rewritten[i],mem_rewrite[i]))<0)error=res[i];
            any|=res[i]==1;again|=rewritten[i];
        }
        fs_scope.resolve=0;
        if(error) { pthread_mutex_unlock(&mem_lock);return mem_answer(linux_error((unsigned)-error)); }
        if(any) {
            uint64_t fifo[2]={0,0};
            int64_t r=mem_path_op(call,w,res,fifo);                               /* unlocks mem_lock */
            if(r==MEM_FIFO) {                                                     /* opened as the container's shared pipe */
                if(syscall_unlocked)return mem_abort();
                return mem_answer(io_fifo_open((dev_t)fifo[0],(ino_t)fifo[1],call==437?le(lookup(x[2],8,4),8):x[2]));
            }
            if(r==MEM_PASS-1)return mem_abort();
            if(r==MEM_PASS-2)return -3;                                           /* answered */
            if(r==MEM_PASS-3) { if(attempt==8)return mem_answer(linux_error(40));probe=0;continue; }   /* upm.h: written back, the host's now */
            return mem_answer(mem_result(r));
        }
        pthread_mutex_unlock(&mem_lock);
        if(syscall_unlocked) { if(again || probe)return mem_abort();return MEM_PASS; }
      host:
        if(attempt==8)return mem_answer(linux_error(40));
        uint64_t saved[9];memcpy(saved,x,sizeof(saved));
        for(unsigned i=0;i<n;i++)if(rewritten[i]) { fs_swap.at[i]=a[i].at;fs_swap.path[i]=mem_rewrite[i];strcpy(text[i],mem_rewrite[i]); }
        mem_inside=1;int rc=guest_syscall_dispatch();mem_inside=0;
        fs_swap.path[0]=fs_swap.path[1]=NULL;
        if(!mem_redirect_take())return rc;
        memcpy(x,saved,sizeof(saved));
        if(n==1 && mem_redirect.path[0]==UPM_MARK[0] && upm_fs) {                /* a pending file the walk found: by its path, no second walk */
            snprintf(pend[0],sizeof pend[0],"%s",mem_redirect.path+1);pending=1;continue;
        }
        probe=1;                                                                 /* the host walk reached a mount: ours after all */
    }
}
/* The memfs part of a path call (mem_lock held; released here). Returns a result
   (>= 0 or -errno), MEM_FIFO (fifo: its identity), MEM_PASS-1 (needs the locked
   path) or MEM_PASS-2 (answered in x0). */
static int64_t mem_path_op(uint64_t call,MemPath *w,const int *res,uint64_t *fifo) {
    uint64_t *x=cpu->gpr;int64_t r=0;MemNode *n=w[0].node;
    if((call==38 || call==276 || call==37) && res[0]!=res[1]) {                 /* between memfs and the host */
        int layer=(res[0]==1 && n && n->fs==upm_fs) || (res[1]==1 && w[1].dir && w[1].dir->fs==upm_fs);
        if(layer)return upm_cross(call,w,res);                                   /* upm.h (it releases mem_lock) */
        r=-18;goto done;                                                         /* EXDEV */
    }
    int layer=n && n->fs==upm_fs;char was[1025];if(layer)snprintf(was,sizeof was,"%s",w[0].canon);
    switch(call) {
    case 56: case 437: {
        uint64_t flags=x[2],mode=x[3];
        if(call==437) {
            const unsigned char *how=lookup(x[2],x[3],4);if(!how) { r=-14;break; }
            for(uint64_t i=24;i<x[3];i++)if(how[i]) { r=-7;goto done; }
            flags=le(how,8);mode=le(how+8,8);uint64_t resolve=le(how+16,8);
            if((resolve&~UINT64_C(0x3f)) || (flags>>32) || mode>07777 || (mode && !(flags&UINT64_C(0x400040)))) { r=-22;break; }
            if(resolve&0x38) { r=resolve&0x20?-11:-18;break; }                      /* RESOLVE_CACHED, BENEATH, IN_ROOT: not in memfs */
        } else if((flags&UINT64_C(0x400000)) && (flags&3)==0) { r=-22;break; }
        r=mem_open(&w[0],flags,mode);
        if(r==MEM_FIFO) { fifo[0]=MEM_DEV(n->fs);fifo[1]=n->ino; }
        break;
    }
    case 79: {
        unsigned char *out=lookup(x[2],128,2);
        if(!out)r=-14;else if(!n)r=-2;else if(w[0].slash && !mem_is(n,MEM_DIR))r=-20;else mem_stat_pack(out,n);
        break;
    }
    case 48: case 439:
        if(x[2]&~UINT64_C(7) || (call==439 && (x[3]&~UINT64_C(0x1300))))r=-22;
        else if(!n)r=-2;else r=mem_access(n,x[2]);
        break;
    case 78: {
        if(!n)r=-2;else if(!mem_is(n,MEM_LNK))r=-22;else if(!x[3] || (int64_t)(int32_t)x[3]<=0)r=-22;
        else { size_t k=strlen(n->target);if(k>x[3])k=(size_t)x[3];unsigned char *out=lookup(x[2],k,2);if(!out)r=-14;else { memcpy(out,n->target,k);r=(int64_t)k; } }
        break;
    }
    case 34: { int error;if(w[0].node)r=-17;else if(!mem_create(&w[0],MEM_DIR|((uint32_t)x[2]&01777&~fs_umask),&error))r=error;break; }
    case 35: r=x[2]&~UINT64_C(0x200)?-22:mem_unlink(&w[0],x[2]);break;
    case 38: case 276: r=mem_rename(&w[0],&w[1],call==276?x[4]:0);break;
    case 36: { char target[1025];if(fs_string(x[0],target)<0)r=errno==EFAULT?-14:errno==ENAMETOOLONG?-36:-2;else r=mem_symlink(target,&w[0]);break; }
    case 37:
        if(x[4]&~UINT64_C(0x1400))r=-22;
        else if(!n)r=-2;
        else r=mem_hardlink(n,&w[1]);
        break;
    case 88: {
        const unsigned char *in=x[2]?lookup(x[2],32,4):NULL;
        if(x[2] && !in)r=-14;else if(x[3]&~UINT64_C(0x1100))r=-22;else if(!n)r=-2;else r=mem_utimens(n,in);
        break;
    }
    case 53: r=!n?-2:mem_chmod(n,x[2]&07777);break;
    case 452: r=!n?-2:mem_is(n,MEM_LNK)?-95:mem_chmod(n,x[2]&07777);break;
    case 54: r=x[4]&~UINT64_C(0x1100)?-22:!n?-2:mem_chown(n,(uint32_t)x[2],(uint32_t)x[3]);break;
    case 45:
        if(!n)r=-2;else if(mem_is(n,MEM_DIR))r=-21;else if(!mem_is(n,MEM_REG) || (int64_t)x[1]<0)r=-22;
        else if(!mem_may(n,2))r=-13;else if(n->fs->readonly)r=-30;else r=mem_resize(n,x[1]);
        break;
    case 33: {
        uint32_t type=(uint32_t)(x[2]&0170000);
        if(type && type!=0010000 && type!=MEM_REG && type!=0140000) { r=-1;break; }
        int error;if(w[0].node)r=-17;else if(!mem_create(&w[0],(type?type:MEM_REG)|((uint32_t)x[2]&07777&~fs_umask),&error))r=error;
        break;
    }
    case 49: {
        if(!n) { r=-2;break; }
        GuestCwd *next=mem_cwd_new(n,&r);if(!next)break;
        pthread_mutex_unlock(&mem_lock);fs_cwd_release();fs_cwd=next;return 0;
    }
    case 43: { unsigned char *out=lookup(x[1],120,2);if(!out)r=-14;else if(!n)r=-2;else mem_statfs_pack(out,n->fs);break; }
    case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: {
        if(!n) { r=-2;break; }
        char name[1025]="";
        if((call<11 || call>13) && fs_string(x[1],name)<0) { r=errno==EFAULT?-14:-34;break; }
        if(call>=11 && call<=13)r=mem_xattr(n,call,"",x[1],x[2],0);
        else r=mem_xattr(n,call,name,x[2],x[3],x[4]);
        break;
    }
    default: r=-38;
    }
    if(!r && n && (call==53 || call==452 || call==54 || call==88 || (call>=5 && call<=7) || (call>=14 && call<=16)))mem_notify_child(w[0].dir,w[0].name,n,IN_ATTRIB);
    if(!r && n && call==45)mem_notify_child(w[0].dir,w[0].name,n,IN_MODIFY);
    if(!r && layer && (call==35 || ((call==38 || call==276) && !(call==276 && (x[4]&2)))))upm_gone(was);  /* upm.h: no longer at that path */
done:
    pthread_mutex_unlock(&mem_lock);
    return r;
}
/* System calls on memfs, before the host's handlers see them (guest_syscall_dispatch):
   MEM_PASS when the call is not memfs's, else as guest_syscall_dispatch returns. */
static int mem_route(uint64_t call) {
    uint64_t *x=cpu->gpr;
    mem_redirect.pending=0;
    switch(call) {
    case 63:case 64:case 65:case 66:case 67:case 68:case 69:case 70:case 286:case 287:case 62:case 80:case 61:case 46:case 47:
    case 82:case 83:case 84:case 52:case 55:case 25:case 29:case 32:case 44:case 50:case 223:case 57:case 213:case 267: {
        MemOpen *o=mem_fd(x[0]);return o?mem_fd_call(call,o):MEM_PASS;
    }
    case 71: if(!mem_fd(x[0]) && !mem_fd(x[1]))return MEM_PASS;return mem_answer(mem_copy(x[1],x[2],x[0],0,x[3]));
    case 285: if(!mem_fd(x[0]) && !mem_fd(x[2]))return MEM_PASS;return mem_answer(x[5]?linux_error(22):mem_copy(x[0],x[1],x[2],x[3],x[4]));
    case 17: {                                                                  /* getcwd in memfs */
        if(!fs_cwd || !fs_cwd->mem)return MEM_PASS;
        char path[1025];pthread_mutex_lock(&mem_lock);mem_dir_path(fs_cwd->mem,path);pthread_mutex_unlock(&mem_lock);
        if(!*path)return mem_answer(linux_error(2));
        size_t size=strlen(path)+1;if(x[1]<size)return mem_answer(linux_error(34));
        unsigned char *out=lookup(x[0],size,2);if(!out)return mem_answer(linux_error(14));
        memcpy(out,path,size);return mem_answer(size);
    }
    }
    return mem_path_call(call);
}
#ifdef VMFREE_HVF
/* guest_syscall_unlocked's memfs part (the mm lock held, not the runtime lock): 1 served,
   0 for the locked path, -1 not memfs's. */
static int mem_route_unlocked(uint64_t call) {
    syscall_unlocked=1;syscall_abort=0;
    int r=mem_route(call);
    syscall_unlocked=0;
    if(r==MEM_PASS)return -1;
    if(syscall_abort)return 0;
    if(r==-2) { cpu->pc+=4;return 0; }
    cpu->exclusive_valid=0;return 1;
}
#endif
/* exec of a memfs file: a host copy for the loader (exe: its guest path). Returns the
   descriptor, MEM_HOST_PATH when the host resolves the path (fs_swap may hold a
   rewritten one; mem_exec_done clears it), or -errno. probe: after fs_walk redirected. */
static int mem_exec_open(uint64_t dir,uint64_t at,char exe[1025],int probe) {
    if(!mem_active || (!mem_mount_count && !probe))return MEM_HOST_PATH;
    char text[1025];if(fs_string(at,text)<0)return MEM_HOST_PATH;
    MemPath w;int rewritten;
    for(unsigned attempt=0;attempt<8;attempt++) {
        pthread_mutex_lock(&mem_lock);
        int r=mem_resolve(dir,text,1,probe,&w,&rewritten,mem_rewrite[0]);
        if(r<=0) {
            pthread_mutex_unlock(&mem_lock);
            if(r<0)return r;
            if(!rewritten)return MEM_HOST_PATH;
            strcpy(text,mem_rewrite[0]);fs_swap.at[0]=at;fs_swap.path[0]=mem_rewrite[0];probe=1;continue;
        }
        MemNode *n=w.node;int fd=-1;
        if(!n)r=-2;
        else if(!mem_is(n,MEM_REG))r=-13;
        else if(n->fs->noexec || !(n->mode&0111) || !mem_may(n,1))r=-13;
        else {
            char path[]="/tmp/dockiva-exec-XXXXXX";fd=mkstemp(path);
            if(fd<0)r=-12;
            else {
                unlink(path);(void)fcntl(fd,F_SETFD,FD_CLOEXEC);
                unsigned char *buffer=malloc(1<<20);
                for(uint64_t done=0;buffer && done<n->size;) {
                    uint64_t got=mem_read_at(n,done,buffer,1<<20);
                    if(!got || write(fd,buffer,(size_t)got)!=(ssize_t)got) { r=-12;break; }
                    done+=got;
                }
                if(!buffer)r=-12;
                free(buffer);
                if(r>0 && (fchmod(fd,(mode_t)(n->mode&07777))<0 || lseek(fd,0,SEEK_SET)<0))r=-12;
                if(r>0)snprintf(exe,1025,"%s",w.canon);
            }
        }
        pthread_mutex_unlock(&mem_lock);
        if(r>0)return fd;
        if(fd>=0)close(fd);
        return r;
    }
    return -40;
}
static void mem_exec_done(void) { fs_swap.path[0]=fs_swap.path[1]=NULL; }
/* --cwd in memfs: 1 entered, 0 not memfs's, -1 with errno. */
static int mem_start_cwd(const char *path) {
    if(!mem_mount_count)return 0;
    MemPath w;int rewritten;char rewrite[1025];int64_t error=0;
    pthread_mutex_lock(&mem_lock);
    int r=mem_resolve((uint64_t)-100,path,1,0,&w,&rewritten,rewrite);
    GuestCwd *next=r==1 && w.node?mem_cwd_new(w.node,&error):NULL;
    pthread_mutex_unlock(&mem_lock);
    if(r==0)return 0;
    if(!next) { errno=r<0?(int)-r:w.node?(int)-error:ENOENT;return -1; }
    fs_cwd_release();fs_cwd=next;return 1;
}
static size_t mem_mount_line(unsigned i,char *out,size_t capacity,int mountinfo,unsigned id) {
    const MemFs *fs=mem_mounts[i];char options[128];
    snprintf(options,sizeof options,"%s,nosuid,nodev%s,relatime,size=%lluk,mode=%o",fs->readonly?"ro":"rw",fs->noexec?",noexec":"",
             (unsigned long long)((fs->limit?fs->limit:proc_mem_total()/2)>>10),(unsigned)(fs->root->mode&07777));
    int n=mountinfo?snprintf(out,capacity,"%u 20 0:%u / %s %s - tmpfs tmpfs %s\n",id,(unsigned)MEM_DEV(fs),fs->path,options,options)
                   :snprintf(out,capacity,"tmpfs %s tmpfs %s 0 0\n",fs->path,options);
    return n>0 && (size_t)n<capacity?(size_t)n:0;
}
/* inotify_add_watch on a memfs path: the watch descriptor, MEM_HOST_PATH, or -errno. */
static int64_t mem_inotify_watch(struct GuestInotify *in,uint64_t at,uint32_t mask,int follow) {
    if(!mem_mount_count)return MEM_HOST_PATH;
    char text[1025];if(fs_string(at,text)<0)return MEM_HOST_PATH;
    MemPath w;int rewritten;char rewrite[1025];
    pthread_mutex_lock(&mem_lock);
    int r=mem_resolve((uint64_t)-100,text,follow,0,&w,&rewritten,rewrite);
    if(r<=0 || !w.node) { pthread_mutex_unlock(&mem_lock);return r<0?r:r?MEM_HOST_PATH:-2; }
    if((mask&IN_ONLYDIR) && !mem_is(w.node,MEM_DIR)) { pthread_mutex_unlock(&mem_lock);return -20; }
    pthread_mutex_lock(&in->lock);
    int64_t wd=-1;
    for(unsigned i=0;i<in->nwatch;i++)if(in->watch[i].mem==w.node) { in->watch[i].mask=(mask&IN_MASK_ADD)?in->watch[i].mask|mask:mask;wd=in->watch[i].wd; }
    if(wd<0) {
        if(in->nwatch==INOTIFY_WATCHES)wd=-28;
        else {
            struct GuestInotifyWatch *x=&in->watch[in->nwatch++];
            x->wd=(int)(wd=++in->next_wd);x->dir=mem_is(w.node,MEM_DIR);x->mask=mask;x->host[0]=0;x->mem=w.node;
            __atomic_fetch_add(&mem_watching,1,__ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&in->lock);pthread_mutex_unlock(&mem_lock);
    return wd;
}
/* bind/connect of an AF_UNIX path in memfs: a socket node whose identity names the
   abstract socket the host binds (compat.h). 0, -errno, MEM_HOST_PATH, or 1 with the
   host path to use in rewrite. ponytail: getsockname reports that abstract name, not
   the path; keep the path in the description if a program checks it. */
static int64_t mem_unix_address(int fd,const char *guest,int binding,int probe,struct NetDescription *d,char rewrite[1025]) {
    if(!mem_mount_count)return MEM_HOST_PATH;
    MemPath w;int rewritten;int64_t e=0;
    pthread_mutex_lock(&mem_lock);
    int r=mem_resolve((uint64_t)-100,guest,!binding,probe,&w,&rewritten,rewrite);
    if(r<=0) { pthread_mutex_unlock(&mem_lock);return r<0?r:rewritten?1:MEM_HOST_PATH; }
    MemNode *n=w.node;int created=0;
    if(binding) { int error;if(n)e=-98;else if(!mem_create(&w,0140000|(0777&~fs_umask),&error))e=error;else { n=w.node;created=1; } }
    else if(!n)e=-2;
    else if((n->mode&0170000)!=0140000)e=-111;
    else if(!mem_may(n,2))e=-13;
    unsigned char name[48];unsigned length=0;MemNode *dir=w.dir;uint64_t ino=n?n->ino:0;
    if(!e) { name[0]=0;length=1+(unsigned)snprintf((char *)name+1,sizeof(name)-1,"dockiva-memfs-%llx-%llx",(unsigned long long)MEM_DEV(n->fs),(unsigned long long)ino); }
    if(dir)dir->refs++;
    pthread_mutex_unlock(&mem_lock);
    if(!e) {
        uint64_t result=net_abstract_address(fd,name,length,binding,d);
        e=(int64_t)result<0?(int64_t)result:0;
    }
    pthread_mutex_lock(&mem_lock);
    if(e && created && dir) { MemEntry *x=mem_find(dir,w.name);if(x && x->node->ino==ino)mem_unlink_entry(x); } /* the bind failed: no node */
    mem_node_put(dir);
    pthread_mutex_unlock(&mem_lock);
    return e;
}
