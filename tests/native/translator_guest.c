/* What x86 translators (FEX, Box64) need from the arm64 kernel ABI beyond the usual:
   EL0 reads of the ID registers, emulated as Linux does (HWCAP_CPUID; FEX reads
   MIDR_EL1 and ID_AA64* with MRS to choose its code generation, glibc reads MIDR_EL1),
   with fields agreeing with AT_HWCAP; and SIGBUS/BUS_ADRALN at the address of an
   unaligned atomic (FEX backpatches x86 lock-prefixed accesses on it); and
   MAP_GROWSDOWN mappings (Box64 allocates its x86 stack so). FEX also needs openat2
   with RESOLVE_IN_ROOT (its x86 root's files), execveat (it re-runs itself for an x86
   execve), personality, close_range and a /proc/self/fd listing. Run with --signals
   (handlers) and --root holding this program as /t, /file ("root") and /d/file ("d")
   with /d/abs -> /file. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    register U x4 __asm__("x4")=(U)-1,x5 __asm__("x5")=0;    /* mmap: no descriptor, offset 0 */
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static long call6(U n,U a,U b,U c,U d,U e) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory","cc");
    return (long)x0;
}
static unsigned checks;
static void check(int ok) { checks++;if(!ok) { char m[]="translator FAIL at check 00\n";m[25]=(char)('0'+checks/10);m[26]=(char)('0'+checks%10);call(64,1,(U)m,sizeof(m)-1,0);call(94,1,0,0,0);for(;;){} } }
#define MRS(name) ({ U v;__asm__ volatile("mrs %0," name:"=r"(v));v; })
#define FIELD(v,shift) (((v)>>(shift))&15)
static volatile U bus_signal,bus_code,bus_addr;
static void bus(int signal,void *info,void *context) {        /* record, then step over the access */
    bus_signal=(U)signal;bus_code=*(int *)((char *)info+8);bus_addr=*(U *)((char *)info+16);
    *(U *)((char *)context+440)+=4;                          /* uc_mcontext.pc */
}
static unsigned char data[64] __attribute__((aligned(64)));
/* The first byte of the file openat2 (how: flags, mode, resolve; size) opens from dir, or its error. */
static long first_byte(long dir,const char *path,U resolve,U size) {
    U how[5]={0,0,resolve,0,0};long fd=call6(437,(U)dir,(U)path,(U)how,size,0);if(fd<0)return fd;
    char c=0;call(63,(U)fd,(U)&c,1,0);call(57,(U)fd,0,0,0);return c;
}
static void files_and_exec(char **argv,char **envp) {
    long d=call(56,(U)-100,(U)"/d",0x4000,0);check(d>=0);           /* O_DIRECTORY */
    check(first_byte(d,"abs",0x10,24)=='d');                        /* RESOLVE_IN_ROOT: /file is /d/file */
    check(first_byte(d,"/file",0x10,24)=='d' && first_byte(d,"../file",0x10,24)=='d');
    check(first_byte(d,"abs",0,24)=='r');                           /* no RESOLVE_*: the root's /file */
    check(first_byte(d,"abs",0x08,24)==-18 && first_byte(d,"../file",0x08,24)==-18); /* BENEATH: EXDEV */
    check(first_byte(d,"abs",0x04,24)==-40 && first_byte(d,"file",0x04,24)=='d');      /* NO_SYMLINKS: ELOOP */
    check(first_byte(d,"file",0,16)==-22 && first_byte(d,"file",0x20,24)==-11 && first_byte(d,"file",0x40,24)==-22);
    U big[5]={0,0,0,1,0};check(call6(437,(U)d,(U)"file",(U)big,32,0)==-7);               /* E2BIG */
    check(call(92,0xffffffffUL,0,0,0)==0 && call(92,0x40000,0,0,0)==0 && call(92,0xffffffffUL,0,0,0)==0x40000);
    call(92,0,0,0,0);
    long list=call(56,(U)-100,(U)"/proc/self/fd",0x4000,0);check(list>=0);
    char buf[2048];long n=call(61,(U)list,(U)buf,sizeof(buf),0);check(n>0);      /* getdents64 */
    int saw=0;for(long at=0;at<n;at+=*(unsigned short *)(buf+at+16)) { const char *name=buf+at+19;
        if(name[0]=='0'+d/10 && name[1]=='0'+d%10 && !name[2] && d>=10)saw=1;if(name[0]=='0'+d && !name[1] && d<10)saw=1; }
    check(saw);call(57,(U)list,0,0,0);
    check(call(24,(U)d,50,0,0)==50 && call(24,(U)d,51,0,0)==51 && call(24,(U)d,52,0,0)==52);  /* dup3 */
    check(call(436,50,51,0,0)==0 && call(25,50,1,0,0)==-9 && call(25,51,1,0,0)==-9 && call(25,52,1,0,0)==0);
    check(call(436,52,52,4,0)==0 && call(25,52,1,0,0)==1 && call(436,3,2,0,0)==-22);    /* CLOSE_RANGE_CLOEXEC; first > last */
    long root=call(56,(U)-100,(U)"/",0x4000,0);check(root>=0);
    char *again[]={argv[0],"exec",0};
    check(call6(281,(U)root,(U)"t",(U)again,(U)envp,0x2)==-22);    /* unknown flag */
    call6(281,(U)root,(U)"t",(U)again,(U)envp,0);check(0);           /* relative to dirfd: returns only on failure */
}
__asm__(".globl _start\n_start: mov x0,sp\n b start_c\n");
void start_c(U *sp) {
    char **argv=(char **)(sp+1),**envp=argv+sp[0]+1;
    if(sp[0]>1 && argv[1][0]=='e') { call(64,1,(U)"translator PASS\n",16,0);call(94,0,0,0,0);for(;;){} } /* after execveat */
    U *p=sp+1+sp[0]+1;while(*p)p++;p++;                       /* past argv and envp: auxv */
    U hwcap=0;for(;p[0];p+=2)if(p[0]==16)hwcap=p[1];
    check(hwcap&(1UL<<11));                                    /* HWCAP_CPUID */
    U midr=MRS("midr_el1"),mpidr=MRS("mpidr_el1"),isar0=MRS("id_aa64isar0_el1"),isar1=MRS("id_aa64isar1_el1");
    U pfr0=MRS("id_aa64pfr0_el1"),mmfr2=MRS("id_aa64mmfr2_el1"),unallocated=MRS("S3_0_C0_C7_7");
    check(midr>>24);                                           /* an implementer (0x61: Apple) */
    check(mpidr==0x80000000UL);
    check((FIELD(isar0,20)>=2)==!!(hwcap&(1UL<<8)));            /* LSE atomics, HWCAP_ATOMICS */
    check((FIELD(isar0,4)>=1)==!!(hwcap&(1UL<<3)));             /* AES */
    check((FIELD(isar0,16)>=1)==!!(hwcap&(1UL<<7)));            /* CRC32 */
    check((FIELD(isar1,20)>=1)==!!(hwcap&(1UL<<15)));           /* LRCPC */
    check(!(isar1&0xff000ff0UL));                               /* no pointer authentication */
    check(FIELD(pfr0,16)!=15 && FIELD(pfr0,0)>=1);              /* FP present, EL0 AArch64 */
    check((FIELD(mmfr2,32)>=1)==!!(hwcap&(1UL<<25)));           /* LSE2, HWCAP_USCAT */
    check(!unallocated);
    U xzr;__asm__ volatile("mrs xzr,midr_el1\n mov %0,#7":"=r"(xzr));check(xzr==7); /* Rt 31 discards */
    U action[4]={(U)bus,0x4,0,0};check(call(134,7,(U)action,0,8)==0); /* SIGBUS, SA_SIGINFO */
    U value=1;volatile unsigned char *odd=data+15;              /* crosses a 16-byte boundary: faults even with LSE2 */
    __asm__ volatile("ldar %0,[%1]":"=&r"(value):"r"(odd):"memory");
    check(bus_signal==7 && bus_code==1 && bus_addr==(U)odd);    /* BUS_ADRALN at the address */
    bus_signal=0;__asm__ volatile("stlr %0,[%1]"::"r"(value),"r"(odd):"memory");
    check(bus_signal==7 && bus_addr==(U)odd);                   /* a store too, without looping on it */
    long stack=call(222,0,0x40000,3,0x122);                /* MAP_GROWSDOWN: Box64's x86 stack */
    check(stack>0 && !(stack&4095));
    if(stack>0) { *(volatile U *)(stack+0x40000-8)=9;check(*(volatile U *)(stack+0x40000-8)==9); }
    files_and_exec(argv,envp);for(;;){}
}
