// #715: explicit(true) on a constructor template of an ordinary class makes its specializations explicit ([dcl.fct.spec], [over.match.funcs.general])
struct S {
    long v;
    template <class U> explicit(true) S(U u) : v(u) {}
};

S direct(1);
S copied = 1; //! error
