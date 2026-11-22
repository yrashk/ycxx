// Functional conversions of namespace-qualified template-ids ([expr.type.conv]).
namespace source {
template<class> struct Box {};
template<class T> constexpr T twice(T v) { return v + v; }
template<class T> inline constexpr T seed = 6;
}
namespace alias = source;

auto direct_braces = source::Box<int>{};
auto direct_parentheses = source::Box<int>();
auto alias_braces = alias::Box<int>{};
auto alias_parentheses = alias::Box<int>();
static_assert(alias::twice<int>(5) == 10);
static_assert(alias::seed<int> + 1 == 7);
