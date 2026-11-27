// #715: explicit(!same<U, T>) is true for the deduced specialization Opt<int>::Opt<long>, so copy-initialization from long is ill-formed ([over.match.funcs.general], [over.match.copy])
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

template <class T> struct Opt {
    T v;
    template <class U> explicit(!same<U, T>) Opt(U u) : v(u) {}
};

Opt<int> same_type = 1;
Opt<int> other_type = 1L; //! error
