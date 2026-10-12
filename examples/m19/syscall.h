static long call5(long n,long a,long b,long c,long d,long e) {
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x8 __asm__("x8")=n,x5 __asm__("x5")=0;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");
    return x0;
}
static unsigned length(const char *s) { unsigned n=0;while(s[n])n++;return n; }
static void output(long fd,const char *s) { call5(64,fd,(long)s,length(s),0,0); }
static int equal(const char *a,const char *b) { while(*a && *a==*b) { a++;b++; }return *a==*b; }
static int number(const char *s) { int n=0;if(!*s)return -1;while(*s) { if(*s<'0'||*s>'9')return -1;n=n*10+*s++-'0';if(n>255)return -1; }return n; }
