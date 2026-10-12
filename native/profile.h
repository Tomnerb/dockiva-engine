/* Opt-in counters; SIGUSR1 requests a snapshot at an interpreter boundary. */
#include <signal.h>
static int profiling;
static volatile sig_atomic_t profile_requested;
static uint64_t profile_instructions, profile_lookups, profile_probes, profile_cache_hits;
static uint64_t profile_syscalls[512];
static uint64_t profile_decode_checks, profile_decode_hits, profile_decode_misses;
static void profile_signal(int number) { (void)number; profile_requested=1; }
static void profile_dump(void) {
    if(!profiling) return;
    fprintf(stderr,"VMFREE_PROFILE {\"instructions\":%" PRIu64 ",\"memory_lookups\":%" PRIu64 ",\"segment_probes\":%" PRIu64 ",\"syscalls\":{",profile_instructions,profile_lookups,profile_probes);
    int comma=0;
    for(unsigned i=0;i<512;i++) if(profile_syscalls[i]) {
        fprintf(stderr,"%s\"%u\":%" PRIu64,comma?",":"",i,profile_syscalls[i]);comma=1;
    }
    fprintf(stderr,"},\"lookup_cache_hits\":%" PRIu64 ",\"decode_checks\":%" PRIu64 ",\"decode_hits\":%" PRIu64 ",\"decode_misses\":%" PRIu64 ",\"allocated_bytes\":%" PRIu64 ",\"peak_allocated_bytes\":%" PRIu64 ",\"allocation_limit_bytes\":%" PRIu64 ",\"allocation_denials\":%" PRIu64 "}\n",profile_cache_hits,profile_decode_checks,profile_decode_hits,profile_decode_misses,resource_used,resource_peak,resource_limit,resource_denials);fflush(stderr);
}
