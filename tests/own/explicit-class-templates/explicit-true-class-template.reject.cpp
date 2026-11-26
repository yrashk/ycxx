// #715: explicit(true) on a constructor of a class template is explicit, so copy-initialization from int is ill-formed ([dcl.fct.spec], [over.match.copy])
template <class T> struct W {
    T v;
    explicit(true) W(T x) : v(x) {}
};

W<int> direct(1);
W<int> copied = 1; //! error
