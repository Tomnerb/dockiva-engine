/* A directory read while the memory layer (--upper-memory, native/upm.h) writes its
   files back, as GNU tar and find read a Go module cache just downloaded or
   extracted: nobody changes the tree, so every listing must hold each name once
   (getdents64 with small, middling and large buffers), and the directory's times,
   size and mode must stay as they were, by descriptor and by path (between its two
   stats of a directory tar says "file changed as we read it" when the ctime moved or
   the size grew, 1.35 when the mtime or the mode did: every other directory here is
   0555, as Go leaves a module's, and its write-back opens it up for each file; and
   tar leaves out of the archive a name the listing lacked). The mtime
   utimensat set stays through the write-back (tar -x sets a directory's after
   filling it); a file made in the directory moves its times at once, and they stay
   through that file's write-back too. Run by scripts/verify_overlay.py with and
   without the memory layer on an empty upper directory; it then checks the files
   and the directories' times on disk. Prints what went wrong, counted by kind. */
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
#define DIRS 6
#define FILES 300
#define T 1000000000UL                                           /* the mtime utimensat sets */
#define MEMORY (1UL<<62)                                         /* upm.h's inode numbers */
enum { MISSING,EXTRA,TIMES,SIZE,MODE,UTIME,BUMP,STUCK,CALLS,KINDS };
static const char *const kind[KINDS]={"missing","extra","times","size","mode","utime","bump","stuck","calls"};
static U bad[KINDS];
static unsigned char st[128],buffer[32768];
static U u64(const unsigned char *p) { U v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v; }
static unsigned slen(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void say(const char *s) { C3(64,1,s,slen(s)); }
static void say_number(U v) { char d[24];unsigned k=sizeof d;d[--k]=0;do d[--k]=(char)('0'+v%10);while(v/=10);say(d+k); }
static const char stem[]="module-file-name-";
static void dir_path(char *p,int d) { const char *s="/w/d0";unsigned n=0;while(s[n]) { p[n]=s[n];n++; }p[n-1]=(char)('0'+d);p[n]=0; }
static void file_path(char *p,int d,int i) {
    dir_path(p,d);unsigned n=slen(p);p[n++]='/';
    for(unsigned k=0;stem[k];k++)p[n++]=stem[k];
    p[n++]=(char)('0'+i/100);p[n++]=(char)('0'+i/10%10);p[n++]=(char)('0'+i%10);p[n++]='.';p[n++]='g';p[n++]='o';p[n]=0;
}
static int pending(const char *path) { return C4(79,AT,path,st,0)==0 && u64(st+8)>=MEMORY; }
static void put(const char *path) { long fd=C4(56,AT,path,01101,0644);if(fd<0 || C3(64,fd,"x",1)!=1 || C1(57,fd))bad[CALLS]++; }
static U now_ms(void) { U t[2];C2(113,1,t);return t[0]*1000+t[1]/1000000; }
/* One listing of an open directory: each of its FILES names once, "new" as often as fresh says, both dots. */
static void list(long fd,unsigned size,int fresh) {
    static unsigned char seen[FILES];int dots=0,made=0;
    for(int i=0;i<FILES;i++)seen[i]=0;
    for(;;) {
        long n=C3(61,fd,buffer,size);
        if(n<0)bad[CALLS]++;
        if(n<=0)break;
        for(long at=0;at<n;) {
            const char *name=(const char *)buffer+at+19;unsigned k=0;
            while(stem[k] && name[k]==stem[k])k++;
            if(name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2])))dots++;
            else if(name[0]=='n' && name[1]=='e' && name[2]=='w' && !name[3])made++;
            else if(stem[k] || name[k]<'0' || name[k]>'2' || slen(name+k)!=6)bad[EXTRA]++;
            else { int i=(name[k]-'0')*100+(name[k+1]-'0')*10+(name[k+2]-'0');if(i>=FILES || seen[i]++)bad[EXTRA]++; }
            at+=buffer[at+16]|(unsigned)buffer[at+17]<<8;
        }
    }
    for(int i=0;i<FILES;i++)bad[MISSING]+=!seen[i];
    if(dots!=2 || made!=fresh)bad[EXTRA]++;
}
/* A directory's times and size as first seen, and whether the stat just made (r) still shows them. */
static struct { U mtime[2],ctime[2],size,mode; } base[DIRS];
static U mode_of(void) { return st[16]|(U)st[17]<<8|(U)st[18]<<16|(U)st[19]<<24; }
static void keep(int d) { base[d].mtime[0]=u64(st+88);base[d].mtime[1]=u64(st+96);base[d].ctime[0]=u64(st+104);base[d].ctime[1]=u64(st+112);base[d].size=u64(st+48);base[d].mode=mode_of(); }
static void same(int d,long r) {
    if(r) { bad[CALLS]++;return; }
    if(u64(st+88)!=base[d].mtime[0] || u64(st+96)!=base[d].mtime[1] || u64(st+104)!=base[d].ctime[0] || u64(st+112)!=base[d].ctime[1])bad[TIMES]++;
    if(u64(st+48)!=base[d].size)bad[SIZE]++;
    if(mode_of()!=base[d].mode)bad[MODE]++;
}
/* Read directories from..DIRS-1 over and over until none of their sampled files is in memory any more, and a while longer. */
static void read_all(int from,int fresh) {
    static const unsigned sizes[3]={256,1000,sizeof buffer};
    char path[64];U start=now_ms();
    for(unsigned pass=0;;pass++) {
        for(int d=from;d<DIRS;d++) {
            dir_path(path,d);long fd=C4(56,AT,path,040000,0);if(fd<0) { bad[CALLS]++;continue; }
            same(d,C2(80,fd,st));
            list(fd,sizes[(pass+(unsigned)d)%3],d==0?fresh:0);
            same(d,C2(80,fd,st));
            C1(57,fd);
            same(d,C4(79,AT,path,st,0));
        }
        int left=0;
        for(int d=from;d<DIRS;d++) { file_path(path,d,0);left+=pending(path);file_path(path,d,FILES-1);left+=pending(path); }
        if(fresh)left+=pending("/w/d0/new");
        U spent=now_ms()-start;
        if(!left && spent>=500)break;
        if(spent>60000) { bad[STUCK]++;break; }
    }
}
void _start(void) {
    char path[64];U times[4]={T,0,T,0},total=0;
    if(C3(34,AT,"/w",0755))bad[CALLS]++;
    for(int d=0;d<DIRS;d++) {
        dir_path(path,d);if(C3(34,AT,path,0755))bad[CALLS]++;
        for(int i=0;i<FILES;i++) { file_path(path,d,i);put(path); }
    }
    for(int d=0;d<DIRS;d++) {                                              /* as tar -x: the directory's mode and mtime, once it is filled */
        dir_path(path,d);
        if((d&1) && C3(53,AT,path,0555))bad[CALLS]++;                      /* as Go leaves a module's: its write-back opens it up meanwhile */
        if(C4(88,AT,path,times,0) || C4(79,AT,path,st,0))bad[CALLS]++;
        if(u64(st+88)!=T || u64(st+96))bad[UTIME]++;
        if(mode_of()!=(d&1?040555u:040755u))bad[MODE]++;
        keep(d);
    }
    read_all(0,0);
    /* a new file: its directory's times move at once, and then stay */
    U clock[2];C2(113,0,clock);
    if(C4(88,AT,"/w/d0",times,0) || C4(79,AT,"/w/d0",st,0))bad[CALLS]++;
    if(u64(st+88)!=T)bad[UTIME]++;
    put("/w/d0/new");
    if(C4(79,AT,"/w/d0",st,0))bad[CALLS]++;
    if(u64(st+88)+1<clock[0] || u64(st+104)+1<clock[0])bad[BUMP]++;
    keep(0);
    read_all(0,1);
    for(int k=0;k<KINDS;k++)total+=bad[k];
    if(!total)say("dirlist PASS\n");
    else { say("dirlist FAIL");for(int k=0;k<KINDS;k++) { say(" ");say(kind[k]);say("=");say_number(bad[k]); }say("\n"); }
    C1(94,total?1:0);for(;;){}
}
