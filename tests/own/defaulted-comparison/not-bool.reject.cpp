// [class.eq]/1: a defaulted `operator==` returns `bool`.
struct O {
    int v;
    friend int operator==(O, O) = default;  //! error
};
