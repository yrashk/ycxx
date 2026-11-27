// #715: copy-list-initialization considers explicit constructors; if the best is explicit it is ill-formed even with a non-explicit one viable ([over.match.list])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
    W(long x) : v(static_cast<T>(x)) {}
};

W<int> ordinary_copy = 1;
W<int> copy_list = {1}; //! error
