// The type-trait built-ins ycxx implements (DECISIONS §1.2, docs/EXTENSIONS.md): the ones
// libycxx's headers probe with `__has_builtin` and use, each answered from ycxx's own type system.
// The preprocessor answers `__has_builtin` from this table; sema parses and evaluates them.
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
};

struct TypeTraitInfo {
  std::string_view name;
  TypeTrait trait;
  std::uint8_t arity;  // its type arguments
  bool transforms;     // a type, not a `bool`
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
    TypeTraitInfo{"__is_empty", TypeTrait::is_empty, 1, false},
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
};

constexpr const TypeTraitInfo* type_trait(std::string_view name) {
  for (const TypeTraitInfo& t : type_traits)
    if (t.name == name) return &t;
  return nullptr;
}
// The first spelling of `t`, Clang's where GCC spells it differently.
constexpr const TypeTraitInfo& type_trait_info(TypeTrait t) {
  for (const TypeTraitInfo& i : type_traits)
    if (i.trait == t) return i;
  return type_traits[0];
}

}  // namespace ycxx::lex
