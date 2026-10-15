/* Linux's address space beyond the mmap arena: any number of PROT_NONE reservations,
   holes punched in them, MAP_FIXED / MAP_FIXED_NOREPLACE anywhere in the user range
   (anonymous, file-backed, shared), mremap, and the heap's unused room. Run with --mmap-limit-mib 64 (so
   128 MiB reservations lie beyond the arena) and --process-tree (fork). Writes the
   number of passed checks (4 bytes); on a failure, that number and exit status 99. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)&checks,4,0,0,0);call(93,99,0,0,0,0,0);for(;;){} } }
#define MiB (1UL<<20)
#define R 1   /* PROT_READ */
#define RW 3
#define PRIV_ANON 0x22
#define FIXED 0x10
#define NOREPLACE 0x100000
static long map(U a,U n,U prot,U flags) { return call(222,a,n,prot,flags,(U)-1,0); }
static long map_file(U a,U n,U prot,U flags,U fd) { return call(222,a,n,prot,flags,fd,0); }
static long unmap(U a,U n) { return call(215,a,n,0,0,0,0); }
static long remap(U a,U old,U n,U flags,U target) { return call(216,a,old,n,flags,target,0); }
static volatile U *at(U a) { return (volatile U *)a; }
/* fork; the child runs body and exits 0 if it returns 1. Returns the child's status. */
static int forked(int (*body)(U),U arg) {
    long pid=call(220,17,0,0,0,0,0);
    if(!pid)call(93,body(arg)?0:1,0,0,0,0,0);
    int status=-1;call(260,(U)pid,(U)&status,0,0,0,0);
    return status;
}
static int child_write(U a) { *at(a)=7;return *at(a)==7; }
static U reserve[40];
#ifndef SECTIONS
#define SECTIONS 7 /* 1 reservations, 2 MAP_FIXED, 4 mremap: -DSECTIONS=n runs a part alone */
#endif
void _start(void) {
    if(SECTIONS&1) {
    /* 1. Forty reservations beyond a 64 MiB arena (there were at most 8). */
    for(unsigned i=0;i<40;i++) { reserve[i]=(U)map(0,128*MiB,0,PRIV_ANON);check((long)reserve[i]>0 && reserve[i]>=0x100000000000UL); }
    U r=reserve[0];
    /* A hole in the middle splits the reservation: the hole is free, the rest is not. */
    check(unmap(r+32*MiB,16*MiB)==0);
    check(map(r+32*MiB,16*MiB,RW,PRIV_ANON|NOREPLACE)==(long)(r+32*MiB));
    *at(r+32*MiB)=5;check(*at(r+32*MiB)==5);
    check(map(r,4096,RW,PRIV_ANON|NOREPLACE)==-17);
    check(map(r+64*MiB,4096,RW,PRIV_ANON|NOREPLACE)==-17);
    /* Commits on both sides of the hole; fork keeps them private (copy-on-write). */
    check(map(r,4096,RW,PRIV_ANON|FIXED)==(long)r);*at(r)=42;
    check(call(226,r+64*MiB,8192,RW,0,0,0)==0);*at(r+64*MiB)=43;
    check(forked(child_write,r)==0 && *at(r)==42);
    check(forked(child_write,r+64*MiB)==0 && *at(r+64*MiB)==43);
    for(unsigned i=0;i<40;i++)check(unmap(reserve[i],128*MiB)==0);
    for(unsigned i=0;i<40;i++) { reserve[i]=(U)map(0,128*MiB,0,PRIV_ANON);check((long)reserve[i]>0); }
    for(unsigned i=0;i<40;i++)check(unmap(reserve[i],128*MiB)==0);
    }
    if(SECTIONS&2) {
    /* 2. MAP_FIXED outside the arena: below it, in the reservation range, above the stack. */
    U low=0x10000000,mid=0x500000000000UL,high=0xa00000000000UL;
    check(map(low,8192,RW,PRIV_ANON|FIXED)==(long)low);*at(low+4096)=11;check(*at(low+4096)==11);
    check(map(low,8192,RW,PRIV_ANON|FIXED)==(long)low && *at(low+4096)==0);     /* replaced: fresh zero pages */
    check(map(low,4096,RW,PRIV_ANON|NOREPLACE)==-17);
    check(map(low+8192,4096,RW,PRIV_ANON|NOREPLACE)==(long)(low+8192));
    check(unmap(low,12288)==0 && map(low,4096,RW,PRIV_ANON|NOREPLACE)==(long)low);
    check(map(mid,4096,RW,PRIV_ANON|FIXED)==(long)mid);*at(mid)=12;check(*at(mid)==12);
    check(map(high,4096,RW,PRIV_ANON|FIXED)==(long)high);*at(high)=13;check(*at(high)==13);
    check(map(0x1000,4096,RW,PRIV_ANON|FIXED)==-1);                              /* below mmap_min_addr */
    check(map(0xffffffff0000UL,4096,RW,PRIV_ANON|FIXED)==-12);                    /* beyond the user range */
    check(call(226,mid,4096,R,0,0,0)==0 && *at(mid)==12);
    /* File-backed, private. */
    long fd=call(279,(U)"wide",0,0,0,0,0);check(fd>=0);
    check(call(64,(U)fd,(U)"hello, wide world",17,0,0,0)==17);
    U file=0x20000000;
    check(map_file(file,4096,R,0x12,(U)fd)==(long)file);                          /* MAP_PRIVATE|MAP_FIXED */
    check(*(volatile unsigned char *)file=='h' && *(volatile unsigned char *)(file+7)=='w' && *(volatile unsigned char *)(file+17)==0);
    /* Shared and anonymous: a forked child's write shows. */
    U shared=0x30000000;
    check(map(shared,8192,RW,0x31)==(long)shared);                                /* MAP_SHARED|MAP_ANONYMOUS|MAP_FIXED */
    check(forked(child_write,shared+4096)==0 && *at(shared+4096)==7);
    check(unmap(shared,8192)==0 && map(shared,4096,RW,PRIV_ANON|NOREPLACE)==(long)shared);
    }
    if(SECTIONS&4) {
    /* 3. mremap. */
    U far=0x40000000;
    U p=(U)map(0,32768,RW,PRIV_ANON);check((long)p>0);
    check(unmap(p+16384,16384)==0);
    *at(p)=21;*at(p+12288)=22;
    check(remap(p,16384,32768,0,0)==(long)p && *at(p)==21 && *at(p+12288)==22);  /* grows in place */
    *at(p+28672)=23;check(*at(p+28672)==23);
    check(remap(p,32768,8192,0,0)==(long)p);                                     /* shrinks */
    check(map(p+8192,4096,RW,PRIV_ANON|NOREPLACE)==(long)(p+8192));               /* blocks growing in place */
    check(remap(p,8192,16384,0,0)==-12);
    U q=(U)remap(p,8192,16384,1,0);                                               /* MREMAP_MAYMOVE */
    check((long)q>0 && q!=p && *at(q)==21 && *at(q+8192)==0);
    check(map(p,4096,RW,PRIV_ANON|NOREPLACE)==(long)p);                           /* the old range is gone */
    check(remap(q,16384,16384,3,far)==(long)far && *at(far)==21);                 /* MREMAP_FIXED, out of the arena */
    U back=(U)remap(far,16384,65536,1,0);check(back==far && *at(back)==21);       /* grows in place: free after it */
    U old=back;U moved=(U)remap(old,65536,65536,5,0);                             /* MREMAP_DONTUNMAP */
    check((long)moved>0 && moved!=old && *at(moved)==21 && *at(old)==0);
    *at(old)=24;check(*at(old)==24);
    U s=(U)map(0,8192,RW,0x21);check((long)s>0);                                  /* MAP_SHARED|MAP_ANONYMOUS */
    U alias=(U)remap(s,0,8192,1,0);check((long)alias>0 && alias!=s);              /* old size 0: the same pages again */
    *at(s+4096)=25;check(*at(alias+4096)==25);
    U t=(U)remap(s,8192,8192,3,0x50000000);check(t==0x50000000 && *at(t+4096)==25);
    *at(t)=26;check(*at(alias)==26);
    U lone=(U)map(0,4096,RW,0x21);*at(lone)=28;                                   /* shared, mapped once: grows (mmap.resize) */
    U grown=(U)remap(lone,4096,1<<20,1,0);check((long)grown>0 && *at(grown)==28);
    *at(grown+(1<<20)-8)=29;check(*at(grown+(1<<20)-8)==29);
    check(remap(p+1,4096,8192,1,0)==-22 && remap(p,4096,8192,8,0)==-22 && remap(p,4096,8192,2,far)==-22);
    check(remap(0x60000000,4096,8192,1,0)==-14);                                   /* nothing mapped */
    U res=(U)map(0,128*MiB,0,PRIV_ANON);check(res>=0x100000000000UL);
    check(map(res,4096,RW,PRIV_ANON|FIXED)==(long)res);*at(res)=27;
    U fromres=(U)remap(res,4096,8192,1,0);check((long)fromres>0 && *at(fromres)==27);
    }
    {   /* The heap's unused room takes fixed mappings (FEX guards the break with one);
           brk then stops short of them and returns the old break. */
        U b=(U)call(214,0,0,0,0,0,0);check(b && !(b&4095));
        check((U)call(214,b+8192,0,0,0,0,0)==b+8192);*at(b)=30;
        check(map(b+8192,4096,0,PRIV_ANON|NOREPLACE)==(long)(b+8192));
        check((U)call(214,b+3*4096,0,0,0,0,0)==b+8192);
        check(map(b+6*4096,4096,RW,PRIV_ANON|FIXED)==(long)(b+6*4096));*at(b+6*4096)=31;check(*at(b+6*4096)==31);
        check(map(b,4096,0,PRIV_ANON|NOREPLACE)==-17);                            /* the heap in use */
        check(unmap(b+8192,4096)==0 && (U)call(214,b+3*4096,0,0,0,0,0)==b+3*4096 && *at(b)==30);
    }
    call(64,1,(U)&checks,4,0,0,0);call(93,0,0,0,0,0,0);for(;;){}
}
