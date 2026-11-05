/* Opt-in counters; SIGUSR1 requests a snapshot at an interpreter boundary. */
#include <signal.h>
static int profiling;
static volatile sig_atomic_t profile_requested;
static uint64_t profile_instructions, profile_lookups, profile_probes, profile_cache_hits;
static uint64_t profile_write_faults,profile_blocks; /* blocks: calls that blocked (a scheduler round trip each) */ /* hypervisor: first writes to lazily charged or copy-on-write pages */
static uint64_t profile_syscalls[512];
/* Hypervisor backend (atomic: workers count them without the runtime lock): calls
   and faults served without it, exits that took it, and time spent waiting for it. */
static uint64_t profile_unlocked_calls,profile_locked_exits,profile_lock_waits,profile_lock_wait_ns,profile_mm_waits,profile_mm_wait_ns,profile_spurious_faults,profile_fault_around,profile_futex_inplace,profile_futex_inplace_woken,profile_inplace_waits,profile_inplace_woken;
static uint64_t profile_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
#define PROFILE_ADD(counter,n) do { if(profiling)__atomic_fetch_add(&(counter),(n),__ATOMIC_RELAXED); } while(0)
static uint64_t profile_decode_checks, profile_decode_hits, profile_decode_misses;
/* Fork costs (hypervisor): backing snapshots; table builds, in-place syncs and
   whole-arena passes (count, ns, pages mapped); write faults by what they needed
   (an owned page remapped, a page made private and charged, a snapshot). */
static uint64_t profile_snapshots,profile_snapshot_ns,profile_builds,profile_build_ns,profile_partials,profile_partial_ns,profile_arena_passes,profile_arena_ns,profile_arena_pages;
static uint64_t profile_fault_owned,profile_fault_charged,profile_fault_snapshot,profile_overlays,profile_overlay_copies,profile_view_copies;
static uint64_t profile_holds,profile_hold_waits,profile_hold_wait_ns,profile_hold_timeouts; /* fork holds: holds, waits (and their time), waits that ran out (the write copies) */
/* Exit accounting (hypervisor, --profile): per class (syscall number; 512 write
   fault, 513 preemption, 514 other exception) the exits served without the runtime
   lock and with it, and the host time from the exit until the guest resumes (or the
   slice ends). Worker time: in hv_vcpu_run, waiting (idle, in-place futex waits,
   short sleeps, direct socket waits) and in the scheduler between slices. */
#define PROFILE_CLASSES 515
static uint64_t profile_exit_n[2][PROFILE_CLASSES],profile_exit_ns[2][PROFILE_CLASSES];
static uint64_t profile_guest_ns,profile_wait_ns[4],profile_sched_ns,profile_slices;
static _Thread_local uint64_t profile_waited_ns,profile_exit_at,profile_slice_end; /* this worker's waits since the exit; its pending exit; its last slice's end */
static _Thread_local int profile_class=-1,profile_locked;

static inline void profile_wait(unsigned kind,uint64_t since) {
    if(!profiling)return;
    uint64_t ns=profile_now_ns()-since;profile_waited_ns+=ns;__atomic_fetch_add(&profile_wait_ns[kind],ns,__ATOMIC_RELAXED);
}
/* The pending exit's handling ended (the guest resumes, or the slice returns). */
static inline void profile_exit_done(void) {
    if(!profiling || profile_class<0)return;
    uint64_t ns=profile_now_ns()-profile_exit_at;ns=ns>profile_waited_ns?ns-profile_waited_ns:0;
    __atomic_fetch_add(&profile_exit_n[profile_locked][profile_class],1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profile_exit_ns[profile_locked][profile_class],ns,__ATOMIC_RELAXED);
    profile_class=-1;profile_locked=0;profile_waited_ns=0;
}
static void profile_signal(int number) { (void)number; profile_requested=1; }
static uint64_t dbg_n,dbg_wake,dbg_lock,dbg_mm,dbg_spin;
#ifdef __APPLE__
#include <mach/mach.h>
#endif
static void profile_dump(void) {
    if(!profiling) return;
#ifdef __APPLE__
    { task_events_info_data_t ev;mach_msg_type_number_t n=TASK_EVENTS_INFO_COUNT;   /* host page faults: guest time hides them */
      if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&ev,&n)==KERN_SUCCESS)fprintf(stderr,"VMFREE_HOST {\"faults\":%d,\"cow_faults\":%d,\"pageins\":%d}\n",ev.faults,ev.cow_faults,ev.pageins); }
