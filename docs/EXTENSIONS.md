# Extensions

DECISIONS §1.2: ycxx accepts an extension only because a system header or libycxx needs it. Each
one is listed here with the headers that need it and what ycxx does with it. Nothing is added for
its own sake.

| Extension | Needed by | What ycxx does | Since |
| --- | --- | --- | --- |
| GNU attributes `__attribute__((…))`, before a declaration, among its decl-specifiers, and after a declarator | the macOS SDK's `<stdio.h>` family (`__printflike`, `__deprecated_msg`, `__swift_nonisolated_unsafe`), `sys/cdefs.h` | Parsed and ignored, as `[[…]]` attributes it does not know are ([dcl.attr.grammar]/6) | #133 |
| `asm` labels `__asm("…")`, `__asm__("…")`, `asm("…")` after a declarator | the macOS SDK's `__DARWIN_ALIAS` and `__DARWIN_ALIAS_STARTING` (`fopen`, `fwrite`, …) | The string is the entity's symbol, exactly as written, in place of its linkage name; one label per entity; not yet on class members (#190) | #133 |
| Type-trait built-ins: predicates `__is_same`, `__is_void`, `__is_integral`, `__is_floating_point`, `__is_arithmetic`, `__is_enum`, `__is_scoped_enum`, `__is_class`, `__is_union`, `__is_pointer`, `__is_member_pointer`, `__is_member_object_pointer`, `__is_member_function_pointer`, `__is_reference`, `__is_lvalue_reference`, `__is_rvalue_reference`, `__is_array`, `__is_bounded_array`, `__is_unbounded_array`, `__is_function`, `__is_object`, `__is_const`, `__is_volatile`, `__is_empty`, `__is_aggregate`, `__is_polymorphic`, `__is_abstract`, `__is_final`, `__has_virtual_destructor`; transformations `__remove_cv`, `__remove_const`, `__remove_volatile`, `__remove_cvref`, `__remove_reference_t` (and GCC's `__remove_reference`), `__remove_pointer`, `__add_pointer`, `__add_lvalue_reference`, `__add_rvalue_reference`, `__remove_extent`, `__remove_all_extents`, `__decay`, `__underlying_type` | libycxx's `core/prim_traits.hpp`, `config.hpp` and `<type_traits>`, which probe them with `__has_builtin` | Answered from ycxx's own type system as the corresponding standard trait ([meta.unary], [meta.rel], [meta.trans]); a predicate is a `bool` prvalue, a transformation a type (of a dependent type, a `TTransform` that substitution applies, mangled `u11__remove_cvIT_E` as Clang does). `__has_builtin` answers 1 for exactly these (`lex::type_traits`); the traits that need constructors, triviality or conversions answer 0 until implemented. ycxx has no pointers to members, virtual functions or `final` yet, so those predicates are false | #211 |
| `__make_integer_seq<Seq, T, N>`, Clang's built-in template | libycxx's `config.hpp` (`__y_make_integer_seq`, so `std::make_integer_sequence`), which probes it with `__has_builtin` | `Seq<T, 0, 1, …, N-1>` for a class or alias template `Seq`, an integral type `T` other than `bool` and a value `N` of type `T` not negative, as Clang has it ([intseq.make]); with a dependent argument a dependent type that substitution applies and deduction does not see into, mangled as a global class template named `__make_integer_seq` as Clang does. Not usable as a template argument itself (#272). `__has_builtin(__make_integer_seq)` answers 1 (`lex::builtin_templates`) | #208 |

## The GNU dialect (`__GNUC__`)

ycxx predefines `__GNUC__` 4, `__GNUC_MINOR__` 2 and `__GNUC_PATCHLEVEL__` 1, as Clang does,
alongside `__ycxx__` (DECISIONS §1.5, #229). It does not define `__GNUG__`, `__clang__` or
`__llvm__`. libycxx recognizes ycxx by `__ycxx__` before it looks at `__GNUC__`, so no GCC path
of libycxx's applies to ycxx. The macOS SDK's headers then take their `__GNUC__` paths; what those
use, in the headers libycxx's C library headers reach (`<cstdio>`, `<cstdlib>`, `<cmath>`, …):

| Extension | Needed by | What ycxx does | Since |
| --- | --- | --- | --- |
| `__builtin_va_list`, a type name | `arm/_types.h`'s `__darwin_va_list` (so `va_list`, `vprintf`'s parameters), libycxx's `<cstdarg>` | The target's va_list type: `char*` in Apple's arm64 ABI (`ast::Target::va_list`). A typedef-name the implementation declares, found where lookup finds nothing else, so a declaration of the same name hides it; not yet on x86-64 Linux, whose `__va_list_tag[1]` ycxx does not have | #229 |
| GNU attributes `__always_inline__` (`__header_always_inline`), `__format__(__strfmon__, …)` and `__format__(__strftime__, …)` (`__strfmonlike`, `__strftimelike`) | `sys/cdefs.h`, `_types.h`, `math.h` | Parsed and ignored, as the other GNU attributes (above) | #133 |
| `__builtin_fabsf`, `__builtin_fabs`, `__builtin_fabsl`, `__builtin_inff`, `__builtin_inf`, `__builtin_infl` | `math.h`'s inline `__inline_isfinite*`, `__inline_isinf*`, `__inline_isnormal*` (its `__GNUC__` and `__FINITE_MATH_ONLY__ == 0` path) | Not yet (they need floating-point values, #201): `built-in function '…' is not supported yet` where called | — |
| `__builtin_huge_val`, `__builtin_huge_valf`, `__builtin_huge_vall`, `__builtin_nanf` | `math.h`'s `HUGE_VAL`, `HUGE_VALF`, `HUGE_VALL`, `NAN` | Not yet (#201): diagnosed as above where a program uses the macro | — |
| `__builtin_alloca` | `alloca.h`'s `alloca` | Not yet: diagnosed as above where a program calls `alloca` | — |
| `__builtin_constant_p` | `libkern/_OSByteOrder.h`'s `__DARWIN_OSSwapInt16/32/64` (`ntohs`, `htonl`, …) | Not yet: diagnosed as above where a program uses the macros | — |
| `__builtin_offsetof` | `sys/_types.h`'s `__offsetof`, libycxx's `offsetof` | Not yet: diagnosed as above where a program uses `offsetof`, its arguments (a type first) skipped | — |
| `__builtin_expect` | the SDK's `assert.h`'s `assert` | Not yet: diagnosed as above where a program uses `assert`, which did not compile before #229 either (its other path calls `abort` undeclared; this one also needs `__func__`, [dcl.fct.def.general], which ycxx does not have yet) | — |

The built-in functions marked "not yet" are listed in one table, `lex::lacking_builtins`
(#248). Only those are diagnosed as not supported: any other `__builtin_` name is an ordinary
name, undeclared unless declared, so a requires-expression that probes it is unsatisfied, as
libycxx's in-language probes need (`tests/own/builtins/unknown-probe.accept.cpp`).

The other `__GNUC__` tests in those headers select nothing new for ycxx: `sys/cdefs.h`'s
`#warning "Unsupported compiler detected"` goes; `__signed` is `sys/cdefs.h`'s macro for
`signed`; `__header_inline` is `inline` in C++; `__deprecated_msg`, the availability macros and the
`GCC diagnostic` pragmas want GCC 4.5 or 4.6; `__DARWIN_NULL` wants `__GNUG__` (`__null`);
`_stdio.h`'s inline `__sputc` wants `__STDC__`; the `_FORTIFY_SOURCE` paths are C only; and
`libkern/arm/_OSByteOrder.h`'s byte swaps use `__builtin_bswap32` and `__builtin_bswap64` only
under `__llvm__` (their portable path has a local union, which ycxx does not have yet).

Standard features that are parsed and not yet acted on, which are not extensions:

- `[[…]]` attribute-specifiers on declarations and statements are skipped
  ([dcl.attr.grammar]/6). `[[gnu::visibility]]` and the other attributes libycxx's `config.hpp`
  uses are therefore without effect for now.
