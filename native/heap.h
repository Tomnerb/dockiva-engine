/* Pages first.. were freed: the free hint, and map_find's cache, no longer hold below them. */
static void map_freed(GuestAddressSpace *space,unsigned first) {
    if(first<space->map_free_hint)space->map_free_hint=first;
    if(first<space->map_large_from)space->map_large_pages=0;
}
static uint64_t linux_error(unsigned error) { return UINT64_C(0)-error; }
static uint64_t page_up(uint64_t n) { return (n+4095)&~UINT64_C(4095); }
/* The heap follows the program (not its interpreter), as on Linux. */
static void heap_init(void) {
    uint64_t end=0,interpreter=address_space->interpreter_base?address_space->interpreter_base:UINT64_MAX;
    for(unsigned i=0;i<address_space->nseg;i++)
        if(address_space->seg[i].base<interpreter && address_space->seg[i].base+address_space->seg[i].size>end) end=address_space->seg[i].base+address_space->seg[i].size;
    if(end>GUEST_VA_END-BRK_CAP-4095) fail("No room for bounded guest heap");
    address_space->brk_base=address_space->brk_current=page_up(end);
    if(address_space->brk_base<MAP_BASE+address_space->map_capacity && MAP_BASE<address_space->brk_base+BRK_CAP) fail("ELF heap overlaps mmap arena");
    uint64_t stack=STACK_TOP-STACK_SIZE-UINT64_C(0x200000);                  /* with the vDSO below it */
    if(address_space->brk_base<STACK_TOP && stack<address_space->brk_base+BRK_CAP) fail("ELF heap exceeds supported address range");
    for(unsigned i=0;i<address_space->nseg;i++)
        if(address_space->seg[i].base>=address_space->brk_base && address_space->seg[i].base<address_space->brk_base+BRK_CAP) fail("ELF heap overlaps the interpreter");
    for(unsigned i=0;i<address_space->nseg;i++)
        if(address_space->seg[i].base<MAP_BASE+address_space->map_capacity && MAP_BASE<address_space->seg[i].base+address_space->seg[i].size) fail("ELF overlaps mmap arena");
}
static unsigned char *heap_lookup(uint64_t addr,uint64_t count,unsigned permission) {
    if(addr>=address_space->brk_base && addr-address_space->brk_base<address_space->brk_pages && count<=address_space->brk_pages-(addr-address_space->brk_base)) {
        if(!(permission&6))return NULL;
        if((permission&2) && !address_space_write(address_space,address_space->brk_backing,(size_t)(addr-address_space->brk_base),(size_t)count))return NULL;
        return backing_data(address_space->brk_backing)+(size_t)(addr-address_space->brk_base);
    }
    if(!backing_data(address_space->map_backing) || !address_space->map_flags || addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || count>address_space->map_capacity-(addr-MAP_BASE)) return NULL;
    uint64_t off=addr-MAP_BASE, first=off/4096, last=(off+(count?count:1)-1)/4096;
    for(uint64_t i=first;i<=last;i++) if(!(address_space->map_flags[i]&permission)) return NULL;
    if((permission&2) && !address_space_write(address_space,address_space->map_backing,(size_t)off,(size_t)count))return NULL;
    return backing_data(address_space->map_backing)+(size_t)off;
}
static int map_outside_used(uint64_t address,uint64_t bytes);   /* mapping.h */
static uint64_t guest_brk(uint64_t requested) {
    if(requested<address_space->brk_base || requested-address_space->brk_base>BRK_CAP) return address_space->brk_current;
    uint64_t needed=page_up(requested-address_space->brk_base);
    /* Growing over a mapping placed in the heap's room fails: the old break, as Linux. */
    if(needed>address_space->brk_pages && map_outside_used(address_space->brk_base+address_space->brk_pages,needed-address_space->brk_pages))return address_space->brk_current;
    if(needed && !backing_data(address_space->brk_backing)) { address_space->brk_backing=backing_reserve(BRK_CAP); if(!backing_data(address_space->brk_backing)) return address_space->brk_current; }
    if(needed>address_space->brk_pages && !address_space_make_private(address_space,address_space->brk_backing,(size_t)address_space->brk_pages,(size_t)(needed-address_space->brk_pages)))return address_space->brk_current;
    if(needed>address_space->brk_pages) memset(backing_data(address_space->brk_backing)+(size_t)address_space->brk_pages,0,(size_t)(needed-address_space->brk_pages));
    address_space->brk_pages=needed; address_space->brk_current=requested;
    address_space_changed(address_space);return requested;
}
static uint64_t guest_mmap(uint64_t length,uint64_t prot,uint64_t flags,uint64_t offset) {
    if(!length || offset%4096 || prot>3) return linux_error(22);
    if(flags!=0x22) return linux_error(95); /* Only private anonymous, no fixed flags. */
    if(length>address_space->map_capacity || !address_space_map_flags(address_space)) return linux_error(12);
    unsigned pages=(unsigned)(page_up(length)/4096), start=0, free_run=0;
    for(unsigned i=0;i<address_space->map_capacity/4096;i++) {
        if(address_space->map_flags[i]) free_run=0;
        else { if(!free_run) start=i; if(++free_run==pages) break; }
    }
    if(free_run<pages) return linux_error(12);
    if(!backing_data(address_space->map_backing)) { address_space->map_backing=backing_reserve(address_space->map_capacity); if(!backing_data(address_space->map_backing)) return linux_error(12); }
    if(!address_space_discard_map(address_space,(size_t)start*4096,(size_t)pages*4096))return linux_error(12);
    unsigned char permission=(unsigned char)(8|((prot&1)?4:0)|((prot&2)?2:0));
    for(unsigned i=start;i<start+pages;i++) address_space->map_flags[i]=permission;
    address_space_arena_changed(address_space,start,start+pages);return MAP_BASE+(uint64_t)start*4096;
}
static uint64_t shared_unmap(uint64_t address,uint64_t bytes); /* mapping.h */
static int reserve_unmap(uint64_t address,uint64_t bytes);      /* mapping.h */
static int file_copy_demote(uint64_t address,uint64_t bytes,int partial); /* mapping.h */
static uint64_t guest_munmap(uint64_t addr,uint64_t length) {
    if(addr%4096 || !length || length>UINT64_MAX-4095) return linux_error(22);
    uint64_t bytes=page_up(length);
    if(addr<MAP_BASE || addr-MAP_BASE>=address_space->map_capacity || bytes>address_space->map_capacity-(addr-MAP_BASE))
    {
        /* Outside the arena: reservations (every mmap made there) and their commits go;
           anything else is left and the call succeeds, as Linux's munmap of an unmapped
           range does (ClickHouse calls munmap(NULL, n) after a failed mmap). The
           loader's own segments (program, interpreter, stack) stay. */
        if(bytes>UINT64_C(0x1000000000000)-addr)return linux_error(22);           /* beyond the user address space */
        return reserve_unmap(addr,bytes)<0?linux_error(12):0;
    }
    unsigned first=(unsigned)((addr-MAP_BASE)/4096), pages=(unsigned)(bytes/4096);
    if(!address_space->map_flags) return 0;
    if(!file_copy_demote(addr,bytes,1)) return linux_error(12);          /* part of a file copy */
    uint64_t shared=shared_unmap(addr,bytes);if(shared) return shared;
    if(!address_space_discard_map(address_space,(size_t)first*4096,(size_t)pages*4096)) return linux_error(12); /* release the pages */
    for(unsigned i=first;i<first+pages;i++) address_space->map_flags[i]=0;
    map_freed(address_space,first);
    address_space_arena_changed(address_space,first,first+pages);return 0; /* Unmapped holes are permitted. */
}
