// #715: an explicit constructor of a class template declared in the class and defined outside it serves direct-initialization ([dcl.fct.spec], [class.conv.ctor])
template <class T> struct W {
    T v;
    explicit W(T x);
};

template <class T> W<T>::W(T x) : v(x) {}

W<int> a(1);
W<int> b{2};
W<char> c = W<char>('c');
