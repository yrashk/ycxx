// [class.friend]/7, [basic.lookup.argdep]: a friend defined in a class template is not visible
// to ordinary or qualified lookup in its namespace; only argument-dependent lookup finds it.
namespace geometry {
template <class T>
struct Box {
    T value;
    friend T unwrap(const Box& b) { return b.value; }
};

int qualified(const Box<int>& b) {
    return geometry::unwrap(b); //! error
}

int parenthesized(const Box<int>& b) {
    return (unwrap)(b); //! error
}
}
