extern int library_value(void);
extern long write(int,const void *,unsigned long);
extern int __libc_start_main(int (*)(int,char **),int,char **,void *,void *,void *);
static int application(int argc,char **argv) {
    (void)argc;(void)argv;
    if(library_value()!=45 || library_value()!=46)return 7;
    return write(1,"shared library constructor and TLS passed\n",42)==42?0:8;
}
void guest_main(int argc,char **argv) { __libc_start_main(application,argc,argv,0,0,0); }
