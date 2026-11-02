#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
/* Darwin hides required non-POSIX declarations in strict POSIX mode. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_UNLIMITED_SELECT)
#define _DARWIN_UNLIMITED_SELECT 1 /* poll_now (net.h): select() past FD_SETSIZE */
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* Bounded Linux user-mode runtime: interpreter and optional native (Hypervisor.framework) backend. */
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <sys/random.h>
#include <limits.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdatomic.h>
#include "resources.h"
static _Thread_local jmp_buf exec_recovery;   /* fail() while this worker loads an exec */
static _Thread_local int exec_loading;
static _Thread_local int exec_staging;         /* an exec builds its image apart (process.h): startup leaves /proc to the swap */

#define LIMIT (16u * 1024u * 1024u)
static unsigned elf_input_limit=LIMIT;
#include "runtime_context.h"
static RuntimeContext runtime_context={.memory={.map_capacity=MAP_CAP}};
static _Thread_local GuestAddressSpace *address_space=&runtime_context.memory; /* the worker's current process */
/* The guest thread this worker runs (each scheduler worker is a host thread with its own). */
static _Thread_local GuestCpuState *cpu=&runtime_context.cpu;
static int trace;
static struct { uint64_t address;uint32_t word; } trace_tail[16];
static unsigned trace_cursor;
static int lookup_cache_enabled=1;
#include "profile.h"
static void net_cleanup(void);
static void fs_cleanup(void);
static void io_cleanup(void);
static void schedule_cleanup(void);
static void image_free(void);
static void cleanup(void) { schedule_cleanup();profile_dump(); image_free(); net_cleanup(); fs_cleanup(); io_cleanup(); }
static void fail(const char *message) { if(exec_loading) longjmp(exec_recovery,1); if(trace) {
        fprintf(stderr,"guest pc=0x%" PRIx64 "\n",cpu->pc);
        for(unsigned i=0;i<16;i++) { unsigned at=(trace_cursor+i)%16;if(trace_tail[at].word)fprintf(stderr,"tail pc=0x%" PRIx64 " instruction=0x%08" PRIx32 "\n",trace_tail[at].address,trace_tail[at].word); }
        for(unsigned i=0;i<31;i++)fprintf(stderr,"x%u=0x%" PRIx64 "%s",i,cpu->gpr[i],i%4==3?"\n":" ");fprintf(stderr,"\n");
    } fprintf(stderr,"dockiva: %s\n",message); exit(125); }
static uint64_t le(const unsigned char *p, unsigned n) {
    uint64_t value=0; for (unsigned i=0;i<n;i++) value |= (uint64_t)p[i] << (8*i); return value;
}
#include "address_space_ops.h"
#include "heap.h"
static unsigned char *lookup(uint64_t addr, uint64_t count, unsigned permission) {
    /* A write that would need the lock (copy-on-write, a first charge) fails here
       with syscall_abort set when the call runs unlocked. */
    if(profiling) profile_lookups++;
    if (count>LIMIT) return NULL;
    /* Only immutable ELF/stack segments are cached. Heap mappings stay dynamic.
       Zero-byte accesses retain the original scan's segment-boundary ordering. */
    if(lookup_cache_enabled && count && permission<8) {
        Segment *cached=address_space->lookup_cache[permission];
        if(cached) {
            if(profiling) profile_probes++;
            if(addr>=cached->base && addr-cached->base<cached->size &&
               count<=cached->size-(addr-cached->base) && (cached->flags & permission)) {
                if(profiling) profile_cache_hits++;
                if((permission&2) && !address_space_write(address_space,cached->backing,cached->offset+(size_t)(addr-cached->base),(size_t)count))return NULL;
                return segment_data(cached)+(size_t)(addr-cached->base);
            }
        }
    }
    for (unsigned i=0;i<address_space->nseg;i++) {
        if(profiling) profile_probes++;
        Segment *s=&address_space->seg[i];
        if (addr>=s->base && addr-s->base<=s->size && count<=s->size-(addr-s->base)) {
            if(!(s->flags & permission)) return NULL;
            if(lookup_cache_enabled && count && permission<8) address_space->lookup_cache[permission]=s;
            if((permission&2) && !address_space_write(address_space,s->backing,s->offset+(size_t)(addr-s->base),(size_t)count))return NULL;
            return segment_data(s)+(size_t)(addr-s->base);
        }
    }
    return heap_lookup(addr,count,permission);
}
#define IO_BLOCK UINT64_C(0x8000000000000000)
static int schedule_enabled,schedule_active;
static void epoll_forget_socket(int host);
#include "net.h"
#include "fs.h"
#include "mapping.h"
#include "io.h"
#include "fadvise.h"
#include "epoll.h"
#include "compat.h"
#include "memfs.h"
#include "vdso.h"
/* Read-only segments of programs run recently, shared by later execs of the same
   file (Linux serves them from its page cache): an exec reuses their bytes instead of
   copying them, and the hypervisor mapping that goes with them. Keyed by the file's
   identity and the segment; a write still copies the page it touches. Bounded in
   entries and bytes; the oldest goes first. */
#define TEXT_CACHE 64
#define TEXT_CACHE_BYTES (UINT64_C(1)<<30)
static struct TextCache { dev_t dev;ino_t ino;off_t size;long mtime_s,mtime_ns;uint64_t off,bytes,mem,last;MemoryBacking *backing; } text_cache[TEXT_CACHE];
static uint64_t text_cache_clock,text_cache_bytes;
static int text_cache_same(const struct TextCache *c,const struct stat *st,uint64_t off,uint64_t bytes,uint64_t mem) {
    return c->backing && c->dev==st->st_dev && c->ino==st->st_ino && c->size==st->st_size && c->mtime_s==(long)st->st_mtimespec.tv_sec
        && c->mtime_ns==(long)st->st_mtimespec.tv_nsec && c->off==off && c->bytes==bytes && c->mem==mem;
}
static MemoryBacking *text_cache_find(const struct stat *st,uint64_t off,uint64_t bytes,uint64_t mem) {
    for(unsigned i=0;i<TEXT_CACHE;i++)if(text_cache_same(&text_cache[i],st,off,bytes,mem)) {
        if(!backing_retain(text_cache[i].backing,1))return NULL;
        text_cache[i].last=++text_cache_clock;return text_cache[i].backing;
    }
    return NULL;
}
static void text_cache_add(const struct stat *st,uint64_t off,uint64_t bytes,uint64_t mem,MemoryBacking *backing) {
    if(mem>TEXT_CACHE_BYTES/4 || !backing_retain(backing,1))return;
    for(;;) {                                                  /* room: a free slot and the byte budget */
        unsigned victim=0,free_slot=TEXT_CACHE;
        for(unsigned i=0;i<TEXT_CACHE;i++) {
            if(!text_cache[i].backing) { if(free_slot==TEXT_CACHE)free_slot=i;continue; }
            if(!text_cache[victim].backing || text_cache[i].last<text_cache[victim].last)victim=i;
        }
        if(free_slot<TEXT_CACHE && text_cache_bytes+mem<=TEXT_CACHE_BYTES) {
            text_cache[free_slot]=(struct TextCache){st->st_dev,st->st_ino,st->st_size,(long)st->st_mtimespec.tv_sec,(long)st->st_mtimespec.tv_nsec,
                off,bytes,mem,++text_cache_clock,backing};
            text_cache_bytes+=mem;return;
        }
        if(!text_cache[victim].backing) { backing_release(backing);return; }
        text_cache_bytes-=text_cache[victim].mem;backing_release(text_cache[victim].backing);memset(&text_cache[victim],0,sizeof(text_cache[victim]));
    }
}
/* Mappings of programs run recently (keyed as text_cache): a later exec of the same
   file reads its headers and data from pages already mapped, instead of mapping,
   faulting in and unmapping the file each time (a shell script runs the same few
   programs, and every one of them the same loader). Read-only, never charged; the
   loader never keeps a pointer into one past load_stream. Execs are serialized. */
