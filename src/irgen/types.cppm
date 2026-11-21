// The type and constant half of `Facts`, over the AST's `types` and `values` interners and a
// target (#33; ast.md §5). The declaration half (`is_constexpr`, `is_main`, `ellipsis_after`) and
// string literals, which the interners do not hold yet, are added by whoever composes the facts:
// the driver from `decl_sema`, the tests from their own tables.
export module ycxx.irgen:types;

import std;
import ycxx.ast;
import :facts;

export namespace ycxx::irgen {

class TypeFacts {
 public:
  TypeFacts(ast::Interners& in, const ast::Target& target) : in_(&in), target_(&target) {}

  ast::Interners& interners() const { return *in_; }
  const ast::Target& target() const { return *target_; }

  // A reference is the pointer that implements it ([dcl.ref]).
  ast::QualType canonical(ast::QualType q) const {
    if (is_reference(q)) return ast::without_cv(in_->canonical(ref_pointer(q)));
    return ast::without_cv(in_->canonical(q));
  }
  bool is_reference(ast::QualType q) const { return ast::is_reference(*in_, q); }
  ast::QualType referee(ast::QualType q) const {
    ast::QualType c = in_->types.canon(q);
    return in_->canonical(in_->types.is<ast::TLRef>(c) ? in_->types.get<ast::TLRef>(c).referee : in_->types.get<ast::TRRef>(c).referee);
  }
  std::uint64_t bound(ast::QualType q) const {
    ast::ConstId b = in_->types.get<ast::TArray>(in_->types.canon(q)).bound;
    return b == ast::ConstId::none ? 0 : in_->values.get<ast::VInt>(b).bits.lo;
  }

  // An enumeration's underlying type ([dcl.enum]), from whoever knows the enumerations.
  std::function<ast::QualType(ast::EntityId)> enum_underlying;
  // A class's direct base classes in declaration order ([class.derived]), from whoever knows the
  // classes: an aggregate's first elements ([dcl.init.aggr]/2).
  std::function<std::vector<ast::EntityId>(ast::EntityId)> bases_of = [](ast::EntityId) { return std::vector<ast::EntityId>{}; };
  std::function<std::uint8_t(ast::EntityId, std::uint32_t)> bit_field_width =
      [](ast::EntityId, std::uint32_t) -> std::uint8_t { return 0; };
  std::function<std::optional<ast::QualType>(ast::EntityId, std::uint32_t)> field_type =
      [](ast::EntityId, std::uint32_t) { return std::nullopt; };

  TypeClass classify(ast::QualType q) const {
    if (is_reference(q)) return TypeClass::pointer;
    ast::QualType c = in_->types.canon(q);
    if (in_->types.is<ast::TEnum>(c)) return TypeClass::integer;
    if (auto b = ast::builtin_of(*in_, c)) {
      if (*b == ast::Builtin::void_) return TypeClass::void_;
      if (*b == ast::Builtin::bool_) return TypeClass::bool_;
      if (ast::is_integral(*b)) return TypeClass::integer;
      if (ast::is_floating(*b)) return TypeClass::floating;
      return TypeClass::other;
    }
    if (in_->types.is<ast::TPointer>(c)) return TypeClass::pointer;
    if (in_->types.is<ast::TArray>(c)) return TypeClass::array;
    if (in_->types.is<ast::TFunction>(c)) return TypeClass::function;
    if (in_->types.is<ast::TClass>(c)) return TypeClass::record;
    return TypeClass::other;
  }

  IntRep integer(ast::QualType q) const {
    ast::QualType c = in_->types.canon(q);
    if (in_->types.is<ast::TEnum>(c)) return integer(enum_underlying(in_->types.get<ast::TEnum>(c).entity));
    ast::Builtin b = *ast::builtin_of(*in_, q);
    return {ast::is_signed(*target_, b), ast::width_of(*target_, b)};
  }

  ast::QualType promoted(ast::QualType q, std::uint8_t width = 0) { return ast::promoted(*in_, *target_, q, width); }
  // Pointee and element types keep their cv: `pointer_to` of them must give the same pointer type.
  ast::QualType pointee(ast::QualType q) const { return in_->canonical(ast::pointee_of(*in_, q)); }
  // A class type's class.
  ast::EntityId class_entity(ast::QualType q) const { return in_->types.get<ast::TClass>(in_->types.canon(q)).entity; }
  ast::QualType element(ast::QualType q) const {
    return in_->canonical(in_->types.get<ast::TArray>(in_->types.canon(q)).elem);
  }
  ast::QualType pointer_to(ast::QualType q) {
    if (is_reference(q)) return ast::pointer_to(*in_, ref_pointer(q));
    return ast::pointer_to(*in_, in_->canonical(q));
  }
  ast::QualType return_type(ast::QualType q) const {
    return canonical(in_->types.get<ast::TFunction>(in_->types.canon(q)).ret);
  }
  // A function type's parameter count when it ends in an ellipsis ([dcl.fct]); nullopt for one
  // that does not. Whoever composes the facts answers `ellipsis_after` with it.
  std::optional<std::uint32_t> parameters_before_ellipsis(ast::QualType fn) const {
    ast::QualType c = in_->types.canon(fn);
    if (!in_->types.is<ast::TFunction>(c)) return std::nullopt;
    auto f = in_->types.get<ast::TFunction>(c);
    if (!f.variadic) return std::nullopt;
    return std::uint32_t(in_->seqs.elements(f.params).size());
  }
  ast::QualType int_type() { return ast::builtin(*in_, ast::Builtin::int_); }
  ast::QualType bool_type() { return ast::builtin(*in_, ast::Builtin::bool_); }
  ast::QualType ptrdiff_type() { return ast::builtin(*in_, target_->ptrdiff_type); }

 private:
  // The pointer type that implements reference type `q`, interned when the reference was: a
  // reference's referee pointer is interned by whoever made the reference type, or here.
  ast::QualType ref_pointer(ast::QualType q) const {
    return ast::pointer_to(*in_, referee(q));
  }

 public:
  ast::ConstId int_constant(ast::QualType q, std::int64_t v) {
    ast::QualType c = in_->types.canon(q);
    if (in_->types.is<ast::TEnum>(c)) q = enum_underlying(in_->types.get<ast::TEnum>(c).entity);  // its value's type
    return ast::int_value(*in_, *target_, canonical(q), v);
  }
  std::optional<ast::ConstId> float_constant(ast::QualType q, double v) {
    q = canonical(q);
    if (!ast::computes(ast::float_format(*in_, *target_, q))) return std::nullopt;
    return ast::float_value(*in_, *target_, q, v);
  }
  ast::ConstId null_constant(ast::QualType) { return in_->intern_value(ast::VNullptr{}); }

 private:
  ast::Interners* in_;
  const ast::Target* target_;
};

}  // namespace ycxx::irgen
