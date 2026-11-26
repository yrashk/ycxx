// #715: explicit(sizeof(T) > 1) is true for W<int>, so copy-initialization of W<int> from int is ill-formed while W<char> converts ([dcl.fct.spec], [over.match.copy])
template <class T> struct W {
    T v;
    explicit(sizeof(T) > 1) W(T x) : v(x) {}
};

W<char> c_copy = 'a';
W<int> i_copy = 1; //! error
