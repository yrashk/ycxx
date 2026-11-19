// ι (#313): an argument that is an overload set takes part in overload resolution: a candidate
// is viable when the set has a member of its parameter's type, and the function-to-pointer
// conversion is an exact match, better than a boolean conversion ([over.match.viable],
// [over.ics.scs], [over.over], [temp.deduct.call]).
// EXIT: 0
int f(int) { return 1; }
int g(long) { return 2; }
int k(int) { return 3; }
int k(long) { return 4; }

int use(int (*fn)(int)) { return 100 + fn(0); }
int use(int (*fn)(long), int extra) { return 200 + fn(0) + extra; }
int use(bool) { return 300; }

int one(int (*fn)(long)) { return 10 + fn(0); }
int one(int (*fn)(const char*)) { return 20; }

template <class T> int apply(int (*fn)(T), T v) { return fn(v); }

int main() {
  // f has only f(int): the pointer overload is an exact match, the bool one a conversion.
  if (use(f) != 101) return 1;
  if (use(&f) != 101) return 2;
  // k has k(int): use(int(*)(int)) is viable and wins.
  if (use(k) != 103) return 3;
  // With two arguments only use(int(*)(long), int) has the right arity; it selects k(long).
  if (use(k, 5) != 209) return 4;
  // g(long) matches use(int(*)(long), int).
  if (use(g, 1) != 203) return 5;
  // Only one(int(*)(long)) has a parameter type the set {k(int), k(long)} can produce.
  if (one(k) != 14) return 6;
  // With T given explicitly, the parameter's type selects k(int) or k(long).
  if (apply<int>(k, 0) != 3) return 7;
  if (apply<long>(k, 0) != 4) return 8;
  // The set {f} has one member: T is deduced from it.
  if (apply(f, 0) != 1) return 9;
  return 0;
}
