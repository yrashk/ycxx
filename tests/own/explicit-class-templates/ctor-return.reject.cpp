// #715: returning int from a function returning W<int> is copy-initialization; the explicit constructor is not a candidate ([stmt.return], [over.match.copy])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
};

W<int> ok() { return W<int>(1); }
W<int> bad() { return 1; } //! error
