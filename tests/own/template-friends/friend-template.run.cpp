// [temp.friend]/1, [class.friend]/6-7: a friend function template defined in a class, or in a
// class template, is a function template of the class's namespace that only argument-dependent
// lookup finds; its specializations are deduced and instantiated as any function template's.
namespace ops {
class Meter {
    int raw;

public:
    constexpr Meter(int r) : raw(r) {}

    template <class T>
    friend constexpr int scaled(const Meter& m, T factor) { return m.raw * int(factor); }
};

template <class U>
class Box {
    U u;

public:
    constexpr Box(U v) : u(v) {}

    template <class T>
    friend constexpr U add(const Box& b, T t) noexcept { return b.u + U(t); }
};
}

static_assert(scaled(ops::Meter(3), 2) == 6);
static_assert(add(ops::Box<int>(4), 1L) == 5);

int main() {
    ops::Meter m(5);
    if (scaled(m, 3) != 15) return 1;
    if (scaled(m, 2.0) != 10) return 2;
    ops::Box<long> b(7);
    if (add(b, 1) != 8) return 3;
    if (add(ops::Box<int>(1), 'a' - 'a') != 1) return 4;
    return 0;
}
