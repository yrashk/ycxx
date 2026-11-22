namespace N {
    inline namespace V { enum E { item }; }
    struct Wrap { Wrap(E); };
    template<class T> int operator+(Wrap, T) = delete;
}
int use(N::E e) { return e + 1; }
