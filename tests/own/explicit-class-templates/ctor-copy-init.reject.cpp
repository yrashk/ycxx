// #715: copy-initialization from int cannot use the explicit constructor of W<int> ([class.conv.ctor], [over.match.copy])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
};

W<int> direct(1);
W<int> copied = 1; //! error
