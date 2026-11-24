// #1099: a builtin's argument list is an expression-list ([expr.call]), so an argument may be a
// pack expansion ([temp.variadic]/5), as in any call. The call stays dependent until the pack is
// expanded; the expanded arguments are then checked and converted as written ones are.
// (`__builtin_nan` takes only a string literal, which a function parameter pack cannot carry; the
// arity test covers it.)
// EXIT: 0
using Size = decltype(sizeof 0);

template <class... A> Size length(A... a) { return __builtin_strlen(a...); }
template <class... A> int compare(A... a) { return __builtin_memcmp(a...); }
template <class... A> const void* find(A... a) { return __builtin_memchr(a...); }
template <class... A> double absolute(A... a) { return __builtin_fabs(a...); }
template <class... A> float absolute_float(A... a) { return __builtin_fabsf(a...); }
template <class... A> double infinity(A... a) { return __builtin_inf(a...); }
template <class... A> double huge(A... a) { return __builtin_huge_val(a...); }
template <class... A> int probe(A... a) { return __builtin_constant_p(a...); }
template <class... A> void trap_if_any(A... a) {
  if (sizeof...(A) > 99) __builtin_trap(a...);
}

// The expansion with written arguments before or after it.
template <class... A> int compare_two(A... a) { return __builtin_memcmp(a..., Size{2}); }
template <class... A> int compare_with(const void* p, A... a) { return __builtin_memcmp(p, a...); }
// With a deduced return type, in a member template of a class template, and in a lambda.
// (A trailing `-> decltype(__builtin_strlen(a...))` waits for #1115, as `decltype(h(a...))` does.)
template <class... A> auto typed(A... a) { return __builtin_strlen(a...); }
template <class... A> struct Outer {
  template <class... B> static Size inner(B... b) { return __builtin_strlen(b...); }
};
template <class... A> Size through_lambda(A... a) {
  return [&] { return __builtin_strlen(a...); }();
}
// A member template of a class template, expanding the class template's pack.
template <class... A> struct Bound {
  template <class N> static int compare(A... a, N n) { return __builtin_memcmp(a..., n); }
  // A pattern naming both templates' packs.
  template <class... B> static int skip(A... a, B... b) { return __builtin_memcmp((a + b)..., Size{1}); }
};
// A generic lambda's own pack, still unbound when the enclosing template is instantiated.
template <class T> int generic(T t) {
  return [](auto... x) { return __builtin_memcmp(x..., Size{2}); }(t, t);
}
// A pattern larger than the pack's name, and a builtin call inside an outer expansion.
template <class... A> Size offset(A... a) { return __builtin_strlen((a + 1)...); }
constexpr Size sum() { return 0; }
template <class... T> constexpr Size sum(Size first, T... rest) { return first + sum(rest...); }
template <class... A> Size lengths(A... a) { return sum(__builtin_strlen(a)...); }
// A non-type pack in a constant expression.
template <int... N> constexpr int constant = __builtin_constant_p(N...);
static_assert(constant<3> == 1);

int main() {
  if (length("abc") != 3) return 1;
  if (compare("ab", "ab", Size{2}) != 0 || compare("ab", "ac", Size{2}) >= 0) return 2;
  const char s[] = "xyz";
  if (find(s, 'y', Size{3}) != s + 1) return 3;
  if (absolute(-2.0) != 2.0 || absolute_float(-1.5f) != 1.5f) return 4;
  if (infinity() <= 1e300 || huge() <= 1e300) return 5;
  if (probe(1) > 1) return 7;  // a parameter: 0 or, folded after inlining, 1
  trap_if_any();
  if (compare_two("ab", "ab") != 0 || compare_with("ab", "ab", Size{2}) != 0) return 8;
  if (typed("ab") != 2) return 9;
  if (Outer<int>::inner("abcd") != 4) return 10;
  if (through_lambda("ab") != 2) return 11;
  if (Bound<const char*, const char*>::compare("ab", "ab", Size{2}) != 0) return 12;
  if (Bound<const char*, const char*>::skip("xab", "yab", 1, 1) != 0) return 15;
  if (Bound<const char*, const char*>::skip("xab", "yac", 2, 2) >= 0) return 16;
  if (offset("abc") != 2) return 13;
  if (lengths("a", "bc", "def") != 6) return 14;
  if (generic("ab") != 0) return 17;
  return 0;
}
