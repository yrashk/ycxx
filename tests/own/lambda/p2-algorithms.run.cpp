// θ step 2 (#280), spec-only: hand-written count_if, for_each and transform over arrays, taking the
// predicate or operation as a template parameter, called with capturing and captureless lambdas
// ([expr.prim.lambda.closure], [expr.prim.lambda.capture], [temp.deduct.call]).
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
  int a[6] = {1, 2, 3, 4, 5, 6};
  if (count_if(a, a + 6, [](int v) { return v % 2 == 0; }) != 3) return 1;
  int limit = 4;
  if (count_if(a, a + 6, [limit](int v) { return v > limit; }) != 2) return 2;
  int sum = 0;
  for_each(a, a + 6, [&sum](int v) { sum += v; });
  if (sum != 21) return 3;
  for_each(a, a + 6, [](int& v) { v *= 10; });
  if (a[5] != 60) return 4;
  auto stateful = for_each(a, a + 6, [n = 0](int) mutable { ++n; return n; });
  if (stateful(0) != 7) return 5;
  long out[6];
  long* end = transform(a, a + 6, out, [](int v) -> long { return v + 1; });
  if (end != out + 6 || out[0] != 11 || out[5] != 61) return 6;
  return 0;
}
