/* EL1 handlers: system calls answered inside the guest, without a VM exit (about 1 us
   each, against 0.1 to 0.2 on Linux). Compiled freestanding for aarch64 and turned
   into hvf_el1_code.h by scripts/build_el1.py; never linked into the runtime.

   The vector (hvf_backend.h) calls el1_entry with the vCPU's frame (x0..x30 of the
   guest, then the HF_ words) and the context page, on a stack of its own, with the
   guest's x2..x18 and x30 saved; it returns 1 when it answered (the result in frame[0])
   or 0, and the call then exits to the runtime as before. It never faults: user
   memory is touched only after an address translation (AT) says EL0 could. It reads
   what the runtime shares (the lookup cache) as the runtime's own lock-free readers
   do. No globals, no string constants, no library calls: the code must run where it
   is copied. */
#include <stdint.h>
#include "hvf_el1.h"

#define HF_FAST 40
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
static inline uint32_t load16(const volatile unsigned char *p) { return *(const volatile uint16_t *)(const volatile void *)p; }
static inline void put(volatile unsigned char *out,unsigned at,uint64_t value,unsigned size) {
    for(unsigned i=0;i<size;i++)out[at+i]=(unsigned char)(value>>(8*i));
}
static uint32_t hash(const char *p) { uint32_t h=2166136261u;for(;*p;p++)h=(h^(unsigned char)*p)*16777619u;return h; }
static int prefix(const char *full,const char *mount,uint32_t n) {      /* full is mount or below it */
    for(uint32_t i=0;i<n;i++)if(full[i]!=mount[i])return 0;
    return !full[n] || full[n]=='/';
}

/* newfstatat (79) and statx (291) of an absolute path the lookup cache knows, as
   fs_stat_cached and fs_stat_pack (or statx_from_stat) answer it. */
