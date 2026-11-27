// #715: explicit(!same<U, long>) on a constructor template of an ordinary class makes the int specialization explicit for a parameter's copy-initialization ([over.match.funcs.general], [over.match.copy])
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

struct S {
    long v;
    template <class U> explicit(!same<U, long>) S(U u) : v(u) {}
};

long take(S s) { return s.v; }

long ok() { return take(1L); }
long bad() { return take(1); } //! error
