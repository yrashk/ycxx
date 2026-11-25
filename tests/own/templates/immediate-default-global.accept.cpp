// [temp.param], [temp.arg.nontype], [expr.const.imm]
namespace library {
namespace detail {
consteval unsigned align_for(unsigned n) { return n >= 4 ? 4 : n; }
}
}

template<unsigned N, unsigned A = ::library::detail::align_for(N)>
struct Storage { static constexpr unsigned value = A; };

static_assert(Storage<8>::value == 4);
static_assert(Storage<3>::value == 3);
