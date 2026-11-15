/* A directory of more than 4096 entries lists whole: getdents64 used to answer
   EOVERFLOW at its 4097th record and drop what that call had gathered, which musl and
   busybox take for the end (ls of 5000 files printed 4085 and exited 0; tar would
   leave files out, rm -r could not remove the directory). 5000 files in /big (made
   here: the upper layer's alone, or in memory under --upper-memory) and 5000 in /low
   (the image's too, with its file "keep": a merged listing), read with getdents64
   buffers of 24 (room for a dot, not for a name: EINVAL, and the name comes with the
   next, larger buffer), 2048 (musl's) and 65536 bytes, then again from lseek 0 on the
   same descriptor: every name once. /ctl (the image's alone) holds nine names and one
   with a control character, which getdents64 refuses: EACCES once, after the names
   gathered before it, and the rest follow. Then /big is emptied as busybox rm -r does
   it, unlinking what each getdents64 returned before the next, and removed.
   Run by scripts/verify_overlay.py with and without the memory layer. With it, a
   listing made while files are being written back is not judged (a name can be missing
   then for another reason): one right after the files are made, all still in memory,
   and the rest once all are on disk. Prints what went wrong, counted by kind. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");return (long)x0;
}
#define C1(n,a) call(n,(U)(a),0,0,0,0,0)
#define C2(n,a,b) call(n,(U)(a),(U)(b),0,0,0,0)
#define C3(n,a,b,c) call(n,(U)(a),(U)(b),(U)(c),0,0,0)
#define C4(n,a,b,c,d) call(n,(U)(a),(U)(b),(U)(c),(U)(d),0,0)
#define AT (-100)
#define FILES 5000
#define MEMORY (1UL<<62)                                         /* upm.h's inode numbers */
enum { MISSING,EXTRA,CALLS,REFUSED,REMOVE,STUCK,KINDS };
static const char *const kind[KINDS]={"missing","extra","calls","refused","remove","stuck"};
static U bad[KINDS];static long first_error;
static unsigned char st[128],buffer[65536];
static const char *const dirs[2]={"/big","/low"};
static U u64(const unsigned char *p) { U v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void say(const char *s) { C3(64,1,s,slen(s)); }
static void say_number(U v) { char d[24];unsigned k=sizeof d;d[--k]=0;do d[--k]=(char)('0'+v%10);while(v/=10);say(d+k); }
static void failed(long r) { bad[CALLS]++;if(!first_error)first_error=r; }
static void file_path(char *p,int d,int i) {
    unsigned n=0;while(dirs[d][n]) { p[n]=dirs[d][n];n++; }
    p[n++]='/';p[n++]='f';p[n++]=(char)('0'+i/1000);p[n++]=(char)('0'+i/100%10);p[n++]=(char)('0'+i/10%10);p[n++]=(char)('0'+i%10);p[n]=0;
}
/* The number f1234 names; -1 for another name. */
static int number(const char *name) {
    if(name[0]!='f' || slen(name)!=5)return -1;
    int i=0;for(int k=1;k<5;k++) { if(name[k]<'0' || name[k]>'9')return -1;i=i*10+(name[k]-'0'); }
    return i<FILES?i:-1;
}
static U pending(void) {                                         /* files still in memory */
    char path[16];U n=0;
    for(int d=0;d<2;d++)for(int i=0;i<FILES;i++) { file_path(path,d,i);n+=C4(79,AT,path,st,0)==0 && u64(st+8)>=MEMORY; }
    return n;
}
/* One listing from where the descriptor stands: each of the FILES names once, "keep" as often as kept, both dots. */
static void list(long fd,unsigned size,int kept,U *wrong) {
    static unsigned char seen[FILES];int dots=0,keeps=0;
    for(int i=0;i<FILES;i++)seen[i]=0;
    for(;;) {
        long n=C3(61,fd,buffer,size);
        if(n==-22 && size<32)n=C3(61,fd,buffer,40);
        if(n<0) { wrong[CALLS]++;if(!first_error)first_error=n; }
        if(n<=0)break;
        for(long at=0;at<n;) {
            const char *name=(const char *)buffer+at+19;int i=number(name);
            if(name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2])))dots++;
            else if(name[0]=='k' && name[1]=='e' && name[2]=='e' && name[3]=='p' && !name[4])keeps++;
            else if(i<0 || seen[i]++)wrong[EXTRA]++;
            at+=buffer[at+16]|(unsigned)buffer[at+17]<<8;
        }
    }
    for(int i=0;i<FILES;i++)wrong[MISSING]+=!seen[i];
    if(dots!=2 || keeps!=kept)wrong[EXTRA]++;
}
/* Both directories with a buffer of size bytes, each twice on one descriptor (the second from lseek 0). */
static void read_both(unsigned size,U *wrong) {
    for(int d=0;d<2;d++) {
        long fd=C4(56,AT,dirs[d],040000,0),r;if(fd<0) { failed(fd);continue; }
        list(fd,size,d,wrong);
        if((r=C3(62,fd,0,0)))failed(r);
        list(fd,size,d,wrong);
        C1(57,fd);
    }
}
/* /ctl: g0..g8 once each whatever the buffer, and one EACCES for the tenth name. */
static void read_refused(unsigned size) {
    long fd=C4(56,AT,"/ctl",040000,0);int seen[9]={0},dots=0,errors=0;
    if(fd<0) { failed(fd);return; }
    for(int calls=0;calls<64;calls++) {
        long n=C3(61,fd,buffer,size);
        if(n==-13) { errors++;continue; }
        if(n<0)failed(n);
        if(n<=0)break;
        for(long at=0;at<n;at+=buffer[at+16]|(unsigned)buffer[at+17]<<8) {
            const char *name=(const char *)buffer+at+19;
            if(name[0]=='.')dots++;
            else if(name[0]=='g' && name[1]>='0' && name[1]<='8' && !name[2])seen[name[1]-'0']++;
            else bad[EXTRA]++;
        }
    }
    C1(57,fd);
    for(int i=0;i<9;i++)bad[MISSING]+=seen[i]!=1;
    if(errors!=1 || dots!=2)bad[REFUSED]++;
}
void _start(void) {
    static const unsigned sizes[3]={24,2048,sizeof buffer};
    char path[16];long r;U total=0;
    if((r=C3(34,AT,"/big",0755)))failed(r);
    for(int d=0;d<2;d++)for(int i=0;i<FILES;i++) { file_path(path,d,i);long fd=C4(56,AT,path,01101,0644);if(fd<0)failed(fd);else C1(57,fd); }
    /* All still in memory (or none ever was: no memory layer): judged if no write-back began meanwhile. */
    U early[KINDS]={0},born=pending();
    read_both(sizeof buffer,early);
    if(pending()==born)for(int k=0;k<KINDS;k++)bad[k]+=early[k];
    else C3(64,2,"bigdir: the first listing is not judged, the write-back began\n",62);
    read_refused(sizeof buffer);read_refused(48);
    U ms[2]={0,50000000};
    for(unsigned waited=0;pending();waited++) { if(waited>2400) { bad[STUCK]++;break; }C2(101,ms,0); }
    if(!bad[STUCK])for(unsigned s=0;s<3;s++)read_both(sizes[s],bad);
    /* rm -r: unlink what each call returned, then the next call; the directory is empty at the end. */
    long fd=C4(56,AT,"/big",040000,0);U removed=0;
    if(fd<0)failed(fd);
    else for(;;) {
        long n=C3(61,fd,buffer,2048);
        if(n<0)failed(n);
        if(n<=0)break;
        for(long at=0;at<n;at+=buffer[at+16]|(unsigned)buffer[at+17]<<8) {
            const char *name=(const char *)buffer+at+19;
            if(name[0]=='.')continue;
            if(C3(35,fd,name,0))bad[REMOVE]++;else removed++;
        }
    }
    if(fd>=0)C1(57,fd);
    if(removed!=FILES || C3(35,AT,"/big",0x200))bad[REMOVE]++;
    for(int k=0;k<KINDS;k++)total+=bad[k];
    if(!total)say("bigdir PASS\n");
    else {
        say("bigdir FAIL");for(int k=0;k<KINDS;k++) { say(" ");say(kind[k]);say("=");say_number(bad[k]); }
        say(" first-error=-");say_number((U)-first_error);say("\n");
    }
    C1(94,total?1:0);for(;;){}
}
