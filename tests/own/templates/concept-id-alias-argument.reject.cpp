// #1548: the concept-id argument of an alias template selects a class without the named member
// once the alias's arguments are known ([temp.alias]/2, [temp.res.general]/4). Clang and GCC
// report it in the alias, ycxx at the alias's use.
template<class T> concept Small = sizeof(T) < 4;
template<bool B, class T, class F> struct cond { using type = T; };
template<class T, class F> struct cond<false, T, F> {};
template<class I> using pick = typename cond<Small<I>, int, long>::type; //! error[clang]
pick<char> fine; //! error[gcc]@-1
pick<double> wrong; //! error[ycxx]
