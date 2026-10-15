/* File calls of one threaded process racing each other and the calls that change
   what they use: worker threads open, write, read, stat, rename, link, mkdir and
   unlink their own files (by absolute path, cwd-relative and dirfd-relative) and
   check every byte, while a chaos thread dup2s other files over and closes the
   descriptor they pread, closes their directory descriptors' sibling copies, opens
   and fills scratch files (reusing freed host descriptors), unmaps and remaps the
   buffer they read into (a canary mapping must never change), and forks children
   that do file I/O, exec this program, or start threads in I/O and exit_group.
   Finally the main thread execs this program while threads are still in file calls.
   A use of a closed descriptor reads 'C' scratch bytes or writes into them; a use of
   unmapped memory corrupts the canary or crashes. Without CLONE_THREAD (the
   interpreter) the same work runs in turn. Needs /fs_stress (this program) in a
   writable root. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
__asm__(".global _start\n_start:\n    mov x0, sp\n    bl start\n");
#define AT (U)-100
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void fail(unsigned line) {
    char m[40]="fs-stress FAIL line ";unsigned n=20;char d[8];unsigned k=0;
    do { d[k++]=(char)('0'+line%10);line/=10; } while(line);
    while(k)m[n++]=d[--k];
    m[n++]='\n';call(64,1,(U)m,n,0,0,0);call(94,1,0,0,0,0,0);for(;;){}
}
#define CHECK(x) do { if(!(x))fail(__LINE__); } while(0)
static void copy(char *to,const char *from) { while((*to++=*from++)!=0){} }
static char *number(char *at,U v) { char d[24];unsigned k=0;do { d[k++]=(char)('0'+v%10);v/=10; } while(v);while(k)*at++=d[--k];*at=0;return at; }
static char *join(char *at,const char *s) { while(*s)*at++=*s++;*at=0;return at; }
static U next(U *x) { *x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x; }
static unsigned char pattern(U id,U i,U off) { return (unsigned char)(off*7+id*31+i*13+(off>>9)); }
static U le(const unsigned char *p,unsigned n) { U v=0;for(unsigned k=0;k<n;k++)v|=(U)p[k]<<(8*k);return v; }
static void nap(U ns) { U t[2]={0,ns};call(101,(U)t,0,0,0,0,0); }

#define WORKERS 6
#define VICTIM 200                       /* the descriptor workers pread while chaos replaces it */
#define REGION (64*1024)
static U iterations=120,threaded;
static unsigned char stacks[WORKERS+5][65536] __attribute__((aligned(16)));
static int done_word[WORKERS+5];
static volatile int stop_chaos;
static U spun;                           /* spinner iterations */
static unsigned char *region;            /* unmapped and remapped by chaos; workers only pass it to read */
static int dirfd_copy[WORKERS];

