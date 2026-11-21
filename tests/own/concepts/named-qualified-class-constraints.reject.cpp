namespace N {
    template<class T> concept I = __is_same(T, int);
    template<I T> struct X;
}
N::X<int>* good;
N::X<double>* bad; //! error

template<class T> concept IsInt = __is_same(T, int);
template<IsInt T> struct WithMember { using Member = int; };
WithMember<int>::Member* good_member;
WithMember<double>::Member* bad_member; //! error
