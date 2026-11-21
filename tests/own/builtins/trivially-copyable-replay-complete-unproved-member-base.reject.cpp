// XFAIL: clang,gcc This valid complete class is outside the current ycxx classifier.
template<bool> struct Flag {};
struct Member { int payload; };
struct Container { Member member; };
template<class T> struct Check : Flag<__is_trivially_copyable(T)> {}; //! error

constexpr auto demand = sizeof(Check<Container>);
