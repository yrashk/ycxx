// [class.spaceship]/1.2, /2.2: an undefined synthesized comparison deletes the function.
#include "inputs/compare.hpp"
struct A { int value; int operator<=>(const A&) const = default; };
template<class T> concept HasSpaceship = requires(const T& a, const T& b) { a <=> b; };
static_assert(!HasSpaceship<A>);
