// #715: copy-list-initialization that selects an explicit constructor of a class template is ill-formed ([over.match.list])
template <class T> struct W {
    T v;
    explicit W(T x) : v(x) {}
};

W<int> direct_list{1};
W<int> copy_list = {1}; //! error
