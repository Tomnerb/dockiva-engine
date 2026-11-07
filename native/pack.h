/* Image files from one mapped file. macOS's open() costs 10 to 15 us however short
   the path (the open itself, not the lookup) and admits about 100k a second machine-
   wide; Node opens a thousand modules at start, the Go compiler every source file.
   <lower>.pack holds the image's small regular files (not ELF ones: the loader maps
   those) back to back, with an index by inode. An open of an image file it holds
   makes no host open: the slot gets the placeholder descriptor (the lower directory,
   one for every such slot and never closed by them, so a stray host call on it fails
   rather than reading the wrong file) and a PackOpen, the open description (offset;
   shared by dup and fork as on Linux) from a pool in the process block (fs.h), which
   the EL1 handlers allocate from too. read, pread, lseek, fstat and close are served from the mapping
   (fs_pack_call); anything else asks fs_host for the descriptor, which opens the
   real file at the description's offset and puts it in the slot (fs_pack_real), so
   every other call works as before.

   The pack (--lower-pack FILE; dockivad names one beside the image's root) is built
   once per image, in the background by the first run that finds none (pack_build):
   written under a temporary name, then renamed. An entry counts only if the image file still has its size, time and
   path, so a stale pack is ignored. */
#include <sys/mman.h>
#include <pthread.h>
#define PACK_MAGIC "DKVPACK1"
#define PACK_FILE_MAX (1u<<20)                       /* larger files are read in place */
#define PACK_TOTAL_MAX (UINT64_C(1)<<30)
typedef struct { char magic[8];uint64_t count,entries,strings,strings_size,data,total; } PackHeader;
typedef HvfPackEntry PackEntry;                      /* hvf_el1.h */
typedef HvfPackOpen PackOpen;                        /* hvf_el1.h */
_Static_assert(sizeof(struct stat)<=sizeof(((PackOpen *)0)->st),"PackOpen.st holds a struct stat");
static const unsigned char *pack_base;static uint64_t pack_bytes,pack_count;static const PackEntry *pack_entries;
static int pack_placeholder=-1;
static pthread_mutex_t pack_lock=PTHREAD_MUTEX_INITIALIZER;

