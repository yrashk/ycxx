// θ step 3 (#281), spec-only: one generic lambda passed to hand-written for_each, count_if and transform
// templates over arrays of different element types; each algorithm instantiation calls its own
// specialization of the function call operator ([expr.prim.lambda.closure], [temp.deduct.call],
// [expr.call]).
template<class T, class P> int count_if(const T* first, const T* last, P pred) {
  int n = 0;
  for (; first != last; ++first)
    if (pred(*first)) ++n;
  return n;
}
template<class T, class F> F for_each(T* first, T* last, F f) {
  for (; first != last; ++first) f(*first);
  return f;
}
template<class T, class U, class F> U* transform(const T* first, const T* last, U* out, F op) {
  for (; first != last; ++first, ++out) *out = op(*first);
  return out;
}

int main() {
  int ints[5] = {1, 2, 3, 4, 5};
  char chars[4] = {'a', 'B', 'c', 'D'};
  long longs[3] = {10, 20, 30};
  auto even = [](auto v) { return v % 2 == 0; };
  if (count_if(ints, ints + 5, even) != 2) return 1;
  if (count_if(chars, chars + 4, even) != 2) return 2;
  if (count_if(longs, longs + 3, even) != 3) return 3;
  long sum = 0;
  auto add = [&sum](auto v) { sum += v; };
  for_each(ints, ints + 5, add);
  for_each(longs, longs + 3, add);
  if (sum != 75) return 4;
  for_each(ints, ints + 5, [](auto& v) { v *= 2; });
  for_each(chars, chars + 4, [](auto& v) { v = v + 1; });
  if (ints[4] != 10 || chars[0] != 'b') return 5;
  long out[5] = {};
  transform(ints, ints + 5, out, [](auto v) { return v + 1000L; });
  if (out[0] != 1002 || out[4] != 1010) return 6;
  int codes[4] = {};
  transform(chars, chars + 4, codes, [](auto v) { return v - 'a'; });
  if (codes[0] != 1 || codes[2] != 3) return 7;
  return 0;
}
