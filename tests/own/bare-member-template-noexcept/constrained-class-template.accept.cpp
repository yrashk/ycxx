// [except.spec], [temp.inst], [temp.constr]
template<class Key>
struct Reader {
    template<int Index> requires (Index > 0)
    int read(Key value) const noexcept { return value + Index; }
};

const Reader<int>& first() noexcept;
const Reader<long>& second() noexcept;
static_assert(noexcept(first().read<1>(7)));
static_assert(noexcept(second().read<2>(9L)));
