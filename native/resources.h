/* Aggregate allocation budget shared by every guest task in this interpreter.
   Counts requested payloads plus aligned headers, not total host RSS/libc state. */
#include <stddef.h>
#include <sys/mman.h>
/* Live guest processes per container: the first plus PROCESS_LIMIT children. Task
   slots form a 64-bit mask, and pids double as 8-bit hypervisor ASIDs. */
#define PROCESS_LIMIT 63
typedef union { max_align_t alignment;struct { size_t bytes; } value; } ResourceHeader;
#define RESOURCE_MAPPED ((size_t)1<<(sizeof(size_t)*8-1))
static uint64_t resource_limit=UINT64_C(256)*1024*1024,resource_used,resource_peak,resource_denials;
/* Charge bytes against the budget, or fail; scheduler workers charge concurrently. */
static int resource_charge(uint64_t bytes) {
    uint64_t used=__atomic_load_n(&resource_used,__ATOMIC_RELAXED);
    do {
        if(used>resource_limit || bytes>resource_limit-used) { __atomic_fetch_add(&resource_denials,1,__ATOMIC_RELAXED);return 0; }
    } while(!__atomic_compare_exchange_n(&resource_used,&used,used+bytes,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    used+=bytes;
    uint64_t peak=__atomic_load_n(&resource_peak,__ATOMIC_RELAXED);
    while(used>peak && !__atomic_compare_exchange_n(&resource_peak,&peak,used,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    return 1;
}
static void resource_uncharge(uint64_t bytes) { __atomic_fetch_sub(&resource_used,bytes,__ATOMIC_RELAXED); }
static void *resource_malloc(size_t bytes) {
    if(bytes>SIZE_MAX-sizeof(ResourceHeader) || !resource_charge(bytes+sizeof(ResourceHeader))) { errno=ENOMEM;return NULL; }
    ResourceHeader *p=malloc(sizeof(*p)+bytes);if(!p) { resource_uncharge(bytes+sizeof(ResourceHeader));return NULL; }
    p->value.bytes=bytes+sizeof(*p);
    return p+1;
}
static void *resource_calloc(size_t count,size_t size) {
    if(size && count>SIZE_MAX/size) { resource_denials++;errno=ENOMEM;return NULL; }
    /* Blocks of 1 MiB or more are fresh anonymous memory: zero from the kernel and
       never touched until used (a forked arena's page table is 6 MiB, mostly unused;
       malloc would clear it). The header's top bit marks them for resource_free. */
    size_t bytes=count*size;
    if(bytes>SIZE_MAX/2-sizeof(ResourceHeader) || !resource_charge(bytes+sizeof(ResourceHeader))) { errno=ENOMEM;return NULL; }
    ResourceHeader *p;
    if(bytes>=(1u<<20)) {
        p=mmap(NULL,sizeof(*p)+bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(p==MAP_FAILED) { resource_uncharge(bytes+sizeof(ResourceHeader));errno=ENOMEM;return NULL; }
        p->value.bytes=(bytes+sizeof(*p))|RESOURCE_MAPPED;
    } else {
        p=calloc(1,sizeof(*p)+bytes);if(!p) { resource_uncharge(bytes+sizeof(ResourceHeader));return NULL; }
        p->value.bytes=bytes+sizeof(*p);
    }
    return p+1;
}
static void resource_free(void *p) {
    if(!p)return;ResourceHeader *header=(ResourceHeader *)p-1;size_t bytes=header->value.bytes&~RESOURCE_MAPPED;
    resource_uncharge(bytes);
    if(header->value.bytes&RESOURCE_MAPPED)munmap(header,bytes);else free(header);
}
#define malloc resource_malloc
#define calloc resource_calloc
#define free resource_free
