/* First fit: the lowest run of pages free arena pages at or above from (the
   address space's map_free_hint), or total if none. Eight page flags are tested at
   once while inside mapped memory or a long free run. */
static unsigned map_find_free(const unsigned char *flags,unsigned from,unsigned total,unsigned pages) {
    unsigned run=0,start=0;
    for(unsigned i=from;i<total;) {
        if(!(i&7) && i+8<=total) {
            uint64_t w;memcpy(&w,flags+i,8);
            int has_zero=((w-UINT64_C(0x0101010101010101))&~w&UINT64_C(0x8080808080808080))!=0;
            if(!has_zero) { run=0;i+=8;continue; }
            if(!w && run+8<pages) { if(!run)start=i;run+=8;i+=8;continue; }
        }
        if(flags[i])run=0;else { if(!run)start=i;if(++run==pages)return start; }
        i++;
    }
    return total;
}
/* Shared memory: a segment over arena pages marked MAP_RESERVED. backing is the
   memory to share (System V shm) or NULL for fresh zeroed memory (MAP_SHARED|
   MAP_ANONYMOUS, charged in full now). fixed is a page address in the arena, or 0. */
static uint64_t shared_map(uint64_t fixed,uint64_t length,uint64_t prot,MemoryBacking *backing) {
    if(!length || length>address_space->map_capacity || prot>7 || (prot&6)==6)return linux_error(22);
    if(!address_space_map_flags(address_space))return linux_error(12);
    if(address_space->nseg>=128)return linux_error(12);
    unsigned pages=(unsigned)(page_up(length)/4096),start=0;
    if(fixed) {
        if(fixed%4096 || fixed<MAP_BASE || fixed-MAP_BASE>=address_space->map_capacity || page_up(length)>address_space->map_capacity-(fixed-MAP_BASE))return linux_error(22);
        start=(unsigned)((fixed-MAP_BASE)/4096);
        for(unsigned i=start;i<start+pages;i++)if(address_space->map_flags[i])return linux_error(22);  /* no replacing existing mappings */
    } else {
        start=map_find_free(address_space->map_flags,address_space->map_free_hint,address_space->map_capacity/4096,pages);
        if(start==address_space->map_capacity/4096)return linux_error(12);
        if(start==address_space->map_free_hint)address_space->map_free_hint=start+pages;
    }
    if(backing) { if(!backing_retain(backing,1))return linux_error(12); }
    else if(!(backing=backing_alloc((size_t)pages*4096,1)))return linux_error(12);
    unsigned flags=SEG_SHARED|((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0);
    address_space->seg[address_space->nseg++]=(Segment){MAP_BASE+(uint64_t)start*4096,(uint64_t)pages*4096,flags,backing,0};
    for(unsigned i=start;i<start+pages;i++)address_space->map_flags[i]=MAP_RESERVED;
    address_space_changed(address_space);return MAP_BASE+(uint64_t)start*4096;
}
/* Remove the shared segments inside [address,address+bytes); a partly covered one is EINVAL. */
static uint64_t shared_unmap(uint64_t address,uint64_t bytes) {
    for(unsigned i=0;i<address_space->nseg;) {
        Segment *s=&address_space->seg[i];
        if(!(s->flags&SEG_SHARED) || s->base+s->size<=address || s->base>=address+bytes) { i++;continue; }
        if(s->base<address || s->base+s->size>address+bytes)return linux_error(22);
        unsigned first=(unsigned)((s->base-MAP_BASE)/4096),pages=(unsigned)(s->size/4096);
        for(unsigned p=first;p<first+pages;p++)address_space->map_flags[p]=0;
        if(first<address_space->map_free_hint)address_space->map_free_hint=first;
        backing_release(s->backing);address_space->seg[i]=address_space->seg[--address_space->nseg];
        address_space_changed(address_space);
    }
    return 0;
}
/* Private file-backed guest mappings. Bytes remain ordinary non-executable host
   allocations; MAP_FIXED is confined to the bounded guest mmap arena. */
static int dynamic_enabled;
static uint64_t guest_dynamic_mmap(void) {
    uint64_t addr=cpu->gpr[0],length=cpu->gpr[1],prot=cpu->gpr[2],flags=cpu->gpr[3],offset=cpu->gpr[5];
    if(!length || length>address_space->map_capacity || offset%4096 || offset>INT64_MAX-length || prot>7 || (prot&6)==6)return linux_error(22);
    if(!address_space_map_flags(address_space))return linux_error(12);
    /* Linux ignores MAP_DENYWRITE, MAP_EXECUTABLE and MAP_STACK. Stack guards
       still use ordinary PROT_NONE/mprotect; this does not create a thread. */
    flags&=~UINT64_C(0x2d800);                       /* also MAP_NORESERVE and MAP_POPULATE: hints */
    if((flags&3) && (flags&3)!=2 && (flags&32) && !(flags&~UINT64_C(0x33)))   /* MAP_SHARED|MAP_ANONYMOUS */
        return shared_map((flags&16)?addr:0,length,prot,NULL);
    /* A read-only MAP_SHARED file mapping (ICU data, for instance) can never write
       through, so a private copy is equivalent. */
    if((flags&3) && (flags&3)!=2 && !(flags&32) && !(prot&2))flags=(flags&~UINT64_C(3))|2;
    if((flags&~UINT64_C(0x32)) || (flags&3)!=2)return linux_error(95);
    int fd=-1;
    if(!(flags&32)) { fd=fs_host(cpu->gpr[4]);if(fd<0)return linux_error(9);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISREG(st.st_mode))return linux_error(19);
    }
    unsigned pages=(unsigned)(page_up(length)/4096),start=0;
    if(flags&16) {
        if(addr%4096 || addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || page_up(length)>address_space->map_capacity-(addr-MAP_BASE))return linux_error(22);
        start=(unsigned)((addr-MAP_BASE)/4096);
    } else {
        start=map_find_free(address_space->map_flags,address_space->map_free_hint,address_space->map_capacity/4096,pages);
        if(start==address_space->map_capacity/4096)return linux_error(12);
        if(start==address_space->map_free_hint)address_space->map_free_hint=start+pages;
    }
    /* Read into a temporary buffer first, so a failed fixed mapping preserves
       both old contents and protections. */
    unsigned char *bytes=NULL;
    if(fd>=0) {
        if(!(bytes=calloc(pages,4096)))return linux_error(12);
        size_t done=0;
        while(done<length) {
            ssize_t n=pread(fd,bytes+done,(size_t)(length-done),(off_t)(offset+done));
            if(n<0 && errno==EINTR)continue;
            if(n<0) { uint64_t e=fs_error();free(bytes);return e; }
            if(!n)break;done+=(size_t)n;
        }
    }
    if(!backing_data(address_space->map_backing)) { address_space->map_backing=backing_reserve(address_space->map_capacity);if(!backing_data(address_space->map_backing)) { free(bytes);return linux_error(12); } }
    if(bytes) { /* file contents are written (and charged) now */
        if(!address_space_make_private(address_space,address_space->map_backing,(size_t)start*4096,(size_t)pages*4096)) { free(bytes);return linux_error(12); }
        memcpy(backing_data(address_space->map_backing)+(size_t)start*4096,bytes,(size_t)pages*4096);free(bytes);
    } else if(!address_space_discard_map(address_space,(size_t)start*4096,(size_t)pages*4096))return linux_error(12); /* anonymous: zero, charged on first write */
    unsigned char permission=(unsigned char)((fd>=0?16:0)|8|((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0));
    for(unsigned i=start;i<start+pages;i++)address_space->map_flags[i]=permission;
    address_space_arena_changed(address_space,start,start+pages);return MAP_BASE+(uint64_t)start*4096;
}
static uint64_t guest_mprotect(void) {
    uint64_t addr=cpu->gpr[0],length=cpu->gpr[1],prot=cpu->gpr[2];
    if(addr%4096 || length>UINT64_MAX-4095 || prot>7 || (prot&6)==6)return linux_error(22);
    if(!length)return 0;
    uint64_t bytes=page_up(length);
    if(addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || bytes>address_space->map_capacity-(addr-MAP_BASE)) {
        if(bytes>UINT64_MAX-addr)return linux_error(22);
        for(unsigned i=0;i<address_space->nseg;i++) {
            Segment old=address_space->seg[i];uint64_t end=addr+bytes,oldend=old.base+old.size;
            if(addr<(old.base&~UINT64_C(4095)) || end>oldend || end<=old.base || addr>=oldend)continue;
            uint64_t begin=addr>old.base?addr:old.base;
            unsigned extra=(begin>old.base)+(end<oldend);if(address_space->nseg+extra>128)return linux_error(12);
            /* Split mapping views over the same bytes. Acquire every new
               reference before publishing metadata; no segment data is copied. */
            if(!backing_retain(old.backing,extra))return linux_error(12);
            unsigned permissions=((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0);
            address_space->seg[i]=(Segment){begin,end-begin,permissions,old.backing,old.offset+(size_t)(begin-old.base)};
            if(begin>old.base)address_space->seg[address_space->nseg++]=(Segment){old.base,begin-old.base,old.flags,old.backing,old.offset};
            if(end<oldend)address_space->seg[address_space->nseg++]=(Segment){end,oldend-end,old.flags,old.backing,old.offset+(size_t)(end-old.base)};
            address_space_changed(address_space);return 0;
        }
        return linux_error(95);
    }
    unsigned first=(unsigned)((addr-MAP_BASE)/4096),pages=(unsigned)(bytes/4096);
    if(!address_space->map_flags)return linux_error(12);
    for(unsigned i=0;i<address_space->nseg;i++) {                         /* a whole shared segment */
        Segment *s=&address_space->seg[i];
        if(!(s->flags&SEG_SHARED) || s->base+s->size<=addr || s->base>=addr+bytes)continue;
        if(s->base!=addr || s->size!=bytes)return linux_error(22);
        s->flags=SEG_SHARED|((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0);address_space_changed(address_space);return 0;
    }
    for(unsigned i=first;i<first+pages;i++)if(!address_space->map_flags[i])return linux_error(12);
    unsigned char permission=(unsigned char)(8|((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0));
    for(unsigned i=first;i<first+pages;i++)address_space->map_flags[i]=(unsigned char)((address_space->map_flags[i]&16)|permission);
    address_space_arena_changed(address_space,first,first+pages);return 0;
}

/* Linux madvise advice values are not host ABI values. The three access-pattern
   hints are accepted on mapped guest ranges; this runtime has no read-ahead
   policy to change. MADV_DONTNEED releases anonymous mmap-arena pages. Other reclamation/fork advice reports Linux EINVAL, never success. No
   guest address is passed to host madvise. */
static uint64_t guest_madvise(void) {
    uint64_t addr=cpu->gpr[0],length=cpu->gpr[1];int32_t advice=(int32_t)cpu->gpr[2];
    if(advice==4) {
        /* MADV_DONTNEED on anonymous mmap-arena pages: zero them and give the memory
           back. Other memory, including file-backed pages, keeps EINVAL. MADV_FREE
           (8) stays EINVAL like a pre-4.5 kernel: Redis then skips its arm64
           MADV_FREE fork-bug probe (which reads /proc/self/smaps), and jemalloc
           purges with MADV_DONTNEED instead. */
        if((addr&4095) || length>UINT64_MAX-4095)return linux_error(22);
        uint64_t bytes=page_up(length);if(!bytes)return 0;
        if(!address_space->map_flags || addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || bytes>address_space->map_capacity-(addr-MAP_BASE))return linux_error(22);
        unsigned first=(unsigned)((addr-MAP_BASE)/4096),pages=(unsigned)(bytes/4096);
        for(unsigned i=first;i<first+pages;i++) {
            if(!address_space->map_flags[i])return linux_error(12);
            if(address_space->map_flags[i]&16)return linux_error(22);
        }
        if(!address_space_discard_map(address_space,(size_t)first*4096,(size_t)pages*4096))return linux_error(12);
        address_space_arena_changed(address_space,first,first+pages);return 0; /* released pages are write-protected again */
    }
    if(advice<0 || advice>2 || (addr&4095) || length>UINT64_MAX-4095)return linux_error(22);
    uint64_t bytes=page_up(length);
    if(bytes>UINT64_MAX-addr)return linux_error(22);
    uint64_t end=addr+bytes,cursor=addr;
    while(cursor<end) {
        uint64_t next=cursor;
        for(unsigned i=0;i<address_space->nseg;i++) {
            uint64_t low=address_space->seg[i].base&~UINT64_C(4095),high=page_up(address_space->seg[i].base+address_space->seg[i].size);
            if(cursor>=low && cursor<high) { next=high;break; }
        }
        if(next==cursor && cursor>=address_space->brk_base && cursor-address_space->brk_base<address_space->brk_pages)next=address_space->brk_base+address_space->brk_pages;
        if(next==cursor && cursor>=MAP_BASE && cursor-MAP_BASE<address_space->map_capacity) {
            unsigned page=(unsigned)((cursor-MAP_BASE)/4096);
            if(address_space->map_flags[page])next=cursor+4096; /* PROT_NONE still denotes mapped memory. */
        }
        if(next==cursor)return linux_error(12);
        cursor=next;
    }
    return 0;
}

/* System V shared memory, shared by every process of the container. Each table
   entry holds one reference to its memory; attachments hold the others, so the
   attach count is the backing's references minus one. Removed (IPC_RMID) entries
   are freed once nothing is attached. */
#define SHM_SLOTS 64
typedef struct { int32_t key;unsigned used,removed,mode;uint64_t size,cpid,lpid,atime,dtime,ctime;MemoryBacking *backing; } ShmSegment;
static ShmSegment shm_segments[SHM_SLOTS];
static uint64_t shm_now(void) { return (uint64_t)time(NULL); }
static void shm_reclaim(void) {
    for(unsigned i=0;i<SHM_SLOTS;i++)if(shm_segments[i].used && shm_segments[i].removed && shm_segments[i].backing->refs==1) {
        backing_release(shm_segments[i].backing);memset(&shm_segments[i],0,sizeof(shm_segments[i]));
    }
}
static uint64_t shm_get(void) {
    int32_t key=(int32_t)cpu->gpr[0];uint64_t size=cpu->gpr[1],flags=cpu->gpr[2];
    shm_reclaim();
    if(key) for(unsigned i=0;i<SHM_SLOTS;i++)if(shm_segments[i].used && !shm_segments[i].removed && shm_segments[i].key==key) {
        if((flags&01000) && (flags&02000))return linux_error(17);                  /* IPC_CREAT|IPC_EXCL: EEXIST */
        if(size>shm_segments[i].size)return linux_error(22);
        return i;
    }
    if(key && !(flags&01000))return linux_error(2);
    if(!size || size>(uint64_t)address_space->map_capacity)return linux_error(22);
    unsigned i=0;while(i<SHM_SLOTS && shm_segments[i].used)i++;
    if(i==SHM_SLOTS)return linux_error(28);
    MemoryBacking *backing=backing_alloc((size_t)page_up(size),1);if(!backing)return linux_error(12);
    shm_segments[i]=(ShmSegment){key,1,0,(unsigned)(flags&0777),size,process_pid,0,0,0,shm_now(),backing};
    return i;
}
static uint64_t shm_attach(void) {
    uint64_t id=cpu->gpr[0],address=cpu->gpr[1],flags=cpu->gpr[2];
    if(id>=SHM_SLOTS || !shm_segments[id].used)return linux_error(22);
    if(shm_segments[id].removed)return linux_error(43);                         /* EIDRM */
    if(flags&~(UINT64_C(010000)|020000|040000))return linux_error(22);
    if(address && (flags&020000))address&=~UINT64_C(4095);                     /* SHM_RND */
    uint64_t result=shared_map(address,shm_segments[id].size,(flags&010000)?1:3,shm_segments[id].backing);
    if((int64_t)result>=0) { shm_segments[id].lpid=process_pid;shm_segments[id].atime=shm_now(); }
    return result;
}
static uint64_t shm_detach(void) {
    uint64_t address=cpu->gpr[0];
    for(unsigned i=0;i<address_space->nseg;i++) {
        Segment *s=&address_space->seg[i];
        if(s->base!=address || !(s->flags&SEG_SHARED))continue;
        for(unsigned j=0;j<SHM_SLOTS;j++)if(shm_segments[j].used && shm_segments[j].backing==s->backing) {
            shm_segments[j].lpid=process_pid;shm_segments[j].dtime=shm_now();
            uint64_t result=shared_unmap(s->base,s->size);shm_reclaim();return result;
        }
    }
    return linux_error(22);
}
static uint64_t shm_control(void) {
    uint64_t id=cpu->gpr[0],command=cpu->gpr[1]&~UINT64_C(0x100);              /* IPC_64 */
    if(id>=SHM_SLOTS || !shm_segments[id].used)return linux_error(22);
    if(command==0) { shm_segments[id].removed=1;shm_segments[id].key=0;shm_reclaim();return 0; }   /* IPC_RMID */
    if(command==1 || command==11 || command==12)return 0;                       /* IPC_SET, SHM_LOCK, SHM_UNLOCK */
    if(command!=2 && command!=13)return linux_error(22);                        /* IPC_STAT, SHM_STAT */
    unsigned char *out=lookup(cpu->gpr[2],112,2);if(!out)return linux_error(14);
    memset(out,0,112);                                                          /* struct shmid64_ds */
    fs_pack(out,0,(uint32_t)shm_segments[id].key,4);fs_pack(out,4,guest_uid,4);fs_pack(out,8,guest_gid,4);
    fs_pack(out,12,guest_uid,4);fs_pack(out,16,guest_gid,4);fs_pack(out,20,shm_segments[id].mode|(shm_segments[id].removed?01000:0),4);
    fs_pack(out,48,shm_segments[id].size,8);fs_pack(out,56,shm_segments[id].atime,8);fs_pack(out,64,shm_segments[id].dtime,8);
    fs_pack(out,72,shm_segments[id].ctime,8);fs_pack(out,80,shm_segments[id].cpid,4);fs_pack(out,84,shm_segments[id].lpid,4);
    fs_pack(out,88,shm_segments[id].backing->refs-1,8);                        /* shm_nattch */
    return command==13?id:0;
}
