// [except.spec], [expr.unary.noexcept]
struct Reader {
    template<int Index>
    int read(int value) const noexcept { return value + Index; }

    template<int Index>
    int optional_read(int) const;

    template<int Index>
    int (throwing_read)(int) const noexcept(false);
};

const Reader& receiver() noexcept;
static_assert(noexcept(receiver().read<2>(3)));
static_assert(!noexcept(receiver().optional_read<2>(3)));
static_assert(!noexcept(receiver().throwing_read<2>(3)));
