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
#include <fcntl.h>
static volatile sig_atomic_t stopping;
static void stop(int signal) { (void)signal;stopping=1; }
/* SIGCHLD writes here, so the wait below sees the worker's exit at once (a SIGCHLD
   alone would not end poll, and docker run would wait out a 100 ms tick). */
static int exited[2]={-1,-1};
static void child_exited(int signal) { (void)signal;int saved=errno;(void)!write(exited[1],"x",1);errno=saved; }
static void wake(int signal) { (void)signal; }
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
    if(sigaction(SIGTERM,&action,NULL))return 125;
    /* On a terminal (docker exec -t) ^C, ^\ and hangup are the runtime's: it gives them
       to the guest's foreground job. The guard only waits (a handler is reset by exec,
       so the runtime keeps the default); job-control stops would freeze the runtime. */
    int terminal=isatty(0);
    struct sigaction pass={0};pass.sa_handler=wake;sigemptyset(&pass.sa_mask);
    if(sigaction(SIGINT,terminal?&pass:&action,NULL))return 125;
    if(terminal && (sigaction(SIGQUIT,&pass,NULL) || sigaction(SIGHUP,&pass,NULL) || signal(SIGTSTP,SIG_IGN)==SIG_ERR ||
                    signal(SIGTTIN,SIG_IGN)==SIG_ERR || signal(SIGTTOU,SIG_IGN)==SIG_ERR))return 125;
    if(pipe(exited) || fcntl(exited[0],F_SETFD,FD_CLOEXEC) || fcntl(exited[1],F_SETFD,FD_CLOEXEC) || fcntl(exited[1],F_SETFL,O_NONBLOCK))return 125;
    struct sigaction reaped={0};reaped.sa_handler=child_exited;sigemptyset(&reaped.sa_mask);reaped.sa_flags=SA_NOCLDSTOP;
    if(sigaction(SIGCHLD,&reaped,NULL))return 125;
    pid_t child=fork();if(child<0)return 125;
    if(!child) { close(3);signal(SIGCHLD,SIG_DFL);execv(argv[1],argv+1);perror("worker exec");_exit(125); }
    unsigned stopping_ticks=0;
    for(;;) {
        int status;pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child)return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
        if(result<0&&errno!=EINTR)return 125;
        /* The supervisor signals the whole process group, so the runtime already has
           the stop signal (repeating it can force an unclean exit); kill after 10 s. */
        if(stopping && ++stopping_ticks>=100)(void)kill(child,SIGKILL);
        struct pollfd watch[2]={{3,POLLIN|POLLHUP,0},{exited[0],POLLIN,0}};int ready=poll(watch,2,100);
        if(ready>0 && watch[1].revents) { char drain[16];(void)!read(exited[0],drain,sizeof drain);continue; } /* reap above */
        if(ready>0 || (ready<0&&errno!=EINTR)) {
            /* Our dedicated process group contains only this guard and worker.
               Supervisor crash closes the pipe; never leave an orphan guest. */
            (void)kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}
            return 125;
        }
    }
}
