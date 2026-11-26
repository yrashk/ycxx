// #715: a braced argument copy-list-initializes the W<int> parameter; selecting its explicit two-argument constructor is ill-formed ([over.match.list])
template <class T> struct W {
    T v;
    explicit W(T x, T y) : v(x + y) {}
};

int take(W<int> w) { return w.v; }

int ok() { return take(W<int>{1, 2}); }
int bad() { return take({1, 2}); } //! error
