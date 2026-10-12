/* Linux AArch64 LP64 sysinfo is 112 bytes. Never copy the host's struct/ABI.
   Memory describes our accounted allocation budget, not host RAM/RSS. Swap,
   shared-memory pools, buffer caches and high memory are absent. Load-average
   accounting is not implemented; those three fields are reserved as zero. */
static uint64_t sysinfo_started_ms;
static unsigned sysinfo_threads=1;
static uint64_t guest_sysinfo(void) {
    unsigned char *out=lookup(cpu->gpr[0],112,2);
    if(!out)return linux_error(14);
    uint64_t now=net_now(),uptime=now>=sysinfo_started_ms?(now-sysinfo_started_ms)/1000:0;
    unsigned procs=sysinfo_threads;
    for(unsigned i=0;i<process_count;i++)if(process_children[i].pid)procs++;
    /* The legacy synchronous child is executing before its waitable slot exists. */
    if(process_depth && !schedule_active)procs++;
    memset(out,0,112);
    fs_pack(out,0,uptime,8);
    fs_pack(out,32,resource_limit,8);
    fs_pack(out,40,resource_used<resource_limit?resource_limit-resource_used:0,8);
    fs_pack(out,80,procs,2);
    fs_pack(out,104,1,4); /* all memory quantities are bytes */
    return 0;
}
