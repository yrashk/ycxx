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
int lookup(int value = read(T{})) { return value; }
namespace adl {
constexpr int read(Tag) { return 7; }
}
int late_adl() { return lookup<adl::Tag>(); }

template<class T> constexpr int earlier_parameter(T x, int size = sizeof(x)) { return size; }
static_assert(earlier_parameter(7) == sizeof(int));

constexpr int written_offset = 3;
template<class T>
constexpr int later_parameter(int size = sizeof(T) + written_offset, int written_offset = 2) {
    return size;
}
static_assert(later_parameter<char>() == 4);
