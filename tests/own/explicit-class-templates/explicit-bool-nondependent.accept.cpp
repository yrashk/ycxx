// #715: explicit(false) and other false constant expressions make a constructor of a class template converting ([dcl.fct.spec], [class.conv.ctor])
template <class T> struct W {
    T v;
    explicit(false) W(T x) : v(x) {}
};

template <class T> struct V {
    T v;
    explicit(sizeof(char) > 1) V(T x) : v(x) {}
};

template <class T> struct M {
    T v;
    template <class U> explicit(0) M(U u) : v(u) {}
};

struct S {
    int v;
    explicit(1 == 2) S(int x) : v(x) {}
};

W<int> w_copy = 1;
W<int> w_list = {1};
V<int> v_copy = 2;
V<int> v_list = {2};
M<long> m_copy = 3;
M<long> m_list = {'m'};
S s_copy = 4;

int take(W<int> w) { return w.v; }
V<int> give() { return 5; }
int use() { return take(6) + give().v + take({7}); }
