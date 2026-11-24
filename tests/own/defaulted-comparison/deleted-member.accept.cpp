// [class.compare.default], [class.eq]: a member with no usable `==` defines the defaulted
// equality operator as deleted, so an equality comparison of the class is not valid.
struct NoEq {
    int v;
};
struct O {
    NoEq n;
    friend bool operator==(const O&, const O&) = default;
};
struct M {
    NoEq n;
    bool operator==(const M&) const = default;
};
struct U {
    union {
        int i;
    };
    friend bool operator==(U, U) = default;
};
struct P {
    int v;
    friend bool operator==(const P&, const P&) = default;
};

template<class T> concept equality = requires(const T& a) { a == a; };
static_assert(!equality<O>);
static_assert(!equality<M>);
static_assert(!equality<U>);  // [class.compare.default]/2: a variant member
static_assert(equality<P>);