#endif
    if(dbg_n)fprintf(stderr,"DBG n=%llu wake=%llu lock=%llu mm=%llu spun=%llu\n",(unsigned long long)dbg_n,(unsigned long long)(dbg_wake/dbg_n),(unsigned long long)(dbg_lock/dbg_n),(unsigned long long)(dbg_mm/dbg_n),(unsigned long long)dbg_spin);
    fprintf(stderr,"VMFREE_PROFILE {\"unlocked_calls\":%" PRIu64 ",\"locked_exits\":%" PRIu64 ",\"lock_waits\":%" PRIu64 ",\"lock_wait_ms\":%" PRIu64 ",\"mm_waits\":%" PRIu64 ",\"mm_wait_ms\":%" PRIu64 ",\"spurious_write_faults\":%" PRIu64 ",\"fault_around_pages\":%" PRIu64 ",\"futex_inplace\":%" PRIu64 ",\"futex_inplace_woken\":%" PRIu64 ",\"inplace_waits\":%" PRIu64 ",\"inplace_woken\":%" PRIu64 ",\"blocks\":%" PRIu64 ",\"write_faults\":%" PRIu64 ",\"instructions\":%" PRIu64 ",\"memory_lookups\":%" PRIu64 ",\"segment_probes\":%" PRIu64 ",\"syscalls\":{",profile_unlocked_calls,profile_locked_exits,profile_lock_waits,profile_lock_wait_ns/1000000,profile_mm_waits,profile_mm_wait_ns/1000000,profile_spurious_faults,profile_fault_around,profile_futex_inplace,profile_futex_inplace_woken,profile_inplace_waits,profile_inplace_woken,profile_blocks,profile_write_faults,profile_instructions,profile_lookups,profile_probes);
    int comma=0;
    for(unsigned i=0;i<512;i++) if(profile_syscalls[i]) {
        fprintf(stderr,"%s\"%u\":%" PRIu64,comma?",":"",i,profile_syscalls[i]);comma=1;
    }
    fprintf(stderr,"},\"lookup_cache_hits\":%" PRIu64 ",\"decode_checks\":%" PRIu64 ",\"decode_hits\":%" PRIu64 ",\"decode_misses\":%" PRIu64 ",\"allocated_bytes\":%" PRIu64 ",\"peak_allocated_bytes\":%" PRIu64 ",\"allocation_limit_bytes\":%" PRIu64 ",\"allocation_denials\":%" PRIu64 "}\n",profile_cache_hits,profile_decode_checks,profile_decode_hits,profile_decode_misses,resource_used,resource_peak,resource_limit,resource_denials);
    if(profile_snapshots || profile_builds || profile_partials || profile_holds)
        fprintf(stderr,"VMFREE_FORK {\"snapshots\":%" PRIu64 ",\"snapshot_us\":%" PRIu64 ",\"builds\":%" PRIu64 ",\"build_us\":%" PRIu64 ",\"partials\":%" PRIu64 ",\"partial_us\":%" PRIu64 ",\"arena_passes\":%" PRIu64 ",\"arena_us\":%" PRIu64 ",\"arena_pages\":%" PRIu64 ",\"fault_owned\":%" PRIu64 ",\"fault_charged\":%" PRIu64 ",\"fault_snapshot\":%" PRIu64 ",\"holds\":%" PRIu64 ",\"hold_waits\":%" PRIu64 ",\"hold_wait_us\":%" PRIu64 ",\"hold_timeouts\":%" PRIu64 ",\"overlays\":%" PRIu64 ",\"overlay_copies\":%" PRIu64 ",\"view_copies\":%" PRIu64 "}\n",
                profile_snapshots,profile_snapshot_ns/1000,profile_builds,profile_build_ns/1000,profile_partials,profile_partial_ns/1000,profile_arena_passes,profile_arena_ns/1000,profile_arena_pages,profile_fault_owned,profile_fault_charged,profile_fault_snapshot,profile_holds,profile_hold_waits,profile_hold_wait_ns/1000,profile_hold_timeouts,profile_overlays,profile_overlay_copies,profile_view_copies);
    if(profile_slices) {
        fprintf(stderr,"VMFREE_EXITS {\"guest_ms\":%" PRIu64 ",\"wait_idle_ms\":%" PRIu64 ",\"wait_futex_ms\":%" PRIu64 ",\"wait_sleep_ms\":%" PRIu64 ",\"wait_socket_ms\":%" PRIu64 ",\"sched_ms\":%" PRIu64 ",\"slices\":%" PRIu64 ",\"classes\":{",
                profile_guest_ns/1000000,profile_wait_ns[0]/1000000,profile_wait_ns[1]/1000000,profile_wait_ns[2]/1000000,profile_wait_ns[3]/1000000,profile_sched_ns/1000000,profile_slices);
        int first=1;
        for(unsigned i=0;i<PROFILE_CLASSES;i++)if(profile_exit_n[0][i] || profile_exit_n[1][i]) {
            fprintf(stderr,"%s\"%u\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]",first?"":",",i,profile_exit_n[0][i],profile_exit_ns[0][i]/1000,profile_exit_n[1][i],profile_exit_ns[1][i]/1000);first=0;
        }
        fprintf(stderr,"}}\n");
    }
    fflush(stderr);
}
