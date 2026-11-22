// The other side of dependent-template-args.run.cpp, compiled by Clang: declarations only, so
// every call below names a specialization defined by the test's object.
template<bool B, class T, class F> struct cond { using type = T; };
template<class T, class F> struct cond<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond_t = typename cond<B, T, F>::type;
template<class T> constexpr bool always_v = true;
template<class T> constexpr bool narrow_v = sizeof(T) < 4;
template<class T, class U> constexpr bool same_size_v = sizeof(T) == sizeof(U);
template<int N> constexpr bool positive_v = N > 0;
template<int N> struct Tag {};
template<class T> cond_t<always_v<T>, const T&, T&&> mv(T& x);
template<class T> cond_t<!narrow_v<T>, int, long> wide(T);
template<class T> cond_t<same_size_v<T, const T*>, int, long> pointer_sized(T);
template<int N> cond_t<positive_v<N>, int, long> sign(Tag<N>);
template<int N> cond_t<!positive_v<N>, int, long> nonpositive(Tag<N>);
namespace std { inline namespace __y1 {
template<class T> constexpr bool std_v = true;
template<class T> cond_t<std_v<T>, int, long> in_std(T);
}}

int peer() {
  int i = 7;
  char c = 1;
  if (&mv(i) != &i) return 11;
  if (wide(i) + wide(c) != 2) return 12;
  if (pointer_sized(i) + pointer_sized(1L) != 4) return 13;
  if (sign(Tag<4>{}) != 4 || nonpositive(Tag<-2>{}) != 2) return 14;
  if (std::in_std(i) != 3) return 15;
  return 0;
}
