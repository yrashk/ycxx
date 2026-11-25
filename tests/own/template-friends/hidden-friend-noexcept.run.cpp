// [temp.friend], [except.spec]/13, [temp.inst]: a friend defined in a class template may have a
// noexcept-specifier that depends on the template's arguments; each specialization's friend has
// its own exception specification.
namespace wrapped {
template <class T>
struct Wrap {
    T v;
    friend bool operator==(const Wrap& a, const Wrap& b) noexcept(noexcept(a.v == b.v)) { return a.v == b.v; }
    friend T get(const Wrap& w) noexcept(sizeof(T) > 1) { return w.v; }
};
}

struct Loose {
    int x;
    bool operator==(const Loose& o) const { return x == o.x; }
};

static_assert(noexcept(wrapped::Wrap<int>{1} == wrapped::Wrap<int>{1}));
static_assert(!noexcept(wrapped::Wrap<Loose>{} == wrapped::Wrap<Loose>{}));
static_assert(noexcept(get(wrapped::Wrap<int>{1})));
static_assert(!noexcept(get(wrapped::Wrap<char>{1})));

int main() {
    wrapped::Wrap<Loose> a{{4}}, b{{4}};
    if (!(a == b)) return 1;
    return get(wrapped::Wrap<int>{3}) == 3 ? 0 : 2;
}
