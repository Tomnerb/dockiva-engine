/* Ordinary POSIX C program; no vmfree headers, syscalls, or special entrypoint. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if(argc==1) return write(1,"static musl OK\n",15)==15?0:2;
    if(argc!=3 || strcmp(argv[1],"cat")) return 7;
    int fd=open(argv[2],O_RDONLY);
    if(fd<0) return errno==ENOENT?3:4;
    char buffer[256];
    for(;;) {
        ssize_t n=read(fd,buffer,sizeof buffer);
        if(n<0) return 5;
        if(!n) break;
        ssize_t at=0;
        while(at<n) {
            ssize_t k=write(1,buffer+at,(size_t)(n-at));
            if(k<=0) return 6;
            at+=k;
        }
    }
    return close(fd)?8:0;
}
