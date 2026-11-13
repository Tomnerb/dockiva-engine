/* Linux AArch64 LP64 sysinfo is 112 bytes. Never copy the host's struct/ABI.
   Values match /proc (fs.h): uptime since the container's start, load averages,
   memory as the budget (or the host's, if smaller) and what the container charged.
   Charges beyond it show as swap in use (resource_swap). Shared-memory pools, buffer
   caches and high memory are absent. */
static unsigned sysinfo_threads=1;
static uint64_t guest_sysinfo(void) {
    unsigned char *out=lookup(cpu->gpr[0],112,2);
    if(!out)return linux_error(14);
    uint64_t now=proc_now_ns(),boot=proc_boot_ns(),uptime=now>boot?(now-boot)/1000000000u:0;
    uint64_t total=proc_mem_total(),used=proc_mem_used();double load[3];proc_load_sample(load);
    unsigned procs=sysinfo_threads;
    for(unsigned i=0;i<process_count;i++)if(process_children[i].pid)procs++;
    /* The legacy synchronous child is executing before its waitable slot exists. */
    if(process_depth && !schedule_active)procs++;
    memset(out,0,112);
    fs_pack(out,0,uptime,8);
    for(unsigned i=0;i<3;i++)fs_pack(out,8+i*8,(uint64_t)(load[i]*65536),8);   /* SI_LOAD_SHIFT 16 */
    fs_pack(out,32,total,8);
    fs_pack(out,40,used<total?total-used:0,8);
    uint64_t swapped=used>total?used-total:0;                                   /* beyond memory: swap in use */
    fs_pack(out,64,resource_swap,8);fs_pack(out,72,swapped<resource_swap?resource_swap-swapped:0,8);
    fs_pack(out,80,procs,2);
    fs_pack(out,104,1,4); /* all memory quantities are bytes */
    return 0;
}
