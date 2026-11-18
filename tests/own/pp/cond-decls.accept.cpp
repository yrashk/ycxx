// [cpp.cond]: conditionals choose between declarations; the kept ones form a well-formed
// translation unit (a skipped alternative would redeclare or be invalid).
#define LEVEL 2
#if LEVEL > 1
int level() { return LEVEL * 10 + 1; }
#else
double level() { return 0.5; }
#endif
#ifdef LEVEL
int configured = 1;
#else
int configured = this is not valid C++;
#endif
#if defined(MISSING) || LEVEL == 3
int extra = 0;
#elif LEVEL == 2
int extra = level() - 21;
#else
#error not reached
#endif
#ifndef LEVEL
undeclared_name x;
#endif
int sum() { return level() + configured + extra; }
