/* Writable and executable pages (JITs: V8, the JVM): the hypervisor backend runs
   guest code directly, so they work there; the interpreter's decode cache would go
   stale, so it keeps refusing them. */
static int map_rwx_allowed;
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
/* First fit for pages arena pages, or the arena size if none. A run that long found
   at start means no such run lies below it, so the next search for as many or more
   starts there (minus the pages of a run that may straddle it), not at the hint. */
/* An mmap address hint (no MAP_FIXED), as Linux takes it: used when the whole range
   lies in the arena and is free; else the arena size (search instead). */
static unsigned map_hint(GuestAddressSpace *space,uint64_t addr,unsigned pages) {
    unsigned total=(unsigned)(space->map_capacity/4096);
    if(!addr || addr%4096 || addr<MAP_BASE || addr-MAP_BASE>=space->map_capacity || (uint64_t)pages*4096>space->map_capacity-(addr-MAP_BASE))return total;
    unsigned start=(unsigned)((addr-MAP_BASE)/4096);
    for(unsigned i=start;i<start+pages;i++)if(space->map_flags[i])return total;
    return start;
}
static unsigned map_find(GuestAddressSpace *space,unsigned pages) {
    unsigned total=space->map_capacity/4096,from=space->map_free_hint;
    /* A reservation of 1 GiB or more starts on a 4 GiB boundary when one is free: a
       JVM that finds none of its preferred class space addresses takes what it gets,
       and its compressed class base must be 4 GiB aligned on arm64 (JDK 22). */
    if(pages>=(1u<<18))
        for(uint64_t at=(MAP_BASE+UINT64_C(0xffffffff))&~UINT64_C(0xffffffff);at-MAP_BASE<space->map_capacity;at+=UINT64_C(1)<<32) {
            unsigned start=map_hint(space,at,pages);if(start!=total)return start;
        }
    if(space->map_large_pages && pages>=space->map_large_pages && space->map_large_from>from)
        from=space->map_large_from-(space->map_large_from-from<space->map_large_pages-1?space->map_large_from-from:space->map_large_pages-1);
    unsigned start=map_find_free(space->map_flags,from,total,pages);
    if(start!=total) { space->map_large_from=start;space->map_large_pages=pages; }
    return start;
}
static void file_share_unmapped(MemoryBacking *backing); /* below */
/* Turns the uncharged file copies overlapping [address, address+bytes) that it does
   not wholly cover (all of them, with partial 0) into ordinary private arena pages,
   charged now: ld.so maps a library's whole span, then maps its segments over it
   (MAP_FIXED) and protects the gaps, which needs ordinary pages. 0 if out of memory. */
static int file_copy_demote(uint64_t address,uint64_t bytes,int partial) {
    for(unsigned i=0;i<address_space->nseg;i++) {
        Segment s=address_space->seg[i];
        if(!(s.flags&SEG_FILE_COPY) || s.base>=address+bytes || s.base+s.size<=address)continue;
        if(partial && s.base>=address && s.base+s.size<=address+bytes)continue;
        if(!backing_data(address_space->map_backing)) { address_space->map_backing=backing_reserve(address_space->map_capacity);if(!backing_data(address_space->map_backing))return 0; }
        size_t offset=(size_t)(s.base-MAP_BASE);unsigned first=(unsigned)(offset/4096),pages=(unsigned)(s.size/4096);
        if(!address_space_make_private(address_space,address_space->map_backing,offset,(size_t)s.size))return 0;
        memcpy(backing_data(address_space->map_backing)+offset,segment_data(&s),(size_t)s.size);
        address_space->seg[i]=address_space->seg[--address_space->nseg];backing_release(s.backing);
        for(unsigned p=first;p<first+pages;p++)address_space->map_flags[p]=(unsigned char)(8|16|(s.flags&7));
        address_space_arena_changed(address_space,first,first+pages);address_space_changed(address_space);
        i=(unsigned)-1;                                                  /* the table changed: start over */
    }
    return 1;
}
/* Shared memory: a segment over arena pages marked MAP_RESERVED. backing is the
   memory to share (System V shm) or NULL for fresh zeroed memory (MAP_SHARED|
   MAP_ANONYMOUS, charged in full now). fixed is a page address in the arena, or 0. */
