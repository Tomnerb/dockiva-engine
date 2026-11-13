/* Memory a program gives back is no longer charged to the budget, as Linux uncharges
   it from the cgroup: V8 decommits with mmap(MAP_FIXED, PROT_NONE) and discards with
   MADV_FREE, falling back to MADV_DONTNEED on EINVAL (as mimalloc and Go do); it frees
   large objects with munmap; glibc shrinks its heap with brk. Each way, in the mmap
   arena and in a reservation (beyond the arena), must uncharge the 16 MiB written.
   Then the swap a container may use beyond its memory (docker run --memory-swap): 96
   MiB written under a 64 MiB budget with 64 MiB of swap, 32 MiB of it shown as swap in
   use. Run with --memory-limit-mib 64 --memory-swap-mib 64 --mmap-limit-mib 64: a leak
   of a few rounds exceeds the budget. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static unsigned checks;static char where[]="release-charge FAIL at ?/?\n";
static void check(int ok) { checks++;if(!ok) { call(64,1,(U)where,sizeof(where)-1,0,0,0);call(94,(U)checks,0,0,0,0,0);for(;;){} } }
#define MB (1ul<<20)
#define SIZE (16*MB)
/* sysinfo (bytes): memory and swap in use. */
static U info[14];
static U used(void) { check(call(179,(U)info,0,0,0,0,0)==0);return info[4]-info[5]+info[8]-info[9]; }
static void touch(char *p,U bytes) { for(U i=0;i<bytes;i+=4096)p[i]=1; }
/* Write 16 MiB at p (made writable by commit), release it by release, and check that
   the charge rose by about 16 MiB and fell back. */
static void round_trip(char *p,int (*commit)(char *),int (*release)(char *)) {
    U before=used();check(commit(p));touch(p,SIZE);
    check(used()>=before+SIZE-MB);
    check(release(p));
    check(used()<before+MB);
}
static int rw(char *p) { return call(226,(U)p,SIZE,3,0,0,0)==0; }
static int fixed_rw(char *p) { return call(222,(U)p,SIZE,3,0x32,(U)-1,0)==(long)p; }
static int unmap(char *p) { return call(215,(U)p,SIZE,0,0,0,0)==0; }
static int fixed_none(char *p) { return call(222,(U)p,SIZE,0,0x4032,(U)-1,0)==(long)p; }   /* MAP_FIXED|MAP_NORESERVE, PROT_NONE */
static int dontneed(char *p) { return call(233,(U)p,SIZE,4,0,0,0)==0; }
/* MADV_FREE answers EINVAL (a pre-4.5 kernel): callers fall back to MADV_DONTNEED. */
static int free_or_dontneed(char *p) { return call(233,(U)p,SIZE,8,0,0,0)==-22 && dontneed(p); }
static int none_dontneed(char *p) { return call(226,(U)p,SIZE,0,0,0,0)==0 && free_or_dontneed(p); }
void _start(void) {
    int (*releases[])(char *)={unmap,fixed_none,dontneed,free_or_dontneed,none_dontneed};
    for(unsigned r=0;r<5;r++) {
        where[23]=(char)('0'+r);where[25]='a';
        char *a=(char *)call(222,0,SIZE,3,0x22,(U)-1,0);check((long)a>0);         /* the arena */
        round_trip(a,rw,releases[r]);
        if(releases[r]!=unmap)check(unmap(a));
        /* A reservation (more than the 64 MiB arena), committed by mprotect (V8, .NET)
           and by MAP_FIXED (Go, FEX). */
        char *base=(char *)call(222,0,256*MB,0,0x4022,(U)-1,0);check((long)base>0 && (U)base>=0x100000000000ul);
        where[25]='m';round_trip(base,rw,releases[r]);
        where[25]='f';round_trip(base+SIZE,fixed_rw,releases[r]);
        check(call(215,(U)base,256*MB,0,0,0,0)==0);
    }
    /* brk: a heap shrunk gives its pages back (glibc trims its top so). */
    where[23]='b';where[25]='k';
    U start=(U)call(214,0,0,0,0,0,0),before=used();
    check((U)call(214,start+4*MB,0,0,0,0,0)==start+4*MB);touch((char *)start,4*MB);
    check(used()>=before+4*MB-MB);
    check((U)call(214,start,0,0,0,0,0)==start);check(used()<before+MB);
    check((U)call(214,start+4*MB,0,0,0,0,0)==start+4*MB && ((char *)start)[4096]==0);   /* grown again: zero */
    check((U)call(214,start,0,0,0,0,0)==start);
    /* Swap: 96 MiB under 64 MiB of memory; 32 MiB of it is swap in use. */
    where[23]='s';where[25]='w';
    char *big=(char *)call(222,0,256*MB,0,0x4022,(U)-1,0);check((long)big>0 && call(226,(U)big,96*MB,3,0,0,0)==0);
    touch(big,96*MB);used();
    check(info[4]==64*MB && info[5]==0 && info[8]==64*MB && info[9]<=34*MB && info[9]>=28*MB);
    check(call(215,(U)big,256*MB,0,0,0,0)==0);used();check(info[9]==64*MB);
    call(64,1,(U)"release-charge PASS\n",20,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
