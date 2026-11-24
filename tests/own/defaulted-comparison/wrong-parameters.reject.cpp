// [class.compare.default]/1: a defaulted comparison operator function has two parameters of
// type `const C&` or two of type `C`.
struct O {
    int v;
    friend bool operator==(O, int) = default;  //! error
};
