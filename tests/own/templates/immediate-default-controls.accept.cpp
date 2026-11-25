// [temp.param], [temp.arg.nontype], [expr.const.imm]
namespace defaults {
consteval unsigned align_for(unsigned n) { return n >= 4 ? 4 : n; }
}

template<unsigned N, unsigned A = 4>
struct Literal { static constexpr unsigned value = A; };

template<unsigned N, unsigned A = ::defaults::align_for(8)>
struct Concrete { static constexpr unsigned value = A; };

static_assert(Literal<3>::value == 4);
static_assert(Concrete<3>::value == 4);
