// #715: an explicit constructor of a class template serves every direct-initialization ([class.conv.ctor], [dcl.init.general])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
    explicit W(T x, T y) : v(x + y) {}
};

W<int> parens(1);
W<int> braces{1};
W<int> two_args(1, 2);
W<int> two_braced{1, 2};

int use() {
    W<int> a = W<int>(1);
    W<int> b = W<int>{2};
    W<int> c = static_cast<W<int>>(3);
    W<int> d = (W<int>)4;
    W<long> e = W<long>(5, 6);
    return a.v + b.v + c.v + d.v + static_cast<int>(e.v);
}
