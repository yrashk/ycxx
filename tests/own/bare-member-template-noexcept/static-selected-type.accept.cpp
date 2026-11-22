// [except.spec], [conv.fctptr], [temp.deduct.funcaddr]
struct Counter {
    template<class Increment>
    static int count(Increment increment) noexcept { return 7 + increment; }
};

using Nonthrowing = int (*)(int) noexcept;
Nonthrowing selected = &Counter::count<int>;
static_assert(noexcept(Counter::count(2)));
