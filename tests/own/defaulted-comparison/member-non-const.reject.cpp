// [class.compare.default]/1: a defaulted member comparison operator function is
// const-qualified when its parameter is `const C&`.
struct P {
    int x;
    bool operator==(const P&) = default;  //! error
};
