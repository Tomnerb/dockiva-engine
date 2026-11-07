#define main vmfree_main
#include "../../native/runtime.c"
#undef main
#include <assert.h>
static uint64_t invoke(uint64_t call,uint64_t a,uint64_t b,uint64_t c) {
    cpu->gpr[0]=a;cpu->gpr[1]=b;cpu->gpr[2]=c;return io_call(call);
}
int main(void) {
    MemoryBacking *backing=backing_alloc(PIPE_CAP+8192,1);assert(backing);
    unsigned char *memory=backing_data(backing);
    address_space->seg[0]=(Segment){0x1000,PIPE_CAP+8192,6,backing,0};address_space->nseg=1;address_space_changed(address_space);
    cpu->gpr[0]=1;cpu->gpr[1]=2048;assert(io_pipe()==linux_error(14));
    cpu->gpr[0]=0x1000;cpu->gpr[1]=0;assert(io_pipe()==linux_error(95));
    cpu->gpr[1]=2049;assert(io_pipe()==linux_error(22));
    cpu->gpr[1]=2048|524288;assert(io_pipe()==0);
    unsigned r=(unsigned)le(memory,4),w=(unsigned)le(memory+4,4);assert(r==3 && w==4); /* the lowest free numbers */
    assert(invoke(25,r,1,0)==1);assert(invoke(25,w,3,0)==2049);
    assert(invoke(63,r,0x2000,1)==linux_error(11));
    assert(invoke(63,w,0x2000,1)==linux_error(9));assert(invoke(64,r,0x2000,1)==linux_error(9));
    assert(invoke(64,w,1,1)==linux_error(14));assert(invoke(64,w,1,0)==0);
    assert(invoke(64,w,0x2000,PIPE_CAP+1)==linux_error(22));
    memset(memory+4096,'x',PIPE_CAP);
    assert(invoke(64,w,0x2000,PIPE_CAP-2)==PIPE_CAP-2);
    assert(invoke(64,w,0x2000,3)==linux_error(11));
    /* Small writev must also be atomic when the pipe has only two bytes free. */
    fs_pack(memory+256,0,0x2000,8);fs_pack(memory+256,8,1,8);
    fs_pack(memory+256,16,0x2000,8);fs_pack(memory+256,24,2,8);
    cpu->gpr[0]=w;cpu->gpr[1]=0x1100;cpu->gpr[2]=2;assert(guest_writev()==linux_error(11));
    assert(io_pipes[0].used==PIPE_CAP-2);
    assert(invoke(64,w,0x2000,4097)==2);assert(invoke(64,w,0x2000,1)==linux_error(11));
    assert(guest_dup(w,1,0,0)==1);assert(io_pipes[0].writers==2);
    assert(guest_dup(999,1,0,0)==linux_error(9));assert(IO(1).kind==IO_WRITE);
    assert(guest_dup(w,64,0,0)==64 && io_pipes[0].writers==3);assert(guest_close(64)==0 && io_pipes[0].writers==2); /* any target number */
    assert(invoke(57,w,0,0)==0);assert(invoke(57,w,0,0)==linux_error(9));
    assert(invoke(63,r,0x2000,PIPE_CAP)==PIPE_CAP);assert(invoke(63,r,0x2000,1)==linux_error(11));
    assert(invoke(57,1,0,0)==0);assert(invoke(63,r,0x2000,1)==0);
    assert(invoke(62,r,0,0)==linux_error(29));assert(invoke(80,r,0x1100,0)==0);assert((le(memory+256+16,4)&0170000)==0010000);
    assert(invoke(57,r,0,0)==0);assert(!io_pipes[0].data);
    cpu->gpr[0]=0x1000;cpu->gpr[1]=2048;assert(io_pipe()==0);r=(unsigned)le(memory,4);w=(unsigned)le(memory+4,4);
    assert(invoke(57,r,0,0)==0);assert(invoke(64,w,0x2000,1)==linux_error(32));assert(invoke(57,w,0,0)==0);
    /* Clone retains endpoints, restores the parent's table, and shares payload. */
    cpu->gpr[0]=0x1000;cpu->gpr[1]=2048|524288;assert(io_pipe()==0);r=(unsigned)le(memory,4);w=(unsigned)le(memory+4,4);
    GuestIO parent[IO_END];assert(io_clone(parent)==0);assert(io_pipes[0].readers==2);
    static unsigned char kinds[GUEST_FD_LIMIT];static unsigned short slots[GUEST_FD_LIMIT],guests[IO_END]; /* the parent's descriptor map, as fork keeps it */
    memcpy(kinds,fd_kind,sizeof(kinds));memcpy(slots,fd_index,sizeof(slots));memcpy(guests,io_guest,sizeof(guests));
    assert(guest_dup(w,1,0,0)==1);io_exec();assert(io_slot(w)<0 && IO(1).kind==IO_WRITE);
    assert(invoke(64,1,0x2000,3)==3);io_restore(parent);assert(io_pipes[0].readers==1 && io_pipes[0].writers==1);
    memcpy(fd_kind,kinds,sizeof(kinds));memcpy(fd_index,slots,sizeof(slots));memcpy(io_guest,guests,sizeof(guests));
    assert(invoke(63,r,0x2000,3)==3);assert(invoke(57,w,0,0)==0);assert(invoke(57,r,0,0)==0);
    /* Exercise both the pipe-object and descriptor-table bounds and reclamation. */
    /* Pipes end at whichever comes first: pipe objects or this process's slots. */
    unsigned pipes=PIPE_SLOTS<(IO_END-3)/2?PIPE_SLOTS:(IO_END-3)/2;
    for(unsigned i=0;i<pipes;i++) { cpu->gpr[0]=0x1000;cpu->gpr[1]=2048;assert(io_pipe()==0); }
    cpu->gpr[0]=0x1000;cpu->gpr[1]=2048;assert(io_pipe()==linux_error(24));
    unsigned dups=0;while((int64_t)guest_dup(3,-1,0,0)>=0)dups++;
    assert(dups+2*pipes+3==IO_END && io_free_slot(0)==-1);io_cleanup(); /* every slot the pipes and stdio left */
    for(unsigned i=0;i<PIPE_SLOTS;i++)assert(!io_pipes[i].data);
    memset(fd_kind+3,0,sizeof(fd_kind)-3); /* io_cleanup released every slot */
    FILE *file=tmpfile();assert(file);FS(0).fd=dup(fileno(file))+1;assert(FS(0).fd>0);
    IO(1)=(GuestIO){IO_FILE,1,dup(fileno(file)),NULL,NULL,NULL,NULL,NULL,NULL};assert(IO(1).host>=0);assert(invoke(25,1,1,0)==1);
    assert(invoke(25,1,2,0)==0);assert(invoke(25,1,1,0)==0);
    fs_writable=0;assert(invoke(64,1,0x2000,2)==linux_error(30));
    fs_writable=1;fs_written=64u*1024u*1024u-1;assert(invoke(64,1,0x2000,2)==linux_error(27));
    fs_written=0;assert(invoke(64,1,0x2000,2)==2);assert(fs_written==2);
    assert(lseek(FS(0).fd-1,0,SEEK_CUR)==2);
    assert(invoke(62,1,FS_FILE_LIMIT,0)==FS_FILE_LIMIT);assert(invoke(64,1,0x2000,1)==linux_error(27));
    assert(invoke(62,1,0,0)==0);assert(invoke(63,1,0x2000,2)==2);
    io_cleanup();close(FS(0).fd-1);FS(0).fd=0;fclose(file);
    image_free();puts("PASS M20 pipe bounds, EOF, EAGAIN, EPIPE, atomic writev, dup ownership, clone, exec and reclamation");return 0;
}
