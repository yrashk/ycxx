enum E { item };
E value();
namespace N {
    struct Result {};
    typedef Result (*Fn)();
    struct Wrap { Wrap(E); };
    int operator,(Wrap, Fn) = delete;
}
N::Fn use(N::Fn f) { return (value(), f); }
