/* Linux ARM64 fadvise64(fd, offset, len, advice). Advice is nonbinding:
   validate the guest ABI but leave host cache/readahead policy unchanged.
   In particular DONTNEED neither discards guest data nor substitutes for fsync. */
static uint64_t guest_fadvise(void) {
    uint64_t guest=(uint32_t)cpu->gpr[0];int host=fs_host(guest);
    if(io_managed(guest)) {
        GuestIO *f=&IO(guest);
        if(!f->kind)return linux_error(9);
        if(f->kind==IO_READ || f->kind==IO_WRITE)return linux_error(29);
        if(f->kind!=IO_FILE)return linux_error(95);
        host=f->host;
    }
    if(host<0)return net_host(guest)>=0?linux_error(95):linux_error(9);
    struct stat st;if(fstat(host,&st)<0)return fs_error();
    if(S_ISFIFO(st.st_mode))return linux_error(29);
    if(!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))return linux_error(95);
    int32_t advice=(int32_t)cpu->gpr[3];
    if((int64_t)cpu->gpr[2]<0 || advice<0 || advice>5)return linux_error(22);
    /* Linux permits offsets beyond EOF, including negative offsets here.
       No range arithmetic is needed because no host cache operation is issued. */
    return 0;
}
