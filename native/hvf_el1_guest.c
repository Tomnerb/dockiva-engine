/* EL1 handlers: system calls answered inside the guest, without a VM exit (about 1 us
   each, against 0.1 to 0.2 on Linux). Compiled freestanding for aarch64 and turned
   into hvf_el1_code.h by scripts/build_el1.py; never linked into the runtime.

   The vector (hvf_backend.h) calls el1_entry with the vCPU's frame (x0..x30 of the
   guest, then the HF_ words) and the context page, on a stack of its own, with the
   guest's x2..x18 and x30 saved; it returns 1 when it answered (the result in frame[0])
   or 0, and the call then exits to the runtime as before. It never faults: user
   memory is touched only after an address translation (AT) says EL0 could. It reads
   what the runtime shares (the lookup cache) as the runtime's own lock-free readers
   do, and changes a process's descriptor tables only under its mm lock, taken by one
   compare-and-swap (busy: the runtime answers). No globals, no string constants, no
   library calls: the code must run where it is copied. */
#include <stdint.h>
#include "hvf_el1.h"

#define HF_FAST 40
#define HF_FDS 47
#define HF_TASK 53                                     /* the process's tables in the block, its mm lock (hvf_backend.h) */
#define HF_MM 54
#define HF_HELD 55                                     /* the lock word held, for the fault vector */
#define USER_MASK UINT64_C(0x00ffffffffffffff)     /* TBI0: the top byte is a tag */

