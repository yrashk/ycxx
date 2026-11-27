// #715: explicit(sizeof(char)), explicit(0) and explicit(1) convert to bool without narrowing and are valid in a class template ([dcl.fct.spec], [expr.const.const])
template <class T> struct W {
    T v;
    explicit(sizeof(char)) W(T x) : v(x) {}
    explicit(0) W(T x, T y) : v(x + y) {}
    template <class U> explicit(1) W(U* p) : v(0) {}
};

W<int> direct(1);
W<int> pair = {1, 2};
W<int> from_pointer(static_cast<int*>(nullptr));
