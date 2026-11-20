// Grouped declarators preserve unknown bounds and array parameters adjust once ([dcl.array], [dcl.fct], #222).
struct S { int value; };
extern volatile S storage[][2];
using WholeReference = volatile S (&)[][2];
using WholePointer = volatile S (*)[][2];
static_assert(__is_same(WholeReference, decltype((storage))));
static_assert(__is_same(WholePointer, decltype(&storage)));
void grouped(volatile S (&r)[][2], volatile S (*p)[][2]) {
  static_assert(__is_same(decltype(r), WholeReference));
  static_assert(__is_same(decltype(p), WholePointer));
}
void adjusted(volatile S rows[][2]);
using Adjusted = void (*)(volatile S (*)[2]);
static_assert(__is_same(decltype(&adjusted), Adjusted));
