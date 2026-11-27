// #715: explicit(!same<U, T>) depends on the constructor template's own parameter and is evaluated after deduction: copy-initialization from T works, from other types only direct-initialization ([over.match.funcs.general], [dcl.fct.spec])
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;

template <class T> struct Opt {
    T v;
    template <class U> explicit(!same<U, T>) Opt(U u) : v(u) {}
};

struct S {
    long v;
    template <class U> explicit(!same<U, long>) S(U u) : v(u) {}
};

Opt<int> o_copy = 1;
Opt<int> o_list = {2};
Opt<int> o_direct(3L);
Opt<int> o_braced{'c'};
S s_copy = 4L;
S s_direct(5);

int take(Opt<int> o) { return o.v; }
int use() { return take(6) + take(Opt<int>(7L)) + static_cast<Opt<int>>(8.0).v; }
