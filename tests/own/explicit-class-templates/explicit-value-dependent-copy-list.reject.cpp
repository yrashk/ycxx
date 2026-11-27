// #715: copy-list-initialization admits explicit candidates, so choosing the explicit specialization Opt<int>::Opt<long> over a viable converting constructor is ill-formed ([over.match.list], [over.match.funcs.general])
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

template <class T> struct Opt {
    T v;
    template <class U> explicit(!same<U, T>) Opt(U u) : v(u) {}
    Opt(double d) : v(0) {}
};

Opt<int> copied = 1L;
Opt<int> copy_list = {1L}; //! error