static inline int user_ok(uint64_t va,int write) {
    uint64_t par;
    if(write)__asm__ volatile("at s1e0w, %1\n\tisb\n\tmrs %0, par_el1":"=r"(par):"r"(va):"memory");
    else __asm__ volatile("at s1e0r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(par):"r"(va):"memory");
    return !(par&1);
}
/* Every page of [va, va+size) writable at EL0. */
static int user_writable(uint64_t va,uint64_t size) {
    for(uint64_t p=va&~UINT64_C(4095);p<va+size;p+=4096)if(!user_ok(p,1))return 0;
    return 1;
}
static inline uint64_t load64(const volatile unsigned char *p) { return *(const volatile uint64_t *)(const volatile void *)p; }
static inline uint32_t load32(const volatile unsigned char *p) { return *(const volatile uint32_t *)(const volatile void *)p; }
static inline uint32_t load16(const volatile unsigned char *p) { return *(const volatile uint16_t *)(const volatile void *)p; }
static inline void put(volatile unsigned char *out,unsigned at,uint64_t value,unsigned size) {
    for(unsigned i=0;i<size;i++)out[at+i]=(unsigned char)(value>>(8*i));
}
static uint32_t hash(const char *p) { uint32_t h=2166136261u;for(;*p;p++)h=(h^(unsigned char)*p)*16777619u;return h; }
static int prefix(const char *full,const char *mount,uint32_t n) {      /* full is mount or below it */
    for(uint32_t i=0;i<n;i++)if(full[i]!=mount[i])return 0;
    return !full[n] || full[n]=='/';
}

/* The canonical form of the absolute guest path at address, as fs_stat_cached builds
   it, in full: its length, or 0 where the runtime must decide ("..", reserved names,
   "/", /proc, /dev, /sys, mounts, a final "." or "/", a path the cache cannot hold). */
static unsigned el1_path(const HvfEl1Context *c,uint64_t address,char full[1025]) {
    char path[1025];unsigned n=0;
    for(;;n++) {
        if(n==1025)return 0;
        if((!n || !((address+n)&4095)) && !user_ok(address+n,0))return 0;
        if(!(path[n]=*(const volatile char *)(address+n)))break;
    }
    if(!n || path[0]!='/' || path[n-1]=='/')return 0;
    unsigned f=0;const char *last=path;
    for(const char *p=path;*p;) {
        while(*p=='/')p++;
        const char *q=p;while(*q && *q!='/')q++;
        unsigned m=(unsigned)(q-p);
        if(m)last=p;
        if(m>255 || (m==2 && p[0]=='.' && p[1]=='.') || (m>=4 && p[0]=='.' && p[1]=='w' && p[2]=='h' && p[3]=='.'))return 0;
        if(m && !(m==1 && p[0]=='.')) {
            if(f+m+1>1024)return 0;
            full[f++]='/';for(unsigned i=0;i<m;i++)full[f++]=p[i];
        }
        p=q;
    }
    if(last[0]=='.' && (!last[1] || last[1]=='/'))return 0;              /* the leaf "." */
    full[f]=0;
    if(!f || f>=c->path)return 0;                                       /* "/"; the cache keeps shorter paths only */
    if(full[1]=='p' && full[2]=='r' && full[3]=='o' && full[4]=='c')return 0;
    if(full[1]=='d' && full[2]=='e' && full[3]=='v')return 0;
    if(full[1]=='s' && full[2]=='y' && full[3]=='s')return 0;
    uint64_t version=__atomic_load_n(&c->version,__ATOMIC_ACQUIRE);
    if(version&1)return 0;
    uint32_t mounts=c->mounts;
    if(mounts>HVF_EL1_MOUNTS)return 0;
    for(uint32_t i=0;i<mounts;i++)if(prefix(full,c->mount[i],c->mount_length[i]))return 0;
    if(__atomic_load_n(&c->version,__ATOMIC_ACQUIRE)!=version)return 0;
    return f;
}
/* ov_stamp, then ov_cache_get's two ways for full: 1 with *layer, *lower and (layer 1)
   the host's struct stat copied into st; 0 when the cache has no current answer. */
static int el1_lookup(const HvfEl1Context *c,const char *full,unsigned f,int follow,unsigned *layer,unsigned *lower,uint64_t st[18]) {
    const volatile uint64_t *gen=(const volatile uint64_t *)c->gen;
    uint32_t h=hash(full);
    uint64_t now_gen=__atomic_load_n(&gen[0],__ATOMIC_ACQUIRE),now_made=__atomic_load_n(&gen[1],__ATOMIC_ACQUIRE);
    uint64_t now_bucket=__atomic_load_n(&gen[2+((h>>16)&(c->buckets-1))],__ATOMIC_ACQUIRE);
    for(unsigned way=0;way<2;way++) {
        const volatile unsigned char *e=(const volatile unsigned char *)(c->cache+(uint64_t)(((h*2u+(unsigned)follow)^(way*2u))&(c->slots-1))*c->entry);
        uint64_t s=__atomic_load_n((const uint64_t *)(const void *)(e+c->e_seq),__ATOMIC_ACQUIRE);
        if(!s || (s&1))continue;
        uint64_t at_gen=load64(e+c->e_gen),at_made=load64(e+c->e_made),at_bucket=load64(e+c->e_bucket);
        unsigned l=e[c->e_layer],lw=e[c->e_lower],efollow=e[c->e_follow];
        int same=1;for(unsigned i=0;i<=f;i++)if(e[c->e_path+i]!=(unsigned char)full[i]) { same=0;break; }
        for(unsigned i=0;i<18;i++)st[i]=load64(e+c->e_st+8*i);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(__atomic_load_n((const uint64_t *)(const void *)(e+c->e_seq),__ATOMIC_RELAXED)!=s || efollow!=(unsigned)follow || !same)continue;
        if(at_gen!=now_gen || at_bucket!=now_bucket || (l==3 && at_made!=now_made))return 0;   /* stale: the runtime walks */
        *layer=l;*lower=lw;return 1;
    }
    return 0;
}
static inline uint64_t st_field(const uint64_t st[18],uint32_t offset,unsigned size) {
    const unsigned char *p=(const unsigned char *)st+offset;uint64_t v=0;
    for(unsigned i=0;i<size;i++)v|=(uint64_t)p[i]<<(8*i);
    return v;
}

/* newfstatat (79) and statx (291) of an absolute path the lookup cache knows, as
   fs_stat_cached and fs_stat_pack (or statx_from_stat) answer it. */
static long el1_stat(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t call=frame[8],flags=frame[call==79?3:2],buffer=(frame[call==79?2:4])&USER_MASK;
    if(flags&~(call==79?UINT64_C(0x1900):UINT64_C(0x6900)))return 0;           /* 0x100|0x800|0x1000, 0x100|0x800|0x6000 */
    uint64_t uid=frame[HF_FAST+3],gid=frame[HF_FAST+5];
    if(uid>UINT32_MAX || gid>UINT32_MAX)return 0;
    int follow=!(flags&0x100);
    char full[1025];unsigned f=el1_path(c,frame[1]&USER_MASK,full);if(!f)return 0;
    unsigned layer,lower;uint64_t st[18];
    if(!el1_lookup(c,full,f,follow,&layer,&lower,st))return 0;
    if(layer!=1) { frame[0]=(uint64_t)(int64_t)(layer==3 && lower?-20:-2);return 1; }
    uint32_t mode=(uint32_t)st_field(st,c->s_mode,2),nlink=(uint32_t)st_field(st,c->s_nlink,2),type=mode&0170000;
    uint64_t ino=st_field(st,c->s_ino,8),size=st_field(st,c->s_size,8);
    uint64_t times[6]={st_field(st,c->s_atime,8),st_field(st,c->s_atime+8,8),st_field(st,c->s_mtime,8),st_field(st,c->s_mtime+8,8),st_field(st,c->s_ctime,8),st_field(st,c->s_ctime+8,8)};
    if(type!=0100000 && type!=0040000 && type!=0120000 && type!=0140000 && type!=0010000)return 0;
    unsigned bytes=call==79?128:256;
    if(!user_writable(buffer,bytes))return 0;
    volatile unsigned char *out=(volatile unsigned char *)buffer;
    for(unsigned i=0;i<bytes;i++)out[i]=0;
    if(!nlink)nlink=1;
    if(call==79) {                                                      /* fs_stat_pack */
        put(out,8,ino,8);put(out,16,mode&0177777,4);put(out,20,nlink,4);put(out,24,uid,4);put(out,28,gid,4);
        put(out,48,size,8);put(out,56,4096,4);put(out,64,(size+511)/512,8);
        for(unsigned i=0;i<3;i++) { put(out,72+i*16,times[2*i],8);put(out,80+i*16,times[2*i+1],8); }
    } else {                                                            /* statx_from_stat */
        put(out,0,0x7ff,4);put(out,4,4096,4);put(out,16,nlink,4);put(out,20,uid,4);put(out,24,gid,4);
        put(out,28,mode&0177777,2);put(out,32,ino,8);put(out,40,size,8);put(out,48,(size+511)/512,8);
        put(out,64,times[0],8);put(out,72,times[1],4);                /* atime */
        put(out,96,times[4],8);put(out,104,times[5],4);               /* ctime */
        put(out,112,times[2],8);put(out,120,times[3],4);              /* mtime */
    }
    frame[0]=0;return 1;
}

/* ---- Descriptors of image files in the pack (pack.h), under the process's mm lock ---- */
static inline volatile unsigned char *at(const HvfEl1Context *c,uint64_t offset) { return (volatile unsigned char *)(c->block+offset); }
static inline void put64(volatile unsigned char *p,uint64_t v) { *(volatile uint64_t *)(volatile void *)p=v; }
static inline void put32(volatile unsigned char *p,uint32_t v) { *(volatile uint32_t *)(volatile void *)p=v; }
static inline void put16(volatile unsigned char *p,uint32_t v) { *(volatile uint16_t *)(volatile void *)p=(uint16_t)v; }
static int el1_lock(uint64_t *frame,const HvfEl1Context *c,uint64_t mm) {
    uint32_t *word=(uint32_t *)(void *)at(c,c->b_mm+4*mm);
    for(unsigned spin=0;spin<256;spin++) {                           /* holds last microseconds */
        uint32_t free_word=0;
        if(__atomic_compare_exchange_n(word,&free_word,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) { frame[HF_HELD]=(uint64_t)word;return 1; }
        __asm__ volatile("yield");
    }
    return 0;
}
static void el1_unlock(uint64_t *frame,const HvfEl1Context *c,uint64_t mm) {
    frame[HF_HELD]=0;(void)__atomic_exchange_n((uint32_t *)(void *)at(c,c->b_mm+4*mm),0,__ATOMIC_RELEASE);
}

/* openat (56) of an image file the pack holds, read-only: what fs_open does there, a
   placeholder slot and a new open description, with no host call. */
static long el1_open(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t flags=frame[2],task=frame[HF_TASK],mm=frame[HF_MM],mirror=frame[HF_FDS];
    /* O_NOCTTY, O_NONBLOCK, O_NOFOLLOW, O_LARGEFILE, O_NOATIME, O_CLOEXEC: read-only, nothing made */
    if(!c->pack_count || task>UINT32_MAX || !mirror || (flags&~(UINT64_C(256)|2048|32768|131072|262144|524288)))return 0;
    char full[1025];unsigned f=el1_path(c,frame[1]&USER_MASK,full);if(!f)return 0;
    unsigned layer,lower;uint64_t st[18];
    if(!el1_lookup(c,full,f,!(flags&32768),&layer,&lower,st) || layer!=1)return 0;
    uint64_t mode=st_field(st,c->s_mode,2),size=st_field(st,c->s_size,8),ino=st_field(st,c->s_ino,8);
    if((mode&0170000)!=0100000 || size>c->file_limit)return 0;
    /* pack_find: the entry for this inode, if it still has the file's size, time and path */
    const HvfPackEntry *entries=(const HvfPackEntry *)c->pack_entries;
    uint64_t low=0,high=c->pack_count;
    while(low<high) { uint64_t mid=(low+high)/2;if(entries[mid].ino<ino)low=mid+1;else high=mid; }
    if(low==c->pack_count)return 0;
    const HvfPackEntry *e=&entries[low];
    if(e->ino!=ino || e->size!=size || (uint64_t)e->mtime!=st_field(st,c->s_mtime,8) || (uint64_t)e->mtime_ns!=st_field(st,c->s_mtime+8,8) || e->path_length!=f-1)return 0;
    const char *name=(const char *)(c->pack_strings+e->path);
    for(unsigned i=0;i<f-1;i++)if(name[i]!=full[i+1])return 0;
    if(!el1_lock(frame,c,mm))return 0;
    uint64_t fs=c->b_fs+task*c->fs_size,fds=c->b_fds+task*c->fds_size;
    unsigned slot=0,fd=0;
    while(slot<c->fs_slots && load32(at(c,fs+c->t_fd+4*slot)))slot++;
    while(fd<c->fd_limit && at(c,fds+c->d_kind+fd)[0])fd++;
    long done=0;
    if(slot<c->fs_slots && fd<c->fd_limit) {
        for(unsigned tries=0;tries<64;tries++) {                       /* pack_open_new */
            uint32_t i=__atomic_fetch_add((uint32_t *)(void *)at(c,c->b_hint),1,__ATOMIC_RELAXED)%HVF_EL1_PACK_OPENS,free_slot=0;
            HvfPackOpen *o=(HvfPackOpen *)(void *)at(c,c->b_pack+(uint64_t)i*sizeof(HvfPackOpen));
            if(!__atomic_compare_exchange_n(&o->used,&free_slot,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
            o->entry=(uint64_t)(e-entries);o->offset=0;o->real=-1;
            for(unsigned k=0;k<18;k++)o->st[k]=st[k];
            __atomic_store_n(&o->refs,1,__ATOMIC_RELEASE);
            int cloexec=(flags&524288)!=0;                              /* fs_install */
            put32(at(c,fs+c->t_fd+4*slot),c->marker);at(c,fs+c->t_cloexec+slot)[0]=(unsigned char)cloexec;
            at(c,fs+c->t_ro+slot)[0]=2;at(c,fs+c->t_dev+slot)[0]=0;at(c,fs+c->t_sole+slot)[0]=0;
            put64(at(c,fs+c->t_dirs+8*slot),0);put16(at(c,fs+c->t_guest+2*slot),fd);
            put64(at(c,fs+c->t_pack+8*slot),c->host_block+c->b_pack+(uint64_t)i*sizeof(HvfPackOpen));
            put16(at(c,fds+c->d_index+2*fd),slot);                       /* fd_bind */
            __atomic_store_n((unsigned char *)(void *)at(c,fds+c->d_kind+fd),(unsigned char)c->fd_file,__ATOMIC_RELEASE);
            __atomic_store_n((unsigned char *)(mirror+fd),(unsigned char)(c->mirror_file|(cloexec?c->mirror_cloexec:0)),__ATOMIC_RELEASE);
            frame[0]=fd;done=1;break;
        }
    }
    el1_unlock(frame,c,mm);
    return done;
}
/* close (57) of a descriptor el1_open (or the runtime) gave an image file in the pack,
   while no slot of its description holds the real file and the process holds no
   record lock: the slot and the number go, and the description with its last one. */
static long el1_close(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t fd=frame[0],task=frame[HF_TASK],mm=frame[HF_MM],mirror=frame[HF_FDS],pid=frame[HF_FAST];
    if(!c->pack_count || task>UINT32_MAX || !mirror || fd>=c->fd_limit || pid>UINT32_MAX)return 0;
    if(load16(at(c,c->b_locks+2*pid)))return 0;
    if(!el1_lock(frame,c,mm))return 0;
    uint64_t fs=c->b_fs+task*c->fs_size,fds=c->b_fds+task*c->fds_size;
    long done=0;
    if(at(c,fds+c->d_kind+fd)[0]==c->fd_file) {
        unsigned slot=(unsigned)load16(at(c,fds+c->d_index+2*fd));
        uint64_t pack=slot<c->fs_slots?load64(at(c,fs+c->t_pack+8*slot)):0;
        if(pack && load32(at(c,fs+c->t_fd+4*slot))==c->marker && pack>=c->host_block+c->b_pack &&
           pack<c->host_block+c->b_pack+(uint64_t)HVF_EL1_PACK_OPENS*sizeof(HvfPackOpen)) {
            HvfPackOpen *o=(HvfPackOpen *)(void *)at(c,pack-c->host_block);
            if(__atomic_load_n(&o->real,__ATOMIC_ACQUIRE)<0) {
                put32(at(c,fs+c->t_fd+4*slot),0);at(c,fs+c->t_cloexec+slot)[0]=0;at(c,fs+c->t_sole+slot)[0]=0;
                put64(at(c,fs+c->t_pack+8*slot),0);
                put16(at(c,fds+c->d_index+2*fd),0);                      /* fd_unbind */
                __atomic_store_n((unsigned char *)(void *)at(c,fds+c->d_kind+fd),0,__ATOMIC_RELEASE);
                __atomic_store_n((unsigned char *)(mirror+fd),0,__ATOMIC_RELEASE);
                if(!__atomic_sub_fetch(&o->refs,1,__ATOMIC_ACQ_REL))__atomic_store_n(&o->used,0,__ATOMIC_RELEASE);   /* pack_unref */
                frame[0]=0;done=1;
            }
        }
    }
    el1_unlock(frame,c,mm);
    return done;
}

/* ---- Small anonymous regions (hvf_el1.h's HvfMmCache), under the process's mm lock ---- */
static HvfMmCache *el1_regions(const HvfEl1Context *c,uint64_t mm) { return (HvfMmCache *)(void *)at(c,c->b_mmc+mm*sizeof(HvfMmCache)); }
/* Zero a page of a registered region as it is handed out again: one EL0 may write gets
   DC ZVA a block at a time (stores where that is barred). A read-only one must already
   read as zeros: the runtime's own writes into guest memory (read(), say) leave a page
   mapped read-only until the guest writes it, so it is checked; 0 if it is not (a fault
   reading it releases the lock and the runtime answers). */
static int el1_zero_page(uint64_t page) {
    if(user_ok(page,1)) {
        uint64_t id;__asm__ volatile("mrs %0, dczid_el0":"=r"(id));
        if(!(id&16)) {
            uint64_t block=UINT64_C(4)<<(id&15);
            for(uint64_t p=page;p<page+4096;p+=block)__asm__ volatile("dc zva, %0"::"r"(p):"memory");
        } else for(uint64_t p=page;p<page+4096;p+=8)*(volatile uint64_t *)p=0;
        return 1;
    }
    uint64_t any=0;
    for(uint64_t p=page;p<page+4096;p+=64) {
        const volatile uint64_t *q=(const volatile uint64_t *)p;
        any|=q[0]|q[1]|q[2]|q[3]|q[4]|q[5]|q[6]|q[7];
    }
    return !any;
}
/* mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) of as many pages
   as a parked region: that region, zeroed. */
static long el1_mmap(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t length=frame[1],task=frame[HF_TASK],mm=frame[HF_MM];
    /* Go's heap hints on arm64 (i<<40 | 0x4000000000 for i 0x7f down to 0, 64 MiB of
       PROT_NONE): far above the arena (at most 24 GiB), so the runtime maps each
       elsewhere and Go unmaps it and tries the next, 256 calls at every start. ENOMEM
       says the same at once: Go goes on to the next hint, then takes any address. */
    if(frame[2]==0 && frame[3]==0x22 && length==(UINT64_C(64)<<20) && (frame[0]&((UINT64_C(1)<<40)-1))==UINT64_C(0x4000000000) &&
       (frame[0]>>40)<0x80 && frame[4]==UINT64_MAX && frame[5]==0) { frame[0]=(uint64_t)-12;return 1; }
    if(frame[0] || frame[2]!=3 || frame[3]!=0x22 || !length || length>HVF_EL1_REGION_PAGES*4096 || task>UINT32_MAX || !c->block)return 0;
    uint32_t pages=(uint32_t)((length+4095)/4096);
    if(!el1_lock(frame,c,mm))return 0;
    HvfMmCache *m=el1_regions(c,mm);long done=0;
    for(uint32_t i=0;i<m->parked;i++) {
        if(m->park_pages[i]!=pages)continue;
        uint64_t base=m->park_base[i];int zero=1;
        for(uint32_t k=0;k<pages && zero;k++)zero=el1_zero_page(base+(uint64_t)k*4096);
        if(!zero)break;                                                   /* the runtime maps afresh; this one stays parked */
        m->parked--;m->parked_pages-=pages;m->park_base[i]=m->park_base[m->parked];m->park_pages[i]=m->park_pages[m->parked];
        frame[0]=base;done=1;break;
    }
    el1_unlock(frame,c,mm);
    return done;
}
/* munmap of exactly a registered region: parked, still mapped (a second munmap of it
   succeeds, as Linux's of an unmapped range does). */
static long el1_munmap(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t base=frame[0],length=frame[1],task=frame[HF_TASK],mm=frame[HF_MM];
    if((base&4095) || !length || length>HVF_EL1_REGION_PAGES*4096 || task>UINT32_MAX || !c->block)return 0;
    uint32_t pages=(uint32_t)((length+4095)/4096);
    if(!el1_lock(frame,c,mm))return 0;
    HvfMmCache *m=el1_regions(c,mm);long done=0;
    unsigned bucket=(unsigned)((base>>12)%(HVF_EL1_REGISTERED/4))*4;
    for(unsigned k=0;k<4 && !done;k++)if(m->pages[bucket+k]==pages && m->base[bucket+k]==base) {
        for(uint32_t i=0;i<m->parked;i++)if(m->park_base[i]==base) { frame[0]=0;done=1; }
        if(!done && m->parked<HVF_EL1_PARKED && m->parked_pages+pages<=HVF_EL1_PARK_PAGES) {
            m->park_base[m->parked]=base;m->park_pages[m->parked]=pages;m->parked++;m->parked_pages+=pages;
            frame[0]=0;done=1;
        }
        break;
    }
    el1_unlock(frame,c,mm);
    return done;
}

/* futex (98), the calls that wake no one and wait for nothing: FUTEX_WAIT(_BITSET)
   whose word no longer holds the value (EAGAIN), and FUTEX_WAKE(_BITSET) with no
   waiter counted on the address (0; hvf_el1.h). A waker stored the word before its
   call, so a barrier orders that store before the count's read; every waiter raises
   its count before it reads the word. Anything else, the runtime answers. */
static long el1_futex(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t address=frame[0],pid=frame[HF_FAST];unsigned op=(unsigned)frame[1],command=op&127;
    if((op&~255u) || (address&3) || (address>>48) || !c->block || pid==UINT64_MAX)return 0;   /* no FUTEX_CLOCK_REALTIME */
    if((command==9 || command==10) && !(uint32_t)frame[5])return 0;          /* EINVAL */
    if(command==0 || command==9) {
        if(!user_ok(address,0))return 0;
        if(frame[3]) {                                                          /* a timeout the runtime would refuse: its EINVAL */
            uint64_t ts=frame[3];
            if((ts&7) || (ts>>48) || !user_ok(ts,0) || !user_ok(ts+15,0))return 0;
            uint64_t sec=load64((const volatile unsigned char *)ts),ns=load64((const volatile unsigned char *)(ts+8));
            if((int64_t)sec<0 || ns>=1000000000 || sec>=(UINT64_C(1)<<32))return 0;
        }
        if(__atomic_load_n((const uint32_t *)address,__ATOMIC_SEQ_CST)==(uint32_t)frame[2])return 0;   /* it waits: the runtime */
        frame[0]=(uint64_t)-11;return 1;
    }
    if(command==1 || command==10) {
        if((int32_t)frame[2]<0 || !user_ok(address,0))return 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        const uint32_t *counts=(const uint32_t *)(const void *)at(c,c->b_futex);
        if(__atomic_load_n(&counts[HVF_EL1_FUTEX_SLOTS],__ATOMIC_SEQ_CST) || __atomic_load_n(&counts[hvf_el1_futex_slot(pid,address)],__ATOMIC_SEQ_CST))return 0;
        frame[0]=0;return 1;
    }
    return 0;
}

long el1_entry(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t call=frame[8];
    if(call==79 || call==291)return el1_stat(frame,c);
    if(call==56)return el1_open(frame,c);
    if(call==57)return el1_close(frame,c);
    if(call==222)return el1_mmap(frame,c);
    if(call==215)return el1_munmap(frame,c);
    if(call==98)return el1_futex(frame,c);
    return 0;
}
