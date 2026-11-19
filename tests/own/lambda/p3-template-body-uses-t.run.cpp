// θ step 3 (#281), spec-only: the template parameters of a template lambda are usable in its body like
// those of any function template: to declare locals, in sizeof, in casts and as template arguments
// ([expr.prim.lambda.general], [expr.prim.lambda.closure], [temp.res]).
template<class A, class B> struct same { static constexpr bool value = false; };
template<class A> struct same<A, A> { static constexpr bool value = true; };
template<class T> struct holder { T value; };

int main() {
  auto copy = []<class T>(T x) { T y = x; y = y + 1; return y; };
  if (copy(1) != 2) return 1;
  if (copy('a') != 'b') return 2;
  auto size = []<class T>(T) { return sizeof(T); };
  if (size('a') != 1) return 3;
  if (size(1L) != sizeof(long)) return 4;
  auto wrap = []<class T>(T x) { return holder<T>{x}; };
  if (!same<decltype(wrap('c')), holder<char>>::value) return 5;
  if (wrap(9).value != 9) return 6;
  auto zero = []<class T>(T) { T z{}; return z; };
  if (zero(5) != 0) return 7;
  auto narrow = []<class T>(long v, T) { return static_cast<T>(v); };
  if (!same<decltype(narrow(300L, 'a')), char>::value) return 8;
  if (narrow(65L, 'a') != 'A') return 9;
  auto arr = []<class T>(T x) { T a[3] = {x, x, x}; return a[0] + a[1] + a[2]; };
  if (arr(4) != 12) return 10;
  return 0;
}
