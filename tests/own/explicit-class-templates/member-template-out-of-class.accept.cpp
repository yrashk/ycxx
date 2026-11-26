// #715: an explicit constructor template of a class template defined outside the class serves direct-initialization ([dcl.fct.spec], [temp.mem])
template <class T> struct W {
    T v;
    template <class U> explicit W(U u);
};

template <class T> template <class U> W<T>::W(U u) : v(u) {}

W<long> a(1);
W<long> b{'b'};
