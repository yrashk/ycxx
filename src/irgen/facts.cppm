// What IR generation asks of types, constants and declarations (docs/design/ast.md §3.6, §5):
// the `Facts` concept. `function_ir` reads trees itself and asks everything else here, so it does
// not depend on how the type and value interners (#33) and the declaration cells (#41) answer.
// Types given to a fact may be sugared and cv-qualified; types returned are canonical.
export module ycxx.irgen:facts;

import std;
import ycxx.ast;

export namespace ycxx::irgen {

namespace ast = ycxx::ast;

// The classes of types the α slice's IR generation distinguishes (#39).
// `record` is a class type ([class]).
enum class TypeClass : std::uint8_t { void_, bool_, integer, pointer, array, function, record, other };

// An integral type's representation: bool is an unsigned 1-bit integer ([basic.fundamental]).
struct IntRep {
  bool is_signed = false;
  std::uint32_t width = 0;
  constexpr bool operator==(const IntRep&) const = default;
};

template <class F>
concept Facts = requires(F& f, ast::QualType q, ast::ConstId c, ast::EntityId e, std::int64_t v) {
  { f.canonical(q) } -> std::same_as<ast::QualType>;  // cv-unqualified
  { f.classify(q) } -> std::same_as<TypeClass>;
  { f.integer(q) } -> std::same_as<IntRep>;        // pre: bool or integer
  { f.promoted(q) } -> std::same_as<ast::QualType>;  // [conv.prom]; the type itself if none applies
  { f.pointee(q) } -> std::same_as<ast::QualType>;   // pre: pointer
  { f.element(q) } -> std::same_as<ast::QualType>;   // pre: array
  { f.pointer_to(q) } -> std::same_as<ast::QualType>;
  { f.return_type(q) } -> std::same_as<ast::QualType>;  // of a function type
  { f.int_type() } -> std::same_as<ast::QualType>;
  { f.ptrdiff_type() } -> std::same_as<ast::QualType>;  // std::ptrdiff_t ([support.types.layout])
  { f.int_constant(q, v) } -> std::same_as<ast::ConstId>;  // `v` converted to integral type q
  { f.null_constant(q) } -> std::same_as<ast::ConstId>;    // the null pointer value of type q
  // The address of a string literal object: a VLvalue whose base is the VString `c` ([lex.string]).
  { f.string_address(c) } -> std::same_as<ast::ConstId>;
  { f.is_constexpr(e) } -> std::same_as<bool>;  // a constexpr or consteval function
  { f.is_main(e) } -> std::same_as<bool>;       // the program's `main` ([basic.start.main])
  // A function declared with an ellipsis ([dcl.fct]): how many parameters precede it; nullopt
  // for a function without one.
  { f.ellipsis_after(e) } -> std::same_as<std::optional<std::uint32_t>>;
  // References ([dcl.ref]) are pointers in the IR: `canonical` and `classify` see a pointer.
  { f.is_reference(q) } -> std::same_as<bool>;
  { f.referee(q) } -> std::same_as<ast::QualType>;  // pre: reference
  { f.bound(q) } -> std::same_as<std::uint64_t>;    // pre: array of known bound
};

}  // namespace ycxx::irgen
