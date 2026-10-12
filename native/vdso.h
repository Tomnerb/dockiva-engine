/* A minimal Linux vDSO for the native backend. __kernel_clock_gettime and
   __kernel_gettimeofday read CNTVCT_EL0 at EL0, so musl and glibc read the clock
   without a system call. Hypervisor.framework defines the guest counter as
   mach_absolute_time() minus the vCPU's vtimer offset, so
       monotonic ns = (CNTVCT + voff) * numer / denom   (= host CLOCK_UPTIME_RAW,
                      which like Linux CLOCK_MONOTONIC excludes sleep)
       realtime ns  = monotonic ns + real_offset        (refreshed by the runtime)
   Other clocks return -ENOSYS and the C library makes the system call. One page
   pair is shared, read-only, by every guest process. Image layout (vaddr = offset):
     0x000 ELF header, 0x040 PT_LOAD and PT_DYNAMIC, 0x0b0 dynamic, 0x150 DT_HASH,
     0x168 symbols, 0x1b0 strings, 0x200 code, 0x300 versym, 0x310 verdef,
     0x2000 data {voff,numer,denom,real_offset}.
   Symbols carry the kernel's LINUX_2.6.39 version: Go's runtime requires version
   definitions, and musl and glibc match them as on Linux.

   vdso_code is this source, at text offset 0 (image offset 0x200), assembled with
   clang --target=aarch64-linux-gnu -c vdso.S; rust-objcopy -O binary --only-section=.text:
       .macro NOW real                        // x10 = ns, realtime if \real != 0
           adr x9, data; isb; mrs x10, cntvct_el0
           ldp x11, x12, [x9]; ldp x13, x14, [x9, #16]
           add x10, x10, x11; mul x10, x10, x12; udiv x10, x10, x13
           cbz \real, 9f; add x10, x10, x14
       9:
       .endm
       clock_gettime_entry:                   // int (clockid_t, struct timespec *)
           cmp x0, #7; b.hi 2f; mov x2, #1; lsl x2, x2, x0
           mov x3, #0x73; tst x2, x3; b.eq 2f // REALTIME, MONOTONIC, MONOTONIC_RAW, *_COARSE
           mov x3, #0x21; tst x2, x3; cset x4, ne
           NOW x4
           movz x15, #0xca00; movk x15, #0x3b9a, lsl #16
           udiv x16, x10, x15; msub x17, x16, x15, x10; stp x16, x17, [x1]
           mov x0, #0; ret
       2:  mov x0, #-38; ret                  // -ENOSYS
       gettimeofday_entry:                    // int (struct timeval *, struct timezone *)
           mov x4, #1
           NOW x4
           cbz x0, 3f
           movz x15, #0xca00; movk x15, #0x3b9a, lsl #16
           udiv x16, x10, x15; msub x17, x16, x15, x10
           mov x6, #1000; udiv x17, x17, x6; stp x16, x17, [x0]
       3:  cbz x1, 4f; str xzr, [x1]
       4:  mov x0, #0; ret
       rt_sigreturn_entry:                    // return path for handlers without SA_RESTORER
           mov x8, #139; svc #0
       .org 0x1e00
       data:                                                                          */
