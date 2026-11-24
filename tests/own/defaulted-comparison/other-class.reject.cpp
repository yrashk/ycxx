// [class.compare.default]/1: a defaulted comparison operator function is a friend of the class
// whose objects it compares.
struct Other {
    int v;
};
struct O {
    friend bool operator==(Other, Other) = default;  //! error
};