static uint64_t shared_map(uint64_t fixed,uint64_t length,uint64_t prot,MemoryBacking *backing,size_t offset) {
    uint64_t hint=0;
    if(length>address_space->map_capacity)return linux_error(12); /* more than the arena holds: ENOMEM, as Linux */
    if(!length || prot>7 || ((prot&6)==6 && !map_rwx_allowed))return linux_error(22);
    if(!address_space_map_flags(address_space))return linux_error(12);
    if(address_space->nseg>=SEGMENT_LIMIT)return linux_error(12);
    unsigned pages=(unsigned)(page_up(length)/4096),start=0;
    if(fixed) {
        if(fixed%4096 || fixed<MAP_BASE || fixed-MAP_BASE>=address_space->map_capacity || page_up(length)>address_space->map_capacity-(fixed-MAP_BASE))return linux_error(22);
        start=(unsigned)((fixed-MAP_BASE)/4096);
        for(unsigned i=start;i<start+pages;i++)if(address_space->map_flags[i])return linux_error(22);  /* no replacing existing mappings */
    } else {
        start=map_hint(address_space,hint,pages);if(start==address_space->map_capacity/4096)start=map_find(address_space,pages);
        if(start==address_space->map_capacity/4096)return linux_error(12);
        if(start==address_space->map_free_hint)address_space->map_free_hint=start+pages;
    }
    if(backing) { if(!backing_retain(backing,1))return linux_error(12); }
    else if(!(backing=backing_alloc((size_t)pages*4096,1)))return linux_error(12);
    unsigned flags=SEG_SHARED|((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0);
    address_space->seg[address_space->nseg++]=(Segment){MAP_BASE+(uint64_t)start*4096,(uint64_t)pages*4096,flags,backing,offset};
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
        map_freed(address_space,first);
        MemoryBacking *backing=s->backing;
        address_space->seg[i]=address_space->seg[--address_space->nseg];
        file_share_unmapped(backing);backing_release(backing);
        address_space_changed(address_space);
    }
    return 0;
}
/* Writable MAP_SHARED file mappings (POSIX semaphores in /dev/shm, SQLite's WAL
   index). Every process of the container lives in this runtime, so all mappings of
   one file share one backing, filled from the file and written back to it on msync
   and when the last mapping goes (if it was ever writable); keyed by the file's
   device and inode. Writes to the file update the backing (file_share_written).
   ponytail: read() of the file does not see unflushed mapped changes (Linux's page
   cache unifies them); flush before reads if that ever matters. */
