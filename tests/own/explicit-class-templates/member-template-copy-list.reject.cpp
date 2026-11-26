// #715: copy-list-initialization that selects an explicit constructor template specialization is ill-formed ([over.match.list])
template <class T> struct W {
    T v;
    template <class U> explicit W(U u) : v(u) {}
};

W<long> direct_list{1};
W<long> copy_list = {1}; //! error