#define VDSO_BASE (STACK_TOP-STACK_SIZE-UINT64_C(0x100000)) /* below the guest stack */
#define VDSO_SIZE 16384u
#define VDSO_CODE 0x200u
#define VDSO_DATA 0x2000u
#define VDSO_SIGRETURN (VDSO_BASE+VDSO_CODE+0xd0) /* __kernel_rt_sigreturn */
static MemoryBacking *vdso_backing; /* NULL: no vDSO (interpreter backend) */
#ifdef VMFREE_HVF
static const uint32_t vdso_code[]={
    0xf1001c1f,0x54000348,0xd2800022,0x9ac02042,0xd2800e63,0xea03005f,0x540002a0,0xd2800423,
    0xea03005f,0x9a9f07e4,0x1000eec9,0xd5033fdf,0xd53be04a,0xa940312b,0xa941392d,0x8b0b014a,
    0x9b0c7d4a,0x9acd094a,0xb4000044,0x8b0e014a,0xd299400f,0xf2a7734f,0x9acf0950,0x9b0faa11,
    0xa9004430,0xd2800000,0xd65f03c0,0x928004a0,0xd65f03c0,0xd2800024,0x1000ec49,0xd5033fdf,
    0xd53be04a,0xa940312b,0xa941392d,0x8b0b014a,0x9b0c7d4a,0x9acd094a,0xb4000044,0x8b0e014a,
    0xb4000100,0xd299400f,0xf2a7734f,0x9acf0950,0x9b0faa11,0xd2807d06,0x9ac60a31,0xa9004410,
    0xb4000041,0xf900003f,0xd2800000,0xd65f03c0,0xd2801168,0xd4000001,
};
#include <mach/mach_time.h>
static uint64_t vdso_refreshed,vdso_interval;
static void vdso_build(void) {
    static const char strings[]="\0__kernel_clock_gettime\0__kernel_gettimeofday\0linux-vdso.so.1\0LINUX_2.6.39"; /* names at 1, 24, 46, 62 */
    if(!(vdso_backing=backing_alloc(VDSO_SIZE,1)))fail("Cannot allocate the vDSO");
    unsigned char *p=backing_data(vdso_backing);
    memcpy(p,"\177ELF\2\1\1",7);
    fs_pack(p,16,3,2);fs_pack(p,18,183,2);fs_pack(p,20,1,4);                 /* ET_DYN, EM_AARCH64 */
    fs_pack(p,32,64,8);fs_pack(p,52,64,2);fs_pack(p,54,56,2);fs_pack(p,56,2,2);fs_pack(p,58,64,2);
    const uint64_t load[]={1|(UINT64_C(5)<<32),0,0,0,VDSO_SIZE,VDSO_SIZE,VDSO_SIZE};    /* PT_LOAD r-x */
    const uint64_t dynamic[]={2|(UINT64_C(4)<<32),0xb0,0xb0,0xb0,160,160,8};           /* PT_DYNAMIC */
    for(unsigned i=0;i<7;i++) { fs_pack(p,64+i*8,load[i],8);fs_pack(p,120+i*8,dynamic[i],8); }
    const uint64_t tags[]={4,0x150,5,0x1b0,6,0x168,10,sizeof(strings),11,24,14,46,   /* HASH STRTAB SYMTAB STRSZ SYMENT SONAME */
                           0x6ffffff0,0x300,0x6ffffffc,0x310,0x6ffffffd,2,0,0};    /* VERSYM VERDEF VERDEFNUM */
    for(unsigned i=0;i<20;i++)fs_pack(p,0xb0+i*8,tags[i],8);
    const uint32_t hash[]={1,3,1,0,2,0};                                       /* one bucket: 1 -> 2 */
    for(unsigned i=0;i<6;i++)fs_pack(p,0x150+i*4,hash[i],4);
    fs_pack(p,0x300,0,2);fs_pack(p,0x302,2,2);fs_pack(p,0x304,2,2);           /* versym: both symbols LINUX_2.6.39 */
    const char *names[]={"linux-vdso.so.1","LINUX_2.6.39"};const unsigned offsets[]={46,62};
    for(unsigned i=0;i<2;i++) {                                                /* Elf64_Verdef + Elf64_Verdaux */
        size_t d=0x310+i*28;uint32_t h=0;
        for(const char *c=names[i];*c;c++) { h=(h<<4)+(unsigned char)*c;uint32_t g=h&0xf0000000u;if(g)h^=g>>24;h&=~g; } /* ELF hash */
        fs_pack(p,d,1,2);fs_pack(p,d+2,i?0:1,2);fs_pack(p,d+4,i+1,2);fs_pack(p,d+6,1,2);  /* version, flags (BASE), index, count */
        fs_pack(p,d+8,h,4);fs_pack(p,d+12,20,4);fs_pack(p,d+16,i?0:28,4);fs_pack(p,d+20,offsets[i],4);fs_pack(p,d+24,0,4);
    }
    const uint64_t symbols[][3]={{1,VDSO_CODE,0x74},{24,VDSO_CODE+0x74,0x5c}}; /* name, value, size */
    for(unsigned i=0;i<2;i++) {
        size_t s=0x180+i*24;
        fs_pack(p,s,symbols[i][0],4);p[s+4]=0x12;fs_pack(p,s+6,1,2);       /* GLOBAL FUNC, defined */
        fs_pack(p,s+8,symbols[i][1],8);fs_pack(p,s+16,symbols[i][2],8);
    }
    memcpy(p+0x1b0,strings,sizeof(strings));
    memcpy(p+VDSO_CODE,vdso_code,sizeof(vdso_code));
    mach_timebase_info_data_t base;if(mach_timebase_info(&base)!=KERN_SUCCESS || !base.numer || !base.denom)fail("Cannot read the host timebase");
    fs_pack(p,VDSO_DATA+8,base.numer,8);fs_pack(p,VDSO_DATA+16,base.denom,8);
    vdso_interval=UINT64_C(100000000)*base.denom/base.numer;                  /* 100 ms in ticks */
}
/* realtime - monotonic, so wall-clock steps (NTP, manual changes) reach the guest. */
static void vdso_refresh(void) {
    uint64_t before=clock_gettime_nsec_np(CLOCK_UPTIME_RAW),real=clock_gettime_nsec_np(CLOCK_REALTIME),after=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    /* One 8-byte store: guests on other vCPUs may be reading it. */
    __atomic_store_n((uint64_t *)(void *)(backing_data(vdso_backing)+VDSO_DATA+24),real-(before+(after-before)/2),__ATOMIC_RELEASE);
    vdso_refreshed=mach_absolute_time();
}
static void vdso_start(uint64_t vtimer_offset) {
    fs_pack(backing_data(vdso_backing),VDSO_DATA,vtimer_offset,8);vdso_refresh();
}
static void vdso_tick(void) { if(mach_absolute_time()-vdso_refreshed>vdso_interval)vdso_refresh(); }
#endif