static void worker(void *arg) {
    U id=(U)arg,seed=0x9e3779b97f4a7c15UL*(id+1);
    static unsigned char bufs[WORKERS][3][70000];
    unsigned char *data=bufs[id][0],*back=bufs[id][1],*st=bufs[id][2];
    long dir=call(56,AT,(U)"/s",0x10000|0x80000,0,0,0);CHECK(dir>=0);   /* O_DIRECTORY|O_CLOEXEC */
    dirfd_copy[id]=(int)dir;
    for(U i=0;i<iterations;i++) {
        char rel[64],abs[64],moved[64],sub[64],*p;
        p=join(rel,"w");p=number(p,id);p=join(p,"_");number(p,i);
        p=join(abs,"/s/");join(p,rel);
        p=join(moved,"/s/m");p=number(p,id);p=join(p,"_");number(p,i);
        p=join(sub,"d");p=number(p,id);p=join(p,"_");number(p,i);
        U len=1+next(&seed)%65000;
        for(U k=0;k<len;k++)data[k]=pattern(id,i,k);
        int how=(int)(i%3);                                               /* absolute, cwd-relative, dirfd-relative */
        long fd=how==0?call(56,AT,(U)abs,0102|01000,0644,0,0):how==1?call(56,AT,(U)rel,0102|01000,0644,0,0):call(56,(U)dir,(U)rel,0102|01000,0644,0,0);
        CHECK(fd>=0);
        for(U at=0;at<len;) {                                             /* write and pwrite, odd sizes */
            U n=1+next(&seed)%9000;if(n>len-at)n=len-at;
            long w=(next(&seed)&1)?call(68,(U)fd,(U)(data+at),n,at,0,0):call(64,(U)fd,(U)(data+at),n,0,0,0);
            CHECK(w==(long)n && call(62,(U)fd,at+n,0,0,0,0)==(long)(at+n));    /* pwrite leaves the position */
            at+=n;
        }
        if(i%8==id%8)CHECK(call(82+(i&1),(U)fd,0,0,0,0,0)==0);               /* fsync, fdatasync */
        CHECK(call(80,(U)fd,(U)st,0,0,0,0)==0 && le(st+48,8)==len);
        CHECK(call(79,AT,(U)abs,(U)st,0,0,0)==0 && le(st+48,8)==len);
        CHECK(call(291,(U)dir,(U)rel,0,0x7ff,(U)st,0)==0 && le(st+40,8)==len);
        CHECK(call(62,(U)fd,0,0,0,0,0)==0);
        for(U at=0;at<len;) {
            U n=1+next(&seed)%12000;
            long r=call(63,(U)fd,(U)(back+at),n,0,0,0);
            CHECK(r>0 && (U)r<=n);at+=(U)r;
        }
        CHECK(call(63,(U)fd,(U)back,10,0,0,0)==0);                         /* end of file */
        for(U k=0;k<len;k++)if(back[k]!=data[k])fail(__LINE__);
        U off=next(&seed)%len,n=len-off<4000?len-off:4000;
        CHECK(call(67,(U)fd,(U)back,n,off,0,0)==(long)n);
        for(U k=0;k<n;k++)if(back[k]!=data[off+k])fail(__LINE__);
        long r=call(67,VICTIM,(U)back,4096,0,0,0);                         /* A or B whole, or closed */
        if(r!=-9) { CHECK(r==4096);for(U k=1;k<4096;k++)CHECK(back[k]==back[0]);CHECK(back[0]=='A' || back[0]=='B'); }
        r=call(67,(U)fd,(U)(region+(next(&seed)%(REGION-8192))),8192,0,0,0); /* into memory chaos unmaps */
        CHECK(r==(long)(len<8192?len:8192) || r==-14);
        CHECK(call(57,(U)fd,0,0,0,0,0)==0);
        CHECK(call(38,(U)dir,(U)rel,AT,(U)moved,0,0)==0);
        CHECK(call(79,AT,(U)abs,(U)st,0,0,0)==-2);
        CHECK(call(79,AT,(U)moved,(U)st,0x100,0,0)==0 && le(st+48,8)==len);
        CHECK(call(48,AT,(U)moved,4,0,0,0)==0);
        U times[4]={1000000+id,0,2000000+i,500};
        CHECK(call(88,AT,(U)moved,(U)times,0,0,0)==0);
        CHECK(call(79,AT,(U)moved,(U)st,0,0,0)==0 && le(st+88,8)==2000000+i);
        CHECK(call(34,(U)dir,(U)sub,0755,0,0,0)==0 && call(34,(U)dir,(U)sub,0755,0,0,0)==-17);
        CHECK(call(35,(U)dir,(U)sub,0x200,0,0,0)==0);
        char link[64];long k=call(78,AT,(U)"/s/link",(U)link,sizeof(link),0,0);
        CHECK(k==11 && link[0]=='t' && link[10]=='t');
        CHECK(call(35,AT,(U)moved,0,0,0,0)==0 && call(48,AT,(U)moved,0,0,0,0)==-2);
    }
    CHECK(call(57,(U)dir,0,0,0,0,0)==0);
}
/* File calls, then a wait that never ends: what exec and exit_group interrupt. Bounded,
   so the thread that execs or exits gets a CPU however the scheduler shares one. */
