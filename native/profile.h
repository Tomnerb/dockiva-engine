/* Opt-in counters; SIGUSR1 requests a snapshot at an interpreter boundary. */
#include <signal.h>
static int profiling;
static volatile sig_atomic_t profile_requested;
static uint64_t profile_instructions, profile_lookups, profile_probes, profile_cache_hits;
static uint64_t profile_write_faults,profile_blocks; /* blocks: calls that blocked (a scheduler round trip each) */ /* hypervisor: first writes to lazily charged or copy-on-write pages */
static uint64_t profile_syscalls[512];
/* Hypervisor backend (atomic: workers count them without the runtime lock): calls
   and faults served without it, exits that took it, and time spent waiting for it. */
static uint64_t profile_unlocked_calls,profile_locked_exits,profile_lock_waits,profile_lock_wait_ns,profile_mm_waits,profile_mm_wait_ns,profile_spurious_faults,profile_fault_around,profile_futex_inplace,profile_futex_inplace_woken;
static uint64_t profile_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
#define PROFILE_ADD(counter,n) do { if(profiling)__atomic_fetch_add(&(counter),(n),__ATOMIC_RELAXED); } while(0)
static uint64_t profile_decode_checks, profile_decode_hits, profile_decode_misses;
static void profile_signal(int number) { (void)number; profile_requested=1; }
static void profile_dump(void) {
    if(!profiling) return;
    fprintf(stderr,"VMFREE_PROFILE {\"unlocked_calls\":%" PRIu64 ",\"locked_exits\":%" PRIu64 ",\"lock_waits\":%" PRIu64 ",\"lock_wait_ms\":%" PRIu64 ",\"mm_waits\":%" PRIu64 ",\"mm_wait_ms\":%" PRIu64 ",\"spurious_write_faults\":%" PRIu64 ",\"fault_around_pages\":%" PRIu64 ",\"futex_inplace\":%" PRIu64 ",\"futex_inplace_woken\":%" PRIu64 ",\"blocks\":%" PRIu64 ",\"write_faults\":%" PRIu64 ",\"instructions\":%" PRIu64 ",\"memory_lookups\":%" PRIu64 ",\"segment_probes\":%" PRIu64 ",\"syscalls\":{",profile_unlocked_calls,profile_locked_exits,profile_lock_waits,profile_lock_wait_ns/1000000,profile_mm_waits,profile_mm_wait_ns/1000000,profile_spurious_faults,profile_fault_around,profile_futex_inplace,profile_futex_inplace_woken,profile_blocks,profile_write_faults,profile_instructions,profile_lookups,profile_probes);
    int comma=0;
    for(unsigned i=0;i<512;i++) if(profile_syscalls[i]) {
        fprintf(stderr,"%s\"%u\":%" PRIu64,comma?",":"",i,profile_syscalls[i]);comma=1;
    }
    fprintf(stderr,"},\"lookup_cache_hits\":%" PRIu64 ",\"decode_checks\":%" PRIu64 ",\"decode_hits\":%" PRIu64 ",\"decode_misses\":%" PRIu64 ",\"allocated_bytes\":%" PRIu64 ",\"peak_allocated_bytes\":%" PRIu64 ",\"allocation_limit_bytes\":%" PRIu64 ",\"allocation_denials\":%" PRIu64 "}\n",profile_cache_hits,profile_decode_checks,profile_decode_hits,profile_decode_misses,resource_used,resource_peak,resource_limit,resource_denials);fflush(stderr);
}
