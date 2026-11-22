#include "shared.h"

int seed_a() { return 7; }
int dynamic_a = seed_a();                 // dynamic initialization: __mod_init_func
extern const char* const greeting_a = "alpha";   // a pointer to __cstring in __DATA,__const
static const char16_t wide_text_a[] = u"wa";
const char16_t* wide_a() { return u"wide-a"; }  // a two-byte string literal in __TEXT,__const
double scale_a = 2.5;
double fconst_a(double d) { return d * 1.75 + 0.125; }
int counter_a() {
  static int n = seed_a();  // a block-scope static with a guard
  return ++n;
}
int (*fp_a[2])(int) = {shared_fn, nullptr};
long (*fp_twice_a)(long) = shared_twice;
int use_table_a(int i) { return table[i] + zeros[i] + counter; }
const char16_t* wide_static_a() { return wide_text_a; }
