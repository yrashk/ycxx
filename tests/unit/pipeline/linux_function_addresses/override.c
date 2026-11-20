#ifdef IFUNC_STEP
static int implementation(int value) { return value + 9; }
static int (*resolve(void))(int) { return implementation; }
int external_step(int value) __attribute__((ifunc("resolve")));
#else
int external_step(int value) { return value + 9; }

extern int step_alias(int value) __attribute__((alias("external_step")));
#endif
