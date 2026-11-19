// ι (#313): a function template in the overload set is specialized by deducing its template
// arguments from the target type, the return type included ([over.over], [temp.deduct.funcaddr]).
// EXIT: 0
template <class T> int size_of(T) { return static_cast<int>(sizeof(T)); }

template <class R, class A> R convert(A a) { return a; }

template <class T> T id(T v) { return v; }

int call_char(int (*fn)(char)) { return fn('x'); }

int main() {
  int (*p)(double) = size_of;
  if (p(1.5) != static_cast<int>(sizeof(double))) return 1;
  int (*q)(char) = &size_of;
  if (q('a') != 1) return 2;
  // R comes from the target's return type, A from its parameter.
  long (*c)(int) = convert;
  if (c(7) != 7L) return 3;
  int (&r)(short) = convert;
  if (r(9) != 9) return 4;
  // Different targets make different specializations.
  int (*i)(int) = id;
  long (*l)(long) = id;
  if (i(3) != 3 || l(4L) != 4L) return 5;
  // A parameter and an assignment are targets too.
  if (call_char(size_of) != 1) return 6;
  i = &id;
  if (i(11) != 11) return 7;
  return 0;
}
