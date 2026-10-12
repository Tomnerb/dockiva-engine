static volatile int state=30;
static __thread int local=3;
__attribute__((constructor)) static void initialize(void) { state+=12; }
int library_value(void) { return state+local++; }
