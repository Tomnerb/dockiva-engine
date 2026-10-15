/* What x86 translators (FEX, Box64) need from the arm64 kernel ABI beyond the usual:
   EL0 reads of the ID registers, emulated as Linux does (HWCAP_CPUID; FEX reads
   MIDR_EL1 and ID_AA64* with MRS to choose its code generation, glibc reads MIDR_EL1),
   with fields agreeing with AT_HWCAP; and SIGBUS/BUS_ADRALN at the address of an
   unaligned atomic (FEX backpatches x86 lock-prefixed accesses on it). Run with
   --signals (handlers). */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c,x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
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
__asm__(".globl _start\n_start: mov x0,sp\n b start_c\n");
void start_c(U *sp) {
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
    call(64,1,(U)"translator PASS\n",16,0);call(94,0,0,0,0);for(;;){}
}
