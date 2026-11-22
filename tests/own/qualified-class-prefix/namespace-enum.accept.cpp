// [basic.scope.enum], [dcl.enum]
namespace Prefix {
enum { first = 8, second = Prefix::first + 1 };
}
static_assert(Prefix::second == 9);
