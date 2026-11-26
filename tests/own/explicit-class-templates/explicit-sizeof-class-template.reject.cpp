// #715: a non-dependent true explicit(sizeof(int) > 1) on a constructor of a class template makes it explicit for copy-list-initialization too ([dcl.fct.spec], [over.match.list])
template <class T> struct W {
    T v;
    explicit(sizeof(int) > 1) W(T x) : v(x) {}
};

W<int> direct{1};
W<int> copy_list = {1}; //! error
