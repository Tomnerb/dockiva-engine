/* Go supervisor liveness guard and resource-limit launcher. No guest ABI here. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void stop(int signal) { (void)signal;stopping=1; }
static void limit(int resource,rlim_t value) { struct rlimit lim={value,value};if(setrlimit(resource,&lim)) { perror("worker limit");exit(125); } }
int main(int argc,char **argv) {
    if(argc<2 || argv[1][0]!='/')return 125;
    struct stat st;if(fstat(3,&st) || !S_ISFIFO(st.st_mode))return 125;
    /* No RLIMIT_CPU: containers are long-running services (a 60 s CPU cap killed a busy
       server within minutes), and with any CPU limit set Hypervisor.framework never
       enters the guest. Stopping a container is the user's control, as with Docker. */
    /* The host file-size cap follows the runtime's --file-limit-mib (16 MiB by default). */
    rlim_t file_size=16*1024*1024;
    for(int i=2;i+1<argc;i++)if(!strcmp(argv[i],"--file-limit-mib")) { long long mib=atoll(argv[i+1]);if(mib>0 && mib<=1048576)file_size=(rlim_t)mib<<20; }
    limit(RLIMIT_CORE,0);limit(RLIMIT_FSIZE,file_size);limit(RLIMIT_NOFILE,4096); /* guest sockets and files of every process; at most NET_DESCRIPTIONS */
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL)||sigaction(SIGINT,&action,NULL))return 125;
    pid_t child=fork();if(child<0)return 125;
    if(!child) { close(3);execv(argv[1],argv+1);perror("worker exec");_exit(125); }
    unsigned stopping_ticks=0;
    for(;;) {
        int status;pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child)return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
        if(result<0&&errno!=EINTR)return 125;
        /* The supervisor signals the whole process group, so the runtime already has
           the stop signal (repeating it can force an unclean exit); kill after 10 s. */
        if(stopping && ++stopping_ticks>=100)(void)kill(child,SIGKILL);
        struct pollfd watch={3,POLLIN|POLLHUP,0};int ready=poll(&watch,1,100);
        if(ready>0 || (ready<0&&errno!=EINTR)) {
            /* Our dedicated process group contains only this guard and worker.
               Supervisor crash closes the pipe; never leave an orphan guest. */
            (void)kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}
            return 125;
        }
    }
}
