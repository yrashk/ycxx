// [cmp.concept], [cmp.result]: <compare>'s concepts and traits over the three-way comparison of
// fundamental types.
#include <compare>

template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;

static_assert(std::three_way_comparable<int>);
static_assert(std::three_way_comparable<int*>);
static_assert(std::three_way_comparable<double, std::partial_ordering>);
static_assert(!std::three_way_comparable<double, std::strong_ordering>);
static_assert(same<std::compare_three_way_result_t<int>, std::strong_ordering>);
static_assert(same<std::compare_three_way_result_t<double>, std::partial_ordering>);
static_assert(std::is_lt(1 <=> 2) && std::is_gteq(2.0 <=> 1.0));
