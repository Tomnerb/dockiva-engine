#ifndef VMFREE_RUNTIME_CONTEXT_H
#define VMFREE_RUNTIME_CONTEXT_H
#include "cpu_state.h"
#include "address_space.h"
/* A scheduler worker's execution context: the CPU state of the guest thread it
   runs and its current address space. One runtime process serves one container. */
typedef struct { GuestCpuState cpu;GuestAddressSpace memory; } RuntimeContext;
#endif
