/* Freestanding Linux ARM64 C: no libc, no host-side output substitution. */
static volatile unsigned char scratch[64];
static long output(const char *text, unsigned long length) {
    register unsigned long x0 __asm__("x0") = 1;
    register const char *x1 __asm__("x1") = text;
    register unsigned long x2 __asm__("x2") = length;
    register unsigned long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return (long)x0;
}
int guest_main(unsigned long argc, char **argv, char **envp) {
    if (scratch[63] != 0) return 90; /* PT_LOAD zero-filled BSS */
    if (argc != 2) { output("usage: demo DIGITS\n", sizeof("usage: demo DIGITS\n")-1); return 2; }
    unsigned long count = 0, sum = 0;
    const unsigned char *p = (const unsigned char *)argv[1];
    while (*p) {
        if (*p < '0' || *p > '9' || count == 9) {
            output("error: use 1-9 digits\n", sizeof("error: use 1-9 digits\n")-1); return 3;
        }
        scratch[count] = *p - '0';
        sum += scratch[count];
        ++count; ++p;
    }
    if (count == 0) { output("error: use 1-9 digits\n", sizeof("error: use 1-9 digits\n")-1); return 3; }
    unsigned long tens = 0, ones = sum;
    while (ones >= 10) { ones -= 10; ++tens; }
    char result[7] = {'s','u','m','=', '0', '0', '\n'};
    result[4] = (char)('0' + tens);
    result[5] = (char)('0' + ones);
    output(result, 7);
    if (envp[0]) {
        const char *v = envp[0];
        unsigned long n = 0;
        while (v[n] && n < 128) ++n;
        output(v, n); output("\n", 1);
    }
    return (int)sum;
}
