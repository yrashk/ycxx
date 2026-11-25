// #327 [cpp.line], [dcl.fct.default]
#include "builtin-line-defaults.hpp"

#define DIRECT_LINE() __builtin_LINE()
#define CONCAT(a, b) a ## b
#define PASTED_LINE() CONCAT(__builtin_, LINE)()

#define INCLUDE_LINE 201
#define INCLUDE_NAME first_inclusion
#include "builtin-line-occurrence.hpp"
#undef INCLUDE_LINE
#undef INCLUDE_NAME
#define INCLUDE_LINE 314
#define INCLUDE_NAME second_inclusion
#include "builtin-line-occurrence.hpp"
static_assert(first_inclusion == 201 && second_inclusion == 314);

#ifdef EXPECT_CLANG_BUILTIN_LINE
static_assert(__is_same(decltype(__builtin_LINE()), unsigned));
#endif

#line 100
static_assert(__builtin_LINE() == 100);
static_assert(DIRECT_LINE() == 101);
static_assert(PASTED_LINE() == 102);
static_assert(leaf() == 103);
static_assert(leaf() == 104);
static_assert(outer() == 105);
static_assert(leaf(17) == 17 && outer(23) == 23);
static_assert(wrapper() == 80);
static_assert(noexcept(__builtin_LINE()) && noexcept(outer()));

#line 150
int main() {
  if (reported() != 151) return 1;
  if (reported() != 152) return 2;
  if (reported(7) != 7) return 3;
  return outer() != 154 || wrapper() != 80;
}

struct S { constexpr unsigned leaf(unsigned n = __builtin_LINE()) const { return n; } };
constexpr S object{};
#line 100
static_assert(object.
    leaf(
    ) == 101);
static_assert(object.leaf() == 103);

#line 500
consteval unsigned id(unsigned n) { return n; }
constexpr unsigned with_immediate(unsigned n = __builtin_LINE() + id(4)) { return n; }
static_assert(with_immediate() == 506);

constexpr unsigned conditional_default(unsigned n = id(false ? __builtin_LINE() : 4u)) { return n; }
constexpr unsigned and_default(unsigned n = id(false && __builtin_LINE() != 0)) { return n; }
constexpr unsigned or_default(unsigned n = id(true || __builtin_LINE() != 0)) { return n; }
static_assert(conditional_default() == 4);
static_assert(and_default() == 0);
static_assert(or_default() == 1);

#ifdef EXPECT_CLANG_BUILTIN_LINE
// Clang uses the closing line here. GCC uses the opening line.
#define NAME() LINE
#define LINE() __builtin_LINE()
#line 10
static_assert(NAME()(
) == 11);
#undef LINE
#undef NAME
#endif
