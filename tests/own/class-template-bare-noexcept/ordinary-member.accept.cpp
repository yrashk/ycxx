// [except.spec], [temp.inst], [expr.unary.noexcept]
template<class T> struct Instrument {
    int observe() const noexcept;
    int ordinary() const;
    int potentially_throwing() const noexcept(false);
};

extern const Instrument<int> first;
extern const Instrument<char> second;

static_assert(noexcept(first.observe()));
static_assert(noexcept(second.observe()));
static_assert(!noexcept(first.ordinary()));
static_assert(!noexcept(second.potentially_throwing()));
