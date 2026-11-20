// [expr.new], [expr.delete]: result types, global lookup and unevaluated operands.
namespace N {
struct Incomplete;
int potentially_throwing();

using CvResult = decltype(::new const volatile int(42));
using CvResult = const volatile int*;
using PointerResult = decltype(new (int* const)(nullptr));
using PointerResult = int* const*;
using IncompleteResult = decltype(new Incomplete*{});
using IncompleteResult = Incomplete**;
using UnevaluatedResult = decltype(new int(potentially_throwing()));
using UnevaluatedResult = int*;
using DeleteResult = decltype(::delete static_cast<int*>(nullptr));
using DeleteResult = void;

void exercise() {
  int* value = ::new (int)(19);
  ::delete value;
  Incomplete** pointer = new Incomplete*{};
  ::delete pointer;
}
}
