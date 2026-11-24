// [class.compare.default]: a reference member makes the defaulted equality operator deleted,
// also for a default argument, which is a complete-class context ([class.mem.general]).
struct C {
    int& r;
    static const C& s();
    static bool g(bool b = (s() == s()));  //! error
    friend bool operator==(const C&, const C&) = default;
};
