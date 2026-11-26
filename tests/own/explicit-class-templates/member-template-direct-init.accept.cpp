// #715: an explicit constructor template, in an ordinary class and in a class template, serves direct-initialization after deduction ([class.conv.ctor], [temp.mem])
struct S {
    long v;
    template <class U> explicit S(U u) : v(u) {}
};

template <class T> struct W {
    T v;
    template <class U> explicit W(U u) : v(u) {}
};

S s_parens(1);
S s_braces{'c'};
W<long> w_parens(1);
W<long> w_braces{true};

long use() {
    S a = S(2);
    S b = static_cast<S>(3);
    W<int> c = W<int>(4L);
    W<int> d = static_cast<W<int>>('d');
    return a.v + b.v + c.v + d.v;
}
