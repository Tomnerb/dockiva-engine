#define main adapter_main
#include "../unicorn-adapter/probe.c"
#undef main
static void write_value(uc_engine*u,uint64_t value){
 set(u,UC_ARM64_REG_X0,value);set(u,UC_ARM64_REG_X1,DATA);set(u,UC_ARM64_REG_X30,CODE+sizeof(code_write));
 CHECK(uc_emu_start(u,CODE,CODE+sizeof(code_write),1000000,0));
}
static uint64_t read_value(uc_engine*u){uint64_t v;CHECK(uc_mem_read(u,DATA,&v,8));return v;}
static unsigned writes;
static void count_write(uc_engine*u,uc_mem_type type,uint64_t address,int size,int64_t value,void*opaque){
 (void)u;(void)value;(void)opaque;assert(type==UC_MEM_WRITE&&address==DATA&&size==8);writes++;
}
static void guards(int virt){
 uc_engine*u=engine(code_write,sizeof(code_write),virt,NULL);uc_hook h;
 write_value(u,1);write_value(u,2);writes=0;
 CHECK(uc_hook_add(u,&h,UC_HOOK_MEM_WRITE,count_write,NULL,DATA,DATA+7));
 for(int i=0;i<100;i++)write_value(u,i);
 assert(writes==100&&read_value(u)==99);CHECK(uc_hook_del(u,h));write_value(u,200);assert(writes==100);
 /* Warm writable mapping, unmap it, then map the same address read-only. */
 CHECK(uc_mem_unmap(u,DATA,SIZE));CHECK(uc_mem_map(u,DATA,SIZE,UC_PROT_READ));
 assert(uc_emu_start(u,CODE,CODE+sizeof(code_write),1000000,0)==UC_ERR_WRITE_PROT);CHECK(uc_close(u));

 u=engine(code_write,sizeof(code_write),virt,NULL);write_value(u,11);
 CHECK(uc_ctl_context_mode(u,UC_CTL_CONTEXT_CPU|UC_CTL_CONTEXT_MEMORY));uc_context*ctx;
 CHECK(uc_context_alloc(u,&ctx));CHECK(uc_context_save(u,ctx));
 write_value(u,22);assert(read_value(u)==22);CHECK(uc_context_restore(u,ctx));assert(read_value(u)==11);
 write_value(u,33);assert(read_value(u)==33);CHECK(uc_context_restore(u,ctx));assert(read_value(u)==11);
 CHECK(uc_context_free(ctx));CHECK(uc_close(u));

 /* A formerly non-executable page becomes code, then guest stores rewrite it. */
 u=engine(code_write,sizeof(code_write),virt,NULL);
 uint64_t stub1=UINT64_C(0xd65f03c0d2800020),stub2=UINT64_C(0xd65f03c0d2800040); /* mov x0,1/2; ret */
 write_value(u,stub1);CHECK(uc_mem_protect(u,DATA,SIZE,UC_PROT_ALL));
 set(u,UC_ARM64_REG_X30,DATA+8);CHECK(uc_emu_start(u,DATA,DATA+8,1000000,0));assert(get(u,UC_ARM64_REG_X0)==1);
 write_value(u,stub2);set(u,UC_ARM64_REG_X30,DATA+8);CHECK(uc_emu_start(u,DATA,DATA+8,1000000,0));assert(get(u,UC_ARM64_REG_X0)==2);
 CHECK(uc_close(u));printf("{\"kind\":\"guards\",\"virtual_tlb\":%d,\"hook_writes\":100,\"snapshot_restores\":2,\"remap_protection\":true,\"guest_code_rewrite\":true,\"passed\":true}\n",virt);
}
int main(void){setbuf(stdout,NULL);for(int v=0;v<2;v++)guards(v);return 0;}