static void spinner(void *arg) {
    U id=(U)arg;char abs[64];char *p=join(abs,"/s/spin");number(p,id);
    static unsigned char buffers[WORKERS+5][64];unsigned char *b=buffers[id];
    for(U i=0;i<400;i++) {
        long fd=call(56,AT,(U)abs,0102|01000,0644,0,0);if(fd<0)continue;
        call(64,(U)fd,(U)"spin spin spin",14,0,0,0);call(67,(U)fd,(U)b,14,0,0,0);call(79,AT,(U)abs,(U)b,0,0,0);
        call(57,(U)fd,0,0,0,0,0);__atomic_add_fetch(&spun,1,__ATOMIC_RELAXED);
    }
    int never=0;for(;;)call(98,(U)&never,128,0,0,0,0);
}
/* Until the spinners are well into their loops (yielding, not sleeping: no timer needed). */
static void spinning(void) { U start=__atomic_load_n(&spun,__ATOMIC_RELAXED);while(__atomic_load_n(&spun,__ATOMIC_RELAXED)-start<40)call(124,0,0,0,0,0,0); }
static void chaos_round(U round,U *seed) {
    static unsigned char *canary;
    long a=call(56,AT,(U)"/v",0,0,0,0),b=call(56,AT,(U)"/vb",0,0,0,0);CHECK(a>=0 && b>=0);
    CHECK(call(24,(U)b,VICTIM,0,0,0,0)==VICTIM);                          /* dup2 B over the victim */
    CHECK(call(57,VICTIM,0,0,0,0,0)==0);
    for(int k=0;k<4;k++) {                                                /* reuse freed host descriptors */
        long c=call(56,AT,(U)"/s/scratch",0102|01000,0644,0,0);CHECK(c>=0);
        static unsigned char cs[4096];for(int j=0;j<4096;j++)cs[j]='C';
        CHECK(call(64,(U)c,(U)cs,4096,0,0,0)==4096);CHECK(call(57,(U)c,0,0,0,0,0)==0);
    }
    CHECK(call(24,(U)a,VICTIM,0,0,0,0)==VICTIM);                          /* A again */
    call(57,(U)a,0,0,0,0,0);call(57,(U)b,0,0,0,0,0);
    if(threaded) {                                                        /* sibling copies of the workers' dirfds */
        for(int w=0;w<WORKERS;w++) { long c=dirfd_copy[w]?call(23,(U)dirfd_copy[w],0,0,0,0,0):-1;if(c>=0)call(57,(U)c,0,0,0,0,0); }
    }
    /* The read buffer goes away and comes back; memory freed meanwhile backs a canary. */
    if(canary) { for(int j=0;j<REGION;j++)CHECK(canary[j]=='K');CHECK(call(215,(U)canary,REGION,0,0,0,0)==0); }
    canary=(unsigned char *)call(222,0,REGION,3,0x22,(U)-1,0);CHECK((long)canary>0);   /* never where region is */
    for(int j=0;j<REGION;j++)canary[j]='K';
    CHECK(call(215,(U)region,REGION,0,0,0,0)==0);
    nap(20000);
    CHECK(call(222,(U)region,REGION,3,0x32,(U)-1,0)==(long)region);      /* MAP_FIXED|PRIVATE|ANONYMOUS */
    if(round%2==1) {                                                      /* a child does file I/O, or execs */
        long pid=call(220,17,0,0,0,0,0);
        if(!pid) {
            if(round%4==1) {
                const char *argv[]={"/fs_stress","leaf",0};call(221,(U)"/fs_stress",(U)argv,0,0,0,0);call(94,9,0,0,0,0,0);
            }
            char abs[64];char *p=join(abs,"/s/child");number(p,round);
            long fd=call(56,AT,(U)abs,0102|01000,0644,0,0);if(fd<0)call(94,2,0,0,0,0,0);
            unsigned char x[16];
            if(call(64,(U)fd,(U)"child data",10,0,0,0)!=10 || call(67,(U)fd,(U)x,16,0,0,0)!=10 || x[0]!='c')call(94,3,0,0,0,0,0);
            call(57,(U)fd,0,0,0,0,0);if(call(35,AT,(U)abs,0,0,0,0))call(94,4,0,0,0,0,0);
            call(94,0,0,0,0,0,0);
        }
        CHECK(pid>0);int status=-1;CHECK(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    }
    if(threaded && round%4==3) {                                          /* a threaded child ends mid-I/O */
        long pid=call(220,17,0,0,0,0,0);
        if(!pid) {
            for(U s=0;s<3;s++)spawn(FLAGS,stacks[WORKERS+1+s]+sizeof(stacks[0]),&done_word[WORKERS+1+s],0,&done_word[WORKERS+1+s],spinner,(void *)(WORKERS+1+s));
            spinning();call(94,0,0,0,0,0,0);
        }
        CHECK(pid>0);int status=-1;CHECK(call(260,(U)pid,(U)&status,0,0,0,0)==pid && status==0);
    }
    (void)seed;
}
static void chaos(void *arg) {
    (void)arg;U seed=77;
    for(U round=0;!stop_chaos;round++)chaos_round(round,&seed);
}
static void join_thread(int *word) { int v;while((v=__atomic_load_n(word,__ATOMIC_ACQUIRE))!=0)call(98,(U)word,128,(U)v,0,0,0); }
void start(U *sp) {
    char **argv=(char **)(sp+1);
    if(sp[0]>=2 && argv[1][0]=='l') {                                     /* exec'd: a leaf, or the final image */
        if(argv[1][4]=='-') { long fd=call(56,AT,(U)"/v",0,0,0,0);char b[4];CHECK(fd>=0 && call(63,(U)fd,(U)b,4,0,0,0)==4 && b[0]=='A');out("fs-stress PASS\n"); }
        call(94,0,0,0,0,0,0);
    }
    static unsigned char fill[8192];
    CHECK(call(34,AT,(U)"/s",0755,0,0,0)==0 && call(49,(U)"/s",0,0,0,0,0)==0);   /* cwd /s */
    for(int j=0;j<8192;j++)fill[j]='A';
    long a=call(56,AT,(U)"/v",0102,0644,0,0);CHECK(a>=0 && call(64,(U)a,(U)fill,8192,0,0,0)==8192);
    for(int j=0;j<8192;j++)fill[j]='B';
    long b=call(56,AT,(U)"/vb",0102,0644,0,0);CHECK(b>=0 && call(64,(U)b,(U)fill,8192,0,0,0)==8192);
    CHECK(call(24,(U)a,VICTIM,0,0,0,0)==VICTIM);call(57,(U)a,0,0,0,0,0);call(57,(U)b,0,0,0,0,0);
    CHECK(call(36,(U)"target-test",AT,(U)"/s/link",0,0,0)==0);
    region=(unsigned char *)call(222,0,REGION,3,0x22,(U)-1,0);CHECK((long)region>0);
    threaded=spawn(FLAGS,stacks[WORKERS]+sizeof(stacks[0]),&done_word[WORKERS],0,&done_word[WORKERS],chaos,0)>0;
    if(threaded) {
        for(U w=0;w<WORKERS;w++)CHECK(spawn(FLAGS,stacks[w]+sizeof(stacks[0]),&done_word[w],0,&done_word[w],worker,(void *)w)>0);
        for(int w=0;w<WORKERS;w++)join_thread(&done_word[w]);
        stop_chaos=1;join_thread(&done_word[WORKERS]);
        /* exec while siblings are in file calls: their results are dropped, the image replaced */
        for(U s=0;s<3;s++)CHECK(spawn(FLAGS,stacks[WORKERS+1+s]+sizeof(stacks[0]),&done_word[WORKERS+1+s],0,&done_word[WORKERS+1+s],spinner,(void *)(WORKERS+1+s))>0);
        spinning();
    } else {                                                              /* no threads: the same work in turn */
        iterations=8;
        for(U w=0;w<WORKERS;w++) { worker((void *)w);U seed=1;chaos_round(w+1,&seed); }
    }
    const char *final[]={"/fs_stress","leaf-final",0};
    call(221,(U)"/fs_stress",(U)final,0,0,0,0);
    fail(__LINE__);
}
