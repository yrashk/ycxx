using Step = int (*)(int);

#ifdef WEAK_STEP
extern "C" inline int external_step(int value) { return value + 1; }
#else
extern "C" int external_step(int value) { return value + 1; }
#endif

extern "C" Step provider_address() { return &external_step; }
extern "C" int provider_call(int value) { return external_step(value); }
