#ifndef VMFREE_CPU_STATE_H
#define VMFREE_CPU_STATE_H
#include <stdint.h>

/* Owned CPU state at the execution/ABI boundary. The exclusive-monitor fields
   belong to the interpreter and are cleared on switch. The native backend moves
   this state to and from its vCPU at each exit.
   FPCR/FPSR storage does not add interpreter floating-point instruction support. */
typedef struct {
    uint64_t gpr[32],pc,sp,tls;
    unsigned flag_n,flag_z,flag_c,flag_v;
    unsigned char simd[32][16];
    uint32_t fpcr,fpsr;
    uint64_t exclusive_addr;
    unsigned exclusive_size,exclusive_valid;
    uint64_t altstack_sp,altstack_size; /* sigaltstack, per thread; size 0: disabled */
} GuestCpuState;

#endif
