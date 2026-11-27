// #1548: function templates whose signatures hold a value-dependent concept-id as a template
// argument ([temp.names]/8, [temp.dep.constexpr]/2), mangled `X <expression> E` (Itanium C++ ABI
// §5.1.6): the concept-id is its <source-name> and <template-args>, `!e` is `nt e`. Clang and
// GCC agree on each name here. The Clang peer only declares the templates, so its calls link
// against the specializations this object defines.
// PEER: dependent-concept-args-peer.cpp
// EXIT: 0
template<bool B, class T, class F> struct cond { using type = T; };
template<class T, class F> struct cond<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond_t = typename cond<B, T, F>::type;
template<class T> concept Small = sizeof(T) < 4;
template<class T, class U> concept SameSize = sizeof(T) == sizeof(U);

template<class T> cond_t<Small<T>, int, long> small(T) { return 1; }
template<class T> cond_t<!Small<T>, int, long> big(T) { return 2; }
template<class T> cond_t<SameSize<T, const T*>, int, long> pointer_sized(T) { return 3; }
template<class T> typename cond<Small<T>, int, long>::type direct(T) { return 4; }

int peer();

int main() {
  if (sizeof(small('a')) != sizeof(int) || sizeof(small(1L)) != sizeof(long)) return 1;
  if (sizeof(big(1L)) != sizeof(int) || sizeof(big('a')) != sizeof(long)) return 2;
  if (small('a') + big(1L) + pointer_sized(1L) + direct(0) != 10) return 3;
  return peer();
}
