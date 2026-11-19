// #337 (ι): a switch statement in a constexpr function, evaluated during constant evaluation
// ([dcl.constexpr], [expr.const]), with fallthrough, default, a loop around it and continue. A switch
// on an enumeration is in switch-constexpr-enum.run.cpp.
// EXIT: 0
constexpr int score(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i) {
    switch (i % 4) {
      case 0: total += 1;
      case 1: total += 10; break;
      case 2: continue;
      default: total += 100;
    }
    total += 1000;
  }
  return total;
}

// i=0: 11+1000, 1: 10+1000, 2: skipped, 3: 100+1000, 4: 11+1000
static_assert(score(5) == 4132);
static_assert(score(0) == 0);

constexpr int at_compile_time = score(8);

int main() {
  if (at_compile_time != 2 * 3121) return 1;
  return 0;
}
