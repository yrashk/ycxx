// θ step 3 (#281), spec-only: a generic lambda inside a function template uses the enclosing template's
// parameter alongside its own invented one; each instantiation of the enclosing template has its own
// closure type ([expr.prim.lambda.closure], [temp.inst], [temp.deduct.call]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };

template<class T> long combine(T base, int n) {
  auto mix = [base](auto x) { return static_cast<T>(base + x); };
  long r = mix(n);
  r += mix('\1');
  r += mix(2L);
  return r;
}

template<class T> int sizes() {
  auto both = [](auto x) { return static_cast<int>(sizeof(T) * 10 + sizeof(x)); };
  return both('a');
}

template<class T> bool returnsT() {
  auto f = []<class U>(U u) { return T(u); };
  return same<decltype(f(1)), T>::value;
}

int main() {
  if (combine(10, 1) != 11 + 11 + 12) return 1;
  if (combine(10L, 0) != 10 + 11 + 12) return 2;
  if (combine('\0', 5) != 5 + 1 + 2) return 3;
  if (sizes<char>() != 11) return 4;
  if (sizes<long>() != static_cast<int>(sizeof(long)) * 10 + 1) return 5;
  if (!returnsT<char>()) return 6;
  if (!returnsT<long>()) return 7;
  return 0;
}
