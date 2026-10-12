/* Bounded instruction-word classification cache. Fetch/permissions are always checked first. */
#define DECODE_SLOTS 1024u
typedef struct { uint32_t word; unsigned kind_plus_one; } DecodeEntry;
static DecodeEntry decode_cache[DECODE_SLOTS];
static int decode_cache_enabled=1;
static unsigned classify_instruction(uint32_t ins) {
    if((ins & 0x1f800000u)==0x12800000u) { if(profiling) profile_decode_checks+=1;return 1; }
    if((ins&0x9f000000u)==0x10000000u) { if(profiling) profile_decode_checks+=2;return 2; }
    if((ins&0x9f000000u)==0x90000000u) { if(profiling) profile_decode_checks+=3;return 3; }
    if((ins&0x7c000000u)==0x14000000u) { if(profiling) profile_decode_checks+=4;return 4; }
    if((ins&0xfffffc1fu)==0xd65f0000u || (ins&0xfffffc1fu)==0xd61f0000u || (ins&0xfffffc1fu)==0xd63f0000u) { if(profiling) profile_decode_checks+=5;return 5; }
    if((ins&0xff000010u)==0x54000000u) { if(profiling) profile_decode_checks+=6;return 6; }
    if((ins&0x7e000000u)==0x34000000u) { if(profiling) profile_decode_checks+=7;return 7; }
    if((ins&0x7e000000u)==0x36000000u) { if(profiling) profile_decode_checks+=8;return 8; }
    if((ins&0x1f800000u)==0x11000000u) { if(profiling) profile_decode_checks+=9;return 9; }
    if((ins&0x1fe00000u)==0x0b200000u) { if(profiling) profile_decode_checks+=10;return 10; }
    if((ins&0x1f200000u)==0x0b000000u) { if(profiling) profile_decode_checks+=11;return 11; }
    if((ins&0xffe0fc00u)==0x9b407c00u || (ins&0xffe0fc00u)==0x9bc07c00u) { if(profiling) profile_decode_checks+=12;return 12; }
    if((ins&0x7fe00000u)==0x1b000000u) { if(profiling) profile_decode_checks+=13;return 13; }
    if((ins&0x1f800000u)==0x12000000u) { if(profiling) profile_decode_checks+=14;return 14; }
    if((ins&0x1f800000u)==0x13000000u) { if(profiling) profile_decode_checks+=15;return 15; }
    if((ins&0x1f000000u)==0x0a000000u) { if(profiling) profile_decode_checks+=16;return 16; }
    if((ins&0x7fe0ffe0u)==0x2a0003e0u) { if(profiling) profile_decode_checks+=17;return 17; }
    if((ins&0x3fe00800u)==0x1a800000u) { if(profiling) profile_decode_checks+=18;return 18; }
    if((ins&0xbfe0fc00u)==0x0e000c00u && ((ins>>16)&31) && !(((ins>>16)&31)&(((ins>>16)&31)-1))) { if(profiling) profile_decode_checks+=19;return 19; }
    if((ins&0xfffffc00u)==0x4e083c00u) { if(profiling) profile_decode_checks+=20;return 20; }
    if((ins&0x3f800000u)==0x3d000000u || (ins&0x3fa00000u)==0x3c000000u) { if(profiling) profile_decode_checks+=21;return 21; }
    if((ins&0xff800000u)==0x3d800000u || (ins&0xffa00000u)==0x3c800000u) { if(profiling) profile_decode_checks+=22;return 22; }
    if((ins&0x3e000000u)==0x2c000000u) { if(profiling) profile_decode_checks+=23;return 23; }
    if((ins&0x3fe00410u)==0x3a400000u) { if(profiling) profile_decode_checks+=24;return 24; }
    if((ins&0x3f000000u)==0x39000000u || (ins&0x3f200000u)==0x38000000u || (ins&0x3f200c00u)==0x38200800u) { if(profiling) profile_decode_checks+=25;return 25; }
    if((ins&0x3e000000u)==0x28000000u) { if(profiling) profile_decode_checks+=26;return 26; }
    if((ins&0xffffffe0u)==0xd53b00e0u) { if(profiling) profile_decode_checks+=27;return 27; }
    if((ins&0xffffffe0u)==0xd53bd040u) { if(profiling) profile_decode_checks+=28;return 28; }
    if((ins&0xffffffe0u)==0xd51bd040u) { if(profiling) profile_decode_checks+=29;return 29; }
    if((ins&0xbf20fc00u)==0x0e001800u || (ins&0xbf20fc00u)==0x0e005800u)return 41; /* UZP1/UZP2. */
    if((ins&0xbff89c00u)==0x0f000400u || (ins&0xbff8dc00u)==0x0f008400u)return 47; /* MOVI 32-bit lanes, LSL. */
    if((ins&0xffbff000u)==0x4c007000u || (ins&0xffbff000u)==0x4c9f7000u)return 48; /* Single full Q register LD1/ST1. */
    if((ins&0xff201fe0u)==0x1e201000u && ((ins>>22)&3)<2)return 49; /* Scalar FMOV immediate. */
    if(ins==0xd503233fu || ins==0xd503237fu || ins==0xd50323bfu || ins==0xd50323ffu)return 30; /* PAC hint space; PAuth not advertised. */
    if((ins&0xffa00c00u)==0x3ca00800u)return 22; /* Q register offset. */
    if((ins&0xbf3ffc00u)==0x0e209800u)return 50; /* CMEQ immediate zero. */
    if((ins&0xffbffc00u)==0x1e204000u)return 51; /* FMOV S/D register. */
    if((ins&0x9f20fc00u)==0x0e201c00u)return 52; /* SIMD logical operations. */
    if((ins&0xbf20fc00u)==0x2e203c00u || (ins&0xbf20fc00u)==0x2e203400u)return 41; /* CMHS/CMHI. */
    if((ins&0x9ff80c00u)==0x0f000400u && ((ins>>12)&1) && ((ins>>12)&15)<=11)return 56; /* ORR/BIC immediate. */
    if((ins&0xbf20fc00u)==0x2e20ac00u)return 41; /* UMINP. */
    if((ins&0xbf20fc00u)==0x0e20bc00u)return 41; /* ADDP vector. */
    if((ins&0x1fe0fc00u)==0x1a000000u)return 64; /* ADC/SBC with optional flags. */
    if((ins&0xbff8fc00u)==0x2f00e400u)return 73; /* MOVI byte mask to D/Q. */
    if((ins&0xbfe0fc00u)==0x0e003c00u)return 72; /* UMOV. */
    if((ins&0xbf3ffc00u)==0x2e213800u)return 69; /* SHLL. */
    if(((ins&0xbf80fc00u)==0x0f005400u || (ins&0x9f80fc00u)==0x0f000400u) && ((ins>>16)&127)>=8)return 70; /* SHL/SSHR/USHR. */
    if((ins&0xffe0fc00u)==0x5e000400u)return 71; /* DUP scalar from lane. */
    if((ins&0x7fa00000u)==0x13800000u)return 67; /* EXTR. */
    if((ins&0x9f80fc00u)==0x0f00a400u)return 68; /* SSHLL/USHLL, lower/upper. */
    if((ins&0xbf200000u)==0x0c000000u)return 66; /* SIMD multiple structures. */
    if((ins&0xffe08400u)==0x6e000400u)return 65; /* INS vector lane. */
    if((ins&0xffe0fc00u)==0x4e001c00u)return 63; /* INS general register to vector lane. */
    if((ins&0xbfe08400u)==0x2e000000u)return 62; /* EXT vectors. */
    if((ins&0xbf3ffc00u)==0x0e31b800u)return 61; /* ADDV integer reduction. */
    if((ins&0xbffffc00u)==0x0e205800u)return 60; /* CNT byte vectors. */
    if((ins&0xfffffc00u)==0x5ef1b800u)return 59; /* ADDP scalar D. */
    if((ins&0xbf20fc00u)==0x2e201000u)return 58; /* UADDW/UADDW2. */
    if((ins&0xbf20fc00u)==0x2e20c000u)return 57; /* UMULL/UMULL2 vector. */
    if((ins&0xbf20fc00u)==0x0e204000u)return 55; /* ADDHN/ADDHN2. */
    if((ins&0x3fa00c00u)==0x3c200800u)return 21; /* Scalar SIMD register offset. */
    if((ins&0xbf80fc00u)==0x0f008400u && ((ins>>16)&127)>=8 && ((ins>>16)&127)<64)return 54; /* SHRN/SHRN2. */
    if((ins&0xbfbffc00u)==0x0d008400u)return 53; /* LD1/ST1 single 64-bit lane. */
    if((ins&0x3fbffc00u)==0x089ffc00u)return 46; /* LDAR/STLR, byte through doubleword. */
    if(ins==0xd503241fu || ins==0xd503245fu || ins==0xd503249fu || ins==0xd50324dfu)return 30; /* BTI on unguarded pages. */
    if(ins==0xd503251fu)return 30; /* CHKFEAT hint: no GCS advertised or enabled. */
    if(ins==0xd503201fu) { if(profiling) profile_decode_checks+=30;return 30; }
    if(ins==0xd4000001u) { if(profiling) profile_decode_checks+=31;return 31; }
    if((ins&0x7fe0f800u)==0x1ac00800u) { if(profiling) profile_decode_checks+=32;return 32; }
    if((ins&0xff600000u)==0x9b200000u) { if(profiling) profile_decode_checks+=33;return 33; }
    if((ins&0x7fff8000u)==0x5ac00000u && ((ins>>10)&31)<=5) { if(profiling) profile_decode_checks+=34;return 34; }
    if((ins&0x7fe0f000u)==0x1ac02000u) { if(profiling) profile_decode_checks+=35;return 35; }
    if((ins&0xffffffe0u)==0x6f00e400u) { if(profiling) profile_decode_checks+=36;return 36; }
    if((ins&0x3fff7c00u)==0x085f7c00u) { if(profiling) profile_decode_checks+=37;return 37; }
    if((ins&0x3fe07c00u)==0x08007c00u) { if(profiling) profile_decode_checks+=38;return 38; }
    if((ins&0xfffff0ffu)==0xd503305fu) { if(profiling) profile_decode_checks+=39;return 39; }
    if((ins&0xbfffffe0u)==0x0f000400u)return 36;
    if((ins&0xfffefc00u)==0x9e660000u || (ins&0xfffefc00u)==0x1e260000u)return 40;
    if((ins&0xbf20fc00u)==0x0e208400u || (ins&0xbf20fc00u)==0x2e208400u ||
       (ins&0xbf20fc00u)==0x2e208c00u || (ins&0xbf20fc00u)==0x2e20a400u ||
       (ins&0xbf20fc00u)==0x0e003800u || (ins&0xbf20fc00u)==0x0e007800u ||
       (ins&0xbfe0fc00u)==0x0e000000u || (ins&0xbfe0fc00u)==0x0ea01c00u)return 41;
    if((ins&0xfffff0ffu)==0xd50330bfu || (ins&0xfffff0ffu)==0xd503309fu || ins==0xd5033fdfu)return 42;
    if((ins&0xbff8fc00u)==0x0f00e400u)return 43;
    if((ins&0xbfffffe0u)==0x2f000400u)return 44;
    if((ins&0xffbffc00u)==0x4c00a000u)return 45;
    if(profiling) profile_decode_checks+=39;
    return 0;
}
static unsigned decode_instruction(uint32_t word) {
    if(!decode_cache_enabled) return classify_instruction(word);
    unsigned index=(word*UINT32_C(2654435761))>>(32-10);
    DecodeEntry *entry=&decode_cache[index];
    if(entry->kind_plus_one && entry->word==word) {
        if(profiling) profile_decode_hits++;
        return entry->kind_plus_one-1;
    }
    if(profiling) profile_decode_misses++;
    unsigned kind=classify_instruction(word);
    *entry=(DecodeEntry){word,kind+1};
    return kind;
}
