// XFAIL: clang,gcc This valid complete class is outside the current ycxx classifier.
struct Member { int payload; };
struct Container { Member member; };
template<class T> constexpr bool copied = __is_trivially_copyable(T); //! error

constexpr bool observed = copied<Container>;
