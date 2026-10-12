#define main adapter_main
#include "../unicorn-adapter/probe.c"
#undef main
int main(int argc,char**argv){
 int virt=argc>1?atoi(argv[1]):0;
 uc_engine*u=engine(code_memory64k,sizeof(code_memory64k),virt,NULL);
 uint64_t n=10000000,expected=4*n*(n-1),runs=0;double start=now();
 do {set(u,UC_ARM64_REG_X0,n);CHECK(uc_emu_start(u,CODE,CODE+sizeof(code_memory64k),3000000,0));assert(get(u,UC_ARM64_REG_X0)==expected);runs++;}while(now()-start<8);
 printf("{\"virtual_tlb\":%d,\"runs\":%llu,\"seconds\":%.9f}\n",virt,(unsigned long long)runs,now()-start);
 CHECK(uc_close(u));return 0;
}