#define ELF_MAP_CACHE 16
static struct ElfMap { dev_t dev;ino_t ino;off_t size;long mtime_s,mtime_ns;void *map;uint64_t last; } elf_map_cache[ELF_MAP_CACHE];
static uint64_t elf_map_clock;
static void *elf_map(int fd,const struct stat *st) {
    struct ElfMap *victim=&elf_map_cache[0];
    for(unsigned i=0;i<ELF_MAP_CACHE;i++) {
        struct ElfMap *e=&elf_map_cache[i];
        if(e->map && e->dev==st->st_dev && e->ino==st->st_ino && e->size==st->st_size && e->mtime_s==(long)st->st_mtimespec.tv_sec && e->mtime_ns==(long)st->st_mtimespec.tv_nsec) {
            e->last=++elf_map_clock;return e->map;
        }
        if(!e->map || (victim->map && e->last<victim->last))victim=e;
    }
    void *mapped=mmap(NULL,(size_t)st->st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if(mapped==MAP_FAILED)return mapped;
    if(victim->map)munmap(victim->map,(size_t)victim->size);
    *victim=(struct ElfMap){st->st_dev,st->st_ino,st->st_size,(long)st->st_mtimespec.tv_sec,(long)st->st_mtimespec.tv_nsec,mapped,++elf_map_clock};
    return mapped;
}
/* Returns the load bias. */
static uint64_t load_stream_inner(FILE *f,int interpreter) {
    char interpreter_path[1025]={0};
    struct stat input_stat;
    if(fstat(fileno(f),&input_stat)<0 || !S_ISREG(input_stat.st_mode) || input_stat.st_size<0) { fclose(f);fail("ELF input must be a readable regular file"); }
    if((uint64_t)input_stat.st_size>elf_input_limit) { fclose(f);fail(elf_input_limit==LIMIT?"Input read failed or exceeds 16 MiB":"Input read failed or exceeds configured ELF limit"); }
    size_t size=(size_t)input_stat.st_size;
    /* Mapped, not read: pages load lazily and are not charged to the guest's memory
       limit (an executable is page cache on Linux). Segments are copied out below. */
    void *mapped=size?elf_map(fileno(f),&input_stat):MAP_FAILED;
    static int text_fd=-1;                              /* text segments map from it (left open by a failed load) */
    if(text_fd>=0)close(text_fd);
    text_fd=dup(fileno(f));
    fclose(f);
    if(mapped==MAP_FAILED)fail("ELF input could not be read");
    address_space->file_data=mapped;address_space->file_size=size;
    const unsigned char *d=address_space->file_data;
    if(size<64 || memcmp(d,"\177ELF\002\001\001",7)) fail("Expected little-endian ELF64 version 1");
    uint64_t elf_type=le(d+16,2),bias=0;
    if(interpreter && elf_type!=3)fail("Interpreter must be ET_DYN");
    if((elf_type!=2 && elf_type!=3) || le(d+18,2)!=183 || le(d+20,4)!=1) fail("Only ARM64 ET_EXEC/static PIE version 1 supported");
    uint64_t phoff=le(d+32,8), count=le(d+56,2), total=0;
    for(unsigned i=0;i<address_space->nseg;i++)total+=address_space->seg[i].size;
    if(le(d+52,2)!=64 || le(d+54,2)!=56 || !count || count>128) fail("Invalid program header size/count");
    if(phoff<64 || phoff>size || count*56>size-phoff) fail("Program headers outside file");
    if(elf_type==3) {
        /* Where Linux puts it: a program with an interpreter at ELF_ET_DYN_BASE, the
           interpreter and a static PIE (a loader run directly) just below mmap_base;
           aligned down to the largest segment alignment. */
        uint64_t span=0,align=4096;int dynamic=0;
        for(uint64_t i=0;i<count;i++) {
            const unsigned char *h=d+(size_t)phoff+i*56;
            if(le(h,4)==3)dynamic=1;
            if(le(h,4)!=1)continue;
            uint64_t addr=le(h+16,8),mem=le(h+40,8),a=le(h+48,8);
            if(addr>UINT64_C(1)<<40 || mem>UINT64_C(1)<<40)fail("Invalid load segment bounds");
            if(addr+mem>span)span=addr+mem;
            if(a>align && !(a&(a-1)) && a<=(UINT64_C(1)<<30))align=a;
        }
        bias=(!interpreter && dynamic?PIE_BASE:INTERPRETER_TOP-page_up(span))&~(align-1);
    }
    if(le(d+24,8)>UINT64_MAX-bias)fail("ELF entry overflow");
    cpu->pc=address_space->entry=le(d+24,8)+bias; address_space->phcount=count;
    for(uint64_t i=0;i<count;i++) {
        const unsigned char *h=d+(size_t)phoff+i*56;
        unsigned type=(unsigned)le(h,4), flags=(unsigned)le(h+4,4);
        if(type==3) {
            if(!dynamic_enabled || interpreter || interpreter_path[0])fail("Unsupported ELF interpreter");
            uint64_t off=le(h+8,8),bytes=le(h+32,8);
            if(off>size || bytes>size-off || bytes<2 || bytes>1025 || d[off]!='/' || d[off+bytes-1] || memchr(d+off,0,(size_t)bytes-1))fail("Invalid interpreter path");
            memcpy(interpreter_path,d+off,(size_t)bytes);
        }
        if(type==2 && elf_type!=3 && !dynamic_enabled)fail("Dynamic linking is not implemented");
        if(type==2) {
            uint64_t offset=le(h+8,8),bytes=le(h+32,8);int terminated=0;
            if(offset>size || bytes>size-offset || bytes%16)fail("Invalid static PIE dynamic table");
            for(uint64_t j=0;j<bytes;j+=16) {
                uint64_t tag=le(d+offset+j,8);
                if(!tag) { terminated=1;break; }
                if(tag==1 && !dynamic_enabled)fail("Shared libraries are not implemented");
            }
            if(!terminated)fail("Unterminated static PIE dynamic table");
        }
        if(type!=1) continue;
        if(dynamic_enabled && ((flags&~7u) || (flags&3u)==3u))fail("Unsupported ELF segment permissions");
        uint64_t off=le(h+8,8), addr=le(h+16,8), bytes=le(h+32,8), mem=le(h+40,8), align=le(h+48,8);
        if(bytes>mem || off>size || bytes>size-off || mem>UINT64_MAX-addr) fail("Invalid load segment bounds");
        if(align>1 && ((align & (align-1)) || addr%align!=off%align)) fail("Invalid segment alignment");
        if(addr>UINT64_MAX-bias || mem>UINT64_MAX-(addr+bias))fail("ELF load bias overflow");
        if(align>1 && bias%align)fail("Unsupported static PIE alignment");
        addr+=bias;
        if(phoff>=off && phoff-off<=bytes && count*56<=bytes-(phoff-off)) address_space->phaddr=addr+(phoff-off);
        if(dynamic_enabled) {
            uint64_t prefix=addr&4095;
            if(prefix!=(off&4095) || off<prefix || mem>UINT64_MAX-prefix-4095)fail("Invalid ELF page alignment");
            addr-=prefix;off-=prefix;bytes+=prefix;mem=page_up(mem+prefix);
        }
        /* Loaded pages are charged to the memory budget; this caps one image at 16 MiB,
           or 4x a raised --elf-limit-mib (room for zero-filled data). */
        uint64_t image_limit=elf_input_limit>LIMIT?(uint64_t)elf_input_limit*4:LIMIT;
        if(mem>image_limit-total) fail(image_limit==LIMIT?"Guest memory exceeds 16 MiB limit":"Guest image exceeds 4x the configured ELF limit");
        total+=mem;
        for(unsigned j=0;j<address_space->nseg;j++) {
            Segment *s=&address_space->seg[j];
            if(!(addr<s->base+s->size && s->base<addr+mem))continue;
            /* Segments that share a page (node, Go binaries): as Linux maps them in
               order, the later one takes the page, holding the same file bytes. */
            if(dynamic_enabled && s->base<addr && s->base+s->size<=addr+mem) { s->size=addr-s->base;continue; }
            fail("Overlapping segments unsupported");
        }
        /* Read-only segments (text) are not charged to the memory limit: Linux keeps
           them in its reclaimable page cache (grafana's binary is 485 MB). */
        MemoryBacking *buffer=(flags&2)?NULL:text_cache_find(&input_stat,off,bytes,mem);
#ifdef __APPLE__
        if(!buffer && !(flags&2) && text_fd>=0 && !(off%(uint64_t)vm_page_size) && page_up(bytes)==mem) {
            buffer=backing_map_file(text_fd,(off_t)off,(size_t)mem);
            if(buffer)text_cache_add(&input_stat,off,bytes,mem,buffer);
        }
#endif
        if(!buffer) {
            buffer=(flags&2)?backing_alloc((size_t)mem,1):backing_alloc_text((size_t)mem);
            if(!buffer) fail("Allocation failed");
            memcpy(backing_data(buffer),d+(size_t)off,(size_t)bytes);
            if(!(flags&2))text_cache_add(&input_stat,off,bytes,mem,buffer);
        }
        if(address_space->nseg>=SEGMENT_LIMIT) { backing_release(buffer);fail("Too many load segments"); }
        address_space->seg[address_space->nseg++]=(Segment){addr,mem,flags,buffer,0};
        address_space_changed(address_space);

    }
    if(text_fd>=0) { close(text_fd);text_fd=-1; }
    if(!lookup(cpu->pc,4,1)) fail("Entry point is not executable");
    if(interpreter_path[0]) {
        int fd=fs_open_root_file(interpreter_path);if(fd<0)fail("Cannot open guest interpreter");
        FILE *input=fdopen(fd,"rb");if(!input) { close(fd);fail("Cannot read guest interpreter"); }
        uint64_t main_entry=address_space->entry,main_ph=address_space->phaddr,main_count=address_space->phcount;
        address_space->file_data=NULL;
        address_space->interpreter_base=load_stream_inner(input,1);address_space->entry=main_entry;address_space->phaddr=main_ph;address_space->phcount=main_count;
    }
    return bias;
}
/* The file mapping is loader input only (elf_map_cache owns it): drop it as soon as
   the segments are copied, so no image (vfork shares its parent's) ever holds it. */
static void load_stream(FILE *f) {
    (void)load_stream_inner(f,0);
    address_space->file_data=NULL;
}
static void store_le(uint64_t addr, uint64_t value, unsigned bytes) {
    cpu->exclusive_valid=0;
    unsigned char *p=lookup(addr,bytes,2);
    if(!p && memory_oom) exit(137); /* the interpreter cannot end only this process: the run ends, as an OOM kill */
    if(!p) fail("Guest write is unmapped or permission denied");
    for(unsigned i=0;i<bytes;i++) p[i]=(unsigned char)(value>>(8*i));
}
static uint64_t read_mem(uint64_t addr, unsigned bytes) {
    const unsigned char *p=lookup(addr,bytes,4);
    if(!p) { if(trace)fprintf(stderr,"read address=0x%" PRIx64 " bytes=%u\n",addr,bytes);fail("Guest read is unmapped or permission denied"); }
    return le(p,bytes);
}
/* AT_HWCAP and AT_HWCAP2: when guest code runs natively, the host CPU's features
   (macOS hw.optional.arm.FEAT_*) as Linux reports them, and HWCAP_CPUID (EL0 reads of the
   ID registers are emulated); the interpreter reports none.
   Pointer authentication and BTI are left out: they need kernel support. */
static int native_cpu;
static uint64_t hwcap_bits(int second) {
    if(!native_cpu)return 0;
    static uint64_t cached[2];static int known[2];                             /* the host's CPU does not change: ask once */
    if(known[second!=0])return cached[second!=0];
    static const struct { const char *feature;unsigned bit,second; } features[]={
        {"FEAT_AES",3,0},{"FEAT_PMULL",4,0},{"FEAT_SHA1",5,0},{"FEAT_SHA256",6,0},{"FEAT_CRC32",7,0},{"FEAT_LSE",8,0},
        {"FEAT_FP16",9,0},{"FEAT_FP16",10,0},{"FEAT_RDM",12,0},{"FEAT_JSCVT",13,0},{"FEAT_FCMA",14,0},{"FEAT_LRCPC",15,0},
        {"FEAT_DPB",16,0},{"FEAT_SHA3",17,0},{"FEAT_DotProd",20,0},{"FEAT_SHA512",21,0},{"FEAT_FHM",23,0},{"FEAT_DIT",24,0},
        {"FEAT_LSE2",25,0},{"FEAT_LRCPC2",26,0},{"FEAT_FlagM",27,0},{"FEAT_SSBS",28,0},{"FEAT_SB",29,0},
        {"FEAT_DPB2",0,1},{"FEAT_FlagM2",7,1},{"FEAT_FRINTTS",8,1},{"FEAT_I8MM",13,1},{"FEAT_BF16",14,1}};
    uint64_t bits=second?0:3|UINT64_C(1)<<11;                                          /* FP, ASIMD; CPUID: hvf_id_register */
#ifdef __APPLE__
    for(unsigned i=0;i<sizeof(features)/sizeof(*features);i++) {
        if(features[i].second!=(unsigned)second)continue;
        char name[64];int value=0;size_t size=sizeof(value);snprintf(name,sizeof(name),"hw.optional.arm.%s",features[i].feature);
        if(!sysctlbyname(name,&value,&size,NULL,0) && value)bits|=UINT64_C(1)<<features[i].bit;
    }
#endif
    cached[second!=0]=bits;known[second!=0]=1;
    return bits;
}
static void startup(int argc, const char **argv, unsigned envc, const char **env) {
    if(!exec_staging) { proc_set_cmdline(process_pid?process_pid:1,argc,argv);proc_publish(0); }
    if(!runtime_started_ns) { struct timespec now;clock_gettime(CLOCK_REALTIME,&now);runtime_started_ns=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec; }
    const uint64_t base=STACK_TOP-STACK_SIZE, size=STACK_SIZE;
    if((unsigned)argc>EXEC_ENTRIES || envc>EXEC_ENTRIES) fail("Too many arguments or environment entries");
    for(unsigned i=0;i<address_space->nseg;i++)
        if(base<address_space->seg[i].base+address_space->seg[i].size && address_space->seg[i].base<base+size) fail("ELF overlaps guest stack");
    MemoryBacking *data=backing_reserve((size_t)size); if(!data) fail("Allocation failed");
    address_space->seg[address_space->nseg++]=(Segment){base,size,6,data,0};
    if(vdso_backing) { /* shared, read-only and executable */
        for(unsigned i=0;i<address_space->nseg;i++)
            if(VDSO_BASE<address_space->seg[i].base+address_space->seg[i].size && address_space->seg[i].base<VDSO_BASE+VDSO_SIZE) fail("ELF overlaps the vDSO");
        if(address_space->nseg>=SEGMENT_LIMIT || !backing_retain(vdso_backing,1)) fail("Cannot map the vDSO");
        address_space->seg[address_space->nseg++]=(Segment){VDSO_BASE,VDSO_SIZE,5,vdso_backing,0};
    }
    address_space_changed(address_space);
    static uint64_t args[EXEC_ENTRIES],envs[EXEC_ENTRIES],vector[2*EXEC_ENTRIES+64];
    /* As Linux: argument then environment strings in ascending order (libuv measures
       the process title from argv[0] to the end of the last argument), then an 8-byte
       zero end marker at the top. */
    uint64_t total=0;
    for(unsigned group=0;group<2;group++)
        for(unsigned i=0;i<(group?envc:(unsigned)argc);i++) {
            size_t length=strlen(group?env[i]:argv[i])+1;
            if(length>EXEC_STRINGS-total) fail("Arguments and environment exceed 1 MiB");
            total+=length;
        }
    uint64_t cursor=base+size-8-total,next=cursor;
    for(unsigned group=0;group<2;group++)
        for(unsigned i=0;i<(group?envc:(unsigned)argc);i++) {
            const char *v=group?env[i]:argv[i];size_t length=strlen(v)+1;
            memcpy(backing_data(data)+(size_t)(next-base),v,length);
            if(group) envs[i]=next; else args[i]=next;
            next+=length;
        }
    cursor-=16;
#ifdef __APPLE__
    arc4random_buf(backing_data(data)+(size_t)(cursor-base),16); /* no system call per exec */
#else
    if(getentropy(backing_data(data)+(size_t)(cursor-base),16)<0)fail("Cannot initialize guest startup randomness");
#endif
    uint64_t random_address=cursor;
    /* The strings were copied into the backing directly: charge their pages now. */
    if(!address_space_make_private(address_space,data,(size_t)(cursor-base),(size_t)(base+size-cursor))) fail("Guest memory budget exhausted (--memory-limit-mib)");
    unsigned n=0;
    vector[n++]=(unsigned)argc;
    for(int i=0;i<argc;i++) vector[n++]=args[i];
    vector[n++]=0;
    for(unsigned i=0;i<envc;i++) vector[n++]=envs[i];
    vector[n++]=0;
    unsigned aux=n;                                                                   /* AT_* pairs from here */
    if(address_space->phaddr) { vector[n++]=3;vector[n++]=address_space->phaddr; }
    vector[n++]=4; vector[n++]=56;
    vector[n++]=5; vector[n++]=address_space->phcount;
    vector[n++]=6; vector[n++]=4096;
    vector[n++]=7; vector[n++]=address_space->interpreter_base;
    vector[n++]=9; vector[n++]=address_space->entry;
    vector[n++]=11; vector[n++]=guest_ids.ruid; /* Synthetic UID/EUID/GID/EGID. */
    vector[n++]=12; vector[n++]=guest_uid;
    vector[n++]=13; vector[n++]=guest_ids.rgid;
    vector[n++]=14; vector[n++]=guest_gid;
    vector[n++]=16; vector[n++]=hwcap_bits(0);
    vector[n++]=17; vector[n++]=100;
    vector[n++]=23; vector[n++]=guest_uid!=guest_ids.ruid || guest_gid!=guest_ids.rgid; /* AT_SECURE: a set-ID exec */
    vector[n++]=25; vector[n++]=random_address;
    vector[n++]=26; vector[n++]=hwcap_bits(1);
    vector[n++]=31; vector[n++]=args[0];
    if(vdso_backing) { vector[n++]=33; vector[n++]=VDSO_BASE; } /* AT_SYSINFO_EHDR */
    vector[n++]=0; vector[n++]=0;
    /* Kept for /proc/self/auxv. */
    address_space->auxv_count=n-aux<48?n-aux:48;memcpy(address_space->auxv,vector+aux,address_space->auxv_count*8);
    cpu->sp=(cursor-n*8)&~UINT64_C(15);
    for(unsigned i=0;i<n;i++) store_le(cpu->sp+i*8,vector[i],8);
}
static uint64_t mask_width(unsigned width) { return width==64 ? UINT64_MAX : UINT32_MAX; }
static uint64_t getreg(unsigned r, int allow_sp) { return r==31 ? (allow_sp?cpu->sp:0) : cpu->gpr[r]; }
static void putreg(unsigned r,uint64_t v,unsigned width,int allow_sp) {
    v &= mask_width(width);
    if(r==31) { if(allow_sp) cpu->sp=v; } else cpu->gpr[r]=v;
}
static uint64_t alu(uint64_t a,uint64_t b,unsigned width,int subtract,int flags) {
    uint64_t mask=mask_width(width), sign=UINT64_C(1)<<(width-1);
    a &= mask; b &= mask;
    uint64_t r=(subtract?a-b:a+b)&mask;
    if(flags) {
        cpu->flag_n=(r&sign)!=0; cpu->flag_z=r==0;
        cpu->flag_c=subtract ? a>=b : r<a;
        cpu->flag_v=((subtract ? (a^b) : ~(a^b)) & (a^r) & sign)!=0;
    }
    return r;
}
static int condition(unsigned cond) {
    int result;
    switch(cond>>1) {
        case 0:result=cpu->flag_z;break; case 1:result=cpu->flag_c;break;
        case 2:result=cpu->flag_n;break; case 3:result=cpu->flag_v;break;
        case 4:result=cpu->flag_c&&!cpu->flag_z;break; case 5:result=cpu->flag_n==cpu->flag_v;break;
        case 6:result=!cpu->flag_z && cpu->flag_n==cpu->flag_v;break; default:return 1;
    }
    return (cond&1)?!result:result;
}
/* Unsigned sign extension deliberately uses modulo-2^64 arithmetic. */
static uint64_t extend(uint64_t v, unsigned bits) { uint64_t sign=UINT64_C(1)<<(bits-1); return (v^sign)-sign; }
static uint64_t rotate(uint64_t value,unsigned shift,unsigned width) {
    uint64_t mask=width==64?UINT64_MAX:((UINT64_C(1)<<width)-1);
    value&=mask;shift%=width;
    return shift?((value>>shift)|(value<<(width-shift)))&mask:value;
}
static uint64_t logical_mask(uint32_t ins,unsigned width) {
    unsigned n=(ins>>22)&1, imms=(ins>>10)&63, immr=(ins>>16)&63;
    unsigned encoded=(n<<6)|((~imms)&63),len=0;
    while(encoded>>1) { len++;encoded>>=1; }
    if(len<1 || (width==32 && n)) fail("Reserved logical mask");
    unsigned size=1u<<len, levels=size-1, bits=(imms&levels)+1;
    if(bits==size) fail("Reserved logical mask");
    uint64_t element=rotate((UINT64_C(1)<<bits)-1,immr&levels,size),value=0;
    for(unsigned i=0;i<width;i+=size) value|=element<<i;
    return value;
}
#include "decode.h"
#include "process.h"
#include "terminal.h"
#include "signals.h"
#include "sysinfo.h"
#include "seccomp.h"
#include "syscall.h"
#ifdef VMFREE_HVF
#include "hvf_backend.h"
#endif
static int run(uint64_t steps) {
    if(schedule_enabled && !schedule_active)return schedule_run(steps);
    if(!process_depth && !schedule_active)process_steps=steps;
#ifdef VMFREE_HVF
    if(hvf_active)return hvf_slice();
#endif
    while(process_steps) {
        if(job_control_enabled && schedule_active) { signal_deliver(0);if(schedule_tasks[process_pid-1].terminate_signal || schedule_tasks[process_pid-1].stop_signal)return -1; }
        if(schedule_active) { if(!schedule_slice)return -1;schedule_slice--; }
        process_steps--;
        if(profiling) {
            if(profile_requested) { profile_requested=0;profile_dump(); }
            profile_instructions++;
        }
        if(cpu->pc%4) fail("Unaligned instruction address");
        const unsigned char *p=lookup(cpu->pc,4,1); if(!p) fail("Unmapped or non-executable instruction");
        uint32_t ins=(uint32_t)le(p,4); uint64_t oldpc=cpu->pc; cpu->pc+=4;
        if(trace) { trace_tail[trace_cursor%16].address=oldpc;trace_tail[trace_cursor%16].word=ins;trace_cursor++; }
        unsigned rd=ins&31;
        switch(decode_instruction(ins)) {
        case 1: {
            unsigned width=(ins>>31)?64:32, shift=((ins>>21)&3)*16;
            unsigned op=(ins>>29)&3;
            if(shift>=width || op==1) fail("Reserved move-wide encoding");
            uint64_t value=(uint64_t)((ins>>5)&65535)<<shift;
            if(op==0) value=~value;
            else if(op==3) value=(getreg(rd,0)&~(UINT64_C(65535)<<shift))|value;
            putreg(rd,value,width,0);
        } break;
        case 2: {
            uint64_t imm=(((ins>>5)&0x7ffffu)<<2)|((ins>>29)&3);
            cpu->gpr[rd]=oldpc+extend(imm,21);
        } break;
        case 3: {
            uint64_t imm=(((ins>>5)&0x7ffffu)<<2)|((ins>>29)&3);
            cpu->gpr[rd]=(oldpc&~UINT64_C(4095))+(extend(imm,21)<<12);
        } break;
        case 4: {
            if(ins>>31) cpu->gpr[30]=cpu->pc;
            cpu->pc=oldpc+(extend(ins&0x3ffffffu,26)<<2);
        } break;
        case 5: {
            uint64_t target=getreg((ins>>5)&31,0);
            if((ins&0xfffffc1fu)==0xd63f0000u) cpu->gpr[30]=cpu->pc;
            cpu->pc=target;
        } break;
        case 6: {
            if(condition(ins&15)) cpu->pc=oldpc+(extend((ins>>5)&0x7ffffu,19)<<2);
        } break;
        case 7: {
            uint64_t value=getreg(rd,0)&mask_width((ins>>31)?64:32);
            if((value!=0)==((ins>>24)&1)) cpu->pc=oldpc+(extend((ins>>5)&0x7ffffu,19)<<2);
        } break;
        case 8: {
            unsigned bit=((ins>>26)&32)|((ins>>19)&31);
            if(((getreg(rd,0)>>bit)&1)==((ins>>24)&1)) cpu->pc=oldpc+(extend((ins>>5)&0x3fffu,14)<<2);
        } break;
        case 9: {
            unsigned width=(ins>>31)?64:32, rn=(ins>>5)&31, flags=(ins>>29)&1;
            uint64_t imm=(ins>>10)&4095; if(ins&(1u<<22)) imm<<=12;
            putreg(rd,alu(getreg(rn,1),imm,width,(ins>>30)&1,flags),width,!flags);
        } break;
        case 10: {
            unsigned width=(ins>>31)?64:32,option=(ins>>13)&7,shift=(ins>>10)&7,flags=(ins>>29)&1;
            if(shift>4) fail("Reserved extended add shift");
            unsigned bits=8u<<(option&3);uint64_t b=getreg((ins>>16)&31,0);
            if(bits<64) { b&=(UINT64_C(1)<<bits)-1;if(option&4)b=extend(b,bits); }
            putreg(rd,alu(getreg((ins>>5)&31,1),b<<shift,width,(ins>>30)&1,flags),width,!flags);
        } break;
        case 11: {
            unsigned width=(ins>>31)?64:32, shift=(ins>>10)&63, kind=(ins>>22)&3;
            if(kind==3 || shift>=width) fail("Reserved arithmetic shift");
            uint64_t b=getreg((ins>>16)&31,0)&mask_width(width);
            if(kind==0) b<<=shift;
            else if(kind==1) b>>=shift;
            else if(shift) { uint64_t sign=b>>(width-1); b>>=shift; if(sign) b|=mask_width(width)<<(width-shift); }
            putreg(rd,alu(getreg((ins>>5)&31,0),b,width,(ins>>30)&1,(ins>>29)&1),width,0);
        } break;
        case 12: {
            uint64_t a=getreg((ins>>5)&31,0),b=getreg((ins>>16)&31,0);
            uint64_t lo=(a&UINT32_MAX)*(b&UINT32_MAX);
            uint64_t mid=(a>>32)*(b&UINT32_MAX)+(lo>>32);
            uint64_t carry=mid>>32;
            mid=(mid&UINT32_MAX)+(a&UINT32_MAX)*(b>>32);
            uint64_t hi=(a>>32)*(b>>32)+carry+(mid>>32);
            if(!(ins&(1u<<23))) { if(a>>63)hi-=b;if(b>>63)hi-=a; }
            putreg(rd,hi,64,0);
        } break;
        case 13: {
            uint64_t product=getreg((ins>>5)&31,0)*getreg((ins>>16)&31,0),a=getreg((ins>>10)&31,0);
            putreg(rd,(ins&(1u<<15))?a-product:a+product,(ins>>31)?64:32,0);
        } break;
        case 14: {
            unsigned width=(ins>>31)?64:32, op=(ins>>29)&3;
            uint64_t a=getreg((ins>>5)&31,0), b=logical_mask(ins,width);
            uint64_t v=op==0||op==3?a&b:op==1?a|b:a^b;
            if(op==3) { cpu->flag_n=(v>>(width-1))&1;cpu->flag_z=(v&mask_width(width))==0;cpu->flag_c=cpu->flag_v=0; }
            putreg(rd,v,width,op!=3);
        } break;
        case 15: {
            unsigned width=(ins>>31)?64:32,r=(ins>>16)&63,end=(ins>>10)&63,op=(ins>>29)&3;
            if(r>=width || end>=width || op==3 || ((ins>>22)&1)!=(width==64)) fail("Reserved bitfield");
            unsigned count=end>=r?end-r+1:end+1, shift=end>=r?0:width-r;
            uint64_t mask=count==64?UINT64_MAX:(UINT64_C(1)<<count)-1;
            uint64_t v=((getreg((ins>>5)&31,0)>>(end>=r?r:0))&mask)<<shift;
            if(op==0 && ((v>>(shift+count-1))&1) && shift+count<64) v|=UINT64_MAX<<(shift+count);
            if(op==1) v=(getreg(rd,0)&~(mask<<shift))|v;
            putreg(rd,v,width,0);
        } break;
        case 16: {
            unsigned width=(ins>>31)?64:32,shift=(ins>>10)&63,kind=(ins>>22)&3,op=(ins>>29)&3;
            if(shift>=width) fail("Reserved logical shift");
            uint64_t b=getreg((ins>>16)&31,0)&mask_width(width),a=getreg((ins>>5)&31,0);
            if(kind==0) b<<=shift;
            else if(kind==1) b>>=shift;
            else if(kind==2 && shift) { uint64_t sign=b>>(width-1);b>>=shift;if(sign)b|=mask_width(width)<<(width-shift); }
            else if(kind==3) b=rotate(b,shift,width);
            if(ins&(1u<<21)) b=~b;
            uint64_t v=op==0||op==3?a&b:op==1?a|b:a^b;
            if(op==3) { cpu->flag_n=(v>>(width-1))&1;cpu->flag_z=(v&mask_width(width))==0;cpu->flag_c=cpu->flag_v=0; }
            putreg(rd,v,width,0);
        } break;
        case 17: {
            /* MOV register alias (ORR Rd, ZR, Rm, LSL #0). */
            putreg(rd,getreg((ins>>16)&31,0),(ins>>31)?64:32,0);
        } break;
        case 18: {
            unsigned width=(ins>>31)?64:32;
            uint64_t v=getreg((ins>>5)&31,0);
            if(!condition((ins>>12)&15)) {
                v=getreg((ins>>16)&31,0);
                if(ins&(1u<<30)) v=~v;
                if(ins&(1u<<10)) ++v;
            }
            putreg(rd,v,width,0);
        } break;
        case 19: {
            unsigned bytes=(ins>>16)&31,total=(ins&(1u<<30))?16:8;
            if(bytes>8 || (bytes==8 && total==8))fail("Unsupported vector DUP shape");
            uint64_t value=getreg((ins>>5)&31,0);
            memset(cpu->simd[rd],0,16);
            for(unsigned i=0;i<total;i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*(i%bytes)));
        } break;
        case 20: {
            putreg(rd,le(cpu->simd[(ins>>5)&31],8),64,0);
        } break;
        case 21: {
            unsigned rn=(ins>>5)&31,isload=(ins>>22)&1,mode=0,bytes=1u<<(ins>>30);uint64_t base=getreg(rn,1),offset;
            if(rn==31 && (base&15))fail("Unaligned stack pointer");
            if((ins&0x3fa00c00u)==0x3c200800u) {
                unsigned option=(ins>>13)&7;offset=getreg((ins>>16)&31,0);
                if(option==2)offset&=UINT32_MAX;
                else if(option==6)offset=extend(offset&UINT32_MAX,32);
                else if(option!=3 && option!=7)fail("Reserved SIMD register extension");
                if(ins&(1u<<12))offset<<=(ins>>30);
            }
            else if((ins&0x3f800000u)==0x3d000000u)offset=((ins>>10)&4095)*bytes;
            else {mode=(ins>>10)&3;offset=extend((ins>>12)&511,9);if(mode==2)fail("Unsupported D memory mode");}
            unsigned char *buffer=lookup(base+(mode==1?0:offset),bytes,isload?4:2);if(!buffer)fail("Scalar vector memory fault");
            if(isload){memset(cpu->simd[rd],0,16);memcpy(cpu->simd[rd],buffer,bytes);}else { cpu->exclusive_valid=0;memcpy(buffer,cpu->simd[rd],bytes); }
            if(mode)putreg(rn,base+offset,64,1);
        } break;
        case 22: {
            unsigned rn=(ins>>5)&31,isload=(ins>>22)&1,mode=0;
            uint64_t base=getreg(rn,1),offset;
            if(rn==31 && (base&15))fail("Unaligned SIMD stack access");
            if((ins&0xffa00c00u)==0x3ca00800u) {
                unsigned option=(ins>>13)&7;offset=getreg((ins>>16)&31,0);
                if(option==2)offset&=UINT32_MAX;
                else if(option==6)offset=extend(offset&UINT32_MAX,32);
                else if(option!=3 && option!=7)fail("Reserved SIMD register extension");
                if(ins&(1u<<12))offset<<=4;
            }
            else if((ins&0xff800000u)==0x3d800000u) offset=((ins>>10)&4095)*16;
            else { mode=(ins>>10)&3;offset=extend((ins>>12)&511,9);if(mode==2)fail("Unsupported vector memory mode"); }
            uint64_t addr=base+(mode==1?0:offset);
            unsigned char *buffer=lookup(addr,16,isload?4:2);if(!buffer) { if(trace)fprintf(stderr,"vector address=0x%" PRIx64 " brk=0x%" PRIx64 "+%" PRIu64 "\n",addr,address_space->brk_base,address_space->brk_pages);fail("Vector memory fault"); }
            if(isload)memcpy(cpu->simd[rd],buffer,16);else { cpu->exclusive_valid=0;memcpy(buffer,cpu->simd[rd],16); }
            if(mode)putreg(rn,base+offset,64,1);
        } break;
        case 23: {
            unsigned mode=(ins>>23)&3,rn=(ins>>5)&31,rt2=(ins>>10)&31,isload=(ins>>22)&1;
            unsigned opc=ins>>30,bytes=4u<<opc;
            if(!mode || opc==3 || (isload && rd==rt2))fail("Unsupported vector pair mode");
            uint64_t base=getreg(rn,1),offset=extend((ins>>15)&127,7)*bytes,addr=base+(mode==1?0:offset);
            if(rn==31 && (base&15))fail("Unaligned stack pointer");
            unsigned char *buffer=lookup(addr,bytes*2,isload?4:2);if(!buffer)fail("Vector pair memory fault");
            if(isload) { memset(cpu->simd[rd],0,16);memset(cpu->simd[rt2],0,16);memcpy(cpu->simd[rd],buffer,bytes);memcpy(cpu->simd[rt2],buffer+bytes,bytes); }
            else { cpu->exclusive_valid=0;memcpy(buffer,cpu->simd[rd],bytes);memcpy(buffer+bytes,cpu->simd[rt2],bytes); }
            if(mode!=2)putreg(rn,base+offset,64,1);
        } break;
        case 45: {
            unsigned rn=(ins>>5)&31,isload=(ins>>22)&1;uint64_t addr=getreg(rn,1);
            if(rn==31 && (addr&15))fail("Unaligned stack pointer");
            unsigned char *p=lookup(addr,32,isload?4:2);if(!p)fail("Vector structure memory fault");
            if(isload) { memcpy(cpu->simd[rd],p,16);memcpy(cpu->simd[(rd+1)&31],p+16,16); }
            else { cpu->exclusive_valid=0;memcpy(p,cpu->simd[rd],16);memcpy(p+16,cpu->simd[(rd+1)&31],16); }
        } break;
        case 24: {
            unsigned width=(ins>>31)?64:32;
            if(condition((ins>>12)&15)) {
                uint64_t b=ins&(1u<<11)?(ins>>16)&31:getreg((ins>>16)&31,0);
                (void)alu(getreg((ins>>5)&31,0),b,width,(ins>>30)&1,1);
            } else { cpu->flag_n=(ins>>3)&1;cpu->flag_z=(ins>>2)&1;cpu->flag_c=(ins>>1)&1;cpu->flag_v=ins&1; }
        } break;
        case 25: {
            unsigned size=1u<<(ins>>30), opc=(ins>>22)&3, rn=(ins>>5)&31;
            if(opc==2 && size==8) {
                /* Prefetch is a cache hint; this interpreter has no guest data cache. */
                unsigned mode=(ins>>10)&3;
                if((ins&0x3f000000u)!=0x39000000u &&
                   ((ins&(1u<<21)) ? (mode!=2 || !(((ins>>13)&7)&2)) : mode!=0))
                    fail("Reserved prefetch encoding");
                break;
            }
            if(opc==3 && size>=4) fail("Reserved load encoding");
            uint64_t base=getreg(rn,1), addr=base, offset=0; unsigned mode=0;
            if(rn==31 && (base&15)) fail("Unaligned stack pointer");
            if((ins&0x3f000000u)==0x39000000u) offset=((ins>>10)&4095)*size;
            else if(ins&(1u<<21)) {
                unsigned option=(ins>>13)&7;
                offset=getreg((ins>>16)&31,0);
                if(option==2) offset &= UINT32_MAX;
                else if(option==6) offset=extend(offset&UINT32_MAX,32);
                else if(option!=3 && option!=7) fail("Reserved memory register extension");
                if(ins&(1u<<12)) offset<<=(ins>>30);
            } else {
                mode=(ins>>10)&3;
                if(mode==2) fail("Unprivileged memory operation unsupported");
                offset=extend((ins>>12)&511,9);
                if(mode && rn==rd && rn!=31) fail("Unpredictable memory writeback overlap");
            }
            if(mode!=1) addr+=offset;
            if(opc) { uint64_t value=read_mem(addr,size);
                if(opc>=2) value=extend(value,size*8);
                putreg(rd,value,(size==8 || opc==2)?64:32,0);
            }
            else store_le(addr,getreg(rd,0),size);
            if(mode) putreg(rn,base+offset,64,1);
        } break;
        case 26: {
            unsigned opc=ins>>30, mode=(ins>>23)&3, rn=(ins>>5)&31, rt2=(ins>>10)&31, isload=(ins>>22)&1;
            if(opc==3 || (opc==1 && (!isload || !mode))) fail("Unsupported pair memory variant");
            unsigned bytes=opc==2?8:4;
            if(((mode==1 || mode==3) && rn!=31 && (rn==rd || rn==rt2)) || (isload && rd==rt2)) fail("Unpredictable pair overlap");
            uint64_t base=getreg(rn,1), offset=extend((ins>>15)&127,7)*bytes, addr=base;
            if(rn==31 && (base&15)) fail("Unaligned stack pointer");
            if(mode!=1) addr+=offset;
            if(isload) { uint64_t a=read_mem(addr,bytes), b=read_mem(addr+bytes,bytes); if(opc==1) { a=extend(a,32);b=extend(b,32); } putreg(rd,a,opc==1?64:bytes*8,0); putreg(rt2,b,opc==1?64:bytes*8,0); }
            else { if(!lookup(addr,bytes*2,2)) fail("Guest pair write permission denied"); store_le(addr,getreg(rd,0),bytes); store_le(addr+bytes,getreg(rt2,0),bytes); }
            if(mode==1 || mode==3) putreg(rn,base+offset,64,1);
        } break;
        case 27: {
            putreg(rd,16,64,0); /* DCZID: prohibit DC ZVA. */
        } break;
        case 28: {
            putreg(rd,cpu->tls,64,0);
        } break;
        case 29: {
            cpu->tls=getreg(rd,0);
        } break;
        case 30: {
            /* NOP */
        } break;
        case 31: {
            int result=seccomp_check();if(!result)result=guest_syscall();if(result!=-3)return result;
        } break;
        case 32: {
            unsigned width=(ins>>31)?64:32;
            uint64_t a=getreg((ins>>5)&31,0)&mask_width(width),b=getreg((ins>>16)&31,0)&mask_width(width),value=0;
            if(b) {
                if(ins&(1u<<10)) {
                    int64_t sa=(int64_t)extend(a,width),sb=(int64_t)extend(b,width);
                    value=(sa==INT64_MIN && sb==-1)?(uint64_t)sa:(uint64_t)(sa/sb);
                } else value=a/b;
            }
            putreg(rd,value,width,0);
        } break;
        case 33: {
            uint64_t a=getreg((ins>>5)&31,0)&UINT32_MAX,b=getreg((ins>>16)&31,0)&UINT32_MAX;
            if(!(ins&(1u<<23))) { a=extend(a,32);b=extend(b,32); }
            uint64_t product=a*b,acc=getreg((ins>>10)&31,0);
            putreg(rd,(ins&(1u<<15))?acc-product:acc+product,64,0);
        } break;
        case 34: {
            unsigned width=(ins>>31)?64:32,op=(ins>>10)&31;
            uint64_t value=getreg((ins>>5)&31,0)&mask_width(width),out=0;
            if(!op) { for(unsigned i=0;i<width;i++) out|=((value>>i)&1)<<(width-1-i); }
            else if(op<=3) {
                unsigned group=1u<<op;if(group>width/8) fail("Reserved reverse encoding");
                for(unsigned i=0;i<width/8;i++)out|=((value>>(8*i))&255)<<(8*((i/group)*group+group-1-i%group));
            } else {
                if(op==5 && (value>>(width-1)))value=(~value)&mask_width(width);
                while(out<width && !((value>>(width-1-(unsigned)out))&1))out++;
                if(op==5)out--;
            }
            putreg(rd,out,width,0);
        } break;
        case 35: {
            unsigned width=(ins>>31)?64:32,amount=(unsigned)getreg((ins>>16)&31,0)&(width-1),op=(ins>>10)&3;
            uint64_t value=getreg((ins>>5)&31,0)&mask_width(width),out=value;
            if(amount) {
                if(op==0)out=value<<amount;
                else if(op==1)out=value>>amount;
                else if(op==2) { out=value>>amount;if(value>>(width-1))out|=mask_width(width)<<(width-amount); }
                else out=(value>>amount)|(value<<(width-amount));
            }
            putreg(rd,out,width,0);
        } break;
        case 36: memset(cpu->simd[rd],0,16);break;
        case 37: {
            unsigned size=1u<<(ins>>30);uint64_t addr=getreg((ins>>5)&31,1);
            if(addr%size || (((ins>>5)&31)==31 && (addr&15)))fail("Unaligned exclusive load");
            putreg(rd,read_mem(addr,size),size*8,0);
            cpu->exclusive_addr=addr;cpu->exclusive_size=size;cpu->exclusive_valid=1;
        } break;
        case 38: {
            unsigned size=1u<<(ins>>30);uint64_t addr=getreg((ins>>5)&31,1);
            unsigned success=cpu->exclusive_valid && cpu->exclusive_addr==addr && cpu->exclusive_size==size;
            cpu->exclusive_valid=0;if(addr%size || (((ins>>5)&31)==31 && (addr&15)))fail("Unaligned exclusive store");
            if(success)store_le(addr,getreg(rd,0),size);
            putreg((ins>>16)&31,success?0:1,32,0);
        } break;
        case 52: {
            unsigned total=(ins&(1u<<30))?16:8,rn=(ins>>5)&31,rm=(ins>>16)&31;
            unsigned op=((ins>>22)&3)|((ins>>27)&4);unsigned char out[16]={0};
            for(unsigned i=0;i<total;i++) {
                unsigned a=cpu->simd[rn][i],b=cpu->simd[rm][i],d=cpu->simd[rd][i],v;
                switch(op) {
                case 0:v=a&b;break;case 1:v=a&~b;break;case 2:v=a|b;break;case 3:v=a|~b;break;
                case 4:v=a^b;break;case 5:v=(a&d)|(b&~d);break;case 6:v=(d&~b)|(a&b);break;default:v=(d&b)|(a&~b);break;
                }
                out[i]=(unsigned char)v;
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 73: {
            unsigned imm=((ins>>5)&31)|((ins>>11)&224),total=(ins&(1u<<30))?16:8;
            memset(cpu->simd[rd],0,16);for(unsigned i=0;i<total;i++)cpu->simd[rd][i]=(imm&(1u<<(i%8)))?255:0;
        } break;
        case 72: {
            unsigned imm=(ins>>16)&31,bytes=1,width=(ins&(1u<<30))?64:32;
            if(!imm)fail("Reserved UMOV");
            while(!(imm&bytes))bytes*=2;
            if(bytes>8 || (bytes==8)!=(width==64))fail("Reserved UMOV shape");
            putreg(rd,le(cpu->simd[(ins>>5)&31]+(imm/(bytes*2))*bytes,bytes),width,0);
        } break;
        case 69: {
            unsigned bytes=1u<<((ins>>22)&3),upper=(ins>>30)&1,rn=(ins>>5)&31;
            if(bytes==8)fail("Reserved SHLL shape");
            unsigned char out[16]={0};
            for(unsigned i=0;i<8/bytes;i++)memcpy(out+i*bytes*2+bytes,cpu->simd[rn]+upper*8+i*bytes,bytes);
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 70: {
            unsigned imm=(ins>>16)&127,bits=8,total=(ins&(1u<<30))?16:8,left=(ins>>14)&1;
            if(imm<8)fail("Reserved SIMD shift");
            while(bits*2<=imm)bits*=2;
            if(bits==64 && total==8)fail("Reserved SIMD shift shape");
            unsigned shift=left?imm-bits:bits*2-imm,bytes=bits/8;
            unsigned char out[16]={0};
            for(unsigned i=0;i<total;i+=bytes) {
                uint64_t v=le(cpu->simd[(ins>>5)&31]+i,bytes);
                if(left)v<<=shift;
                else {
                    unsigned sign=!(ins&(1u<<29)) && (v>>(bits-1));
                    v=shift==64?0:v>>shift;
                    if(sign)v|=UINT64_MAX<<(bits-shift);
                }
                for(unsigned j=0;j<bytes;j++)out[i+j]=(unsigned char)(v>>(8*j));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 71: {
            unsigned imm=(ins>>16)&31,bytes=1;
            if(!imm)fail("Reserved scalar DUP");
            while(!(imm&bytes))bytes*=2;
            if(bytes>8)fail("Reserved scalar DUP size");
            uint64_t v=le(cpu->simd[(ins>>5)&31]+(imm/(bytes*2))*bytes,bytes);
            memset(cpu->simd[rd],0,16);for(unsigned j=0;j<bytes;j++)cpu->simd[rd][j]=(unsigned char)(v>>(8*j));
        } break;
        case 67: {
            unsigned width=(ins>>31)?64:32,shift=(ins>>10)&63;
            if(((ins>>22)&1)!=(ins>>31) || shift>=width)fail("Reserved EXTR encoding");
            uint64_t low=getreg((ins>>16)&31,0),high=getreg((ins>>5)&31,0);
            if(width==32)low&=UINT32_MAX;
            putreg(rd,shift?((low>>shift)|(high<<(width-shift))):low,width,0);
        } break;
        case 68: {
            unsigned imm=(ins>>16)&127,bits=8,upper=(ins>>30)&1,uns=(ins>>29)&1;
            if(imm<8 || imm>=64)fail("Reserved widening shift");
            while(bits*2<=imm)bits*=2;
            unsigned bytes=bits/8,shift=imm-bits;unsigned char out[16]={0};
            for(unsigned i=0;i<8/bytes;i++) {
                uint64_t value=le(cpu->simd[(ins>>5)&31]+upper*8+i*bytes,bytes);
                if(!uns)value=extend(value,bits);
                value<<=shift;
                for(unsigned j=0;j<bytes*2;j++)out[i*bytes*2+j]=(unsigned char)(value>>(8*j));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 66: {
            unsigned rn=(ins>>5)&31,rm=(ins>>16)&31,post=(ins>>23)&1,isload=(ins>>22)&1;
            unsigned opcode=(ins>>12)&15,bytes=1u<<((ins>>10)&3),total=(ins&(1u<<30))?16:8;
            unsigned repeat=1,count=1;
            switch(opcode) {
                case 0:count=4;break;case 2:repeat=4;break;
                case 4:count=3;break;case 6:repeat=3;break;
                case 7:break;case 8:count=2;break;case 10:repeat=2;break;
                default:fail("Reserved SIMD structure opcode");
            }
            if((!post && rm) || (bytes==8 && total==8 && count!=1))fail("Reserved SIMD structure shape");
            uint64_t addr=getreg(rn,1),increment=rm==31?total*repeat*count:getreg(rm,0);
            if(rn==31 && (addr&15))fail("Unaligned SIMD stack access");
            unsigned char *p=lookup(addr,total*repeat*count,isload?4:2);
            if(!p)fail("SIMD structure access denied");
            if(!isload)cpu->exclusive_valid=0;
            unsigned offset=0;
            for(unsigned r=0;r<repeat;r++) {
                if(isload)for(unsigned c=0;c<count;c++)memset(cpu->simd[(rd+r+c)&31],0,16);
                for(unsigned lane=0;lane<total/bytes;lane++)for(unsigned c=0;c<count;c++) {
                    unsigned char *v=cpu->simd[(rd+r+c)&31]+lane*bytes;
                    if(isload)memcpy(v,p+offset,bytes);else memcpy(p+offset,v,bytes);
                    offset+=bytes;
                }
            }
            if(post)putreg(rn,addr+increment,64,1);
        } break;
        case 65: {
            unsigned imm=(ins>>16)&31,bytes=imm&(~imm+1),source=(ins>>11)&15;
            if(!bytes || bytes>8 || source%bytes)fail("Reserved vector INS shape");
            unsigned at=(imm/(bytes*2))*bytes;uint64_t value=le(cpu->simd[(ins>>5)&31]+source,bytes);
            for(unsigned i=0;i<bytes;i++)cpu->simd[rd][at+i]=(unsigned char)(value>>(8*i));
        } break;
        case 64: {
            unsigned width=(ins>>31)?64:32,subtract=(ins>>30)&1;
            uint64_t mask=mask_width(width),sign=UINT64_C(1)<<(width-1);
            uint64_t a=getreg((ins>>5)&31,0)&mask,b=getreg((ins>>16)&31,0)&mask;
            if(subtract)b=(~b)&mask;
            uint64_t sum=(a+b)&mask,result=(sum+cpu->flag_c)&mask;
            if(ins&(1u<<29)) {
                cpu->flag_c=sum<a || result<sum;cpu->flag_n=(result&sign)!=0;cpu->flag_z=result==0;
                cpu->flag_v=((~(a^b))&(a^result)&sign)!=0;
            }
            putreg(rd,result,width,0);
        } break;
        case 63: {
            unsigned imm=(ins>>16)&31,bytes=imm&(~imm+1);
            if(!bytes || bytes>8)fail("Reserved INS element size");
            unsigned at=(imm/(bytes*2))*bytes;uint64_t value=getreg((ins>>5)&31,0);
            for(unsigned i=0;i<bytes;i++)cpu->simd[rd][at+i]=(unsigned char)(value>>(8*i));
        } break;
        case 62: {
            unsigned rn=(ins>>5)&31,rm=(ins>>16)&31,total=(ins&(1u<<30))?16:8,offset=(ins>>11)&15;
            if(offset>=total)fail("Reserved EXT offset");
            unsigned char out[16]={0};for(unsigned i=0;i<total;i++) {
                unsigned at=offset+i;out[i]=at<total?cpu->simd[rn][at]:cpu->simd[rm][at-total];
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 61: {
            unsigned rn=(ins>>5)&31,total=(ins&(1u<<30))?16:8,bytes=1u<<((ins>>22)&3);
            if(bytes==8 || (bytes==4 && total==8))fail("Reserved ADDV shape");
            uint64_t value=0;for(unsigned i=0;i<total;i+=bytes)value+=le(cpu->simd[rn]+i,bytes);
            memset(cpu->simd[rd],0,16);for(unsigned i=0;i<bytes;i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*i));
        } break;
        case 60: {
            unsigned rn=(ins>>5)&31,total=(ins&(1u<<30))?16:8;unsigned char out[16]={0};
            for(unsigned i=0;i<total;i++)for(unsigned bit=0;bit<8;bit++)out[i]+=(cpu->simd[rn][i]>>bit)&1;
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 59: {
            unsigned rn=(ins>>5)&31;uint64_t value=le(cpu->simd[rn],8)+le(cpu->simd[rn]+8,8);
            memset(cpu->simd[rd],0,16);for(unsigned i=0;i<8;i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*i));
        } break;
        case 58: {
            unsigned bytes=1u<<((ins>>22)&3),rn=(ins>>5)&31,rm=(ins>>16)&31,part=((ins>>30)&1)*8;
            if(bytes==8)fail("Reserved UADDW vector shape");
            unsigned char out[16];
            for(unsigned i=0;i<8/bytes;i++) {
                uint64_t value=le(cpu->simd[rn]+i*bytes*2,bytes*2)+le(cpu->simd[rm]+part+i*bytes,bytes);
                for(unsigned j=0;j<2*bytes;j++)out[i*2*bytes+j]=(unsigned char)(value>>(8*j));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 57: {
            unsigned bytes=1u<<((ins>>22)&3),rn=(ins>>5)&31,rm=(ins>>16)&31,part=((ins>>30)&1)*8;
            if(bytes==8)fail("Reserved UMULL vector shape");
            unsigned char out[16];
            for(unsigned i=0;i<8/bytes;i++) {
                uint64_t value=le(cpu->simd[rn]+part+i*bytes,bytes)*le(cpu->simd[rm]+part+i*bytes,bytes);
                for(unsigned j=0;j<2*bytes;j++)out[i*2*bytes+j]=(unsigned char)(value>>(8*j));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 56: {
            unsigned cmode=(ins>>12)&15,bytes=cmode<8?4:2,total=(ins&(1u<<30))?16:8;
            unsigned shift=(cmode<8?(cmode>>1):((cmode>>1)&1))*8;
            uint32_t value=(((ins>>5)&31)|((ins>>11)&224))<<shift;
            for(unsigned i=0;i<total;i++) {
                unsigned char mask=(unsigned char)(value>>((i%bytes)*8));
                if(ins&(1u<<29))cpu->simd[rd][i]&=(unsigned char)~mask;else cpu->simd[rd][i]|=mask;
            }
            if(total==8)memset(cpu->simd[rd]+8,0,8);
        } break;
        case 55: {
            unsigned bytes=1u<<((ins>>22)&3),rn=(ins>>5)&31,rm=(ins>>16)&31,upper=(ins>>30)&1;
            if(bytes==8)fail("Reserved ADDHN shape");
            unsigned char out[16]={0};if(upper)memcpy(out,cpu->simd[rd],8);
            for(unsigned i=0;i<8/bytes;i++) {
                uint64_t value=(le(cpu->simd[rn]+i*bytes*2,bytes*2)+le(cpu->simd[rm]+i*bytes*2,bytes*2))>>(bytes*8);
                for(unsigned j=0;j<bytes;j++)out[upper*8+i*bytes+j]=(unsigned char)(value>>(j*8));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 54: {
            unsigned imm=(ins>>16)&127,bits=8,rn=(ins>>5)&31,upper=(ins>>30)&1;
            if(imm<8 || imm>=64)fail("Reserved SHRN shape");
            while(bits*2<=imm)bits*=2;
            unsigned bytes=bits/8,shift=bits*2-imm;unsigned char out[16]={0};
            if(upper)memcpy(out,cpu->simd[rd],8);
            for(unsigned i=0;i<8/bytes;i++) {
                uint64_t value=le(cpu->simd[rn]+i*bytes*2,bytes*2)>>shift;
                for(unsigned j=0;j<bytes;j++)out[upper*8+i*bytes+j]=(unsigned char)(value>>(j*8));
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 53: {
            unsigned rn=(ins>>5)&31,isload=(ins>>22)&1,lane=(ins>>30)&1;uint64_t addr=getreg(rn,1);
            if(rn==31 && (addr&15))fail("Unaligned SIMD stack access");
            unsigned char *p=lookup(addr,8,isload?4:2);if(!p)fail("SIMD lane access denied");
            if(isload)memcpy(cpu->simd[rd]+lane*8,p,8);else { cpu->exclusive_valid=0;memcpy(p,cpu->simd[rd]+lane*8,8); }
        } break;
        case 50: {
            unsigned bytes=1u<<((ins>>22)&3),total=(ins&(1u<<30))?16:8,rn=(ins>>5)&31;
            if(total/bytes<2)fail("Reserved CMEQ vector shape");
            unsigned char out[16]={0};for(unsigned i=0;i<total;i+=bytes)memset(out+i,le(cpu->simd[rn]+i,bytes)?0:255,bytes);
            memcpy(cpu->simd[rd],out,16);
        } break;
        case 51: {
            unsigned bytes=(ins&(1u<<22))?8:4;unsigned char out[16]={0};
            memcpy(out,cpu->simd[(ins>>5)&31],bytes);memcpy(cpu->simd[rd],out,16);
        } break;
        case 48: {
            unsigned rn=(ins>>5)&31,isload=(ins>>22)&1;uint64_t addr=getreg(rn,1);
            if(rn==31 && (addr&15))fail("Unaligned SIMD stack access");
            unsigned char *p=lookup(addr,16,isload?4:2);if(!p)fail("SIMD structure access denied");
            if(isload)memcpy(cpu->simd[rd],p,16);else { cpu->exclusive_valid=0;memcpy(p,cpu->simd[rd],16); }
            if(ins&(1u<<23))putreg(rn,addr+16,64,1);
        } break;
        case 49: {
            unsigned fraction=(ins&(1u<<22))?52:23,exponent=fraction==52?11:8;
            unsigned imm=(ins>>13)&255,b=(imm>>6)&1;
            uint64_t exp=((uint64_t)(b^1)<<(exponent-1))|((b?((UINT64_C(1)<<(exponent-3))-1):0)<<2)|((imm>>4)&3);
            uint64_t value=((uint64_t)(imm>>7)<<(fraction+exponent))|(exp<<fraction)|((uint64_t)(imm&15)<<(fraction-4));
            memset(cpu->simd[rd],0,16);for(unsigned i=0;i<(fraction==52?8u:4u);i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*i));
        } break;
        case 47: {
            unsigned bytes=(ins&(1u<<15))?2:4;
            uint32_t value=(((ins>>5)&31)|((ins>>11)&224))<<(((ins>>13)&(bytes-1))*8);
            unsigned total=(ins&(1u<<30))?16:8;memset(cpu->simd[rd],0,16);
            for(unsigned i=0;i<total;i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*(i%bytes)));
        } break;
        case 46: {
            unsigned size=1u<<(ins>>30),rn=(ins>>5)&31;uint64_t addr=getreg(rn,1);
            if(addr%size || (rn==31 && (addr&15)))fail("Unaligned acquire/release access");
            /* Every guest instruction is serialized on one host thread. No
               pending memory effects can cross an acquire/release operation. */
            if(ins&(1u<<22))putreg(rd,read_mem(addr,size),size==8?64:32,0);
            else store_le(addr,getreg(rd,0),size);
        } break;
        case 39: cpu->exclusive_valid=0;break;
        case 44: memset(cpu->simd[rd],0,16);memset(cpu->simd[rd],255,(ins&(1u<<30))?16:8);break;
        case 43: memset(cpu->simd[rd],0,16);memset(cpu->simd[rd],((ins>>5)&31)|((ins>>11)&224),(ins&(1u<<30))?16:8);break;
        case 42: if(((ins>>8)&3)==0)fail("Reserved barrier option");break; /* Serialized interpreter: no deferred memory effects. */
        case 40: {
            unsigned rn=(ins>>5)&31,bytes=(ins>>31)?8:4;
            if(ins&(1u<<16)) {
                uint64_t value=getreg(rn,0);memset(cpu->simd[rd],0,16);
                for(unsigned i=0;i<bytes;i++)cpu->simd[rd][i]=(unsigned char)(value>>(8*i));
            } else putreg(rd,le(cpu->simd[rn],bytes),bytes*8,0);
        } break;
        case 41: {
            unsigned bytes=1u<<((ins>>22)&3),total=(ins&(1u<<30))?16:8;
            unsigned rn=(ins>>5)&31,rm=(ins>>16)&31,lanes=total/bytes;
            unsigned char out[16]={0};uint32_t op=ins&0xbf20fc00u;
            if((ins&0xbfe0fc00u)==0x0e000000u) {
                for(unsigned i=0;i<total;i++) { unsigned index=cpu->simd[rm][i];out[i]=index<16?cpu->simd[rn][index]:0; }
            } else if((ins&0xbfe0fc00u)==0x0ea01c00u) {
                for(unsigned i=0;i<total;i++)out[i]=cpu->simd[rn][i]|cpu->simd[rm][i];
            } else {
                if(lanes<2)fail("Unsupported vector lane shape");
                for(unsigned i=0;i<lanes;i++) {
                    uint64_t a=le(cpu->simd[rn]+i*bytes,bytes),b=le(cpu->simd[rm]+i*bytes,bytes),v;
                    if(op==0x0e208400u)v=a+b;
                    else if(op==0x2e208400u)v=a-b;
                    else if(op==0x2e208c00u)v=a==b?UINT64_MAX:0;
                    else if(op==0x2e203c00u)v=a>=b?UINT64_MAX:0;
                    else if(op==0x2e203400u)v=a>b?UINT64_MAX:0;
                    else if(op==0x2e20a400u || op==0x2e20ac00u || op==0x0e20bc00u) {
                        if(bytes==8 && op!=0x0e20bc00u)fail("Unsupported vector UMAXP shape");
                        unsigned src=i<lanes/2?rn:rm,at=(i%(lanes/2))*2*bytes;
                        a=le(cpu->simd[src]+at,bytes);b=le(cpu->simd[src]+at+bytes,bytes);v=op==0x0e20bc00u?a+b:op==0x2e20ac00u?(a<b?a:b):(a>b?a:b);
                    } else if(op==0x0e001800u || op==0x0e005800u) {
                        unsigned source=i<lanes/2?rn:rm;
                        unsigned at=2*(i%(lanes/2))+(op==0x0e005800u);
                        v=le(cpu->simd[source]+at*bytes,bytes);
                    } else {
                        unsigned at=i/2+(op==0x0e007800u?lanes/2:0);
                        v=le(cpu->simd[(i&1)?rm:rn]+at*bytes,bytes);
                    }
                    for(unsigned j=0;j<bytes;j++)out[i*bytes+j]=(unsigned char)(v>>(8*j));
                }
            }
            memcpy(cpu->simd[rd],out,16);
        } break;
        default: { fprintf(stderr,"instruction=0x%08" PRIx32 " pc=0x%" PRIx64 "\n",ins,oldpc); fail("Instruction unsupported"); }
        }
        cpu->gpr[31]=0;
    }
    fail("Instruction budget exhausted"); return 125;
}
int main(int argc,char **argv) {
    const char *start_cwd=NULL;
    if(argc==2 && !strcmp(argv[1],"capabilities")) {
        fputs("{\"schema_version\":1,\"runtime_abi\":\"dockiva-native-v1\",\"backends\":{"
             "\"interpreter\":[\"elf64-arm64\",\"private-rootfs\",\"loopback-tcp\",\"published-ports\",\"bounded-dynamic-elf\"]"
             ,stdout);
#ifdef VMFREE_HVF
        /* Advertise native execution only if a VM can really be created here: not
           inside VMs or CI runners without nested virtualization, nor without the entitlement. */
        if(hv_vm_create(NULL)==HV_SUCCESS) {
            (void)hv_vm_destroy();
            fputs(",\"hvf\":[\"elf64-arm64\",\"private-rootfs\",\"loopback-tcp\",\"published-ports\",\"bounded-dynamic-elf\",\"native-threads\",\"bounded-private-fork\"]",stdout);
        }
#endif
        puts("}}");
        return 0;
    }
    atexit(cleanup);
    (void)umask(0); /* Creation modes are filtered by each guest task's umask. */
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR) fail("Cannot configure SIGPIPE");
    if(argc<3 || (strcmp(argv[1],"run") && strcmp(argv[1],"inspect"))) fail("Usage: dockiva-runtime run|inspect ELF [--trace] [--max-steps N] [--env KEY=VALUE] [--argv0 NAME] [-- ARGS...]");
    uint64_t steps=100000;
    int use_hvf=0,signals_forwarded=0;const char *root_path=NULL,*lower_path=NULL;
    static const char *guestargv[EXEC_ENTRIES], *guestenv[EXEC_ENTRIES]; guestargv[0]=argv[2]; int guestargc=1; unsigned envc=0;
    for(int i=3;i<argc;i++) {
        if(!strcmp(argv[i],"--")) {
            while(++i<argc) { if(guestargc==EXEC_ENTRIES) fail("Too many guest arguments"); guestargv[guestargc++]=argv[i]; }
            break;
        } else if(!strcmp(argv[i],"--cpu-backend") && i+1<argc) {
            const char *backend=argv[++i];
            if(!strcmp(backend,"hvf"))use_hvf=native_cpu=1;
            else if(strcmp(backend,"interpreter"))fail("Unknown CPU backend");
        } else if(!strcmp(argv[i],"--root") && i+1<argc) {
            if(fs_root>=0) fail("Root specified twice");
            root_path=argv[++i];fs_root=open(root_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fs_root<0) fail("Cannot open guest root");
        } else if(!strcmp(argv[i],"--lower") && i+1<argc) {
            /* --lower DIR: the image's files beneath the root, shared and never written (overlay.h). */
            if(fs_lower>=0) fail("Lower layer specified twice");
            lower_path=argv[++i];fs_lower=open(lower_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fs_lower<0) fail("Cannot open guest lower layer");
        } else if(!strcmp(argv[i],"--layers-gen") && i+1<argc) {
            /* --layers-gen FILE: the container's count of upper-layer changes, shared by its runs (overlay.h's lookup cache). */
            if(ov_cache_open(argv[++i])) fail("Cannot map the layers' change count");
        } else if(!strcmp(argv[i],"--writable")) {
            fs_writable=1;
        } else if((!strcmp(argv[i],"--bind") || !strcmp(argv[i],"--bind-ro")) && i+2<argc) {
            /* --bind HOST GUEST: the host directory or file appears at GUEST (absolute). */
            int readonly=!strcmp(argv[i],"--bind-ro");
            if(fs_add_mount(argv[i+1],argv[i+2],readonly)<0) { fprintf(stderr,"dockiva: --bind %s %s: %s\n",argv[i+1],argv[i+2],strerror(errno));fail("Invalid bind mount"); }
            i+=2;
        } else if(!strcmp(argv[i],"--tmpfs") && i+1<argc) {
            /* --tmpfs GUEST[:size=..,mode=..,uid=..,gid=..,noexec]: a memfs mount (memfs.h). */
            if(mem_add_mount(argv[++i])<0) { fprintf(stderr,"dockiva: --tmpfs %s: %s\n",argv[i],strerror(errno));fail("Invalid tmpfs mount"); }
        } else if(!strcmp(argv[i],"--argv0") && i+1<argc) {
            guestargv[0]=argv[++i];
        } else if(!strcmp(argv[i],"--env") && i+1<argc) {
            const char *v=argv[++i], *eq=strchr(v,'=');
            if(envc==EXEC_ENTRIES || !eq || eq==v) fail("Too many or invalid --env KEY=VALUE entries");
            guestenv[envc++]=v;
        } else if(!strcmp(argv[i],"--schedule")) schedule_enabled=1;
        else if(!strcmp(argv[i],"--dynamic")) { dynamic_enabled=1; }
        else if(!strcmp(argv[i],"--scheduled-network")) { net_scheduled=schedule_enabled=1; }
        else if(!strcmp(argv[i],"--publish") && i+1<argc) net_publish(argv[++i]);
        else if(!strcmp(argv[i],"--cwd") && i+1<argc) start_cwd=argv[++i];
        else if(!strcmp(argv[i],"--job-control")) { job_control_enabled=terminal_enabled=process_tree_enabled=schedule_enabled=1; }
        else if(!strcmp(argv[i],"--process-tree")) { process_tree_enabled=1;schedule_enabled=1; }
        else if(!strcmp(argv[i],"--forwarded-interrupts"))terminal_forwarded=1;
        else if(!strcmp(argv[i],"--interactive")) { terminal_enabled=1;schedule_enabled=1; }
        else if(!strcmp(argv[i],"--stdin")) stdin_host=1;
        else if(!strcmp(argv[i],"--trace")) trace=1;
        else if(!strcmp(argv[i],"--fsync") && i+1<argc) {                  /* barrier (default), full or fast */
            const char *m=argv[++i];fs_fsync_mode=!strcmp(m,"full")?1:!strcmp(m,"fast")?2:!strcmp(m,"barrier")?0:-1;
            if(fs_fsync_mode<0)fail("--fsync is barrier, full or fast");
        }
        else if(!strcmp(argv[i],"--outbound")) net_outbound=1;
        else if(!strcmp(argv[i],"--proc-publish") && i+1<argc) proc_publish_path=argv[++i]; /* this run's process table, for exec runs and docker top */
        else if(!strcmp(argv[i],"--abstract-dir") && i+1<argc) {      /* the container's abstract Unix socket names */
            const char *dir=argv[++i];if(dir[0]!='/' || strlen(dir)>=sizeof(net_abstract_dir))fail("Invalid --abstract-dir");
            strcpy(net_abstract_dir,dir);
        }
        else if(!strcmp(argv[i],"--proc-peer") && i+1<argc) proc_peer_path=argv[++i];       /* the main run's, shown in this run's /proc */
        else if(!strcmp(argv[i],"--oom-file") && i+1<argc) oom_file=argv[++i]; /* created on an OOM kill (docker inspect's OOMKilled) */
        else if(!strcmp(argv[i],"--hostname") && i+1<argc) {
            const char *name=argv[++i];size_t n=strlen(name);
            if(!n || n>64 || strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")!=n)fail("Invalid --hostname");
            memcpy(guest_hostname,name,n+1);
        }
        else if(!strcmp(argv[i],"--network") && i+1<argc) {
            const char *dir=argv[++i];struct stat st;
            if(strlen(dir)>=sizeof(net_dir) || dir[0]!='/' || stat(dir,&st) || !S_ISDIR(st.st_mode))fail("--network needs an existing directory");
            strcpy(net_dir,dir);
        }
        else if(!strcmp(argv[i],"--ip") && i+1<argc) {
            struct in_addr value;if(inet_pton(AF_INET,argv[++i],&value)!=1)fail("Invalid --ip");
            net_ip=ntohl(value.s_addr);
        }
        else if(!strcmp(argv[i],"--signals")) { job_control_enabled=process_tree_enabled=schedule_enabled=1;signals_forwarded=1; } /* handlers and docker-stop forwarding, without a terminal */
        else if((!strcmp(argv[i],"--file-limit-mib") || !strcmp(argv[i],"--storage-limit-mib")) && i+1<argc) {
            int storage=!strcmp(argv[i],"--storage-limit-mib");const char *s=argv[++i];char *end;errno=0;
            unsigned long long value=strtoull(s,&end,10);
            if(*s<'0' || *s>'9' || errno || *end || value>1048576 || (!storage && !value))fail("File limit must be 1..1048576 MiB; storage 0 (unlimited)..1048576 MiB");
            if(storage) { fs_storage_limit=value<<20;fs_write_limit=value?fs_write_limit:0; }
            else fs_file_limit=value<<20;
        }
        else if(!strcmp(argv[i],"--cpus") && i+1<argc) {           /* workers running tasks at once (native backend) */
            const char *s=argv[++i];char *end;errno=0;unsigned long value=strtoul(s,&end,10);
            if(*s<'0' || *s>'9' || errno || *end || !value || value>WORKER_LIMIT)fail("--cpus must be 1..64");
            worker_count=(unsigned)value;
        }
        else if(!strcmp(argv[i],"--no-decode-cache")) decode_cache_enabled=0;
        else if(!strcmp(argv[i],"--no-lookup-cache")) lookup_cache_enabled=0;
        else if((!strcmp(argv[i],"--elf-limit-mib") || !strcmp(argv[i],"--mmap-limit-mib")) && i+1<argc) {
            int mapping=!strcmp(argv[i],"--mmap-limit-mib");
            const char *s=argv[++i];char *end;errno=0;
            if(*s<'0' || *s>'9')fail("Invalid compatibility limit");
            unsigned long value=strtoul(s,&end,10);
            if(errno || *end || value<16 || value>(mapping?MAP_MAX>>20:1024u))fail("Compatibility limit outside allowed range");
            if(mapping)address_space->map_capacity=(uint64_t)value<<20;
            else elf_input_limit=(unsigned)value*1024*1024;
        }
        else if((!strcmp(argv[i],"--uid") || !strcmp(argv[i],"--gid")) && i+1<argc) {
            int group=!strcmp(argv[i],"--gid");const char *value=argv[++i];char *end;errno=0;
            if(*value<'0' || *value>'9')fail("Invalid guest identity");
            unsigned long long id=strtoull(value,&end,10);
            if(errno || *end || id>=UINT32_MAX)fail("Invalid guest identity");
            if(group)guest_ids.gid=guest_ids.rgid=guest_ids.sgid=(uint32_t)id;else guest_ids.uid=guest_ids.ruid=guest_ids.suid=(uint32_t)id;
        }
        else if(!strcmp(argv[i],"--memory-limit-mib") && i+1<argc) {
            const char *s=argv[++i];char *end;errno=0;
            if(*s<'0' || *s>'9')fail("Invalid memory budget");
            unsigned long value=strtoul(s,&end,10);
            if(errno || *end || !value || value>65536)fail("Memory budget must be 1..65536 MiB");
            resource_limit=(uint64_t)value*1024*1024;
        }
        else if(!strcmp(argv[i],"--profile")) profiling=1;
        else if(!strcmp(argv[i],"--max-steps") && i+1<argc) {
            const char *s=argv[++i]; char *end; errno=0;
            if(*s<'0'||*s>'9') fail("Invalid step budget");
            steps=strtoull(s,&end,10); if(errno || *end) fail("Invalid step budget");
        } else fail("Unknown option");
    }
    if(use_hvf) {
#ifndef VMFREE_HVF
        fail("Hypervisor backend is not compiled into this executable");
#else
        if(terminal_enabled)fail("Hypervisor backend does not yet support terminal options");
        /* --profile under the hypervisor counts system calls, exits, lock waits, fault-around
           and in-place futex waits (guest code runs natively); the fast paths stay on. */
        schedule_enabled=net_scheduled=1; /* each guest thread is a scheduler entity */
        map_rwx_allowed=1;
        fork_copy_pages=16; /* 256 KiB per backing: see address_space_fork_private */
        backing_reaping=1;
#endif
    }
    if(fs_lower>=0 && fs_root<0)fail("A lower layer needs a root");
    if(fs_root>=0 && fs_storage_limit && fs_scan_budget(fs_root,0)<0)fail("Guest root exceeds storage, entry or traversal budget");
    if(start_cwd && fs_start_cwd(start_cwd)<0)fail("Cannot enter guest working directory");
    if(terminal_forwarded && !terminal_enabled)fail("Forwarded interrupts require interactive mode");
    if(!net_dir[0]!=!net_ip || (net_ip && ((net_ip&255)<2 || (net_ip&255)==255)))fail("--network and --ip go together, with a host address (x.y.z.2..254)");
    if(profiling && signal(SIGUSR1,profile_signal)==SIG_ERR) fail("Cannot configure profiling signal");
#ifdef VMFREE_HVF
    if(use_hvf)vdso_build();
#endif
    FILE *input=fopen(argv[2],"rb");if(!input)fail("Cannot open ELF file");
    { /* As execve would: a set-ID file runs as its owner (root, see process_exec); a non-root run holds no capabilities */
        struct stat st;
        if(!fstat(fileno(input),&st)) { if(st.st_mode&S_ISUID)guest_ids.uid=guest_ids.suid=0;if(st.st_mode&S_ISGID)guest_ids.gid=guest_ids.sgid=0; }
        if(guest_uid)cap_held[0]=cap_held[1]=0;
    }
    load_stream(input);
    { /* /proc/1/exe: the ELF's path inside the root */
        char root_real[PATH_MAX],elf_real[PATH_MAX];size_t n;
        for(int layer=0;layer<2;layer++) {                                  /* the root, or its lower layer */
            const char *base=layer?lower_path:root_path;
            if(base && realpath(base,root_real) && realpath(argv[2],elf_real) && !strncmp(elf_real,root_real,n=strlen(root_real)) && elf_real[n]=='/' && strlen(elf_real+n)<sizeof(proc_exe[0])) { strcpy(proc_exe[1],elf_real+n);break; }
        }
    }
    if(!strcmp(argv[1],"inspect")) { printf("ARM64 ELF64; entry=0x%" PRIx64 "; segments=%u\n",cpu->pc,address_space->nseg); return 0; }
    heap_init();
    startup(guestargc,guestargv,envc,guestenv);
    process_steps=steps;
    if(terminal_enabled)terminal_init();
    else if(signals_forwarded)host_signals_install();
    if(stdin_host && !terminal_enabled) {
        int flags=fcntl(0,F_GETFL);
        if(flags<0 || fcntl(0,F_SETFL,flags|O_NONBLOCK)<0)fail("--stdin needs an open standard input");
        io_fds[0]=(GuestIO){IO_INPUT,0,0,NULL,NULL,NULL,NULL,NULL,NULL};
    }
#ifdef VMFREE_HVF
    if(use_hvf)return hvf_run(steps);
#endif
    return run(steps);
}
