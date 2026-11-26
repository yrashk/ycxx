// [temp.param], [temp.res.general], [temp.arg.nontype]
namespace detail {
consteval unsigned size_for(unsigned n) { return n + 30; }
}

namespace library {
namespace detail {
consteval unsigned size_for(unsigned long n) { return n + 2; }
}

template<unsigned N = 8, unsigned Size = detail::size_for(N)>
struct Storage { static constexpr unsigned value = Size; };

namespace detail {
consteval unsigned size_for(unsigned n) { return n + 20; }
}
}

static_assert(library::Storage<>::value == 10);
static_assert(library::Storage<3>::value == 5);
