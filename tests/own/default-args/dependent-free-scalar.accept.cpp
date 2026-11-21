constexpr int seed(long) { return 4; }
template<class T>
constexpr int count(int value = sizeof(T) + seed(0)) { return value; }
constexpr int seed(int) { return 9; }

static_assert(count<int>() == sizeof(int) + 4);
static_assert(count<void>(13) == 13);

namespace adl {
struct Tag {};
}
template<class T>
constexpr int lookup(int value = read(T{})) { return value; }
namespace adl {
constexpr int read(Tag) { return 7; }
}
static_assert(lookup<adl::Tag>() == 7);