#define FILE_SHARES 1024 /* .NET maps each assembly it loads MAP_SHARED */
static struct FileShare { dev_t dev;ino_t ino;int fd;MemoryBacking *backing;size_t size;int writable; } file_shares[FILE_SHARES];
/* Guards the table against file writes, which also run without the runtime lock. */
static pthread_mutex_t file_share_lock=PTHREAD_MUTEX_INITIALIZER;
static void file_share_written(int fd,uint64_t at,const unsigned char *data,size_t bytes) {
    struct stat st;if(fstat(fd,&st)<0)return;
    pthread_mutex_lock(&file_share_lock);
    for(unsigned i=0;i<FILE_SHARES;i++) {
        struct FileShare *f=&file_shares[i];
        if(!f->backing || f->dev!=st.st_dev || f->ino!=st.st_ino || at>=f->size)continue;
        memcpy(backing_data(f->backing)+at,data,bytes<f->size-at?bytes:(size_t)(f->size-at));
    }
    pthread_mutex_unlock(&file_share_lock);
}
static void file_share_forget(struct FileShare *f) {
    pthread_mutex_lock(&file_share_lock);
    close(f->fd);backing_release(f->backing);memset(f,0,sizeof(*f));
    pthread_mutex_unlock(&file_share_lock);
    __atomic_fetch_sub(&file_shares_live,1,__ATOMIC_RELAXED);
}
static void file_share_flush(struct FileShare *f) {
    if(!f->writable)return;                             /* never written through a mapping */
    struct stat st;if(fstat(f->fd,&st)<0)return;
    size_t bytes=(size_t)st.st_size<f->size?(size_t)st.st_size:f->size;
    for(size_t done=0;done<bytes;) {
        ssize_t n=pwrite(f->fd,backing_data(f->backing)+done,bytes-done,(off_t)done);
        if(n<=0)break;done+=(size_t)n;
    }
}
/* A shared segment went away: when only the table still holds the backing, flush and forget. */
static void file_share_unmapped(MemoryBacking *backing) {
    for(unsigned i=0;i<FILE_SHARES;i++)if(file_shares[i].backing==backing && backing->refs==2) {
        file_share_flush(&file_shares[i]);file_share_forget(&file_shares[i]);return;
    }
}
static int file_share_find(uint64_t guestfd) {
    int fd=fs_host(guestfd);struct stat st;if(fd<0 || fstat(fd,&st)<0)return 0;
    for(unsigned i=0;i<FILE_SHARES;i++)if(file_shares[i].backing && file_shares[i].dev==st.st_dev && file_shares[i].ino==st.st_ino)return 1;
    return 0;
}
static uint64_t file_share_map(uint64_t fixed,uint64_t length,uint64_t prot,uint64_t guestfd,uint64_t offset) {
    int fd=fs_host(guestfd);if(fd<0)return linux_error(9);
    if((prot&2) && fs_ro[fs_slot(guestfd)])return linux_error(13);
    struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISREG(st.st_mode))return linux_error(19);
    size_t end=(size_t)page_up(offset+length);
    struct FileShare *f=NULL,*free_entry=NULL;
    for(unsigned i=0;i<FILE_SHARES;i++) {
        if(file_shares[i].backing && file_shares[i].dev==st.st_dev && file_shares[i].ino==st.st_ino)f=&file_shares[i];
        else if(!file_shares[i].backing && !free_entry)free_entry=&file_shares[i];
    }
    if(f && end>f->size) {
        if(f->backing->refs>1)return linux_error(12); /* mapped beyond what others share */
        file_share_flush(f);file_share_forget(f);free_entry=f;f=NULL;
    }
    if(!f) {
        if(!free_entry)return linux_error(23);
        size_t size=end>(size_t)page_up((uint64_t)st.st_size)?end:(size_t)page_up((uint64_t)st.st_size);
        MemoryBacking *backing=backing_alloc(size,1);if(!backing)return linux_error(12);
        for(size_t done=0;done<(size_t)st.st_size && done<size;) {
            ssize_t n=pread(fd,backing_data(backing)+done,size-done,(off_t)done);
            if(n<=0)break;done+=(size_t)n;
        }
        int keep=dup(fd);if(keep<0) { backing_release(backing);return fs_error(); }
        fcntl(keep,F_SETFD,FD_CLOEXEC);
        pthread_mutex_lock(&file_share_lock);
        f=free_entry;*f=(struct FileShare){st.st_dev,st.st_ino,keep,backing,size,0};
        pthread_mutex_unlock(&file_share_lock);
        __atomic_fetch_add(&file_shares_live,1,__ATOMIC_RELAXED);
    }
    if(prot&2)f->writable=1;
    uint64_t result=shared_map(fixed,length,prot,f->backing,(size_t)offset);
    if((int64_t)result<0 && f->backing->refs==1)file_share_forget(f);
    return result;
}
/* msync: write mapped files back (all of them; cheap at this scale). */
static uint64_t guest_msync(void) {
    if(cpu->gpr[0]%4096 || cpu->gpr[2]&~UINT64_C(7))return linux_error(22);
    for(unsigned i=0;i<FILE_SHARES;i++)if(file_shares[i].backing)file_share_flush(&file_shares[i]);
    return 0;
}
/* Reservations beyond the arena: an anonymous private PROT_NONE mapping the arena
   cannot hold (MariaDB reserves 128 GiB for its buffer pool's maximum, a JVM its heap)
   is address space alone, unmapped, from 16 TiB up. A MAP_FIXED anonymous mapping
   inside one commits that range (reserve_commit); munmap gives both back. */
