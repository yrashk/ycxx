template<class, class> using Ignore = int;
template<class T> struct Nested { using type = typename T::nested; }; //! error

template<class T>
using Code = Ignore<typename T::code, typename Nested<T>::type>;

template<class T> Code<T> select(int);
template<class> long select(...);

struct HasCode { using code = long; };
using Reached = decltype(select<HasCode>(0));