static void pack_load(const char *lower,const char *path) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
    struct stat st;void *p=MAP_FAILED;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=(off_t)sizeof(PackHeader))p=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_SHARED,fd,0);
    close(fd);if(p==MAP_FAILED)return;
    const PackHeader *h=p;uint64_t size=(uint64_t)st.st_size;
    if(memcmp(h->magic,PACK_MAGIC,8) || h->total!=size || h->count>size/sizeof(PackEntry) || h->entries>size || h->count*sizeof(PackEntry)>size-h->entries ||
       h->strings>size || h->strings_size>size-h->strings || h->data>size) { munmap(p,(size_t)size);return; }
    const PackEntry *e=(const PackEntry *)(const void *)((const unsigned char *)p+h->entries);
    for(uint64_t i=0;i<h->count;i++)                                   /* every entry within the file, sorted by inode */
        if(e[i].offset<h->data || e[i].size>size-e[i].offset || e[i].path>=h->strings_size || e[i].path_length>h->strings_size-e[i].path ||
           (i && e[i].ino<=e[i-1].ino)) { munmap(p,(size_t)size);return; }
    if((pack_placeholder=open(lower,O_RDONLY|O_DIRECTORY|O_CLOEXEC))<0) { munmap(p,(size_t)size);return; }
    pack_base=p;pack_bytes=size;pack_count=h->count;pack_entries=e;
}
/* The entry for an image file (its stat and canonical guest path), or NULL. */
static const PackEntry *pack_find(const struct stat *st,const char *canon) {
    if(!pack_base || !schedule_active || canon[0]!='/')return NULL;     /* scheduled runs only (fork shares descriptions there) */
    uint64_t low=0,high=pack_count,ino=(uint64_t)st->st_ino;
    while(low<high) { uint64_t mid=(low+high)/2;if(pack_entries[mid].ino<ino)low=mid+1;else high=mid; }
    if(low==pack_count)return NULL;
    const PackEntry *e=&pack_entries[low];
    const PackHeader *h=(const PackHeader *)(const void *)pack_base;
    const char *path=(const char *)pack_base+h->strings+e->path;size_t n=strlen(canon+1);
    if(e->ino!=ino || e->size!=(uint64_t)st->st_size || e->mtime!=st->st_mtimespec.tv_sec || e->mtime_ns!=st->st_mtimespec.tv_nsec ||
       e->path_length!=n || memcmp(path,canon+1,n))return NULL;
    return e;
}
static const char *pack_path(const PackEntry *e) { return (const char *)pack_base+((const PackHeader *)(const void *)pack_base)->strings+e->path; }
static const PackEntry *pack_entry(const PackOpen *o) { return &pack_entries[o->entry]; }
static inline const struct stat *pack_stat(const PackOpen *o) { return (const struct stat *)(const void *)o->st; }
/* A description from the pool (NULL when it is used up: the file is opened as before). */
static PackOpen *pack_open_new(const PackEntry *e,const struct stat *st) {
    for(unsigned n=0;n<HVF_EL1_PACK_OPENS;n++) {
        unsigned i=__atomic_fetch_add(&proc_block->pack_hint,1,__ATOMIC_RELAXED)%HVF_EL1_PACK_OPENS;uint32_t free_slot=0;
        PackOpen *o=&proc_block->pack[i];
        if(!__atomic_compare_exchange_n(&o->used,&free_slot,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
        o->entry=(uint64_t)(e-pack_entries);o->offset=0;o->real=-1;memcpy(o->st,st,sizeof(*st));
        __atomic_store_n(&o->refs,1,__ATOMIC_RELEASE);return o;
    }
    return NULL;
}
static void pack_ref(PackOpen *o) { __atomic_add_fetch(&o->refs,1,__ATOMIC_RELAXED); }
static void pack_unref(PackOpen *o) {
    if(__atomic_sub_fetch(&o->refs,1,__ATOMIC_ACQ_REL))return;
    if(o->real>=0)close(o->real);
    __atomic_store_n(&o->used,0,__ATOMIC_RELEASE);
}
/* Slot slot no longer refers to its PackOpen (closed, or holding the real file now).
   1 when it did: its descriptor is the shared placeholder, which stays open. */
static int fs_pack_drop(unsigned slot) {
    if(!FS(slot).pack)return 0;
    pack_unref(FS(slot).pack);FS(slot).pack=NULL;return 1;
}
/* The real file in place of slot's placeholder: one host open for the description
   (at its offset), then each of its slots takes a duplicate as it is next used. */
static void fs_pack_real(unsigned slot) {
    PackOpen *o=FS(slot).pack;
    pthread_mutex_lock(&pack_lock);
    if(o->real<0) {
        int fd=openat(fs_lower,pack_path(pack_entry(o)),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(fd>=0 && lseek(fd,(off_t)__atomic_load_n(&o->offset,__ATOMIC_ACQUIRE),SEEK_SET)<0) { close(fd);fd=-1; }
        __atomic_store_n(&o->real,fd,__ATOMIC_RELEASE);
    }
    int real=o->real;
    pthread_mutex_unlock(&pack_lock);
    if(real<0)return;                                      /* the placeholder stays: calls on it fail */
    int fd=fcntl(real,F_DUPFD_CLOEXEC,0);if(fd<0)return;
    fs_pack_drop(slot);FS(slot).fd=fd+1;
}

/* ---- Building <lower>.pack ---- */
typedef struct { char *path;uint64_t ino,size;int64_t mtime,mtime_ns; } PackItem;
typedef struct { PackItem *items;size_t count,capacity;uint64_t total; } PackList;
static int pack_item_order(const void *a,const void *b) { uint64_t x=((const PackItem *)a)->ino,y=((const PackItem *)b)->ino;return x<y?-1:x>y; }
static void pack_scan(int dir,const char *prefix,PackList *list) {
    DIR *d=fdopendir(dir);if(!d) { close(dir);return; }
    struct dirent *e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
        char path[PATH_MAX];if(snprintf(path,sizeof path,"%s%s%s",prefix,*prefix?"/":"",e->d_name)>=(int)sizeof path)continue;
        struct stat st;if(fstatat(dirfd(d),e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        if(S_ISDIR(st.st_mode)) { int sub=openat(dirfd(d),e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(sub>=0)pack_scan(sub,path,list);continue; }
        if(!S_ISREG(st.st_mode) || (uint64_t)st.st_size>PACK_FILE_MAX || list->total+(uint64_t)st.st_size>PACK_TOTAL_MAX)continue;
        if(list->count==list->capacity) {
            size_t capacity=list->capacity?list->capacity*2:4096;PackItem *grown=malloc(capacity*sizeof(*grown));   /* malloc and free are resources.h's: no realloc, no strdup */
            if(!grown)continue;
            if(list->count)memcpy(grown,list->items,list->count*sizeof(*grown));
            free(list->items);list->items=grown;list->capacity=capacity;
        }
        size_t length=strlen(path);char *copy=malloc(length+1);if(!copy)continue;
        memcpy(copy,path,length+1);
        list->items[list->count++]=(PackItem){copy,(uint64_t)st.st_ino,(uint64_t)st.st_size,st.st_mtimespec.tv_sec,st.st_mtimespec.tv_nsec};
        list->total+=(uint64_t)st.st_size;
    }
    closedir(d);
}
static int pack_write(int out,const void *data,size_t size,uint64_t at) {
    for(size_t done=0;done<size;) { ssize_t n=pwrite(out,(const char *)data+done,size-done,(off_t)(at+done));if(n<=0)return -1;done+=(size_t)n; }
    return 0;
}
static char pack_lower[PATH_MAX],pack_file[PATH_MAX];
static void *pack_build(void *unused) {
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);                    /* behind the run's own work */
    char final[PATH_MAX],temporary[PATH_MAX];
    snprintf(final,sizeof final,"%s",pack_file);snprintf(temporary,sizeof temporary,"%s.%d",pack_file,(int)getpid());
    int root=open(pack_lower,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(root<0)return NULL;
    PackList list={0};int scan=dup(root);if(scan>=0)pack_scan(scan,"",&list);   /* the scan closes its descriptors */
    qsort(list.items,list.count,sizeof(*list.items),pack_item_order);
    size_t kept=0;                                                          /* one entry per inode (hard links) */
    for(size_t i=0;i<list.count;i++) { if(kept && list.items[kept-1].ino==list.items[i].ino) { free(list.items[i].path);continue; } list.items[kept++]=list.items[i]; }
    list.count=kept;
    int out=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
    PackEntry *entries=calloc(list.count?list.count:1,sizeof(PackEntry));
    uint64_t strings=sizeof(PackHeader)+list.count*sizeof(PackEntry),strings_size=0;
    for(size_t i=0;i<list.count;i++)strings_size+=strlen(list.items[i].path)+1;
    uint64_t data=(strings+strings_size+4095)&~UINT64_C(4095),at=data,name=0;
    int ok=out>=0 && entries;
    unsigned char *buffer=ok?malloc(PACK_FILE_MAX):NULL;ok=ok && buffer;
    size_t used=0;
    for(size_t i=0;ok && i<list.count;i++) {
        PackItem *item=&list.items[i];size_t length=strlen(item->path);
        int fd=openat(root,item->path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)continue;
        struct stat st;ssize_t n=0;
        if(!fstat(fd,&st) && (uint64_t)st.st_ino==item->ino && (uint64_t)st.st_size==item->size)
            for(size_t got=0;got<item->size;got+=(size_t)n)if((n=read(fd,buffer+got,item->size-got))<=0)break;
        close(fd);
        if(n<0 || (uint64_t)st.st_ino!=item->ino || (uint64_t)st.st_size!=item->size)continue;
        if(item->size>=4 && !memcmp(buffer,"\177ELF",4))continue;           /* programs and libraries: mapped, not read */
        if(pack_write(out,buffer,(size_t)item->size,at) || pack_write(out,item->path,length+1,strings+name)) { ok=0;break; }
        entries[used++]=(PackEntry){item->ino,item->size,at,item->mtime,item->mtime_ns,(uint32_t)name,(uint32_t)length};
        at+=(item->size+15)&~UINT64_C(15);name+=length+1;
    }
    PackHeader h={{0},used,sizeof(PackHeader),strings,name,data,at>data?at:data};memcpy(h.magic,PACK_MAGIC,8);
    ok=ok && !ftruncate(out,(off_t)h.total);                                 /* through the last file's padding */
    ok=ok && !pack_write(out,entries,used*sizeof(PackEntry),sizeof(PackHeader)) && !pack_write(out,&h,sizeof h,0) && !fsync(out);
    if(out>=0)close(out);
    if(ok)ok=!renameatx_np(AT_FDCWD,temporary,AT_FDCWD,final,RENAME_EXCL);   /* a run that finished first keeps its pack */
    if(!ok && out>=0)unlink(temporary);
    for(size_t i=0;i<list.count;i++)free(list.items[i].path);
    free(list.items);free(entries);free(buffer);close(root);
    return NULL;
}
/* The run's start: map the image's pack, or build one for later runs. */
static void pack_start(const char *lower,const char *file) {
    pack_load(lower,file);
    if(pack_base || snprintf(pack_lower,sizeof pack_lower,"%s",lower)>=(int)sizeof pack_lower || snprintf(pack_file,sizeof pack_file,"%s",file)>=(int)sizeof pack_file-16)return;
    if(!access(file,F_OK))return;                                            /* one that did not load: no rebuild loop */
    pthread_t builder;if(!pthread_create(&builder,NULL,pack_build,NULL))pthread_detach(builder);
}
