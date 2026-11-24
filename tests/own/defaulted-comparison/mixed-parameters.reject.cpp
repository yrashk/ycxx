// [class.compare.default]/1: both parameters are `const C&`, or both are `C`.
struct O {
    int v;
    friend bool operator==(const O&, O) = default;  //! error
};
