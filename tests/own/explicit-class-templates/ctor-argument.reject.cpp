// #715: passing int to a W<int> parameter is copy-initialization and the explicit constructor is not a candidate ([class.conv.ctor], [over.match.copy])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
};

int take(W<int> w) { return w.v; }

int ok() { return take(W<int>(1)); }
int bad() { return take(1); } //! error
