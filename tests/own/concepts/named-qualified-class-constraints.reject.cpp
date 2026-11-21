namespace N {
    template<class T> concept I = __is_same(T, int);
    template<I T> struct X;
}
N::X<int>* good;
N::X<double>* bad; //! error
