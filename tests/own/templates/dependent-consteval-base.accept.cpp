// #209 [temp.dep.constexpr], [temp.arg.nontype], [class.derived.general].
consteval int padded_count(int count) { return count + 3; }

template<int Count>
struct Storage {};

template<int Count>
struct Buffer : Storage<padded_count(Count)> {};

struct FixedBuffer : Storage<padded_count(3)> {};

static_assert(__is_base_of(Storage<6>, FixedBuffer));
static_assert(__is_base_of(Storage<5>, Buffer<2>));
static_assert(__is_base_of(Storage<10>, Buffer<7>));
