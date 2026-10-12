#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
int main(void) {
    const char *args[64],*env[64];for(unsigned i=0;i<64;i++){args[i]="guest";env[i]="A=B";}
    guest_uid=70;guest_gid=71;
    unsigned char previous[16];
    for(unsigned run_number=0;run_number<2;run_number++) {
        startup(64,args,64,env);assert(!(sp&15));assert(read_mem(sp,8)==64);
        uint64_t at=sp+8,execfn=read_mem(at,8);
        for(unsigned i=0;i<64;i++,at+=8)assert(!strcmp((char *)lookup(read_mem(at,8),6,4),"guest"));
        assert(read_mem(at,8)==0);at+=8;
        for(unsigned i=0;i<64;i++,at+=8)assert(!strcmp((char *)lookup(read_mem(at,8),4,4),"A=B"));
        assert(read_mem(at,8)==0);at+=8;unsigned random_seen=0,exec_seen=0,features=0;
        for(unsigned i=0;i<32;i++,at+=16) {
            uint64_t key=read_mem(at,8),value=read_mem(at+8,8);if(!key)break;
            if(key==25) { unsigned char *bytes=lookup(value,16,4);assert(bytes);random_seen++;
                if(run_number)assert(memcmp(bytes,previous,16));memcpy(previous,bytes,16); }
            if(key==11 || key==12)assert(value==70);
            if(key==13 || key==14)assert(value==71);
            if(key==31){assert(value==execfn);exec_seen++;}
            if(key==16 || key==26){assert(value==0);features++;}
        }
        assert(random_seen==1 && exec_seen==1 && features==2);image_free();assert(resource_used==0);
    }
    unsigned char code[12];fs_pack(code,0,0xd4000001,4);fs_pack(code,4,0xd2800ba8,4);fs_pack(code,8,0xd4000001,4);
    unsigned char output[16];seg[0]=(Segment){0x1000,12,5,code};seg[1]=(Segment){0x2000,16,6,output};nseg=2;
    for(unsigned call=174;call<=177;call++) { pc=0x1000;regs[8]=call;assert(run(10)==(call<=175?70:71)); }
    for(unsigned call=144;call<=146;call+=2) {
        pc=0x1000;regs[8]=call;regs[0]=call==144?71:70;assert(run(10)==0);
        pc=0x1000;regs[8]=call;regs[0]=0;assert(run(10)==255);
    }
    for(unsigned i=0;i<4;i++){pc=0x1000;regs[8]=(uint64_t[]){99,293,233,167}[i];assert(run(10)==218);}
    pc=0x1000;regs[8]=261;regs[0]=0;regs[1]=3;regs[2]=0;regs[3]=0x2000;
    assert(run(10)==0);assert(le(output,8)==1024*1024 && le(output+8,8)==1024*1024);
    pc=0x1000;regs[8]=261;regs[0]=0;regs[2]=0x2000;assert(run(10)==161);
    pc=0x1000;regs[8]=261;regs[0]=0;regs[2]=0;regs[3]=0xffff;assert(run(10)==242);
    memset(output,0xff,16);pc=0x1000;regs[8]=123;regs[0]=0;regs[1]=16;regs[2]=0x2000;
    assert(run(10)==8);assert(le(output,8)==1 && le(output+8,8)==UINT64_MAX);
    pc=0x1000;regs[8]=123;regs[0]=0;regs[1]=7;assert(run(10)==234);
    pc=0x1000;regs[8]=123;regs[0]=999;regs[1]=8;assert(run(10)==253);
    pc=0x1000;regs[8]=123;regs[0]=0;regs[2]=0xffff;assert(run(10)==242);
    for(unsigned clock=0;clock<2;clock++) {
        regs[0]=clock;regs[1]=0x2000;assert(guest_clock(113)==0);
        uint64_t first_seconds=le(output,8),first_nanos=le(output+8,8);assert(first_nanos<1000000000);
        assert(guest_clock(113)==0);uint64_t second_seconds=le(output,8),second_nanos=le(output+8,8);
        if(clock==1)assert(second_seconds>first_seconds || (second_seconds==first_seconds && second_nanos>=first_nanos));
        assert(guest_clock(114)==0);assert(le(output+8,8)<1000000000);
        regs[1]=0;assert(guest_clock(114)==0);assert(guest_clock(113)==linux_error(14));
    }
    regs[0]=999;regs[1]=0x2000;assert(guest_clock(113)==linux_error(22));
    regs[0]=0;regs[1]=0xffff;assert(guest_clock(113)==linux_error(14));
    unsigned char random_bytes[512],old_random[256];memset(random_bytes,0xa5,sizeof(random_bytes));
    seg[2]=(Segment){0x3000,512,6,random_bytes};nseg=3;
    regs[0]=0x3000;regs[1]=512;regs[2]=0;assert(guest_random()==256);
    assert(random_bytes[256]==0xa5);memcpy(old_random,random_bytes,256);
    regs[2]=1;assert(guest_random()==256);assert(memcmp(old_random,random_bytes,256));
    regs[2]=2;assert(guest_random()==linux_error(22));regs[2]=0;
    regs[0]=0xffff;assert(guest_random()==linux_error(14));regs[1]=0;assert(guest_random()==0);
    regs[0]=0x3000;regs[1]=0x3100;assert(guest_gettimeofday()==0);
    assert(le(random_bytes,8)>0 && le(random_bytes+8,8)<1000000 && le(random_bytes+256,8)==0);
    regs[0]=0;regs[1]=0;assert(guest_gettimeofday()==0);
    regs[1]=0xffff;assert(guest_gettimeofday()==linux_error(14));
    nseg=0;memset(seg,0,sizeof(seg));memset(lookup_cache,0,sizeof(lookup_cache));
    puts("PASS maximum argv/env, aligned aux vector, fresh randomness, executable name and feature policy");return 0;
}
