// Qualified lookup stops at each namespace with direct candidates ([namespace.qual]).
namespace deep {
template<class, class> struct Box {};
template<class T> constexpr int select(int) { return 10; }
template<class T> constexpr int Mixed() { return 30; }
}
namespace middle {
using namespace deep;

template<class> struct Box {};
template<class T = int> struct Box;
template<class T> constexpr int select(long) { return 20; }
template<class> struct Mixed {};
}
namespace bridge {
using namespace middle;
}
namespace top {
using namespace bridge;
}
namespace alias = top;

auto nearest_braces = alias::Box<>{};
auto nearest_parentheses = alias::Box<>();
auto mixed_braces = alias::Mixed<int>{};
auto mixed_parentheses = alias::Mixed<int>();
static_assert(alias::select<int>(0) == 20);
