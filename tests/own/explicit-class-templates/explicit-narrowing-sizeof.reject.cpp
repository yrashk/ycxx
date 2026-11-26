// #715: the draft example in a class template: explicit(sizeof(char[2])) narrows 2 to bool and is ill-formed; explicit(sizeof(char)) is fine ([dcl.fct.spec])
template <class T> struct S {
    explicit(sizeof(char[2])) S(char) {} //! error
    explicit(sizeof(char)) S(bool) {}
};
S<int> s(true);
