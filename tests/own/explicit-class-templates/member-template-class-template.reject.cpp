// #715: copy-initialization does not consider an explicit constructor template of a class template ([class.conv.ctor], [over.match.copy])
template <class T> struct W {
    T v;
    template <class U> explicit W(U u) : v(u) {}
};

W<long> direct(1);
W<long> copied = 1; //! error
