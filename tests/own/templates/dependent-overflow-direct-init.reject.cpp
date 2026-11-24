// [temp.arg.nontype], [temp.inst], [dcl.init], [dcl.constexpr]
template<int I>
constexpr int source = I;

template<int I>
constexpr int offset = source<I - 1>; //! error

struct Box {
    int value;
    constexpr Box(int input) : value(input) {}
};

constexpr Box object(offset<(-2147483647 - 1)>);
