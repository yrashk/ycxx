// #773: function templates whose signatures hold a value-dependent template argument, mangled
// `X <expression> E` (Itanium C++ ABI §5.1.6): a dependent variable-template-id is its
// <source-name> and <template-args>, `!e` is `nt e`. The Clang peer only declares the templates,
// so its calls link against the specializations this object defines. Variable templates of the
// global namespace and of ::std, named without `::`, where Clang and GCC agree (#777).
// PEER: dependent-template-args-peer.cpp
// EXIT: 0
template<bool B, class T, class F> struct cond { using type = T; };
template<class T, class F> struct cond<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond_t = typename cond<B, T, F>::type;
template<class T> constexpr bool always_v = true;
template<class T> constexpr bool narrow_v = sizeof(T) < 4;
template<class T, class U> constexpr bool same_size_v = sizeof(T) == sizeof(U);
template<int N> constexpr bool positive_v = N > 0;
template<int N> struct Tag {};

template<class T> cond_t<always_v<T>, const T&, T&&> mv(T& x) { return static_cast<T&&>(x); }
// `!` of a variable-template-id, and a second argument that repeats `T` (a substitution).
template<class T> cond_t<!narrow_v<T>, int, long> wide(T) { return 1; }
template<class T> cond_t<same_size_v<T, const T*>, int, long> pointer_sized(T) { return 2; }
// A non-type template parameter as the variable template's argument.
template<int N> cond_t<positive_v<N>, int, long> sign(Tag<N>) { return N; }
template<int N> cond_t<!positive_v<N>, int, long> nonpositive(Tag<N>) { return -N; }
namespace std { inline namespace __y1 {
template<class T> constexpr bool std_v = true;
template<class T> cond_t<std_v<T>, int, long> in_std(T) { return 3; }
}}

int peer();

int main() {
  int i = 7;
  char c = 1;
  const int& r = mv(i);
  if (&r != &i) return 1;
  if (sizeof(wide(i)) != sizeof(int) || sizeof(wide(c)) != sizeof(long)) return 2;
  if (wide(i) + wide(c) + pointer_sized(i) + pointer_sized(1L) != 6) return 3;
  if (sign(Tag<4>{}) != 4 || nonpositive(Tag<-2>{}) != 2) return 4;
  if (std::in_std(i) != 3) return 5;
  return peer();
}
