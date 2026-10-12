/* The vDSO (AT_SYSINFO_EHDR) resolves like musl's __vdsosym; its clock_gettime
   and gettimeofday agree with the system calls, monotonic time never goes back
   across the two, and CPU-time clocks return -ENOSYS (the libc then makes the call). */
typedef unsigned long U;
static long call3(U n,U a,U b,U c) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return (long)x0;
}
static long call(U n,U a,U b) { return call3(n,a,b,0); }
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { call3(64,1,(U)"vdso FAIL\n",10);call(94,checks,0);for(;;){} } }
static int same(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a==*b; }
static void *symbol(unsigned char *image,const char *name) {
    U phoff=*(U *)(image+32);unsigned phnum=*(unsigned short *)(image+56);U *dynamic=0;
    for(unsigned i=0;i<phnum;i++) { unsigned char *ph=image+phoff+i*56;if(*(unsigned *)ph==2)dynamic=(U *)(image+*(U *)(ph+16)); }
    check(dynamic!=0);
    char *strings=0;unsigned char *symbols=0;unsigned *hash=0;int versioned=0;
    for(U *d=dynamic;d[0];d+=2) {
        if(d[0]==0x6ffffffc || d[0]==0x6ffffff0)versioned++;      /* DT_VERDEF, DT_VERSYM: Go requires them */
        if(d[0]==5)strings=(char *)image+d[1];
        if(d[0]==6)symbols=image+d[1];
        if(d[0]==4)hash=(unsigned *)(image+d[1]);
    }
    check(strings && symbols && hash && versioned==2);
    for(unsigned i=0;i<hash[1];i++) {
        unsigned char *s=symbols+i*24;
        if(*(unsigned short *)(s+6) && same(strings+*(unsigned *)s,name))return image+*(U *)(s+8);
    }
    return 0;
}
static long ns(U *ts) { return (long)(ts[0]*1000000000UL+ts[1]); }
void cmain(U *sp) {
    U *p=sp+1+sp[0]+1;while(*p)p++;p++;                       /* skip argv and envp */
    unsigned char *image=0;for(;p[0];p+=2)if(p[0]==33)image=(unsigned char *)p[1];
    check(image && image[0]==0x7f && image[1]=='E');
    int (*cgt)(long,U *)=(int (*)(long,U *))symbol(image,"__kernel_clock_gettime");
    int (*gtod)(U *,U *)=(int (*)(U *,U *))symbol(image,"__kernel_gettimeofday");
    check(cgt && gtod);
    U a[2],b[2],c[2];
    for(int i=0;i<1000;i++) {                                    /* monotonic: vDSO, syscall, vDSO */
        check(cgt(1,a)==0 && call(113,1,(U)b)==0 && cgt(1,c)==0);
        check(ns(a)<=ns(b) && ns(b)<=ns(c) && ns(c)-ns(a)<5000000 && a[1]<1000000000);
    }
    check(cgt(0,a)==0 && call(113,0,(U)b)==0);                   /* realtime within 50 ms */
    check(ns(b)-ns(a)<50000000 && ns(a)-ns(b)<50000000);
    U tv[2],zone=7;check(gtod(tv,&zone)==0 && zone==0 && tv[1]<1000000 && (tv[0]==b[0] || tv[0]==b[0]+1 || tv[0]+1==b[0]));
    check(cgt(2,a)==-38 && cgt(99,a)==-38);
    call3(64,1,(U)"vdso PASS\n",10);call(94,0,0);for(;;){}
}
__asm__(".global _start\n_start:\n    mov x0, sp\n    b cmain\n");
