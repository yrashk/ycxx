// The type and constant half of `Facts`, over the AST's `types` and `values` interners and a
// target (#33; ast.md §5). The declaration half (`is_constexpr`, `is_main`) and string literals,
// which the interners do not hold yet, are added by whoever composes the facts: the driver from
// `decl_sema`, the tests from their own tables.
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

  ast::QualType canonical(ast::QualType q) const { return ast::without_cv(in_->canonical(q)); }

  TypeClass classify(ast::QualType q) const {
    ast::QualType c = in_->types.canon(q);
    if (auto b = ast::builtin_of(*in_, c)) {
      if (*b == ast::Builtin::void_) return TypeClass::void_;
      if (*b == ast::Builtin::bool_) return TypeClass::bool_;
      if (ast::is_integral(*b)) return TypeClass::integer;
      return TypeClass::other;
    }
    if (in_->types.is<ast::TPointer>(c)) return TypeClass::pointer;
    if (in_->types.is<ast::TArray>(c)) return TypeClass::array;
    if (in_->types.is<ast::TFunction>(c)) return TypeClass::function;
    return TypeClass::other;
  }

  IntRep integer(ast::QualType q) const {
    ast::Builtin b = *ast::builtin_of(*in_, q);
    return {ast::is_signed(*target_, b), ast::width_of(*target_, b)};
  }

  ast::QualType promoted(ast::QualType q) { return ast::promoted(*in_, *target_, q); }
  // Pointee and element types keep their cv: `pointer_to` of them must give the same pointer type.
  ast::QualType pointee(ast::QualType q) const { return in_->canonical(ast::pointee_of(*in_, q)); }
  ast::QualType element(ast::QualType q) const {
    return in_->canonical(in_->types.get<ast::TArray>(in_->types.canon(q)).elem);
  }
  ast::QualType pointer_to(ast::QualType q) { return ast::pointer_to(*in_, in_->canonical(q)); }
  ast::QualType return_type(ast::QualType q) const {
    return canonical(in_->types.get<ast::TFunction>(in_->types.canon(q)).ret);
  }
  ast::QualType int_type() { return ast::builtin(*in_, ast::Builtin::int_); }
  ast::ConstId int_constant(ast::QualType q, std::int64_t v) { return ast::int_value(*in_, *target_, canonical(q), v); }
  ast::ConstId null_constant(ast::QualType) { return in_->intern_value(ast::VNullptr{}); }

 private:
  ast::Interners* in_;
  const ast::Target* target_;
};

}  // namespace ycxx::irgen