#define RESERVE_BASE UINT64_C(0x100000000000)
#define RESERVE_END UINT64_C(0x700000000000)
static int reserve_find(uint64_t address,uint64_t bytes) {
    for(unsigned i=0;i<address_space->nreserve;i++)
        if(address>=address_space->reserve_base[i] && bytes<=address_space->reserve_size[i]-(address-address_space->reserve_base[i]))return (int)i;
    return -1;
}
static uint64_t reserve_new(uint64_t length) {
    uint64_t size=page_up(length),base=address_space->reserve_next?address_space->reserve_next:RESERVE_BASE;
    if(address_space->nreserve==8 || size>RESERVE_END-base)return linux_error(12);
    address_space->reserve_base[address_space->nreserve]=base;address_space->reserve_size[address_space->nreserve++]=size;
    address_space->reserve_next=base+size+(UINT64_C(1)<<30);                     /* a guard gap */
    return base;
}
/* Take segments, or their parts, out of [low, high): commits inside a reservation. */
static uint64_t segments_remove(uint64_t low,uint64_t high) {
    for(unsigned i=0;i<address_space->nseg;) {
        Segment *s=&address_space->seg[i];uint64_t begin=s->base,end=s->base+s->size;
        if(end<=low || begin>=high) { i++;continue; }
        if(low<=begin && high>=end) { backing_release(s->backing);address_space->seg[i]=address_space->seg[--address_space->nseg];continue; }
        if(low>begin && high<end) {                                                  /* split around the hole */
            if(address_space->nseg>=SEGMENT_LIMIT || !backing_retain(s->backing,1))return linux_error(12);
            address_space->seg[address_space->nseg++]=(Segment){high,end-high,s->flags,s->backing,s->offset+(size_t)(high-begin)};
            s->size=low-begin;
        } else if(low<=begin) { s->offset+=(size_t)(high-begin);s->size=end-high;s->base=high; }
        else s->size=low-begin;
        i++;
    }
    address_space_changed(address_space);return 0;
}
/* Each 1 GiB window of the reservation space has one lazily charged backing, found
   through any segment that maps it (its base minus its offset is the window). A
   commit zeroes its range there, maps it, and merges with neighbouring segments of
   the same backing, so a heap committed region by region stays a few segments. */
