// [expr.prim.req.compound]: `<=>` in a requires-expression, as <compare>'s
// `three_way_comparable` writes it.
#include "inputs/compare.hpp"

template<class T> concept C = requires(T a, T b) { { a <=> b }; };
struct NoOrder {};

static_assert(C<int>);
static_assert(C<double>);
static_assert(C<int*>);
static_assert(!C<NoOrder>);
static_assert(!C<void (*)()>);