static long el1_stat(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t call=frame[8],flags=frame[call==79?3:2],buffer=(frame[call==79?2:4])&USER_MASK,address=frame[1]&USER_MASK;
    if(flags&~(call==79?UINT64_C(0x100|0x800|0x1000):UINT64_C(0x100|0x800|0x6000)))return 0;
    uint64_t uid=frame[HF_FAST+3],gid=frame[HF_FAST+5];
    if(uid>UINT32_MAX || gid>UINT32_MAX)return 0;
    int follow=!(flags&0x100);
    char path[1025];unsigned n=0;
    for(;;n++) {
        if(n==1025)return 0;
        if((!n || !((address+n)&4095)) && !user_ok(address+n,0))return 0;
        if(!(path[n]=*(const volatile char *)(address+n)))break;
    }
    if(!n || path[0]!='/' || path[n-1]=='/')return 0;
    /* Canonical: "." and empty parts dropped; "..", reserved names and long parts are the runtime's. */
    char full[1025];unsigned f=0;
    const char *last=path;
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
    /* The runtime's own: /proc, /dev, /sys (by prefix, as it checks) and mounts. */
    if(full[1]=='p' && full[2]=='r' && full[3]=='o' && full[4]=='c')return 0;
    if(full[1]=='d' && full[2]=='e' && full[3]=='v')return 0;
    if(full[1]=='s' && full[2]=='y' && full[3]=='s')return 0;
    uint64_t version=__atomic_load_n(&c->version,__ATOMIC_ACQUIRE);
    if(version&1)return 0;
    uint32_t mounts=c->mounts;
    if(mounts>HVF_EL1_MOUNTS)return 0;
    for(uint32_t i=0;i<mounts;i++)if(prefix(full,c->mount[i],c->mount_length[i]))return 0;
    if(__atomic_load_n(&c->version,__ATOMIC_ACQUIRE)!=version)return 0;
    /* ov_stamp, then ov_cache_get's two ways. */
    const volatile uint64_t *gen=(const volatile uint64_t *)c->gen;
    uint32_t h=hash(full);
    uint64_t now_gen=__atomic_load_n(&gen[0],__ATOMIC_ACQUIRE),now_made=__atomic_load_n(&gen[1],__ATOMIC_ACQUIRE);
    uint64_t now_bucket=__atomic_load_n(&gen[2+((h>>16)&(c->buckets-1))],__ATOMIC_ACQUIRE);
    for(unsigned way=0;way<2;way++) {
        const volatile unsigned char *e=(const volatile unsigned char *)(c->cache+(uint64_t)(((h*2u+(unsigned)follow)^(way*2u))&(c->slots-1))*c->entry);
        uint64_t s=__atomic_load_n((const uint64_t *)(const void *)(e+c->e_seq),__ATOMIC_ACQUIRE);
        if(!s || (s&1))continue;
        uint64_t at_gen=load64(e+c->e_gen),at_made=load64(e+c->e_made),at_bucket=load64(e+c->e_bucket);
        unsigned layer=e[c->e_layer],lower=e[c->e_lower],efollow=e[c->e_follow];
        int same=1;for(unsigned i=0;i<=f;i++)if(e[c->e_path+i]!=(unsigned char)full[i]) { same=0;break; }
        const volatile unsigned char *st=e+c->e_st;
        uint32_t mode=load16(st+c->s_mode),nlink=load16(st+c->s_nlink);
        uint64_t ino=load64(st+c->s_ino),size=load64(st+c->s_size);
        uint64_t times[6]={load64(st+c->s_atime),load64(st+c->s_atime+8),load64(st+c->s_mtime),load64(st+c->s_mtime+8),load64(st+c->s_ctime),load64(st+c->s_ctime+8)};
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(__atomic_load_n((const uint64_t *)(const void *)(e+c->e_seq),__ATOMIC_RELAXED)!=s || efollow!=(unsigned)follow || !same)continue;
        if(at_gen!=now_gen || at_bucket!=now_bucket || (layer==3 && at_made!=now_made))return 0;   /* stale: the runtime walks */
        if(layer!=1) { frame[0]=(uint64_t)(int64_t)(layer==3 && lower?-20:-2);return 1; }
        uint32_t type=mode&0170000;
        if(type!=0100000 && type!=0040000 && type!=0120000 && type!=0140000 && type!=0010000)return 0;
        unsigned bytes=call==79?128:256;
        if(!user_writable(buffer,bytes))return 0;
        volatile unsigned char *out=(volatile unsigned char *)buffer;
        for(unsigned i=0;i<bytes;i++)out[i]=0;
        if(!nlink)nlink=1;
        if(call==79) {                                                  /* fs_stat_pack */
            put(out,8,ino,8);put(out,16,mode&0177777,4);put(out,20,nlink,4);put(out,24,uid,4);put(out,28,gid,4);
            put(out,48,size,8);put(out,56,4096,4);put(out,64,(size+511)/512,8);
            for(unsigned i=0;i<3;i++) { put(out,72+i*16,times[2*i],8);put(out,80+i*16,times[2*i+1],8); }
        } else {                                                        /* statx_from_stat */
            put(out,0,0x7ff,4);put(out,4,4096,4);put(out,16,nlink,4);put(out,20,uid,4);put(out,24,gid,4);
            put(out,28,mode&0177777,2);put(out,32,ino,8);put(out,40,size,8);put(out,48,(size+511)/512,8);
            put(out,64,times[0],8);put(out,72,times[1],4);                /* atime */
            put(out,96,times[4],8);put(out,104,times[5],4);               /* ctime */
            put(out,112,times[2],8);put(out,120,times[3],4);              /* mtime */
        }
        frame[0]=0;return 1;
    }
    return 0;
}

long el1_entry(uint64_t *frame,const HvfEl1Context *c) {
    uint64_t call=frame[8];
    if(call==79 || call==291)return el1_stat(frame,c);
    return 0;
}
