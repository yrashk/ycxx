// [temp.res.general], [temp.dep.general], [temp.arg.nontype]
namespace library {
consteval unsigned adjust(unsigned n) { return n + 30; }
consteval unsigned adjust(unsigned long n) { return n + 3; }

template<unsigned long N, unsigned Result = adjust(N)>
struct Storage { static constexpr unsigned value = Result; };
}

namespace consumer {
consteval unsigned adjust(unsigned long n) { return n + 20; }

static_assert(library::Storage<5>::value == 8);
}
