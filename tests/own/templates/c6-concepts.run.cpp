// ζ c6 (#88): concept definitions, requires-clauses and type-constraints; overload resolution
// drops candidates whose constraints are not satisfied ([temp.constr.constr], [over.match.viable]/3)
// and prefers the more constrained by subsumption ([temp.constr.order], [temp.func.order]/6).
// EXIT: 0
template<class T> concept Ptr = __is_pointer(T);
template<class T> constexpr bool integral = false;
template<> constexpr bool integral<char> = true;
template<> constexpr bool integral<int> = true;
template<> constexpr bool integral<unsigned> = true;
template<class T> concept Int = integral<T>;
template<class T> concept Probe = requires(const T* p) { __builtin_no_such_thing(p); };
template<class T> concept HasV = requires(T t) { t.v; };
struct S { int v; };
static_assert(Ptr<int*> && !Ptr<int> && Int<char> && !Probe<int> && HasV<S> && !HasV<int>);
template<class T> requires Int<T> int f(T) { return 1; }
template<class T> requires Ptr<T> int f(T) { return 2; }
template<Int T> int g(T) { return 1; }
template<class T> int g(T) { return 2; }
template<class T> concept Small = Int<T> && (sizeof(T) <= 2);
template<Int T> int h(T) { return 1; }
template<Small T> int h(T) { return 2; }
template<class T> int k(const T& t) { if constexpr (requires { t.v; }) return t.v; else return 0; }
int main() {
  int x = 0;
  S s{7};
  if (f(1) != 1 || f(&x) != 2) return 1;
  if (g(1) != 1 || g(&x) != 2) return 2;
  if (h(1) != 1 || h((char)1) != 2) return 3;
  if (k(s) != 7 || k(1) != 0) return 4;
  return 0;
}