#define RESERVE_WINDOW (UINT64_C(1)<<30)
static MemoryBacking *reserve_window(uint64_t window) {
    for(unsigned i=0;i<address_space->nseg;i++) {
        Segment *s=&address_space->seg[i];
        if(s->base>=window && s->base-window<RESERVE_WINDOW && s->base-s->offset==window && s->backing && s->backing->size==RESERVE_WINDOW)return s->backing;
    }
    return NULL;
}
/* Join adjacent segments over consecutive bytes of one backing with the same protection. */
static void segments_merge(void) {
    for(int merged=1;merged;) {
        merged=0;
        for(unsigned i=0;i<address_space->nseg && !merged;i++)for(unsigned j=0;j<address_space->nseg && !merged;j++) {
            Segment *a=&address_space->seg[i],*b=&address_space->seg[j];
            if(i==j || a->backing!=b->backing || a->flags!=b->flags || a->base+a->size!=b->base || a->offset+a->size!=b->offset)continue;
            a->size+=b->size;backing_release(b->backing);address_space->seg[j]=address_space->seg[--address_space->nseg];merged=1;
        }
    }
    address_space_changed(address_space);
}
static uint64_t reserve_commit_piece(uint64_t address,uint64_t size,unsigned flags) {
    uint64_t window=address&~(RESERVE_WINDOW-1);size_t offset=(size_t)(address-window);
    MemoryBacking *backing=reserve_window(window);
    if(backing) {                                                                    /* fresh pages read zero, as on Linux */
        if(!address_space_discard(address_space,backing,offset,(size_t)size))return linux_error(12);
        backing=reserve_window(window);                                              /* a copy, if it was shared after fork */
        if(!backing || !backing_retain(backing,1))return linux_error(12);
    } else if(!(backing=backing_reserve((size_t)RESERVE_WINDOW)))return linux_error(12);
    uint64_t e=segments_remove(address,address+size);
    if(e || !flags) { backing_release(backing);return e; }
    for(unsigned i=0;i<address_space->nseg;i++) {                                    /* grow a neighbour instead */
        Segment *s=&address_space->seg[i];
        if(s->backing!=backing || s->flags!=flags)continue;
        if(s->base+s->size==address && s->offset+s->size==offset) { s->size+=size;backing_release(backing);backing=NULL;break; }
        if(address+size==s->base && offset+size==s->offset) { s->base=address;s->offset=offset;s->size+=size;backing_release(backing);backing=NULL;break; }
    }
    if(backing) {
        if(address_space->nseg>=SEGMENT_LIMIT) { backing_release(backing);return linux_error(12); }
        address_space->seg[address_space->nseg++]=(Segment){address,size,flags,backing,offset};
    }
    segments_merge();
    return 0;
}
static uint64_t reserve_commit(uint64_t address,uint64_t length,uint64_t prot) {
    unsigned flags=(unsigned)(((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0));
    for(uint64_t at=address,end=address+page_up(length);at<end;) {
        uint64_t next=(at&~(RESERVE_WINDOW-1))+RESERVE_WINDOW;if(next>end)next=end;
        uint64_t e=reserve_commit_piece(at,next-at,flags);if(e)return e;
        at=next;
    }
    return address;
}
/* mprotect inside a reservation (.NET's GC commits its regions this way): committed
   pages change protection and keep their bytes, as on Linux; uncommitted ones are
   committed, zero, unless the protection is PROT_NONE (then they stay reserved). */
static uint64_t reserve_protect(uint64_t address,uint64_t bytes,uint64_t prot) {
    unsigned flags=(unsigned)(((prot&1)?4:0)|((prot&2)?2:0)|((prot&4)?1:0));uint64_t end=address+bytes;
    for(unsigned i=0;i<address_space->nseg;i++) {
        Segment *s=&address_space->seg[i];uint64_t begin=s->base,top=s->base+s->size;
        if(top<=address || begin>=end || (s->flags&SEG_SHARED))continue;
        unsigned extra=(begin<address)+(top>end);
        if(address_space->nseg+extra>SEGMENT_LIMIT || !backing_retain(s->backing,extra))return linux_error(12);
        Segment old=*s;uint64_t low=begin<address?address:begin,high=top>end?end:top;
        *s=(Segment){low,high-low,flags,old.backing,old.offset+(size_t)(low-begin)};
        if(begin<address)address_space->seg[address_space->nseg++]=(Segment){begin,address-begin,old.flags,old.backing,old.offset};
        if(top>end)address_space->seg[address_space->nseg++]=(Segment){end,top-end,old.flags,old.backing,old.offset+(size_t)(end-begin)};
    }
    for(uint64_t at=address;prot && at<end;) {                                     /* the gaps */
        uint64_t next=end;int inside=0;
        for(unsigned i=0;i<address_space->nseg;i++) {
            Segment *s=&address_space->seg[i];
            if(s->base<=at && at<s->base+s->size) { at=s->base+s->size;inside=1;break; }
            if(s->base>at && s->base<next)next=s->base;
        }
        if(inside)continue;
        uint64_t e=reserve_commit(at,next-at,prot);if((int64_t)e<0)return e;
        at=next;
    }
    segments_merge();return 0;
}
/* munmap beyond the arena: commits and reservations in the range go; 0 if none was there. */
static int reserve_unmap(uint64_t address,uint64_t bytes) {
    int found=0;uint64_t end=address+bytes;
    for(unsigned i=0;i<address_space->nreserve;) {
        uint64_t base=address_space->reserve_base[i],top=base+address_space->reserve_size[i];
        if(top<=address || base>=end) { i++;continue; }
        found=1;
        if(address<=base && end>=top) {
            address_space->reserve_base[i]=address_space->reserve_base[--address_space->nreserve];
            address_space->reserve_size[i]=address_space->reserve_size[address_space->nreserve];continue;
        }
        if(address<=base) { address_space->reserve_base[i]=end;address_space->reserve_size[i]=top-end; }
        else if(end>=top)address_space->reserve_size[i]=address-base;
        i++;                                                                         /* a hole inside: the range stays reserved */
    }
    if(found)(void)segments_remove(address,end);
    return found;
}
/* Private file-backed guest mappings. Bytes remain ordinary non-executable host
   allocations; MAP_FIXED is confined to the bounded guest mmap arena, or a reservation. */
static int dynamic_enabled;
static uint64_t guest_dynamic_mmap(void) {
    uint64_t addr=cpu->gpr[0],length=cpu->gpr[1],prot=cpu->gpr[2],flags=cpu->gpr[3],offset=cpu->gpr[5],hint=(flags&16)?0:addr;
    int anonymous_private=(flags&32) && (flags&3)==2 && !(flags&0x100000);
    if(anonymous_private && (flags&16) && !(addr%4096) && length && length<=RESERVE_END && reserve_find(addr,page_up(length))>=0) {
        if(prot>7 || ((prot&6)==6 && !map_rwx_allowed))return linux_error(22);
        return reserve_commit(addr,length,prot);                                     /* a commit inside a reservation */
    }
    int reservable=anonymous_private && !(flags&16) && !prot && length && length<=RESERVE_END;
    if(length>address_space->map_capacity)return reservable?reserve_new(length):linux_error(12); /* more than the arena holds */
    if(!length || offset%4096 || offset>INT64_MAX-length || prot>7 || ((prot&6)==6 && !map_rwx_allowed))return linux_error(22);
    if(!address_space_map_flags(address_space))return linux_error(12);
    /* Linux ignores MAP_DENYWRITE, MAP_EXECUTABLE and MAP_STACK. Stack guards
       still use ordinary PROT_NONE/mprotect; this does not create a thread. */
    flags&=~UINT64_C(0x2d800);                       /* also MAP_NORESERVE and MAP_POPULATE: hints */
    int noreplace=(flags&0x100000)!=0;               /* MAP_FIXED_NOREPLACE: fixed, but never over a mapping */
    if(noreplace) {
        flags=(flags&~UINT64_C(0x100000))|16;
        if(addr%4096)return linux_error(22);
        if(addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || page_up(length)>address_space->map_capacity-(addr-MAP_BASE))
            return linux_error(12);                  /* outside the arena: this address cannot be had */
        for(uint64_t p=(addr-MAP_BASE)/4096;p<(addr-MAP_BASE+page_up(length))/4096;p++)if(address_space->map_flags[p])return linux_error(17);
    }
    if((flags&3) && (flags&3)!=2 && (flags&32) && !(flags&~UINT64_C(0x33)))   /* MAP_SHARED|MAP_ANONYMOUS */
        return shared_map((flags&16)?addr:0,length,prot,NULL,0);
    if((flags&3) && (flags&3)!=2 && !(flags&32) && (prot&2) && !(flags&~UINT64_C(0x13)))  /* writable MAP_SHARED of a file */
        return file_share_map((flags&16)?addr:0,length,prot,cpu->gpr[4],offset);
    /* A read-only MAP_SHARED file mapping (ICU data, for instance) can never write
       through, so a private copy is equivalent, unless the file is or can be mapped
       shared and writable: then it must see those writes. JITs map a memfd twice,
       asmjit (Erlang's BEAM) the executable view first. */
    if((flags&3) && (flags&3)!=2 && !(flags&32) && !(prot&2) && !(flags&~UINT64_C(0x13)) &&
       (file_share_find(cpu->gpr[4]) || (fs_host(cpu->gpr[4])>=0 && !fs_ro[fs_slot(cpu->gpr[4])])))
        return file_share_map((flags&16)?addr:0,length,prot,cpu->gpr[4],offset);
    if((flags&3) && (flags&3)!=2 && !(flags&32) && !(prot&2))flags=(flags&~UINT64_C(3))|2;
    if((flags&~UINT64_C(0x32)) || (flags&3)!=2)return linux_error(95);
    if(!(flags&48) && !(prot&2) && page_up(length)>=(UINT64_C(16)<<20)) {
        /* A large read-only private file mapping (ClickHouse maps its own 755 MiB binary,
           in two processes): like the loader's text, not charged to the memory budget, as
           Linux keeps such pages in its reclaimable page cache. ponytail: it is a shared
           segment, so a later mprotect to writable would share writes across fork; copy
           on such an mprotect if that ever matters. */
        int fd=fs_host(cpu->gpr[4]);if(fd<0)return linux_error(9);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISREG(st.st_mode))return linux_error(19);
        MemoryBacking *text=backing_alloc_text((size_t)page_up(length));if(!text)return linux_error(12);
        for(size_t done=0;done<length;) {
            ssize_t n=pread(fd,backing_data(text)+done,(size_t)(length-done),(off_t)(offset+done));
            if(n<0 && errno==EINTR)continue;
            if(n<0) { uint64_t e=fs_error();backing_release(text);return e; }
            if(!n)break;done+=(size_t)n;
        }
        uint64_t result=shared_map(0,length,prot,text,0);backing_release(text);
        for(unsigned i=0;(int64_t)result>0 && i<address_space->nseg;i++)if(address_space->seg[i].base==result)address_space->seg[i].flags|=SEG_FILE_COPY;
        return result;
    }
    int fd=-1;
    if(!(flags&32)) { fd=fs_host(cpu->gpr[4]);if(fd<0)return linux_error(9);
        struct stat st;if(fstat(fd,&st)<0)return fs_error();if(!S_ISREG(st.st_mode))return linux_error(19);
    }
    unsigned pages=(unsigned)(page_up(length)/4096),start=0;
    if(flags&16) {
        if(addr%4096 || addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || page_up(length)>address_space->map_capacity-(addr-MAP_BASE))return linux_error(22);
        if(!file_copy_demote(addr,page_up(length),0))return linux_error(12);  /* MAP_FIXED over a file copy */
        start=(unsigned)((addr-MAP_BASE)/4096);
    } else {
        start=map_hint(address_space,hint,pages);if(start==address_space->map_capacity/4096)start=map_find(address_space,pages);
        if(start==address_space->map_capacity/4096)return reservable?reserve_new(length):linux_error(12);
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
    if(addr%4096 || length>UINT64_MAX-4095 || prot>7 || ((prot&6)==6 && !map_rwx_allowed))return linux_error(22);
    if(!length)return 0;
    uint64_t bytes=page_up(length);
    if(addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || bytes>address_space->map_capacity-(addr-MAP_BASE)) {
        if(bytes>UINT64_MAX-addr)return linux_error(22);
        if(reserve_find(addr,bytes)>=0)return reserve_protect(addr,bytes,prot);
        for(unsigned i=0;i<address_space->nseg;i++) {
            Segment old=address_space->seg[i];uint64_t end=addr+bytes,oldend=old.base+old.size;
            if(addr<(old.base&~UINT64_C(4095)) || end>oldend || end<=old.base || addr>=oldend)continue;
            uint64_t begin=addr>old.base?addr:old.base;
            unsigned extra=(begin>old.base)+(end<oldend);if(address_space->nseg+extra>SEGMENT_LIMIT)return linux_error(12);
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
    if(!file_copy_demote(addr,bytes,1))return linux_error(12);            /* part of a file copy */
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

/* Linux madvise advice values are not host ABI values. Pure hints are accepted on
   mapped guest ranges; this runtime has no read-ahead, merging, huge page or core
   dump policy to change. MADV_DONTNEED releases anonymous mmap-arena pages. Other reclamation/fork advice reports Linux EINVAL, never success. No
   guest address is passed to host madvise. */
static uint64_t guest_madvise(void) {
    uint64_t addr=cpu->gpr[0],length=cpu->gpr[1];int32_t advice=(int32_t)cpu->gpr[2];
    if(!(addr&4095) && length && length<=RESERVE_END && reserve_find(addr,page_up(length))>=0) {
        /* Inside a reservation: hints are accepted; MADV_DONTNEED zeroes committed pages. */
        if(advice==4)
            for(unsigned i=0;i<address_space->nseg;i++) {
                Segment *s=&address_space->seg[i];uint64_t low=s->base>addr?s->base:addr,high=s->base+s->size<addr+page_up(length)?s->base+s->size:addr+page_up(length);
                if(low<high && !(s->flags&SEG_SHARED) && !address_space_discard(address_space,s->backing,s->offset+(size_t)(low-s->base),(size_t)(high-low)))return linux_error(12);
            }
        return advice==4 || (advice>=0 && advice<=3) || (advice>=12 && advice<=17)?0:linux_error(22);
    }
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
    /* Hints with nothing to change here: NORMAL, RANDOM, SEQUENTIAL, WILLNEED,
       (UN)MERGEABLE, (NO)HUGEPAGE, DONTDUMP and DODUMP (InnoDB, JVMs, jemalloc). */
    int hint=(advice>=0 && advice<=3) || (advice>=12 && advice<=17);
    if(!hint || (addr&4095) || length>UINT64_MAX-4095)return linux_error(22);
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
    uint64_t result=shared_map(address,shm_segments[id].size,(flags&010000)?1:3,shm_segments[id].backing,0);
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
/* /proc/self/maps: this process's mappings in address order, as Linux lists them
   (glibc's pthread_getattr_np finds the main thread's stack here; .NET needs that).
   Segments, the heap, runs of equally protected arena pages and reservations; names
   only for [stack], [heap] and [vdso]. */
typedef struct { uint64_t from,to;unsigned perm;const char *name; } ProcMap;
static int proc_map_order(const void *a,const void *b) {
    uint64_t x=((const ProcMap *)a)->from,y=((const ProcMap *)b)->from;return x<y?-1:x>y;
}
static size_t proc_maps(char *out,size_t capacity) {
    GuestAddressSpace *space=address_space;size_t limit=space->nseg+space->nreserve+2+(space->map_flags?space->map_top:0),n=0,used=0;
    ProcMap *m=malloc(limit*sizeof(*m));if(!m)return 0;
    for(unsigned i=0;i<space->nseg;i++) {
        Segment *s=&space->seg[i];
        m[n++]=(ProcMap){s->base,s->base+s->size,(s->flags&7)|((s->flags&SEG_SHARED)?8:0),
            s->base==STACK_TOP-STACK_SIZE?"[stack]":s->base==STACK_TOP-STACK_SIZE-UINT64_C(0x100000)?"[vdso]":""};
    }
    if(space->brk_pages)m[n++]=(ProcMap){space->brk_base,space->brk_base+page_up(space->brk_pages),6,"[heap]"};
    for(unsigned p=0;space->map_flags && p<space->map_top && p<space->map_capacity/4096;) {
        unsigned char f=space->map_flags[p];unsigned q=p+1;
        while(q<space->map_top && space->map_flags[q]==f)q++;
        if(f && f!=MAP_RESERVED)m[n++]=(ProcMap){MAP_BASE+(uint64_t)p*4096,MAP_BASE+(uint64_t)q*4096,f&7,""};
        p=q;
    }
    for(unsigned i=0;i<space->nreserve;i++)m[n++]=(ProcMap){space->reserve_base[i],space->reserve_base[i]+space->reserve_size[i],0,""};
    qsort(m,n,sizeof(*m),proc_map_order);
    for(size_t i=0;i<n && used+128<capacity;i++)
        used+=(size_t)snprintf(out+used,capacity-used,"%llx-%llx %c%c%c%c 00000000 00:00 0%s%s\n",(unsigned long long)m[i].from,(unsigned long long)m[i].to,
            (m[i].perm&4)?'r':'-',(m[i].perm&2)?'w':'-',(m[i].perm&1)?'x':'-',(m[i].perm&8)?'s':'p',*m[i].name?"                          ":"",m[i].name);
    free(m);return used;
}
