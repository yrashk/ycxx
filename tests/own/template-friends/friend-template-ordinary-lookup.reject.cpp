// [class.friend]/7, [temp.friend]: a friend function template defined in a class is not found by
// qualified or ordinary unqualified lookup.
namespace ops {
struct Meter {
    template <class T>
    friend int scaled(const Meter&, T factor) { return int(factor); }
};

int qualified(const Meter& m) {
    return ops::scaled(m, 2); //! error
}

int parenthesized(const Meter& m) {
    return (scaled)(m, 2); //! error
}
}
