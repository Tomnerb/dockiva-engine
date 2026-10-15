/* fork, vfork, exec and exit under parallel workers. Each round the parent sets its
   working directory and memory (static, data, stack, brk heap, mmap arena) and
   starts four children at once: a fork that checks its copy of everything, rewrites
   it and exits; a fork that changes identity and execs /leaf; a vfork whose child
   stores to the parent's memory and execs /leaf; a fork whose exec fails (ENOENT)
   and leaves its image intact. Meanwhile the parent rewrites its memory. /leaf
   (fork_exec_stress_leaf) checks the exec'd side. Afterwards the parent checks its
   memory, identity and directory are its own, the vfork child's store is visible,
   and the offset of descriptor 7 moved once per reader (open file descriptions are
   shared across fork and exec). Root: /leaf, /data (4096 'x'), /d0, /d1, /d2. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
static void fail(U code) {
    char m[]="fork-exec-stress FAIL xx\n";m[22]=(char)('0'+code/10%10);m[23]=(char)('0'+code%10);
    call(64,1,(U)m,25,0,0,0);call(94,code,0,0,0,0,0);for(;;){}
}
#define STATICS 8192u
#define ARENA (262144u/8)
#define HEAP (65536u/8)
static volatile U statics[STATICS];
static volatile U data[512]={1,2,3};
static volatile U *arena,*heap;
static volatile U vfork_flag;
static void fill(volatile U *p,U n,U tag) { for(U i=0;i<n;i++)p[i]=tag^i; }
static int same(volatile U *p,U n,U tag) { for(U i=0;i<n;i++)if(p[i]!=(tag^i))return 0;return 1; }
static void handler(int signal) { (void)signal; }
/* vfork and exec without touching the stack the suspended parent still uses. */
static long vfork_exec(const char *path,char **argv,volatile U *flag) {
    long result;
    __asm__ volatile("mov x0,#0x4111\n mov x1,#0\n mov x2,#0\n mov x3,#0\n mov x4,#0\n mov x8,#220\n svc #0\n cbnz x0,1f\n"
                     "mov x9,#1\n str x9,[%3]\n mov x0,%1\n mov x1,%2\n mov x2,#0\n mov x8,#221\n svc #0\n"
                     "mov x0,#99\n mov x8,#93\n svc #0\n1: mov %0,x0\n"
                     :"=r"(result):"r"(path),"r"(argv),"r"(flag):"x0","x1","x2","x3","x4","x8","x9","memory","cc");
    return result;
}
static int all_same(volatile U *stack,U tag) {
    return same(statics,STATICS,tag) && same(data,512,tag) && same(arena,ARENA,tag) && same(heap,HEAP,tag) && same(stack,256,tag);
}
static void fill_all(volatile U *stack,U tag) { fill(statics,STATICS,tag);fill(data,512,tag);fill(arena,ARENA,tag);fill(heap,HEAP,tag);fill(stack,256,tag); }
void _start(void) {
    volatile U stack[256];
    arena=(volatile U *)call(222,0,ARENA*8,3,0x22,(U)-1,0);if((long)arena<0)fail(1);
    U base=(U)call(214,0,0,0,0,0,0);if((U)call(214,base+HEAP*8,0,0,0,0,0)!=base+HEAP*8)fail(2);
    heap=(volatile U *)base;
    long fd=call(56,(U)-100,(U)"/data",0,0,0,0);if(fd<0 || call(24,(U)fd,7,0,0,0,0)!=7)fail(3);
    long cloexec=call(56,(U)-100,(U)"/data",0x80000,0,0,0);if(cloexec<0 || call(24,(U)cloexec,8,0x80000,0,0,0)!=8)fail(4);
    U ignore[4]={1,0,0,0},catch[4]={(U)handler,0x04000000,(U)handler,0}; /* SA_RESTORER: never delivered */
    if(call(134,10,(U)ignore,0,8,0,0) || call(134,12,(U)catch,0,8,0,0))fail(5);
    U hup=1;if(call(135,0,(U)&hup,0,8,0,0))fail(6);
    long parent=call(172,0,0,0,0,0,0);
    U offset=0;
    static char leaf[]="/leaf",dirarg[]="d0",root_id[]="0",user_id[]="1000";
    char *leaf_root[]={leaf,dirarg,root_id,0},*leaf_user[]={leaf,dirarg,user_id,0};
    for(U round=1;round<=150;round++) {
        char dir[]="/d0";dir[2]=(char)('0'+round%3);dirarg[1]=dir[2];
        if(call(49,(U)dir,0,0,0,0,0))fail(7);
        U tag=round*0x9e3779b97f4a7c15u;fill_all(stack,tag);vfork_flag=0;
        long pids[4];
        pids[0]=call(220,17,0,0,0,0,0);
        if(!pids[0]) {                                        /* a fork that checks its copy */
            for(int pass=0;pass<2;pass++)if(!all_same(stack,tag))call(93,20,0,0,0,0,0);
            if(call(173,0,0,0,0,0,0)!=parent)call(93,21,0,0,0,0,0);
            U act[4]={0};if(call(134,12,0,(U)act,8,0,0) || act[0]!=(U)handler)call(93,22,0,0,0,0,0); /* handlers survive fork */
            U mask=0;if(call(135,0,0,(U)&mask,8,0,0) || !(mask&1))call(93,23,0,0,0,0,0);
            char c=0;if(call(63,7,(U)&c,1,0,0,0)!=1 || c!='x')call(93,24,0,0,0,0,0);
            if(call(147,1000,1000,1000,0,0,0) || call(174,0,0,0,0,0,0)!=1000)call(93,25,0,0,0,0,0);
            fill_all(stack,~tag);if(!all_same(stack,~tag))call(93,26,0,0,0,0,0);
            call(93,0,0,0,0,0,0);
        }
        pids[1]=call(220,17,0,0,0,0,0);
        if(!pids[1]) {                                        /* a fork that becomes uid 1000 and execs */
            fill_all(stack,tag+1);
            if(call(149,1000,1000,1000,0,0,0) || call(147,1000,1000,1000,0,0,0))call(93,30,0,0,0,0,0);
            call(221,(U)"/leaf",(U)leaf_user,0,0,0,0);call(93,31,0,0,0,0,0);
        }
        pids[2]=vfork_exec("/leaf",leaf_root,&vfork_flag);    /* returns once the child has execed */
        if(pids[2]<=0)fail(8);
        if(vfork_flag!=1)fail(9);                             /* its store went to this memory */
        pids[3]=call(220,17,0,0,0,0,0);
        if(!pids[3]) {                                        /* a failed exec keeps the image */
            if(call(221,(U)"/missing",(U)leaf_root,0,0,0,0)!=-2)call(93,40,0,0,0,0,0);
            if(!all_same(stack,tag))call(93,41,0,0,0,0,0);
            call(93,0,0,0,0,0,0);
        }
        if(pids[0]<0 || pids[1]<0 || pids[3]<0)fail(10);
        fill_all(stack,tag^0x5555);                           /* while the children run */
        for(int k=0;k<4;k++) {
            int status=-1;long done=call(260,(U)-1,(U)&status,0,0,0,0);
            if(done<=0)fail(11);
            if(status) { char m[]="child x status xx\n";m[6]=(char)('0'+(done==pids[0]?0:done==pids[1]?1:done==pids[2]?2:3));
                         m[15]=(char)('0'+((status>>8)&255)/10%10);m[16]=(char)('0'+((status>>8)&255)%10);call(64,1,(U)m,18,0,0,0);fail(12); }
        }
        if(!all_same(stack,tag^0x5555))fail(13);
        if(call(174,0,0,0,0,0,0) || call(175,0,0,0,0,0,0))fail(14);
        char cwd[16];if(call(17,(U)cwd,sizeof(cwd),0,0,0,0)!=4 || cwd[2]!=dir[2])fail(15);
        offset+=3;if((U)call(62,7,0,1,0,0,0)!=offset)fail(16); /* a forked child and two leaves read one byte each */
    }
    call(64,1,(U)"fork-exec-stress PASS\n",22,0,0,0);call(94,0,0,0,0,0,0);for(;;){}
}
