# Extensions

DECISIONS §1.2: ycxx accepts an extension only because a system header or libycxx needs it. Each
one is listed here with the headers that need it and what ycxx does with it. Nothing is added for
its own sake.

| Extension | Needed by | What ycxx does | Since |
| --- | --- | --- | --- |
| GNU attributes `__attribute__((…))`, before a declaration, among its decl-specifiers, and after a declarator | the macOS SDK's `<stdio.h>` family (`__printflike`, `__deprecated_msg`, `__swift_nonisolated_unsafe`), `sys/cdefs.h` | Parsed and ignored, as `[[…]]` attributes it does not know are ([dcl.attr.grammar]/6) | #133 |
| `asm` labels `__asm("…")`, `__asm__("…")`, `asm("…")` after a declarator | the macOS SDK's `__DARWIN_ALIAS` and `__DARWIN_ALIAS_STARTING` (`fopen`, `fwrite`, …) | The string is the entity's symbol, exactly as written, in place of its linkage name; one label per entity; not yet on class members (#190) | #133 |
| Type-trait built-ins: predicates `__is_same`, `__is_void`, `__is_integral`, `__is_floating_point`, `__is_arithmetic`, `__is_enum`, `__is_scoped_enum`, `__is_class`, `__is_union`, `__is_pointer`, `__is_member_pointer`, `__is_member_object_pointer`, `__is_member_function_pointer`, `__is_reference`, `__is_lvalue_reference`, `__is_rvalue_reference`, `__is_array`, `__is_bounded_array`, `__is_unbounded_array`, `__is_function`, `__is_object`, `__is_const`, `__is_volatile`, `__is_empty`, `__is_aggregate`, `__is_polymorphic`, `__is_abstract`, `__is_final`, `__has_virtual_destructor`; transformations `__remove_cv`, `__remove_const`, `__remove_volatile`, `__remove_cvref`, `__remove_reference_t` (and GCC's `__remove_reference`), `__remove_pointer`, `__add_pointer`, `__add_lvalue_reference`, `__add_rvalue_reference`, `__remove_extent`, `__remove_all_extents`, `__decay`, `__underlying_type` | libycxx's `core/prim_traits.hpp`, `config.hpp` and `<type_traits>`, which probe them with `__has_builtin` | Answered from ycxx's own type system as the corresponding standard trait ([meta.unary], [meta.rel], [meta.trans]); a predicate is a `bool` prvalue, a transformation a type (of a dependent type, a `TTransform` that substitution applies, mangled `u11__remove_cvIT_E` as Clang does). `__has_builtin` answers 1 for exactly these (`lex::type_traits`); the traits that need constructors, triviality or conversions answer 0 until implemented. ycxx has no pointers to members, virtual functions or `final` yet, so those predicates are false | #211 |

Standard features that are parsed and not yet acted on, which are not extensions:

- `[[…]]` attribute-specifiers on declarations and statements are skipped
  ([dcl.attr.grammar]/6). `[[gnu::visibility]]` and the other attributes libycxx's `config.hpp`
  uses are therefore without effect for now.
