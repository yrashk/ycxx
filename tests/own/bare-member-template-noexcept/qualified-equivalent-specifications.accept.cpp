// [except.spec], [temp.mem]
struct Counter {
    template<int Start> static int from_bare(int) noexcept;
    template<int Start> static int to_bare(int) noexcept(true);
};

template<int Start>
int Counter::from_bare(int delta) noexcept(true) { return Start + delta; }

template<int Start>
int Counter::to_bare(int delta) noexcept { return Start + delta; }

static_assert(noexcept(Counter::from_bare<7>(3)));
static_assert(noexcept(Counter::to_bare<7>(3)));
