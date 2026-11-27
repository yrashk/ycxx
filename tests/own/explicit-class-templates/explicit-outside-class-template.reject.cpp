// #715: an explicit-specifier may appear only in the declaration of a constructor within its class definition, not on an out-of-class member definition of a class template ([dcl.fct.spec])
template <class T> struct W {
    T v;
    explicit W(T x);
};

template <class T> explicit W<T>::W(T x) : v(x) {} //! error
