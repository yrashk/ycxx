// [temp.arg.nontype], [temp.inst], [expr.sizeof], [dcl.pre]
// XFAIL: clang,gcc report follow-on static assertion errors during recovery
template<int I>
constexpr int source = I;

template<int I>
constexpr int offset = source<I - 1>; //! error

template<int I>
struct NeedsValue {
    static_assert(offset<I> == 0);
    int payload;
};

static_assert(sizeof(NeedsValue<(-2147483647 - 1)>) == 0);
