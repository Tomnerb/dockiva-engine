#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include "decode_reference.h"
#include <assert.h>
static void check(uint32_t word) {
    unsigned expected=reference_kind(word);
    decode_cache_enabled=1;
    assert(decode_instruction(word)==expected);
    assert(decode_instruction(word)==expected);
    decode_cache_enabled=0;assert(decode_instruction(word)==expected);
}
static void word(unsigned char *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));
}
int main(void) {
    profiling=1;
    const uint32_t fixed[]={0,UINT32_MAX,0xd4000001u,0xd503201fu,0xd65f03c0u,0xd2800540u,0xd2800ba8u,0x10000000u,0xd53bd040u,0xd51bd040u};
    for(unsigned i=0;i<sizeof(fixed)/sizeof(*fixed);i++)check(fixed[i]);
    uint32_t state=123456789;
    for(unsigned i=0;i<1000000;i++) { state=state*1664525u+1013904223u;check(state); }
    assert(sizeof(decode_cache)<=8192);
    static unsigned char code[12],other[12];
    word(code,0xd2800540u); /* movz x0,42 */
    word(code+4,0xd2800ba8u); /* movz x8,93 */
    word(code+8,0xd4000001u);
    seg[0]=(Segment){0x10020,12,7,code};nseg=1;
    for(unsigned enabled=0;enabled<2;enabled++) {
        decode_cache_enabled=(int)enabled;
        word(code,0xd2800540u);pc=seg[0].base;assert(run(10)==42);
        word(code,0xd28000e0u);pc=seg[0].base;assert(run(10)==7);
        /* Identical ADR word at different PCs must retain PC-relative semantics. */
        word(code,0x10000000u);memcpy(other,code,sizeof(code));
        seg[1]=(Segment){0x10040,12,5,other};nseg=2;
        pc=seg[0].base;assert(run(10)==32);
        pc=seg[1].base;assert(run(10)==64);
    }
    seg[0].flags=6;assert(lookup(seg[0].base,4,1)==NULL);
    assert(profile_decode_hits>0 && profile_decode_misses>0 && profile_decode_checks>0);
    puts("PASS classification equivalence, collisions, unknown words, modified code, PC-relative execution, fetch permissions and bounded storage");
    return 0;
}
