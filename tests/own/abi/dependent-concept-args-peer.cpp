// The other side of dependent-concept-args.run.cpp, compiled by Clang: declarations only, so
// every call below names a specialization defined by the test's object.
template<bool B, class T, class F> struct cond { using type = T; };
template<class T, class F> struct cond<false, T, F> { using type = F; };
template<bool B, class T, class F> using cond_t = typename cond<B, T, F>::type;
template<class T> concept Small = sizeof(T) < 4;
template<class T, class U> concept SameSize = sizeof(T) == sizeof(U);
template<class T> cond_t<Small<T>, int, long> small(T);
template<class T> cond_t<!Small<T>, int, long> big(T);
template<class T> cond_t<SameSize<T, const T*>, int, long> pointer_sized(T);
template<class T> typename cond<Small<T>, int, long>::type direct(T);

int peer() {
  if (small('a') + big(1L) != 3) return 11;
  if (pointer_sized(1L) + direct(0) != 7) return 12;
  return 0;
}
