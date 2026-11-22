#include "shared.h"

extern int dynamic_a;
extern const char* const greeting_a;
const char16_t* wide_a();
extern double scale_a;
double fconst_a(double);
int counter_a();
extern int (*fp_a[2])(int);
extern long (*fp_twice_a)(long);
int use_table_a(int);
const char16_t* wide_static_a();

int seed_b() { return 11; }
int dynamic_b = seed_b();
const char16_t* wide_b() { return u"wide-b"; }
double fconst_b(double d) { return d * 0.5 - 0.25; }
int (*fp_b)(int) = shared_fn;

int check() {
  if (dynamic_a != 7) return 1;
  if (dynamic_b != 11) return 2;
  if (greeting_a[0] != 'a' || greeting_a[4] != 'a' || greeting_a[5] != 0) return 3;
  const char16_t* wa = wide_a();
  if (wa[0] != u'w' || wa[5] != u'a' || wa[6] != 0) return 4;
  const char16_t* wb = wide_b();
  if (wb[0] != u'w' || wb[5] != u'b' || wb[6] != 0) return 5;
  if (scale_a != 2.5) return 6;
  if (fconst_a(2.0) != 3.625) return 7;
  if (fconst_b(2.0) != 0.75) return 8;
  if (counter_a() != 8 || counter_a() != 9) return 9;
  if (fp_a[0] != fp_b || fp_a[0] != shared_fn) return 10;
  if (fp_a[0](5) != 15) return 11;
  if (fp_twice_a != shared_twice) return 12;
  if (use_table_a(2) != 35) return 13;
  if (&table[0] != &table[0] || table[3] != 40) return 14;
  const char16_t* ws = wide_static_a();
  if (ws[0] != u'w' || ws[1] != u'a' || ws[2] != 0) return 15;
  counter = 6;
  if (use_table_a(0) != 16) return 16;
  return 0;
}

int main() { return check(); }
