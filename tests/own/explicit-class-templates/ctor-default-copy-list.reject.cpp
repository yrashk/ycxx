// #715: copy-list-initialization from {} of a class template with an explicit default constructor is ill-formed ([over.match.list], [dcl.init.list])
template <class T> struct W {
    T v;
    explicit W() : v(0) {}
};

W<int> direct_list{};
W<int> copy_list = {}; //! error
