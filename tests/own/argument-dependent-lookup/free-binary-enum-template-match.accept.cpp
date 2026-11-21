namespace N {
    inline namespace V { enum E { item }; }
    struct Result {};
    using Fn = Result (*)();
    template<class T> int operator,(T, Fn) { return 1; }
}
int use(N::E e, N::Fn f) { return (e, f); }

namespace Explicit {
struct Wrap { Wrap(N::E); };
template<class T> int operator+(Wrap, T) { return 2; }
}
int named(N::E e) { return Explicit::operator+(e, 1); }
