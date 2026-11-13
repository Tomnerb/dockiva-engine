/* FIFO opens by threads of one process (Node's libuv pool opens both ends of the
   FIFOs that docker/build-push-action's build-record export pipes through):
   pair: one thread opens /fifo for reading, a sibling for writing, in both orders;
     each open must return (Linux pairs them), the reader gets the writer's byte and
     then end of file. The runtime paired a waiting opener only with another
     process, so the two threads waited for each other forever, and the action's
     post step never exited.
   kill: a threaded child whose thread waits in a FIFO open nobody pairs dies of
     SIGTERM (default action), and its parent reaps it.
   Needs a writable root and --signals. */
typedef unsigned long U;
static long call(U n,U a,U b,U c,U d,U e,U f) {
    register U x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b;
    register U x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory","cc");
    return (long)x0;
}
long spawn(U flags,void *stack,int *ptid,U tls,int *ctid,void (*fn)(void *),void *arg);
__asm__(".global spawn\nspawn:\n"
        "    mov x10, x5\n    mov x11, x6\n    mov x8, #220\n    svc #0\n    cbnz x0, 1f\n"
        "    mov x0, x11\n    blr x10\n    mov x0, #0\n    mov x8, #93\n    svc #0\n"
        "1:  ret\n");
#define FLAGS (0x100|0x200|0x400|0x800|0x10000|0x40000|0x100000|0x200000)
#define AT (U)-100
static void out(const char *s) { U n=0;while(s[n])n++;call(64,1,(U)s,n,0,0,0); }
static void fail(const char *s) { out("fifo-threads FAIL ");out(s);out("\n");call(94,1,0,0,0,0,0);for(;;){} }
static U ms(void) { U ts[2];call(113,1,(U)ts,0,0,0,0);return ts[0]*1000+ts[1]/1000000; }
static void sleep_ms(U n) { U t[2]={n/1000,(n%1000)*1000000};call(101,(U)t,0,0,0,0,0); }
static unsigned char stacks[3][32768] __attribute__((aligned(16)));
static int tids[3];
static volatile long read_fd,write_fd;static volatile int read_done,write_done;
static void reader(void *arg) { (void)arg;read_fd=call(56,AT,(U)"/fifo",0,0,0,0);read_done=1; }
static void writer(void *arg) {
    (void)arg;write_fd=call(56,AT,(U)"/fifo",1,0,0,0);
    if(write_fd>=0) { call(64,(U)write_fd,(U)"x",1,0,0,0);call(57,(U)write_fd,0,0,0,0,0); }
    write_done=1;
}
static void until(volatile int *flag,const char *what) { U start=ms();while(!*flag) { if(ms()-start>5000)fail(what);sleep_ms(1); } }
static void pair(int writer_first) {
    read_done=write_done=0;read_fd=write_fd=-1;
    if(call(33,AT,(U)"/fifo",0010600,0,0,0))fail("mkfifo");
    void (*first)(void *)=writer_first?writer:reader,(*second)(void *)=writer_first?reader:writer;
    if(spawn(FLAGS,stacks[0]+sizeof(stacks[0]),&tids[0],0,&tids[0],first,0)<0)fail("clone");
    sleep_ms(50);                                                   /* the first opener waits */
    if(spawn(FLAGS,stacks[1]+sizeof(stacks[1]),&tids[1],0,&tids[1],second,0)<0)fail("clone");
    until(&read_done,writer_first?"reader after a waiting writer":"reader paired with a sibling writer");
    until(&write_done,writer_first?"writer paired with a sibling reader":"writer after a waiting reader");
    if(read_fd<0 || write_fd<0)fail("open");
    char c[2];
    if(call(63,(U)read_fd,(U)c,2,0,0,0)!=1 || c[0]!='x')fail("read the writer's byte");
    if(call(63,(U)read_fd,(U)c,1,0,0,0)!=0)fail("end of file after the writer closed");
    call(57,(U)read_fd,0,0,0,0,0);
    if(call(35,AT,(U)"/fifo",0,0,0,0))fail("unlink");
    while(tids[0] || tids[1])sleep_ms(1);                           /* CLONE_CHILD_CLEARTID: both threads gone */
}
static void waiting_reader(void *arg) { (void)arg;call(56,AT,(U)"/lonely",0,0,0,0); }
static void kill_waiting(void) {
    if(call(33,AT,(U)"/lonely",0010600,0,0,0))fail("mkfifo");
    long child=call(220,17,0,0,0,0,0);if(child<0)fail("fork");
    if(!child) {
        if(spawn(FLAGS,stacks[2]+sizeof(stacks[2]),&tids[2],0,&tids[2],waiting_reader,0)<0)call(93,3,0,0,0,0,0);
        for(;;)sleep_ms(1000);
    }
    sleep_ms(100);
    if(call(129,(U)child,15,0,0,0,0))fail("kill");
    int status=0;U start=ms();long r;
    while(!(r=call(260,(U)child,(U)&status,1,0,0,0))) { if(ms()-start>5000)fail("SIGTERM did not end a thread waiting in a FIFO open");sleep_ms(5); }
    if(r!=child || (status&0x7f)!=15)fail("child status");
    call(35,AT,(U)"/lonely",0,0,0,0);
}
void _start(void) {
    pair(0);pair(1);kill_waiting();
    out("fifo-threads PASS\n");call(94,0,0,0,0,0,0);
}
