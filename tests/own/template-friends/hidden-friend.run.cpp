// [temp.friend], [class.friend]/6-7, [temp.inst]/4: a friend function defined in a class
// template is a separate non-template function for each specialization, defined when the
// specialization is instantiated and found by argument-dependent lookup.
namespace geometry {
template <class T>
class Box {
    T value;

public:
    Box(T v) : value(v) {}

    friend bool operator==(const Box& a, const Box& b) { return a.value == b.value; }
    friend T unwrap(const Box& b) { return b.value + 2; }
};
}

template <class T> struct F { T o; friend bool operator==(const F& a, const F& b) noexcept { return a.o == b.o; } };

int main() {
    geometry::Box<int> a(3), b(3), c(4);
    if (!(a == b)) return 1;
    if (a == c) return 2;
    if (unwrap(a) != 5) return 3;
    geometry::Box<long> d(7);
    if (unwrap(d) != 9) return 4;
    return F<int>{1} == F<int>{1} ? 0 : 5;
}
