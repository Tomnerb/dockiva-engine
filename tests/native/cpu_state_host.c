/* Exercise the real interpreter CPU snapshot in process. */
#define main vmfree_program_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>

static GuestCpuState fixture(unsigned seed) {
    GuestCpuState state={0};
    for(unsigned i=0;i<31;i++)state.gpr[i]=seed*100+i;
    state.pc=0x10000;state.sp=0x20000+seed*16;state.tls=0x30000+seed*16;
    state.flag_n=seed&1;state.flag_z=(seed>>1)&1;
    state.flag_c=(seed>>2)&1;state.flag_v=(seed>>3)&1;
    for(unsigned i=0;i<32;i++)for(unsigned j=0;j<16;j++)state.simd[i][j]=(unsigned char)(seed+i*16+j);
    state.fpcr=(seed&3)<<22;state.fpsr=seed&31;
    return state;
}
static void equal_cpu(const GuestCpuState *actual,const GuestCpuState *expected) {
    assert(!memcmp(actual->gpr,expected->gpr,sizeof(actual->gpr)));
    assert(!memcmp(actual->simd,expected->simd,sizeof(actual->simd)));
    assert(actual->pc==expected->pc && actual->sp==expected->sp && actual->tls==expected->tls);
    assert(actual->flag_n==expected->flag_n && actual->flag_z==expected->flag_z);
    assert(actual->flag_c==expected->flag_c && actual->flag_v==expected->flag_v);
    assert(actual->fpcr==expected->fpcr && actual->fpsr==expected->fpsr);
    assert(!actual->exclusive_valid);
}
int main(void) {
    GuestCpuState states[]={fixture(3),fixture(12)},zero={0};
    RuntimeContext contexts[2]={{0},{0}};
    contexts[0].cpu=states[0];contexts[1].cpu=states[1];
    ProcessImage images[2];
    /* mov x0,x19; mov x8,#93; svc #0. Shared immutable code, distinct CPU state. */
    const uint32_t code[]={0xaa1303e0,0xd2800ba8,0xd4000001};
    address_space->seg[0]=(Segment){0x10000,4096,5,backing_alloc(4096,1),0};assert(address_space->seg[0].backing);
    memcpy(segment_data(&address_space->seg[0]),code,sizeof(code));address_space->nseg=1;
    for(unsigned i=0;i<2;i++) { *cpu=states[i];cpu->exclusive_valid=1;image_save(&images[i]); }
    for(unsigned round=0;round<100;round++) {
        unsigned i=round%2;GuestCpuState inactive=contexts[1-i].cpu;
        cpu=&contexts[i].cpu;image_restore(&images[i]);equal_cpu(cpu,&states[i]);
        assert(run(3)==(int)(states[i].gpr[19]&255));
        GuestCpuState expected=states[i];expected.gpr[0]=expected.gpr[19];expected.gpr[8]=93;expected.pc+=12;
        equal_cpu(cpu,&expected);
        equal_cpu(&contexts[1-i].cpu,&inactive);
    }
    image_free();equal_cpu(cpu,&zero);assert(resource_used==0 && address_space->nseg==0);
    cpu=&runtime_context.cpu;memset(cpu,0,sizeof(*cpu));
    puts("PASS: alternating interpreter CPU snapshots, TLS/SIMD preservation, clear and teardown");
    return 0;
}
