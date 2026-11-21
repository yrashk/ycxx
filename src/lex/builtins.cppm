// Type-trait built-ins recognized by the parser (DECISIONS §1.2, docs/EXTENSIONS.md).
// The preprocessor uses each entry's advertisement flag for libycxx's `__has_builtin` probes.
export module ycxx.lex:builtins;

import std;

export namespace ycxx::lex {

enum class TypeTrait : std::uint8_t {
  // Predicates: a `bool` prvalue.
  is_same, is_void, is_integral, is_floating_point, is_arithmetic, is_enum, is_scoped_enum, is_class,
  is_union, is_pointer, is_member_pointer, is_member_object_pointer, is_member_function_pointer,
  is_reference, is_lvalue_reference, is_rvalue_reference, is_array, is_bounded_array,
  is_unbounded_array, is_function, is_object, is_const, is_volatile, is_empty, is_aggregate,
  is_polymorphic, is_abstract, is_final, has_virtual_destructor,
  // Transformations: a type.
  remove_cv, remove_const, remove_volatile, remove_cvref, remove_reference_t, remove_pointer,
  add_pointer, add_lvalue_reference, add_rvalue_reference, remove_extent, remove_all_extents, decay,
  underlying_type,
  // Predicates with bounded support.
  is_trivially_copyable, is_constructible, is_nothrow_constructible, is_trivially_constructible,
  is_assignable, is_nothrow_assignable, is_trivially_assignable,
  is_convertible, is_nothrow_convertible,
  is_base_of,
  is_destructible,
  is_nothrow_destructible,
  is_trivially_destructible,
};

struct TypeTraitInfo {
  std::string_view name;
  TypeTrait trait;
  std::uint8_t arity;  // required type arguments
  bool transforms;     // a type, not a `bool`
  bool advertised = true;
  bool variadic = false;
  bool replay_dependent = false;  // the fixed-arity parser retains the written argument
};

inline constexpr std::array type_traits{
    TypeTraitInfo{"__is_same", TypeTrait::is_same, 2, false},
    TypeTraitInfo{"__is_void", TypeTrait::is_void, 1, false},
    TypeTraitInfo{"__is_integral", TypeTrait::is_integral, 1, false},
    TypeTraitInfo{"__is_floating_point", TypeTrait::is_floating_point, 1, false},
    TypeTraitInfo{"__is_arithmetic", TypeTrait::is_arithmetic, 1, false},
    TypeTraitInfo{"__is_enum", TypeTrait::is_enum, 1, false},
    TypeTraitInfo{"__is_scoped_enum", TypeTrait::is_scoped_enum, 1, false},
    TypeTraitInfo{"__is_class", TypeTrait::is_class, 1, false},
    TypeTraitInfo{"__is_union", TypeTrait::is_union, 1, false},
    TypeTraitInfo{"__is_pointer", TypeTrait::is_pointer, 1, false},
    TypeTraitInfo{"__is_member_pointer", TypeTrait::is_member_pointer, 1, false},
    TypeTraitInfo{"__is_member_object_pointer", TypeTrait::is_member_object_pointer, 1, false},
    TypeTraitInfo{"__is_member_function_pointer", TypeTrait::is_member_function_pointer, 1, false},
    TypeTraitInfo{"__is_reference", TypeTrait::is_reference, 1, false},
    TypeTraitInfo{"__is_lvalue_reference", TypeTrait::is_lvalue_reference, 1, false},
    TypeTraitInfo{"__is_rvalue_reference", TypeTrait::is_rvalue_reference, 1, false},
    TypeTraitInfo{"__is_array", TypeTrait::is_array, 1, false},
    TypeTraitInfo{"__is_bounded_array", TypeTrait::is_bounded_array, 1, false},
    TypeTraitInfo{"__is_unbounded_array", TypeTrait::is_unbounded_array, 1, false},
    TypeTraitInfo{"__is_function", TypeTrait::is_function, 1, false},
    TypeTraitInfo{"__is_object", TypeTrait::is_object, 1, false},
    TypeTraitInfo{"__is_const", TypeTrait::is_const, 1, false},
    TypeTraitInfo{"__is_volatile", TypeTrait::is_volatile, 1, false},
    TypeTraitInfo{.name = "__is_empty", .trait = TypeTrait::is_empty, .arity = 1,
                  .transforms = false, .replay_dependent = true},
    TypeTraitInfo{"__is_aggregate", TypeTrait::is_aggregate, 1, false},
    TypeTraitInfo{"__is_polymorphic", TypeTrait::is_polymorphic, 1, false},
    TypeTraitInfo{"__is_abstract", TypeTrait::is_abstract, 1, false},
    TypeTraitInfo{"__is_final", TypeTrait::is_final, 1, false},
    TypeTraitInfo{"__has_virtual_destructor", TypeTrait::has_virtual_destructor, 1, false},
    TypeTraitInfo{"__remove_cv", TypeTrait::remove_cv, 1, true},
    TypeTraitInfo{"__remove_const", TypeTrait::remove_const, 1, true},
    TypeTraitInfo{"__remove_volatile", TypeTrait::remove_volatile, 1, true},
    TypeTraitInfo{"__remove_cvref", TypeTrait::remove_cvref, 1, true},
    TypeTraitInfo{"__remove_reference_t", TypeTrait::remove_reference_t, 1, true},
    TypeTraitInfo{"__remove_reference", TypeTrait::remove_reference_t, 1, true},  // GCC's spelling
    TypeTraitInfo{"__remove_pointer", TypeTrait::remove_pointer, 1, true},
    TypeTraitInfo{"__add_pointer", TypeTrait::add_pointer, 1, true},
    TypeTraitInfo{"__add_lvalue_reference", TypeTrait::add_lvalue_reference, 1, true},
    TypeTraitInfo{"__add_rvalue_reference", TypeTrait::add_rvalue_reference, 1, true},
    TypeTraitInfo{"__remove_extent", TypeTrait::remove_extent, 1, true},
    TypeTraitInfo{"__remove_all_extents", TypeTrait::remove_all_extents, 1, true},
    TypeTraitInfo{"__decay", TypeTrait::decay, 1, true},
    TypeTraitInfo{"__underlying_type", TypeTrait::underlying_type, 1, true},
    TypeTraitInfo{.name = "__is_trivially_copyable", .trait = TypeTrait::is_trivially_copyable, .arity = 1,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{"__is_constructible", TypeTrait::is_constructible, 1, false, false, true},
    TypeTraitInfo{"__is_nothrow_constructible", TypeTrait::is_nothrow_constructible, 1, false, false, true},
    TypeTraitInfo{"__is_trivially_constructible", TypeTrait::is_trivially_constructible, 1, false, false, true},
    TypeTraitInfo{.name = "__is_assignable", .trait = TypeTrait::is_assignable, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_nothrow_assignable", .trait = TypeTrait::is_nothrow_assignable, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_trivially_assignable", .trait = TypeTrait::is_trivially_assignable, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_convertible", .trait = TypeTrait::is_convertible, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_nothrow_convertible", .trait = TypeTrait::is_nothrow_convertible, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_base_of", .trait = TypeTrait::is_base_of, .arity = 2,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_destructible", .trait = TypeTrait::is_destructible, .arity = 1,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_nothrow_destructible", .trait = TypeTrait::is_nothrow_destructible, .arity = 1,
                  .transforms = false, .advertised = false, .replay_dependent = true},
    TypeTraitInfo{.name = "__is_trivially_destructible", .trait = TypeTrait::is_trivially_destructible, .arity = 1,
                  .transforms = false, .advertised = false, .replay_dependent = true},
};

constexpr const TypeTraitInfo* type_trait(std::string_view name) {
  for (const TypeTraitInfo& t : type_traits)
    if (t.name == name) return &t;
  return nullptr;
}
// The GNU built-in functions ycxx implements (docs/EXTENSIONS.md). For the floating builtins
// from `<math.h>`, `type` is the type they take and give: 'f' float, 'd' double, 'l' long
// double. `__builtin_inf*` and `__builtin_huge_val*` are +infinity, `__builtin_nan*(s)` the quiet
// NaN whose payload `s` spells (as `strtoull` reads it), `__builtin_fabs*(x)` |x|.
enum class BuiltinFunction : std::uint8_t { fabs, inf, huge_val, nan, addressof, strlen, trap };
struct BuiltinFunctionInfo {
  std::string_view name;
  BuiltinFunction function;
  char type = 0;
};
inline constexpr std::array builtin_functions{
    BuiltinFunctionInfo{"__builtin_addressof", BuiltinFunction::addressof},
    BuiltinFunctionInfo{"__builtin_fabsf", BuiltinFunction::fabs, 'f'},
    BuiltinFunctionInfo{"__builtin_fabs", BuiltinFunction::fabs, 'd'},
    BuiltinFunctionInfo{"__builtin_fabsl", BuiltinFunction::fabs, 'l'},
    BuiltinFunctionInfo{"__builtin_inff", BuiltinFunction::inf, 'f'},
    BuiltinFunctionInfo{"__builtin_inf", BuiltinFunction::inf, 'd'},
    BuiltinFunctionInfo{"__builtin_infl", BuiltinFunction::inf, 'l'},
    BuiltinFunctionInfo{"__builtin_huge_valf", BuiltinFunction::huge_val, 'f'},
    BuiltinFunctionInfo{"__builtin_huge_val", BuiltinFunction::huge_val, 'd'},
    BuiltinFunctionInfo{"__builtin_huge_vall", BuiltinFunction::huge_val, 'l'},
    BuiltinFunctionInfo{"__builtin_nanf", BuiltinFunction::nan, 'f'},
    BuiltinFunctionInfo{"__builtin_nan", BuiltinFunction::nan, 'd'},
    BuiltinFunctionInfo{"__builtin_nanl", BuiltinFunction::nan, 'l'},
    BuiltinFunctionInfo{"__builtin_strlen", BuiltinFunction::strlen},
    BuiltinFunctionInfo{"__builtin_trap", BuiltinFunction::trap},
};
constexpr const BuiltinFunctionInfo* builtin_function(std::string_view name) {
  for (const BuiltinFunctionInfo& b : builtin_functions)
    if (b.name == name) return &b;
  return nullptr;
}

// The variadic builtins have their own argument grammar: va_arg takes a type-id.
enum class VaBuiltin : std::uint8_t { start, legacy_start, arg, copy, end };

constexpr std::optional<VaBuiltin> va_builtin(std::string_view name) {
  using enum VaBuiltin;
  if (name == "__builtin_c23_va_start") return start;
  if (name == "__builtin_va_start") return legacy_start;
  if (name == "__builtin_va_arg") return arg;
  if (name == "__builtin_va_copy") return copy;
  if (name == "__builtin_va_end") return end;
  return std::nullopt;
}

// The GNU built-in functions the SDK's and libycxx's headers call and ycxx does not have yet
// (docs/EXTENSIONS.md, #248): a call of one is diagnosed as not supported, its arguments skipped.
// Any other `__builtin_` name is an ordinary name, undeclared unless declared, so a
// requires-expression probing it is unsatisfied ([expr.prim.req]).
inline constexpr std::array<std::string_view, 4> lacking_builtins{
    "__builtin_alloca", "__builtin_constant_p", "__builtin_offsetof", "__builtin_expect",
};
constexpr bool lacking_builtin(std::string_view name) { return std::ranges::contains(lacking_builtins, name); }

// The built-in templates ycxx implements (docs/EXTENSIONS.md): Clang's
// `__make_integer_seq<Seq, T, N>`, `Seq<T, 0, …, N-1>`, which libycxx's `make_integer_sequence`
// uses when `__has_builtin` says it exists.
inline constexpr std::array<std::string_view, 1> builtin_templates{"__make_integer_seq"};
constexpr bool builtin_template(std::string_view name) { return std::ranges::contains(builtin_templates, name); }

// The first spelling of `t`, Clang's where GCC spells it differently.
constexpr const TypeTraitInfo& type_trait_info(TypeTrait t) {
  for (const TypeTraitInfo& i : type_traits)
    if (i.trait == t) return i;
  return type_traits[0];
}

}  // namespace ycxx::lex
