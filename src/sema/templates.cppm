// Templates (#88): substitution of template arguments into types ([temp.inst], docs/design/ast.md
// §6.3), template argument deduction from a call ([temp.deduct.call], [temp.deduct.type]) and the
// partial ordering of function templates ([temp.func.order], [temp.deduct.partial]).
//
// Template parameters are positional in types (`TTemplateParam{depth, index}`, ast.md §5.1), so
// these are functions of types and argument lists only; what a name means is the caller's.
export module ycxx.sema:templates;

import std;
import ycxx.ast;
import ycxx.interp;
import ycxx.lex;
import :types;

export namespace ycxx::sema {

using ast::TemplateArg;
using ast::TemplateArgKind;

// The depth of the `VDepParam` that stands for a value-dependent expression in a template's
// definition, which only an instantiation evaluates.
inline constexpr u16 dependent_depth = 0x7ffe;

// The arguments a substitution gives, per template-head depth then parameter index (ast.md §5.6);
// a parameter without one stays as it is.
struct Bindings {
  std::vector<std::vector<std::optional<TemplateArg>>> levels;
  // What a placeholder type becomes ([dcl.type.auto.deduct]): the invented parameter deduction
  // works on, then the type it deduced. Without one, a placeholder stays as it is.
  std::optional<QualType> placeholder;

  std::optional<TemplateArg> at(u16 depth, u16 index) const {
    if (depth >= levels.size() || index >= levels[depth].size()) return std::nullopt;
    return levels[depth][index];
  }
  static Bindings of(u16 depth, std::span<const TemplateArg> args) {
    Bindings b;
    b.levels.resize(depth + 1);
    for (const TemplateArg& a : args) b.levels[depth].push_back(a);
    return b;
  }
};

// What deduction needs to know of a call argument ([temp.deduct.call]).
struct ArgShape {
  QualType type{};
  bool lvalue = false;
  bool deducible = true;  // false for an argument that does not supply one type
  std::vector<QualType> overloads{};  // non-template overloads for trial deduction
};

// How far a deduced A may differ from A at the top of a P/A pair ([temp.deduct.call]/4): with
// `more_cv` it may be more cv-qualified (P was a reference, /4.1); with `qualification` A may be
// a pointer that converts to the deduced A by a qualification conversion (/4.2).
// Only call deduction permits dropping a nonthrowing function specification.
struct Allowance {
  bool more_cv = false;
  bool qualification = false;
  bool function_conversion = false;
};

class Templates {
public:
  // `classes` is the unit's class records (#115), which a dependent name's lookup reads.
  Templates(World& w, const Classes& classes) : w_(&w), classes_(&classes) {}

  Types& types() { return w_->types; }
  ast::Interners& ins() { return w_->ins; }

  TemplateArg type_arg(QualType t) { return {TemplateArgKind::type, types().canonical(t), ConstId::none}; }
  TemplateArg value_arg(QualType t, ConstId v) { return {TemplateArgKind::value, types().canonical(Types::unqualified(t)), v}; }

  // The specialization of variable template `templ` for canonical arguments ([temp.spec.general]).
  // `constraints`: a constrained partial specialization's pattern, keyed by its constraints'
  // token fingerprint too (Fable's decision on #234).
  EntityId specialize_variable(EntityId templ, std::span<const TemplateArg> args, Fp128 constraints = {}) {
    const EntityRec& t = w_->entities.rec(templ);
    EntityRec spec{EntityKind::variable, t.name, {}, t.lang, t.linkage, t.first, t.scope};
    spec.templ = templ;
    spec.args.assign(args.begin(), args.end());
    spec.constraints = constraints;
    return w_->entities.entity(std::move(spec));
  }

  // ------------------------------------------------------------ substitution ([temp.inst])

  // `t` with the parameters `b` binds replaced by their arguments. None when that forms a type
  // that cannot exist ([temp.deduct.general]/11): a pointer to a reference, a reference to void, an
  // array of void, references, functions or unknown-bound arrays, a function returning an array
  // or a function, or a parameter of type void. A type parameter's cv-qualifiers are added to its argument
  // ([dcl.type.cv]: on a reference they are dropped).
  std::optional<QualType> substitute(QualType t, const Bindings& b, bool trailing_return = false) {
    TypeRec r = types().rec(t);
    Cv cv = Types::cv(types().canonical(t));
    switch (r.kind) {
    case TypeKind::template_param: {
      auto a = b.at(r.depth, r.index);
      if (!a) return t;
      if (a->kind != TemplateArgKind::type) return std::nullopt;
      return types().add_cv(a->type, cv);
    }
    case TypeKind::class_: {
      // `X<T…>`: the specialization for the substituted arguments ([temp.inst]).
      const EntityRec rec = w_->entities.rec(r.entity);
      if (rec.templ == EntityId::none || !types().is_dependent(t)) return t;
      auto args = substitute_args(rec.args, b);
      if (!args) return std::nullopt;
      return types().add_cv(types().class_type(specialize_class(rec.templ, *args)), cv);
    }
    case TypeKind::dep_name: {
      // `typename Q::n`: the member type `n` of the substituted Q, which must be a complete
      // class declaring one ([temp.deduct.general]/11).
      if (r.dep_form == ast::DepNameForm::template_name) return std::nullopt;  // not a type
      auto q = substitute(r.inner, b);
      if (!q) return std::nullopt;
      if (r.dep_form == ast::DepNameForm::template_id) {
        // `typename Q::template n<args>`: Q's member alias template n applied to the arguments
        // ([temp.names]/4, [temp.alias]/2).
        auto args = substitute_args(ins().seqs.elements(r.args), b);
        if (!args) return std::nullopt;
        if (types().is_dependent(*q))
          return types().add_cv(types().dep_template(*q, r.name, ast::DepNameForm::template_id, *args), cv);
        if (!types().is_class(*q)) return std::nullopt;
        auto templ = member_template_of(types().class_of(*q), r.name);
        if (!templ) return std::nullopt;
        auto t2 = apply_template(*templ, *args);
        if (!t2) return std::nullopt;
        return types().add_cv(*t2, cv);
      }
      if (types().is_dependent(*q)) return types().add_cv(types().dep_name(*q, r.name), cv);
      if (!types().is_class(*q)) return std::nullopt;
      // Asked at a point: the class completed there, a specialization instantiated for it
      // ([temp.inst]/2), or the members a class being defined has declared so far.
      if (member_type) {
        auto t = member_type(types().class_of(*q), r.name);
        if (!t) return std::nullopt;
        return types().add_cv(*t, cv);
      }
      const ClassRec* c = classes_->find(types().class_of(*q));
      if (!c) return std::nullopt;
      for (const auto& [n, mt, te] : c->types)
        if (n == r.name) return types().add_cv(mt, cv);
      return std::nullopt;
    }
    case TypeKind::builtin:
    case TypeKind::enum_:
    case TypeKind::other: return t;
    case TypeKind::placeholder: return b.placeholder ? types().add_cv(*b.placeholder, cv) : t;
    case TypeKind::pointer: {
      auto p = substitute(r.inner, b);
      if (!p || types().is_reference(*p) || types().is_cvref_function(*p)) return std::nullopt;
      return types().add_cv(types().pointer(*p), cv);
    }
    case TypeKind::member_pointer: {
      auto owner = substitute(r.member_owner, b);
      auto member = substitute(r.inner, b);
      if (!owner || !member || !types().valid_member_owner(*owner) || !types().valid_member_type(*member)) return std::nullopt;
      return types().add_cv(types().member_pointer(*owner, *member), cv);
    }
    case TypeKind::reference: {
      auto p = substitute(r.inner, b);
      if (!p || types().is_void(*p) || types().is_cvref_function(*p)) return std::nullopt;
      return r.rvalue_ref ? types().rvalue_ref(*p) : types().lvalue_ref(*p);  // collapsing ([dcl.ref])
    }
    case TypeKind::array: {
      auto e = substitute(r.inner, b);
      if (!e || !types().is_object(*e) || (types().is_array(*e) && types().rec(*e).bound_value == ConstId::none))
        return std::nullopt;
      if (r.bound_value == ConstId::none) return types().array_unknown(*e);
      if (ins().values.is_dependent(r.bound_value)) {
        // A bound that is a non-type parameter; one that is any other dependent expression is
        // known only to an instantiation of the body (#88).
        if (!ins().values.is<ast::VDepParam>(r.bound_value)) return std::nullopt;
        auto p = ins().values.get<ast::VDepParam>(r.bound_value);
        auto a = b.at(p.depth, p.index);
        if (!a) return p.depth == dependent_depth ? std::nullopt : std::optional(types().array_dependent(*e, r.bound_value));
        if (a->kind != TemplateArgKind::value) return std::nullopt;
        if (ins().values.is<ast::VDepParam>(a->value)) return types().array_dependent(*e, a->value);  // a unique value
        if (!ins().values.is<ast::VInt>(a->value)) return std::nullopt;
        // [dcl.array]/1: the bound's value is greater than zero.
        if (ast::is_signed(w_->target, *types().builtin_of(a->type)) && ast::is_negative(ast::int_bits(ins(), a->value)))
          return std::nullopt;
        if (ast::int_bits(ins(), a->value) == ast::Bits128{}) return std::nullopt;
        return types().array_dependent(*e, a->value);
      }
      return types().array(*e, r.bound);
    }
    case TypeKind::function: {
      auto return_type = [&]() -> std::optional<QualType> {
        auto ret = substitute(r.inner, b);
        if (!ret || types().is_array(*ret) || types().is_function(*ret)) return std::nullopt;
        return ret;
      };
      auto ret = trailing_return ? std::optional<QualType>{} : return_type();
      if (!trailing_return && !ret) return std::nullopt;
      std::vector<QualType> ps;
      auto append_parameter = [&](QualType p) {
        if (types().is_void(p) || types().is_cvref_function(p)) return false;
        ps.push_back(types().is_function(p) ? types().pointer(p) : p);
        return true;
      };
      for (QualType p : r.params) {
        // A function parameter pack's type: one parameter for each element ([temp.variadic]/8).
        if (types().kind(p) == TypeKind::pack_expansion) {
          auto xs = substitute_expansion(type_arg(p), b);
          if (!xs) return std::nullopt;
          for (const TemplateArg& x : *xs)
            if (!append_parameter(x.type)) return std::nullopt;
          continue;
        }
        auto s = substitute(p, b);
        if (!s || !append_parameter(*s)) return std::nullopt;
      }
      if (trailing_return) ret = return_type();
      if (!ret) return std::nullopt;
      return types().function(*ret, ps, r.variadic, r.linkage, r.fn_cv, r.fn_ref, r.fn_noexcept);
    }
    case TypeKind::dep_spec: {
      // `TT<args>`: the specialization of TT's argument for the substituted arguments, or the
      // same form while TT is not bound to a template ([temp.arg.template]).
      auto args = substitute_args(ins().seqs.elements(r.args), b);
      if (!args) return std::nullopt;
      auto x = b.at(r.depth, r.index);
      if (!x) return types().add_cv(types().dep_spec(r.depth, r.index, r.pack, *args), cv);
      if (x->kind != TemplateArgKind::template_) return std::nullopt;
      if (x->templ == EntityId::none) {
        TypeRec p = types().rec(x->type);
        // `Q::template n` with Q still dependent: `typename Q::template n<args>`.
        if (p.kind == TypeKind::dep_name)
          return types().add_cv(types().dep_template(p.inner, p.name, ast::DepNameForm::template_id, *args), cv);
        return types().add_cv(types().dep_spec(p.depth, p.index, p.pack, *args), cv);
      }
      auto t2 = apply_template(x->templ, *args);
      if (!t2) return std::nullopt;
      return types().add_cv(*t2, cv);
    }
    case TypeKind::builtin_spec: {
      auto args = substitute_args(ins().seqs.elements(r.args), b);
      if (!args) return std::nullopt;
      auto t2 = builtin_template(ast::BuiltinTemplate(r.trait), *args);
      if (!t2) return std::nullopt;
      return types().add_cv(*t2, cv);
    }
    case TypeKind::pack_expansion: return std::nullopt;  // only in a list, by `substitute_expansion`
    case TypeKind::pack_index: {
      auto x = pack_element(r, b);
      if (!x) return std::nullopt;
      return types().add_cv(*x, cv);
    }
    case TypeKind::transform: {
      auto a = substitute(r.inner, b);
      if (!a) return std::nullopt;
      auto x = transform(lex::TypeTrait(r.trait), *a);
      if (!x) return std::nullopt;
      return types().add_cv(*x, cv);
    }
    }
    return t;
  }

  // ------------------------------------------------------------ built-in templates (docs/EXTENSIONS.md)

  // `__make_integer_seq<Seq, T, N>`, Clang's: `Seq<T, 0, 1, …, N-1>`, for an integral T other
  // than bool and N not negative; a dependent `TBuiltinSpec` while an argument is dependent.
  std::optional<QualType> builtin_template(ast::BuiltinTemplate which, std::span<const TemplateArg> args) {
    if (std::ranges::any_of(args, [&](const TemplateArg& a) { return w_->arg_dependent(a); }))
      return types().builtin_spec(which, args);
    if (args.size() != 3 || args[0].kind != TemplateArgKind::template_ || args[1].kind != TemplateArgKind::type ||
        args[2].kind != TemplateArgKind::value)
      return std::nullopt;
    QualType t = types().canonical(args[1].type);
    if (!types().is_integral(t) || types().is(t, Builtin::bool_)) return std::nullopt;
    // N is a converted constant expression of type T ([temp.arg.nontype]): not narrowed.
    auto nv = converted_value(args[2], t);
    if (!nv || !ins().values.is<ast::VInt>(nv->value)) return std::nullopt;
    ast::Bits128 n = ast::int_bits(ins(), nv->value);
    auto bt = types().builtin_of(t);
    if (bt && ast::is_signed(w_->target, *bt) && ast::is_negative(n)) return std::nullopt;
    constexpr u64 limit = u64(1) << 20;  // beyond any instantiation an implementation would finish
    if (n.hi != 0 || n.lo > limit) return std::nullopt;
    std::vector<TemplateArg> xs{type_arg(t)};
    for (u64 i = 0; i < n.lo; ++i) xs.push_back(value_arg(t, w_->values.integer(t, i)));
    return apply_template(args[0].templ, xs);
  }

  // `T...[I]` with the parameters `b` binds ([dcl.type.pack.index]): the selected element once the
  // pack and the index are known; while the pack is unbound the specifier, and while only the
  // index is dependent the specifier over the pack's elements. In partial ordering a pack's
  // unique stand-in is a pack parameter of its own. An index past the elements, or one at or after
  // an expansion in the pack, fails.
  std::optional<QualType> pack_element(const TypeRec& r, const Bindings& b) {
    auto i = substitute_value(r.bound_value, b);
    if (!i) return std::nullopt;
    if (r.pack) {
      auto es = substitute_args(ins().seqs.elements(r.args), b);
      if (!es) return std::nullopt;
      if (ins().values.is_dependent(*i))
        return types().pack_index(r.inner, *i, ins().intern_seq(std::span<const TemplateArg>(*es)));
      return select_element(*es, *i);
    }
    TypeRec p = types().rec(r.inner);
    auto a = b.at(p.depth, p.index);
    if (!a) return types().pack_index(r.inner, *i);
    if (a->kind == TemplateArgKind::type && types().kind(a->type) == TypeKind::template_param && types().rec(a->type).pack)
      return types().pack_index(types().canonical(a->type), *i);
    if (a->kind != TemplateArgKind::pack) return std::nullopt;
    if (ins().values.is_dependent(*i)) return types().pack_index(r.inner, *i, a->elems);
    return select_element(elements(*a), *i);
  }
  // The element of a pack's arguments at the constant `index`, a type ([dcl.type.pack.index]/2:
  // the index is less than the number of elements). Past `k` known elements, an expansion
  // `U...` of a pack parameter gives `U...[index - k]`, still dependent; another expansion there
  // has no form yet.
  std::optional<QualType> select_element(std::span<const TemplateArg> es, ConstId index) {
    ast::Bits128 v = ast::int_bits(ins(), index);
    auto b = types().builtin_of(ins().values.get<ast::VInt>(index).type);
    if ((b && ast::is_signed(w_->target, *b) && ast::is_negative(v)) || v.hi != 0) return std::nullopt;
    for (std::size_t k = 0; k < es.size(); ++k) {
      if (es[k].kind != TemplateArgKind::type) return std::nullopt;
      TypeRec e = types().rec(es[k].type);
      if (e.kind == TypeKind::pack_expansion) {
        if (k + 1 != es.size() || types().kind(e.inner) != TypeKind::template_param) return std::nullopt;
        ConstId rest = w_->values.integer(types().builtin(Builtin::unsigned_long), v.lo - k);
        return types().pack_index(types().canonical(e.inner), rest);
      }
      if (k == v.lo) return es[k].type;
    }
    return std::nullopt;
  }

  // ------------------------------------------------------------ type-trait built-ins (docs/EXTENSIONS.md)

  // The type a transforming built-in gives for `t`, as the corresponding standard trait's `type`
  // ([meta.trans]); a dependent `TTransform` while `t` is dependent; none where it does not apply
  // (`__underlying_type` of a non-enumeration).
  std::optional<QualType> transform(lex::TypeTrait trait, QualType t) {
    using enum lex::TypeTrait;
    if (types().is_dependent(t)) return types().transform(u8(trait), t);
    QualType c = types().canonical(t);
    auto with_cv = [&](QualType x, Cv keep) { return types().add_cv(Types::unqualified(x), Cv(std::to_underlying(Types::cv(x)) & std::to_underlying(keep))); };
    // An array's cv-qualifiers are its elements' ([basic.type.qualifier]/3).
    auto strip = [&](this auto& self, QualType x, Cv keep) -> QualType {
      if (types().is_array(x)) {
        TypeRec r = types().rec(x);
        QualType e = self(r.inner, keep);
        return r.bound_value == ConstId::none ? types().array_unknown(e) : types().array_dependent(e, r.bound_value);
      }
      return with_cv(x, keep);
    };
    // Whether `x` can be referred to ([defns.referenceable]).
    auto referenceable = [&](QualType x) {
      return !types().is_void(x) && !types().is_cvref_function(x);
    };
    switch (trait) {
    case remove_cv: return strip(c, Cv::none);
    case remove_const: return strip(c, Cv(2));
    case remove_volatile: return strip(c, Cv(1));
    case remove_reference_t: return types().referee(c);
    case remove_cvref: return strip(types().canonical(types().referee(c)), Cv::none);
    case remove_pointer: return types().is_pointer(c) ? types().rec(c).inner : c;
    case add_pointer: {
      QualType x = types().referee(c);
      return referenceable(x) || types().is_void(x) ? types().pointer(x) : c;
    }
    case add_lvalue_reference: return referenceable(c) ? types().lvalue_ref(c) : c;
    case add_rvalue_reference: return referenceable(c) ? types().rvalue_ref(c) : c;
    case remove_extent: return types().is_array(c) ? types().rec(c).inner : c;
    case remove_all_extents: return types().innermost_array_element(c);
    case decay: {
      QualType x = types().canonical(types().referee(c));
      if (types().is_array(x)) return types().pointer(types().rec(x).inner);
      if (types().is_function(x)) return referenceable(x) ? types().pointer(x) : x;
      return Types::unqualified(x);
    }
    case underlying_type: {
      if (!types().is_enum(c)) return std::nullopt;
      const EnumRec* e = classes_->find_enum(types().enum_of(c));
      if (!e || !e->underlying) return std::nullopt;
      return *e->underlying;
    }
    default: return std::nullopt;
    }
  }

  std::optional<ConstId> is_same_value(QualType lhs, QualType rhs) {
    if (!types().is_dependent(lhs) && !types().is_dependent(rhs))
      return w_->values.integer(types().builtin(Builtin::bool_), ins().same_type(lhs, rhs));
    if (!structural_operand(lhs) || !structural_operand(rhs)) return std::nullopt;
    const std::array operands{type_arg(lhs), type_arg(rhs)};
    return ins().intern_value(ast::VDepOp{ast::DepOp::is_same, ins().intern_seq(std::span<const TemplateArg>(operands))});
  }

  // The owning token replay checks the concrete multi-operand query.
  std::optional<ConstId> replayed_type_trait_pattern(ast::DepOp op, std::span<const TemplateArg> operands)
    pre(op == ast::DepOp::is_constructible || op == ast::DepOp::is_nothrow_constructible ||
        op == ast::DepOp::is_trivially_constructible ||
        (operands.size() == 2 && (op == ast::DepOp::is_assignable || op == ast::DepOp::is_nothrow_assignable ||
                                  op == ast::DepOp::is_trivially_assignable || op == ast::DepOp::is_convertible ||
                                  op == ast::DepOp::is_nothrow_convertible || op == ast::DepOp::is_base_of ||
                                  op == ast::DepOp::is_virtual_base_of ||
                                  op == ast::DepOp::is_pointer_interconvertible_base_of ||
                                  op == ast::DepOp::is_layout_compatible ||
                                  op == ast::DepOp::reference_constructs_from_temporary ||
                                  op == ast::DepOp::reference_converts_from_temporary)))
  {
    if (!std::ranges::any_of(operands, [&](const TemplateArg& a) { return types().is_dependent(a.type); }) ||
        !std::ranges::all_of(operands, [&](const TemplateArg& a) { return structural_operand(a); })) return std::nullopt;
    return ins().intern_value(ast::VDepOp{op, ins().intern_seq(operands)});
  }

  // A class specialization has the class kind of its primary ([temp.class.general]).
  std::optional<bool> is_union_class(EntityId c) const {
    if (EntityId primary = w_->entities.rec(c).templ; primary != EntityId::none)
      if (auto key = classes_->is_union_class(primary)) return key;
    return classes_->is_union_class(c);
  }

  std::optional<ConstId> unary_trait_value(ast::DepOp op, QualType type)
    pre(op != ast::DepOp::is_same && op != ast::DepOp::logical_not && op != ast::DepOp::logical_and &&
        op != ast::DepOp::is_constructible && op != ast::DepOp::is_nothrow_constructible &&
        op != ast::DepOp::is_trivially_constructible && op != ast::DepOp::is_assignable &&
        op != ast::DepOp::is_nothrow_assignable && op != ast::DepOp::is_trivially_assignable &&
        op != ast::DepOp::is_convertible && op != ast::DepOp::is_nothrow_convertible && op != ast::DepOp::is_base_of &&
        op != ast::DepOp::is_virtual_base_of && op != ast::DepOp::is_pointer_interconvertible_base_of &&
        op != ast::DepOp::is_layout_compatible &&
        op != ast::DepOp::reference_constructs_from_temporary && op != ast::DepOp::reference_converts_from_temporary)
  {
    if (types().is_dependent(type)) {
      if (!structural_operand(type)) return std::nullopt;
      const std::array operands{type_arg(type)};
      return ins().intern_value(ast::VDepOp{op, ins().intern_seq(std::span<const TemplateArg>(operands))});
    }
    bool result = false;
    switch (op) {
    case ast::DepOp::is_reference: result = types().is_reference(type); break;
    case ast::DepOp::is_const:
    case ast::DepOp::is_volatile: {
      // An array has the cv-qualification of its element type ([basic.type.qualifier]).
      QualType element = types().innermost_array_element(types().canonical(type));
      result = op == ast::DepOp::is_const ? Types::is_const(element) : Types::is_volatile(element);
      break;
    }
    case ast::DepOp::is_array: result = types().is_array(type); break;
    case ast::DepOp::is_bounded_array:
    case ast::DepOp::is_unbounded_array:
      if (types().is_array(type)) {
        const bool has_bound = types().rec(type).bound_value != ConstId::none;
        result = has_bound == (op == ast::DepOp::is_bounded_array);
      }
      break;
    case ast::DepOp::is_pointer: result = types().is_pointer(type); break;
    case ast::DepOp::is_function: result = types().is_function(type); break;
    case ast::DepOp::is_object: result = types().is_object(type); break;
    case ast::DepOp::is_enum: result = types().is_enum(type); break;
    case ast::DepOp::is_scoped_enum:
      if (types().is_enum(type)) {
        const EntityId e = types().enum_of(type);
        const auto scoped = defining_enum_is_scoped ? defining_enum_is_scoped(e) : std::nullopt;
        if (scoped) result = *scoped;
        else if (const EnumRec* record = classes_->find_enum(e)) result = record->scoped;
      }
      break;
    case ast::DepOp::is_member_pointer: result = types().is_member_pointer(type); break;
    case ast::DepOp::is_member_object_pointer:
    case ast::DepOp::is_member_function_pointer:
      if (types().is_member_pointer(type)) {
        const bool member_is_function = types().is_function(types().rec(type).inner);
        result = member_is_function == (op == ast::DepOp::is_member_function_pointer);
      }
      break;
    case ast::DepOp::is_union:
    case ast::DepOp::is_class:
      if (types().is_class(type)) {
        auto is_union = is_union_class(types().class_of(type));
        if (!is_union) return std::nullopt;
        result = *is_union == (op == ast::DepOp::is_union);
      }
      break;
    // The token-owning replay checks completeness and diagnoses a concrete operand.
    case ast::DepOp::is_aggregate:
    case ast::DepOp::is_trivially_copyable:
    case ast::DepOp::is_implicit_lifetime:
    case ast::DepOp::has_unique_object_representations:
    case ast::DepOp::is_destructible:
    case ast::DepOp::is_nothrow_destructible:
    case ast::DepOp::is_trivially_destructible:
    case ast::DepOp::is_final:
    case ast::DepOp::is_polymorphic:
    case ast::DepOp::is_abstract:
    case ast::DepOp::has_virtual_destructor:
    case ast::DepOp::is_standard_layout:
    case ast::DepOp::alignof_type:
    case ast::DepOp::is_empty: return std::nullopt;
    case ast::DepOp::is_same:
    case ast::DepOp::is_constructible:
    case ast::DepOp::is_nothrow_constructible:
    case ast::DepOp::is_trivially_constructible:
    case ast::DepOp::is_assignable:
    case ast::DepOp::is_nothrow_assignable:
    case ast::DepOp::is_trivially_assignable:
    case ast::DepOp::is_convertible:
    case ast::DepOp::is_nothrow_convertible:
    case ast::DepOp::is_base_of:
    case ast::DepOp::is_virtual_base_of:
    case ast::DepOp::is_pointer_interconvertible_base_of:
    case ast::DepOp::is_layout_compatible:
    case ast::DepOp::reference_constructs_from_temporary:
    case ast::DepOp::reference_converts_from_temporary:
    case ast::DepOp::logical_not:
    case ast::DepOp::logical_and: std::unreachable();
    }
    return w_->values.integer(types().builtin(Builtin::bool_), result);
  }

  std::optional<ConstId> negated_variable_value(ConstId v, std::size_t negations)
    pre(negations != 0)
  {
    if (!ins().values.is<ast::VDepVariable>(v) || !structural_value(v)) return std::nullopt;
    for (std::size_t i = 0; i < negations; ++i) v = logical_not_value(v);
    return v;
  }

  std::optional<ConstId> conjunction_value(ConstId left, ConstId right) {
    if (!structural_value(left) || !structural_value(right)) return std::nullopt;
    QualType type = types().builtin(Builtin::bool_);
    const std::array operands{value_arg(type, left), value_arg(type, right)};
    return ins().intern_value(ast::VDepOp{ast::DepOp::logical_and, ins().intern_seq(std::span<const TemplateArg>(operands))});
  }

  // Template arguments `args` with the parameters `b` binds replaced by their arguments; a pack's
  // expansions expanded in place ([temp.variadic]/8).
  std::optional<std::vector<TemplateArg>> substitute_args(std::span<const TemplateArg> args, const Bindings& b) {
    std::vector<TemplateArg> out;
    for (const TemplateArg& a : args) {
      // An expansion as written in `TT<Ts...>`, not grouped into a pack: expanded in place.
      if (is_expansion(a)) {
        auto xs = substitute_expansion(a, b);
        if (!xs) return std::nullopt;
        out.insert(out.end(), xs->begin(), xs->end());
        continue;
      }
      if (a.kind != TemplateArgKind::pack) {
        auto s = substitute_arg(a, b);
        if (!s) return std::nullopt;
        out.push_back(*s);
        continue;
      }
      std::vector<TemplateArg> elems;
      for (const TemplateArg& e : ins().seqs.elements(a.elems)) {
        if (is_expansion(e)) {
          auto xs = substitute_expansion(e, b);
          if (!xs) return std::nullopt;
          elems.insert(elems.end(), xs->begin(), xs->end());
        } else {
          auto x = substitute_arg(e, b);
          if (!x) return std::nullopt;
          elems.push_back(*x);
        }
      }
      out.push_back(pack(elems));
    }
    return out;
  }
  // One type or value argument, substituted.
  std::optional<TemplateArg> substitute_arg(const TemplateArg& a, const Bindings& b) {
    // A template: itself, or the argument of the template template parameter it is.
    if (a.kind == TemplateArgKind::template_) {
      if (a.templ == EntityId::none && types().kind(a.type) == TypeKind::dep_name) {
        // `Q::template n` ([temp.names]/4): Q's member template once Q is a class.
        TypeRec r = types().rec(a.type);
        auto q = substitute(r.inner, b);
        if (!q) return std::nullopt;
        if (types().is_dependent(*q))
          return TemplateArg{TemplateArgKind::template_, types().dep_template(*q, r.name, ast::DepNameForm::template_name),
                             ConstId::none};
        if (!types().is_class(*q)) return std::nullopt;
        auto templ = member_template_of(types().class_of(*q), r.name);
        if (!templ) return std::nullopt;
        return TemplateArg{TemplateArgKind::template_, QualType{}, ConstId::none, {}, *templ};
      }
      if (a.templ != EntityId::none || types().kind(a.type) != TypeKind::template_param) return a;
      TypeRec r = types().rec(a.type);
      auto x = b.at(r.depth, r.index);
      if (!x) return a;
      if (x->kind != TemplateArgKind::template_) return std::nullopt;
      return *x;
    }
    if (a.kind == TemplateArgKind::type) {
      auto s = substitute(a.type, b);
      if (!s) return std::nullopt;
      return type_arg(*s);
    }
    QualType t = a.type;
    if (types().is_dependent(t))
      if (auto st = substitute(t, b)) t = types().canonical(Types::unqualified(*st));
    if (ins().values.is<ast::VDepMember>(a.value)) {
      auto value = substitute_member_value(a.value, b);
      if (!value) return std::nullopt;
      return converted_value(*value, t);
    }
    if (ins().values.is<ast::VDepCall>(a.value)) {
      auto value = substitute_call_value(a.value, b);
      if (!value) return std::nullopt;
      if (ins().values.is_dependent(value->value)) return value_arg(t, value->value);
      return converted_value(*value, t);
    }
    if (ins().values.is<ast::VDepConcept>(a.value)) {
      auto value = substitute_concept_value(a.value, b);
      if (!value) return std::nullopt;
      if (ins().values.is_dependent(*value)) return value_arg(t, *value);
      return converted_value(value_arg(types().builtin(Builtin::bool_), *value), t);
    }
    auto v = substitute_value(a.value, b);
    if (!v) return std::nullopt;
    auto is_predicate = [&](ConstId value) {
      return ins().values.is<ast::VDepOp>(value) &&
             ins().values.get<ast::VDepOp>(value).op != ast::DepOp::alignof_type;
    };
    if ((is_predicate(a.value) || is_predicate(*v)) && !types().is(t, Builtin::bool_))
      return std::nullopt;
    if (ins().values.is<ast::VDepVariable>(a.value) && !ins().values.is_dependent(*v)) {
      QualType source = t;
      if (ins().values.is<ast::VInt>(*v)) source = ins().values.get<ast::VInt>(*v).type;
      if (ins().values.is<ast::VFloat>(*v)) source = ins().values.get<ast::VFloat>(*v).type;
      return converted_value(value_arg(source, *v), t);
    }
    return TemplateArg{TemplateArgKind::value, t, *v};
  }
  // A value with the parameters `b` binds replaced. Evaluate dependent operations when their
  // operands are concrete.
  std::optional<ConstId> substitute_value(ConstId v, const Bindings& b) {
    if (ins().values.is<ast::VDepParam>(v)) {
      auto p = ins().values.get<ast::VDepParam>(v);
      auto a = b.at(p.depth, p.index);
      if (!a) return v;
      if (a->kind != TemplateArgKind::value) return std::nullopt;
      return a->value;
    }
    if (ins().values.is<ast::VDepSizeofPack>(v)) {
      auto p = ins().values.get<ast::VDepSizeofPack>(v);
      auto a = b.at(p.depth, p.index);
      if (!a) return v;
      if (a->kind != TemplateArgKind::pack) return std::nullopt;
      return w_->values.integer(types().builtin(Builtin::unsigned_long), ins().seqs.size(a->elems));
    }
    if (ins().values.is<ast::VDepMember>(v))
      return substitute_member_value(v, b).transform([](const TemplateArg& a) { return a.value; });
    if (ins().values.is<ast::VDepConcept>(v)) return substitute_concept_value(v, b);
    if (ins().values.is<ast::VDepVariable>(v)) return substitute_variable_value(v, b, variable_constant);
    if (ins().values.is<ast::VDepCall>(v))
      return substitute_call_value(v, b).transform([](const TemplateArg& a) { return a.value; });
    if (ins().values.is<ast::VDepOp>(v)) {
      auto op = ins().values.get<ast::VDepOp>(v);
      switch (op.op) {
      case ast::DepOp::alignof_type: {
        auto operand = substitute_arg(ins().seqs.at(op.operands, 0), b);
        if (!operand || !types().is_dependent(operand->type)) return std::nullopt;
        const std::array operands{*operand};
        return ins().intern_value(ast::VDepOp{op.op, ins().intern_seq(std::span<const TemplateArg>(operands))});
      }
      case ast::DepOp::is_constructible:
      case ast::DepOp::is_nothrow_constructible:
      case ast::DepOp::is_trivially_constructible:
      case ast::DepOp::is_assignable:
      case ast::DepOp::is_nothrow_assignable:
      case ast::DepOp::is_trivially_assignable:
      case ast::DepOp::is_convertible:
      case ast::DepOp::is_nothrow_convertible:
      case ast::DepOp::reference_constructs_from_temporary:
      case ast::DepOp::reference_converts_from_temporary:
      case ast::DepOp::is_base_of:
      case ast::DepOp::is_virtual_base_of:
      case ast::DepOp::is_pointer_interconvertible_base_of:
      case ast::DepOp::is_layout_compatible: {
        auto operands = substitute_args(ins().seqs.elements(op.operands), b);
        return operands ? replayed_type_trait_pattern(op.op, *operands) : std::nullopt;
      }
      case ast::DepOp::is_same: {
        auto lhs = substitute_arg(ins().seqs.at(op.operands, 0), b);
        if (!lhs) return std::nullopt;
        auto rhs = substitute_arg(ins().seqs.at(op.operands, 1), b);
        if (!rhs) return std::nullopt;
        return is_same_value(lhs->type, rhs->type);
      }
      case ast::DepOp::logical_not:
      case ast::DepOp::logical_and:
        return substitute_boolean_operand(v, b, BooleanSubstitution::evaluate);
      case ast::DepOp::is_reference:
      case ast::DepOp::is_const:
      case ast::DepOp::is_volatile:
      case ast::DepOp::is_array:
      case ast::DepOp::is_bounded_array:
      case ast::DepOp::is_unbounded_array:
      case ast::DepOp::is_pointer:
      case ast::DepOp::is_function:
      case ast::DepOp::is_enum:
      case ast::DepOp::is_scoped_enum:
      case ast::DepOp::is_union:
      case ast::DepOp::is_class:
      case ast::DepOp::is_member_pointer:
      case ast::DepOp::is_member_object_pointer:
      case ast::DepOp::is_member_function_pointer:
      case ast::DepOp::is_aggregate:
      case ast::DepOp::is_trivially_copyable:
      case ast::DepOp::is_implicit_lifetime:
      case ast::DepOp::has_unique_object_representations:
      case ast::DepOp::is_destructible:
      case ast::DepOp::is_nothrow_destructible:
      case ast::DepOp::is_trivially_destructible:
      case ast::DepOp::is_final:
      case ast::DepOp::is_polymorphic:
      case ast::DepOp::is_abstract:
      case ast::DepOp::has_virtual_destructor:
      case ast::DepOp::is_standard_layout:
      case ast::DepOp::is_empty:
      case ast::DepOp::is_object: {
        auto operand = substitute_arg(ins().seqs.at(op.operands, 0), b);
        if (!operand) return std::nullopt;
        return unary_trait_value(op.op, operand->type);
      }
      }
    }
    return v;
  }

  // `t` with each `typename Q::n` that `f(Q, n)` resolves replaced by what it gives: a member of the
  // current instantiation named with a qualifier, found ([temp.dep.type]/1).
  QualType resolve_dep_names(QualType t, const std::function<std::optional<QualType>(QualType, NameId)>& f) {
    TypeRec r = types().rec(t);
    Cv cv = Types::cv(types().canonical(t));
    switch (r.kind) {
    case TypeKind::dep_name:
      if (r.dep_form != ast::DepNameForm::plain) return t;
      if (auto m = f(r.inner, r.name)) return types().add_cv(*m, cv);
      return t;
    case TypeKind::pointer: return types().add_cv(types().pointer(resolve_dep_names(r.inner, f)), cv);
    case TypeKind::member_pointer: {
      QualType owner = resolve_dep_names(r.member_owner, f), member = resolve_dep_names(r.inner, f);
      if (!types().valid_member_owner(owner) || !types().valid_member_type(member)) return t;
      return types().add_cv(types().member_pointer(owner, member), cv);
    }
    case TypeKind::reference:
      return r.rvalue_ref ? types().rvalue_ref(resolve_dep_names(r.inner, f)) : types().lvalue_ref(resolve_dep_names(r.inner, f));
    case TypeKind::function: {
      std::vector<QualType> ps;
      for (QualType p : r.params) ps.push_back(resolve_dep_names(p, f));
      return types().function(resolve_dep_names(r.inner, f), ps, r.variadic, r.linkage, r.fn_cv, r.fn_ref, r.fn_noexcept);
    }
    default: return t;
    }
  }

  // ------------------------------------------------------------ constraints ([temp.constr])

  // `l op r` of two normal forms (`op` 0: conjunction, 1: disjunction); either may be none.
  ConstId constraint_op(u8 op, ConstId l, ConstId r) {
    if (l == ConstId::none) return r;
    if (r == ConstId::none) return l;
    std::array<ConstId, 2> xs{l, r};
    return ins().intern_value(ast::VConstraint{op, ins().intern_seq(std::span<const ConstId>(xs))});
  }
  // A mapping entry as a template argument, and back.
  TemplateArg mapping_arg(ConstId v) {
    if (ins().values.is<ast::VConstraintPack>(v)) {
      std::vector<TemplateArg> elements;
      for (ConstId e : ins().seqs.elements(ins().values.get<ast::VConstraintPack>(v).elements))
        elements.push_back(mapping_arg(e));
      return pack(elements);
    }
    if (ins().values.is<ast::VType>(v)) {
      auto t = ins().values.get<ast::VType>(v);
      return type_arg(types().add_cv(t.type, Cv(t.cv)));
    }
    if (ins().values.is<ast::VConstraintEnum>(v)) {
      auto e = ins().values.get<ast::VConstraintEnum>(v);
      return value_arg(e.type, e.value);
    }
    // An integer carries its type; another value takes its parameter's (AtomAnalysis).
    return {TemplateArgKind::value, ins().values.is<ast::VInt>(v) ? ins().values.get<ast::VInt>(v).type : QualType{}, v};
  }
  ConstId mapping_value(const TemplateArg& a) {
    if (a.kind == TemplateArgKind::pack) {
      std::vector<ConstId> elements;
      for (const TemplateArg& e : ins().seqs.elements(a.elems))
        elements.push_back(mapping_value(e));
      return ins().intern_value(ast::VConstraintPack{ins().intern_seq(std::span<const ConstId>(elements))});
    }
    if (a.kind == TemplateArgKind::type) {
      QualType c = types().canonical(a.type);
      return ins().intern_value(ast::VType{Types::unqualified(c), u8(std::to_underlying(Types::cv(c)))});
    }
    if (ins().values.is<ast::VInt>(a.value) && types().is_enum(a.type))
      return ins().intern_value(ast::VConstraintEnum{types().canonical(Types::unqualified(a.type)), a.value});
    return a.value;
  }
  // A normal form with the parameters `b` binds replaced in its atoms' mappings
  // ([temp.constr.normal]/1.4); none when a mapping cannot be formed, which in a concept-id is
  // ill-formed.
  std::optional<ConstId> substitute_constraint(ConstId c, const Bindings& b) {
    if (c == ConstId::none) return c;
    if (ins().values.is<ast::VConstraint>(c)) {
      auto v = ins().values.get<ast::VConstraint>(c);
      std::vector<ConstId> ops;
      for (ConstId o : ins().seqs.elements(v.operands)) {
        auto s = substitute_constraint(o, b);
        if (!s) return std::nullopt;
        ops.push_back(*s);
      }
      return ins().intern_value(ast::VConstraint{v.op, ins().intern_seq(std::span<const ConstId>(ops))});
    }
    auto a = ins().values.get<ast::VAtom>(c);
    std::vector<ConstId> m;
    for (ConstId e : ins().seqs.elements(a.mapping)) {
      if (e == ConstId::none) {
        m.push_back(e);
        continue;
      }
      auto s = [&]() -> std::optional<ConstId> {
        if (ins().values.is<ast::VConstraintPack>(e)) {
          const std::array args{mapping_arg(e)};
          return substitute_args(args, b).transform([&](const auto& mapped) { return mapping_value(mapped.front()); });
        }
        if (ins().values.is<ast::VType>(e))
          return substitute_arg(mapping_arg(e), b).transform([&](const TemplateArg& arg) { return mapping_value(arg); });
        if (ins().values.is<ast::VDepParam>(e)) {
          auto p = ins().values.get<ast::VDepParam>(e);
          if (auto arg = b.at(p.depth, p.index); arg && arg->kind == TemplateArgKind::value)
            return mapping_value(*arg);
        }
        if (ins().values.is<ast::VDepMember>(e))
          return substitute_member_value(e, b).transform([&](const TemplateArg& arg) { return mapping_value(arg); });
        if (ins().values.is<ast::VDepCall>(e))
          return substitute_call_value(e, b).transform([&](const TemplateArg& arg) { return mapping_value(arg); });
        if (ins().values.is<ast::VDepVariable>(e))
          return substitute_variable_value(e, b, [&](EntityId spec) -> std::optional<ConstId> {
            if (auto global = variable_global ? variable_global(spec) : std::nullopt; global && global->value)
              return mapping_value(value_arg(types().referee(global->type), *global->value));
            return variable_constant ? variable_constant(spec) : std::nullopt;
          });
        return substitute_value(e, b);
      }();
      // An invalid mapping leaves the atom unsatisfied ([temp.constr.atomic]/3): an empty
      // disjunction, false.
      if (!s) return ins().intern_value(ast::VConstraint{1, ins().intern_seq(std::span<const ConstId>{})});
      m.push_back(*s);
    }
    a.mapping = ins().intern_seq(std::span<const ConstId>(m));
    return ins().intern_value(a);
  }
  // Whether an atom's mapping still names a template parameter: its satisfaction waits for them.
  bool constraint_dependent(ConstId c) {
    if (c == ConstId::none) return false;
    if (ins().values.is<ast::VConstraint>(c))
      return std::ranges::any_of(ins().seqs.elements(ins().values.get<ast::VConstraint>(c).operands),
                                 [&](ConstId o) { return constraint_dependent(o); });
    for (ConstId e : ins().seqs.elements(ins().values.get<ast::VAtom>(c).mapping))
      if (e != ConstId::none && w_->arg_dependent(mapping_arg(e))) return true;
    return false;
  }
  // Whether normal form `p` subsumes `q` ([temp.constr.order]/1): each disjunctive clause of p's
  // disjunctive normal form subsumes each conjunctive clause of q's conjunctive normal form, a
  // clause subsuming another when they share an atom. Past a fixed number of clauses neither
  // subsumes, deterministically.
  bool subsumes(ConstId p, ConstId q) {
    if (q == ConstId::none) return true;
    if (p == ConstId::none) return false;
    auto dnf = clauses(p, 1);
    auto cnf = clauses(q, 0);
    if (!dnf || !cnf) return false;
    for (const auto& pi : *dnf)
      for (const auto& qj : *cnf)
        if (std::ranges::none_of(pi, [&](ConstId a) { return std::ranges::contains(qj, a); })) return false;
    return true;
  }

private:
  // The clauses of `c`'s disjunctive (`outer` 1) or conjunctive (`outer` 0) normal form, each a
  // set of atoms; none past the budget.
  std::optional<std::vector<std::vector<ConstId>>> clauses(ConstId c, u8 outer) {
    constexpr std::size_t budget = 65536;
    if (ins().values.is<ast::VAtom>(c)) return std::vector<std::vector<ConstId>>{{c}};
    auto v = ins().values.get<ast::VConstraint>(c);
    // No operands (an atom whose mapping was invalid, false): no clause beside the others, or one
    // empty clause in every combination.
    if (ins().seqs.size(v.operands) == 0)
      return v.op == outer ? std::vector<std::vector<ConstId>>{} : std::vector<std::vector<ConstId>>{{}};
    std::vector<std::vector<ConstId>> acc;
    bool first = true;
    for (ConstId o : ins().seqs.elements(v.operands)) {
      auto sub = clauses(o, outer);
      if (!sub) return std::nullopt;
      if (v.op == outer) {  // the outer operator: the clauses side by side
        acc.insert(acc.end(), sub->begin(), sub->end());
      } else if (first) {  // the inner operator: every combination
        acc = std::move(*sub);
      } else {
        std::vector<std::vector<ConstId>> next;
        for (const auto& x : acc)
          for (const auto& y : *sub) {
            std::vector<ConstId> z = x;
            for (ConstId a : y)
              if (!std::ranges::contains(z, a)) z.push_back(a);
            next.push_back(std::move(z));
          }
        acc = std::move(next);
      }
      first = false;
      if (acc.size() > budget) return std::nullopt;
    }
    return acc;
  }

public:
  // ------------------------------------------------------------ packs ([temp.variadic])

  // A template-head's parameters as arguments, as the current instantiation and the primary
  // template's own argument list have them ([temp.dep.type]): a pack's is its expansion.
  std::vector<TemplateArg> own_args(std::span<const ast::TemplateParamSema> params) {
    std::vector<TemplateArg> r;
    for (const ast::TemplateParamSema& p : params) {
      TemplateArg a = p.kind == TemplateArgKind::type       ? type_arg(p.type)
                      : p.kind == TemplateArgKind::template_ ? TemplateArg{TemplateArgKind::template_, p.type, ConstId::none}
                                                             : value_arg(p.type, ins().intern_value(ast::VDepParam{p.depth, p.index}));
      if (p.pack) {
        std::array<TemplateArg, 1> e{expansion_of(a)};
        a = pack(e);
      }
      r.push_back(a);
    }
    return r;
  }
  // A pack of `elems`, each canonical as identity compares them (ast.md §5.6).
  TemplateArg pack(std::span<const TemplateArg> elems) {
    for (const TemplateArg& e : elems) {
      // BOOTSTRAP: contracts: a predicate naming a member goes through a local (STATUS).
      bool canonical = e.kind == TemplateArgKind::value || types().canonical(e.type) == e.type;
      contract_assert(canonical);
    }
    return {TemplateArgKind::pack, QualType{}, ConstId::none, ins().intern_seq(elems)};
  }
  std::vector<TemplateArg> elements(const TemplateArg& a) { return ins().seqs.elements(a.elems); }
  bool is_expansion(const TemplateArg& a) {
    if (a.kind == TemplateArgKind::template_) return a.templ == EntityId::none && types().kind(a.type) == TypeKind::pack_expansion;
    return a.kind == TemplateArgKind::type ? types().kind(a.type) == TypeKind::pack_expansion
                                           : a.kind == TemplateArgKind::value && ins().values.is<ast::VDepExpansion>(a.value);
  }
  // The expansion of `pattern` as a pack element.
  TemplateArg expansion_of(const TemplateArg& pattern) {
    if (pattern.kind == TemplateArgKind::type) return type_arg(types().pack_expansion(pattern.type));
    if (pattern.kind == TemplateArgKind::template_)
      return {TemplateArgKind::template_, types().pack_expansion(pattern.type), ConstId::none};
    return {TemplateArgKind::value, pattern.type, ins().intern_value(ast::VDepExpansion{pattern.value})};
  }
  // An expansion's pattern, as an argument of its kind.
  TemplateArg pattern_of(const TemplateArg& e) {
    if (e.kind == TemplateArgKind::type) return type_arg(types().rec(e.type).inner);
    if (e.kind == TemplateArgKind::template_) return {TemplateArgKind::template_, types().rec(e.type).inner, ConstId::none};
    return {TemplateArgKind::value, e.type, ins().values.get<ast::VDepExpansion>(e.value).pattern};
  }
  // The template parameters a type or value names, by (depth, index). Without `nested`, not those
  // of an expansion inside it, which that expansion expands ([temp.variadic]/5): an expansion's
  // own packs.
  void params_in(QualType t, std::vector<std::pair<u16, u16>>& out, bool nested = true) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::template_param: out.push_back({r.depth, r.index}); return;
    case TypeKind::class_:
      for (const TemplateArg& a : w_->entities.rec(r.entity).args) params_in(a, out, nested);
      return;
    case TypeKind::array:
      if (r.bound_value != ConstId::none) params_in(r.bound_value, out, nested);
      params_in(r.inner, out, nested);
      return;
    case TypeKind::pack_expansion:
      if (nested) params_in(r.inner, out, nested);
      return;
    case TypeKind::pack_index:
      // `T...[I]` uses up its pack: it is not an unexpanded pack of an enclosing pattern.
      if (r.pack)
        for (const TemplateArg& a : ins().seqs.elements(r.args)) params_in(a, out, nested);
      else if (nested)
        params_in(r.inner, out, nested);
      params_in(r.bound_value, out, nested);
      return;
    case TypeKind::dep_name:
      params_in(r.inner, out, nested);
      for (const TemplateArg& a : ins().seqs.elements(r.args)) params_in(a, out, nested);
      return;
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::transform: params_in(r.inner, out, nested); return;
    case TypeKind::member_pointer:
      params_in(r.member_owner, out, nested);
      params_in(r.inner, out, nested);
      return;
    case TypeKind::function:
      params_in(r.inner, out, nested);
      for (QualType p : r.params) params_in(p, out, nested);
      return;
    case TypeKind::dep_spec:
      out.push_back({r.depth, r.index});
      [[fallthrough]];
    case TypeKind::builtin_spec:
      for (const TemplateArg& a : ins().seqs.elements(r.args)) params_in(a, out, nested);
      return;
    default: return;
    }
  }
  void params_in(ConstId v, std::vector<std::pair<u16, u16>>& out, bool nested = true) {
    if (ins().values.is<ast::VDepParam>(v)) out.push_back({ins().values.get<ast::VDepParam>(v).depth, ins().values.get<ast::VDepParam>(v).index});
    else if (nested && ins().values.is<ast::VDepExpansion>(v)) params_in(ins().values.get<ast::VDepExpansion>(v).pattern, out, nested);
    else if (ins().values.is<ast::VDepVariable>(v))
      for (const TemplateArg& a : ins().seqs.elements(ins().values.get<ast::VDepVariable>(v).args)) params_in(a, out, nested);
    else if (ins().values.is<ast::VDepMember>(v))
      params_in(ins().values.get<ast::VDepMember>(v).qualifier, out, nested);
    else if (ins().values.is<ast::VDepConcept>(v))
      for (const TemplateArg& a : ins().seqs.elements(ins().values.get<ast::VDepConcept>(v).args)) params_in(a, out, nested);
    else if (ins().values.is<ast::VDepCall>(v))
      for (const TemplateArg& a : ins().seqs.elements(ins().values.get<ast::VDepCall>(v).args)) params_in(a, out, nested);
    else if (ins().values.is<ast::VDepOp>(v))
      for (const TemplateArg& a : ins().seqs.elements(ins().values.get<ast::VDepOp>(v).operands)) params_in(a, out, nested);
  }
  void params_in(const TemplateArg& a, std::vector<std::pair<u16, u16>>& out, bool nested = true) {
    if (a.kind == TemplateArgKind::type) params_in(a.type, out, nested);
    else if (a.kind == TemplateArgKind::value) params_in(a.value, out, nested);
    else if (a.kind == TemplateArgKind::template_) {
      if (a.templ == EntityId::none) params_in(a.type, out, nested);
    }
    else
      for (const TemplateArg& e : elements(a)) params_in(e, out, nested);
  }
  // The packs an expansion's pattern names, not those an expansion inside it expands.
  std::vector<std::pair<u16, u16>> own_packs(const TemplateArg& pattern) {
    std::vector<std::pair<u16, u16>> refs;
    params_in(pattern, refs, false);
    return refs;
  }
  // The bindings of an expansion's `i`-th element: each of its packs `refs` bound in `b` as its
  // `i`-th element.
  Bindings element(const Bindings& b, std::size_t i, std::span<const std::pair<u16, u16>> refs) {
    Bindings r = b;
    for (auto [d, k] : refs) {
      if (d >= r.levels.size() || k >= r.levels[d].size()) continue;
      auto& a = r.levels[d][k];
      if (a && a->kind == TemplateArgKind::pack) {
        std::vector<TemplateArg> es = elements(*a);
        a = i < es.size() ? std::optional(es[i]) : std::nullopt;
      }
    }
    return r;
  }
  // The length of the packs an expansion's pattern names that `b` binds: none when it binds none;
  // a failure when two differ ([temp.variadic]/7).
  std::optional<std::optional<std::size_t>> expansion_length(const TemplateArg& pattern, const Bindings& b) {
    std::vector<std::pair<u16, u16>> refs = own_packs(pattern);
    std::optional<std::size_t> n;
    for (auto [d, i] : refs)
      if (auto a = b.at(d, i); a && a->kind == TemplateArgKind::pack) {
        std::size_t m = ins().seqs.size(a->elems);
        if (n && *n != m) return std::nullopt;
        n = m;
      }
    return n;
  }
  // An expansion `pattern...` substituted: one argument for each element of the packs it names,
  // or, when `b` binds none of them, the expansion with its other parameters substituted.
  std::optional<std::vector<TemplateArg>> substitute_expansion(const TemplateArg& e, const Bindings& b) {
    TemplateArg pattern = pattern_of(e);
    auto n = expansion_length(pattern, b);
    if (!n) return std::nullopt;
    if (!*n) {
      auto s = substitute_arg(pattern, b);
      if (!s) return std::nullopt;
      return std::vector<TemplateArg>{expansion_of(*s)};
    }
    std::vector<TemplateArg> out;
    std::vector<std::pair<u16, u16>> refs = own_packs(pattern);
    for (std::size_t i = 0; i < **n; ++i) {
      auto s = substitute_arg(pattern, element(b, i, refs));
      if (!s) return std::nullopt;
      out.push_back(*s);
    }
    return out;
  }
  // A list of arguments with its packs' elements in place, as deduction compares them
  // ([temp.deduct.type]/9).
  std::vector<TemplateArg> flatten(std::span<const TemplateArg> args) {
    std::vector<TemplateArg> r;
    for (const TemplateArg& a : args) {
      if (a.kind != TemplateArgKind::pack) {
        r.push_back(a);
        continue;
      }
      std::vector<TemplateArg> es = elements(a);
      r.insert(r.end(), es.begin(), es.end());
    }
    return r;
  }

  // Whether `t` names a parameter of the template-head at `depth`: whether deduction can learn
  // anything from it.
  bool mentions(QualType t, u16 depth) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::template_param: return r.depth == depth;
    case TypeKind::class_: {
      std::vector<std::pair<u16, u16>> refs;
      for (const TemplateArg& a : w_->entities.rec(r.entity).args) params_in(a, refs);
      return std::ranges::any_of(refs, [&](auto ref) { return ref.first == depth; });
    }
    case TypeKind::pack_expansion: return mentions(r.inner, depth);
    case TypeKind::pack_index: return false;  // a non-deduced context ([temp.deduct.type]/5)
    case TypeKind::dep_spec: {
      if (r.depth == depth) return true;
      std::vector<std::pair<u16, u16>> refs;
      for (const TemplateArg& a : ins().seqs.elements(r.args)) params_in(a, refs);
      return std::ranges::any_of(refs, [&](auto ref) { return ref.first == depth; });
    }
    case TypeKind::array:
      if (r.bound_value != ConstId::none && ins().values.is<ast::VDepParam>(r.bound_value) &&
          ins().values.get<ast::VDepParam>(r.bound_value).depth == depth)
        return true;
      return mentions(r.inner, depth);
    case TypeKind::pointer:
    case TypeKind::reference: return mentions(r.inner, depth);
    case TypeKind::member_pointer: return mentions(r.member_owner, depth) || mentions(r.inner, depth);
    case TypeKind::function:
      return mentions(r.inner, depth) || std::ranges::any_of(r.params, [&](QualType p) { return mentions(p, depth); });
    default: return false;
    }
  }

  // ------------------------------------------------------------ deduction ([temp.deduct.type])

  // Deduces the parameters of depth `depth` in `p` from `a` ([temp.deduct.type]), into `out`.
  // Other template parameters (the unique types of partial ordering) are types like any other.
  bool deduce(QualType p, QualType a, u16 depth, std::vector<std::optional<TemplateArg>>& out, Allowance allow = {}) {
    QualType pc = types().canonical(p), ac = types().canonical(a);
    Cv cp = Types::cv(pc), ca = Types::cv(ac);
    TypeRec pr = types().rec(pc);
    // `typename Q::n` and dependent decltype operands are non-deduced contexts
    // ([temp.deduct.type]/5), as is a built-in's result.
    // A pack-index-specifier is one too ([temp.deduct.type]/5).
    if (pr.kind == TypeKind::dep_name || pr.kind == TypeKind::transform || pr.kind == TypeKind::pack_index) return true;
    if (partial_deduction_ && pr.kind == TypeKind::builtin && pr.builtin == Builtin::dependent) return true;
    if (pr.kind == TypeKind::template_param && pr.depth == depth) {
      // `cv T` from `cv' A`: T is A without cv, or with the cv `cv'` has beyond `cv`.
      if (!within(cp, ca) && !allow.more_cv) return false;
      QualType v = types().add_cv(Types::unqualified(ac), Cv(std::to_underlying(ca) & ~std::to_underlying(cp)));
      return bind(out, pr.index, {TemplateArgKind::type, types().canonical(v), ConstId::none});
    }
    if (pr.kind == TypeKind::builtin_spec) return true;  // a built-in's result: non-deduced
    if (!(cp == ca || (allow.more_cv && within(ca, cp)))) return false;
    TypeRec ar = types().rec(ac);
    // [temp.deduct.type]/8: `TT<args>` from a class template specialization: TT is its template,
    // and the arguments are deduced from that template's specialization for `args` (which fills
    // its defaults: `TT<int>` from `V<int, int>`).
    if (pr.kind == TypeKind::dep_spec && pr.depth == depth && ar.kind == TypeKind::class_) {
      const EntityRec& spec = w_->entities.rec(ar.entity);
      if (spec.templ == EntityId::none) return false;
      if (!bind(out, pr.index, {TemplateArgKind::template_, QualType{}, ConstId::none, {}, spec.templ})) return false;
      std::vector<TemplateArg> pargs = ins().seqs.elements(pr.args);
      // `TT<Ts...>`: the expansion takes the specialization's arguments as they are.
      if (std::ranges::any_of(flatten(pargs), [&](const TemplateArg& x) { return is_expansion(x); }))
        return deduce_args(pargs, spec.args, depth, out);
      auto own = apply_template(spec.templ, pargs);
      if (!own || !types().is_class(*own)) return false;
      const EntityRec& o = w_->entities.rec(types().class_of(*own));
      if (o.templ != spec.templ) return false;
      return deduce_args(o.args, spec.args, depth, out);
    }
    // From another `TT'<args>` (a unique one, in partial ordering): TT is TT'.
    if (pr.kind == TypeKind::dep_spec && pr.depth == depth && ar.kind == TypeKind::dep_spec) {
      if (!bind(out, pr.index, {TemplateArgKind::template_, types().template_param(ar.depth, ar.index, ar.pack), ConstId::none}))
        return false;
      return deduce_args(ins().seqs.elements(pr.args), ins().seqs.elements(ar.args), depth, out);
    }
    if (pr.kind != ar.kind) return false;
    switch (pr.kind) {
    case TypeKind::dep_spec:
      return pr.depth == ar.depth && pr.index == ar.index && pr.pack == ar.pack &&
             deduce_args(ins().seqs.elements(pr.args), ins().seqs.elements(ar.args), depth, out);
    case TypeKind::builtin_spec: return true;
    case TypeKind::template_param: return pr.depth == ar.depth && pr.index == ar.index && pr.pack == ar.pack;
    case TypeKind::dep_name:
      return types().canonical(pr.inner) == types().canonical(ar.inner) && pr.name == ar.name && pr.dep_form == ar.dep_form &&
             pr.args == ar.args;
    case TypeKind::transform: return pr.trait == ar.trait && types().canonical(pr.inner) == types().canonical(ar.inner);
    case TypeKind::pack_index:  // non-deduced, above
      return pr.inner == ar.inner && pr.bound_value == ar.bound_value && pr.pack == ar.pack && pr.args == ar.args;
    case TypeKind::placeholder: return pr.decltype_auto == ar.decltype_auto;
    case TypeKind::builtin: return pr.builtin == ar.builtin;
    case TypeKind::class_:
      if (pr.entity == ar.entity) return true;
      return deduce_specialization(pr.entity, ar.entity, depth, out);
    case TypeKind::enum_: return pr.entity == ar.entity;
    case TypeKind::pointer:
      // Under /4.2 the pointee may be less cv-qualified in A: a qualification conversion adds it,
      // at a deeper level only below a const one ([conv.qual]).
      return deduce(pr.inner, ar.inner, depth, out,
                    {.more_cv = allow.qualification,
                     .qualification = allow.qualification && Types::is_const(types().canonical(pr.inner)),
                     .function_conversion = allow.function_conversion && types().is_function(pr.inner)});
    case TypeKind::member_pointer:
      return deduce(pr.member_owner, ar.member_owner, depth, out) && deduce(pr.inner, ar.inner, depth, out);
    case TypeKind::reference: return pr.rvalue_ref == ar.rvalue_ref && deduce(pr.inner, ar.inner, depth, out);
    case TypeKind::array:
      // [temp.deduct.type]/8: `T[i]`, a non-type parameter deduced from the bound.
      if (pr.bound_value != ConstId::none && ins().values.is<ast::VDepParam>(pr.bound_value)) {
        auto p = ins().values.get<ast::VDepParam>(pr.bound_value);
        if (p.depth == depth) {
          // A known bound, or in partial ordering a unique value ([temp.func.order]/3).
          if (ar.bound_value == ConstId::none) return false;
          if (ins().values.is<ast::VDepParam>(ar.bound_value) && ins().values.get<ast::VDepParam>(ar.bound_value).depth == depth)
            return false;
          if (!ins().values.is<ast::VInt>(ar.bound_value) && !ins().values.is<ast::VDepParam>(ar.bound_value)) return false;
          QualType size = types().canonical(types().builtin(Builtin::unsigned_long));
          // Keep the array-bound type deduction as a fallback until all
          // compared parameters have supplied their ordinary deductions.
          if (array_bound_types_ && !head_.empty() && head_.front().depth == depth && p.index < head_.size()) {
            TypeRec bound_type = types().rec(types().canonical(head_[p.index].type));
            if (bound_type.kind == TypeKind::template_param && bound_type.depth == depth)
              array_bound_types_->insert(bound_type.index);
          }
          if (!bind(out, p.index, {TemplateArgKind::value, size, ar.bound_value})) return false;
          return deduce(pr.inner, ar.inner, depth, out, {.more_cv = allow.more_cv});
        }
      }
      // An array's cv-qualifiers are its elements' ([basic.type.qualifier]/3).
      return pr.bound == ar.bound && pr.bound_value == ar.bound_value &&
             deduce(pr.inner, ar.inner, depth, out, {.more_cv = allow.more_cv});
    case TypeKind::function: {
      if (pr.variadic != ar.variadic || pr.fn_cv != ar.fn_cv || pr.fn_ref != ar.fn_ref ||
          (pr.fn_noexcept != ar.fn_noexcept && !(allow.function_conversion && ar.fn_noexcept)))
        return false;
      if (!deduce(pr.inner, ar.inner, depth, out)) return false;
      std::vector<TemplateArg> ps, as;
      for (QualType q : pr.params) ps.push_back(type_arg(q));
      for (QualType q : ar.params) as.push_back(type_arg(q));
      return deduce_list(ps, as, depth, out);
    }
    case TypeKind::pack_expansion: return deduce(pr.inner, ar.inner, depth, out);
    case TypeKind::other: return false;
    }
    return false;
  }

  // The specialization of class template `templ` for `args` ([temp.spec.general]): its entity is
  // the template and the canonical arguments (§B1.5).
  EntityId specialize_class(EntityId templ, std::span<const TemplateArg> args, Fp128 constraints = {}) {
    const EntityRec& t = w_->entities.rec(templ);
    EntityRec r{EntityKind::class_, t.name, {}, LangLinkage::cxx, t.linkage, t.first, t.scope};
    r.templ = templ;
    r.args.assign(args.begin(), args.end());
    r.constraints = constraints;
    return w_->entities.entity(std::move(r));
  }

  // `TT<args…>` from a specialization of the same class template ([temp.deduct.type]/8): each
  // argument from the corresponding one, a type by `deduce`, a value parameter by binding.
  bool deduce_specialization(EntityId p, EntityId a, u16 depth, std::vector<std::optional<TemplateArg>>& out) {
    const EntityRec& pr = w_->entities.rec(p);
    const EntityRec& ar = w_->entities.rec(a);
    if (pr.templ == EntityId::none || pr.templ != ar.templ) return false;
    return deduce_args(pr.args, ar.args, depth, out);
  }
  // Template argument list `p` from `a`, argument by argument ([temp.deduct.type]/8): also how a
  // partial specialization matches a specialization ([temp.spec.partial.match]/2).
  bool deduce_args(std::span<const TemplateArg> p, std::span<const TemplateArg> a, u16 depth,
                   std::vector<std::optional<TemplateArg>>& out) {
    return deduce_list(flatten(p), flatten(a), depth, out);
  }
  // A list of P arguments from a list of A arguments, packs' elements in place: pairwise, then a
  // trailing expansion of P from the remaining ones; an expansion of P that is not last makes the
  // list a non-deduced context ([temp.deduct.type]/9, /10).
  bool deduce_list(std::span<const TemplateArg> p, std::span<const TemplateArg> a, u16 depth,
                   std::vector<std::optional<TemplateArg>>& out) {
    auto first = std::ranges::find_if(p, [&](const TemplateArg& x) { return is_expansion(x); });
    std::size_t fixed = static_cast<std::size_t>(first - p.begin());
    if (first != p.end() && fixed + 1 != p.size()) return true;
    if (first == p.end() ? a.size() != p.size() : a.size() < fixed) return false;
    for (std::size_t i = 0; i < fixed; ++i)
      if (is_expansion(a[i]) || !deduce_arg(p[i], a[i], depth, out)) return false;
    if (first == p.end()) return true;
    return deduce_expansion(pattern_of(*first), a.subspan(fixed), depth, out);
  }
  // One argument from one ([temp.deduct.type]/8): a type by `deduce`, a non-type parameter by
  // binding, another value by equality.
  bool deduce_arg(const TemplateArg& x, const TemplateArg& y, u16 depth, std::vector<std::optional<TemplateArg>>& out) {
    if (x.kind != y.kind) return false;
    if (x.kind == TemplateArgKind::type) return deduce(x.type, y.type, depth, out);
    if (x.kind == TemplateArgKind::pack) return deduce_list(elements(x), elements(y), depth, out);
    // A template template parameter from a template ([temp.deduct.type]/8: `TT`); a template
    // only from itself.
    if (x.kind == TemplateArgKind::template_) {
      if (is_expansion(x) && is_expansion(y)) return deduce_arg(pattern_of(x), pattern_of(y), depth, out);
      if (x.templ == EntityId::none && types().kind(x.type) == TypeKind::template_param) {
        TypeRec r = types().rec(x.type);
        if (r.depth == depth) return bind(out, r.index, y);
      }
      if (x.templ != EntityId::none || y.templ != EntityId::none) return x.templ == y.templ;
      return types().canonical(x.type) == types().canonical(y.type);
    }
    if (ins().values.is<ast::VDepParam>(x.value) && ins().values.get<ast::VDepParam>(x.value).depth == depth)
      return bind(out, ins().values.get<ast::VDepParam>(x.value).index, y);
    if (is_expansion(x) && is_expansion(y)) return deduce_arg(pattern_of(x), pattern_of(y), depth, out);
    // An expression naming parameters is a non-deduced context ([temp.deduct.type]). The
    // specialized type or substituted partial pattern is checked once the bindings are known.
    if (ins().values.is<ast::VDepOp>(x.value) || ins().values.is<ast::VDepVariable>(x.value) ||
        ins().values.is<ast::VDepCall>(x.value)) return true;
    return x.value == y.value;
  }
  // The packs an expansion's `pattern` names, each from the arguments `a` in turn: every
  // element deduced as a parameter of its own, the others agreeing across elements
  // ([temp.deduct.type]/9). A pack whose leading elements were specified explicitly is extended
  // past them: as many leading arguments are theirs ([temp.arg.explicit]/9).
  bool deduce_expansion(const TemplateArg& pattern, std::span<const TemplateArg> a, u16 depth,
                        std::vector<std::optional<TemplateArg>>& out, Allowance allow = {}) {
    std::vector<u16> packs;
    for (auto [d, i] : own_packs(pattern))
      if (d == depth && i < out.size() && is_pack_ref(pattern, depth, i) && !std::ranges::contains(packs, i)) packs.push_back(i);
    std::map<u16, std::vector<TemplateArg>> elems;
    std::size_t skip = 0;
    for (u16 k : packs)
      if (explicit_packs_.contains(k) && out[k] && out[k]->kind == TemplateArgKind::pack) {
        elems[k] = elements(*out[k]);
        skip = std::max(skip, elems[k].size());
      }
    for (std::size_t j = skip; j < a.size(); ++j) {
      std::vector<std::optional<TemplateArg>> tmp = out;
      for (u16 k : packs) tmp[k] = std::nullopt;
      bool ok = is_expansion(a[j]) ? deduce_arg(pattern, pattern_of(a[j]), depth, tmp)
                                   : (pattern.kind == TemplateArgKind::type && a[j].kind == TemplateArgKind::type
                                          ? deduce(pattern.type, a[j].type, depth, tmp, allow)
                                          : deduce_arg(pattern, a[j], depth, tmp));
      if (!ok) return false;
      for (std::size_t k = 0; k < out.size(); ++k) {
        if (std::ranges::contains(packs, k)) continue;
        if (tmp[k] && !out[k]) out[k] = tmp[k];
      }
      for (u16 k : packs) {
        // A pattern that deduces no element of a pack is a non-deduced context for it
        // ([temp.deduct.type]/5): another list deduces it, or nothing does.
        if (!tmp[k]) {
          if (j == skip && !explicit_packs_.contains(k)) return true;
          return false;
        }
        elems[k].push_back(is_expansion(a[j]) ? expansion_of(*tmp[k]) : *tmp[k]);
      }
    }
    for (u16 k : packs) {
      TemplateArg p = pack(elems[k]);
      if (out[k] && *out[k] != p && !explicit_packs_.contains(k)) return false;
      out[k] = p;
      explicit_packs_.erase(k);  // extended once; another list must agree with it
    }
    return true;
  }
  // Whether parameter `i` of the head at `depth` is a pack: by the head being deduced when it is
  // known; else a type parameter marked so, or a non-type one in a non-type pattern.
  bool is_pack_ref(const TemplateArg& pattern, u16 depth, u16 i) {
    if (!head_.empty() && head_[0].depth == depth && i < head_.size()) return head_[i].pack != 0;
    if (pattern.kind == TemplateArgKind::value) return true;
    std::vector<QualType> found;
    pack_types_in(pattern.type, found);
    return std::ranges::any_of(found, [&](QualType q) { return types().rec(q).index == i; });
  }
  void pack_types_in(QualType t, std::vector<QualType>& out) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::template_param:
      if (r.pack) out.push_back(t);
      return;
    case TypeKind::class_:
      for (const TemplateArg& a : flatten(w_->entities.rec(r.entity).args))
        if (a.kind == TemplateArgKind::type) pack_types_in(a.type, out);
      return;
    case TypeKind::dep_name:
      pack_types_in(r.inner, out);
      for (const TemplateArg& a : flatten(ins().seqs.elements(r.args)))
        if (a.kind == TemplateArgKind::type) pack_types_in(a.type, out);
      return;
    case TypeKind::array:
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::transform:
    case TypeKind::pack_expansion: pack_types_in(r.inner, out); return;
    case TypeKind::member_pointer:
      pack_types_in(r.member_owner, out);
      pack_types_in(r.inner, out);
      return;
    case TypeKind::function:
      pack_types_in(r.inner, out);
      for (QualType p : r.params) pack_types_in(p, out);
      return;
    default: return;
    }
  }

  // ------------------------------------------------------------ calls ([temp.deduct.call])

  // The template arguments of the function template `tmpl` for a call with `args`, after the
  // explicitly specified `explicit_args` ([temp.arg.explicit]); none when deduction fails
  // ([temp.deduct.general]).
  // `defaults` are its parameters' default arguments, by position ([dcl.fct.default]).
  std::optional<std::vector<TemplateArg>> deduce_call(const ast::DeclSema& tmpl, std::span<const ArgShape> args,
                                                       std::span<const TemplateArg> explicit_args,
                                                       std::span<const ast::NodeId> defaults = {}) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    if (params.empty()) return std::nullopt;
    Deducing deducing(this, params);
    u16 depth = params[0].depth;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!explicit_into(params, explicit_args, out)) return std::nullopt;
    TypeRec f = types().rec(tmpl.type);
    // A trailing function parameter pack takes the remaining arguments ([temp.deduct.call]/1); one
    // that is not trailing is not supported yet.
    const std::size_t n = f.params.size();
    const bool trailing = n > 0 && types().kind(f.params[n - 1]) == TypeKind::pack_expansion;
    const std::size_t fixed = trailing ? n - 1 : n;
    for (std::size_t i = 0; i < fixed; ++i)
      if (types().kind(f.params[i]) == TypeKind::pack_expansion) return std::nullopt;
    // Trailing parameters with default arguments need none ([temp.deduct.call]/1: only those with
    // an argument take part).
    // Before a trailing pack, the defaulted parameters just before it.
    std::size_t defaulted = 0;
    const std::size_t last = std::min(fixed, defaults.size());
    while (defaulted < last && defaults[last - 1 - defaulted] != ast::NodeId::none) ++defaulted;
    const std::size_t least = fixed - defaulted;  // with no pack, `fixed` is every parameter
    if (args.size() < least || (args.size() > n && !trailing && !f.variadic)) return std::nullopt;
    // The explicitly specified arguments are substituted first ([temp.deduct.general]/2); packs
    // stay open, to be extended by deduction ([temp.arg.explicit]/9).
    Bindings given = bindings(depth, out);
    for (auto& a : given.levels[depth])
      if (a && a->kind == TemplateArgKind::pack) a = std::nullopt;
    auto valid_return = [&] {
      auto ret = substitute(f.inner, given);
      return ret && !types().is_array(*ret) && !types().is_function(*ret);
    };
    std::vector<QualType> explicit_parameters;
    std::optional<QualType> explicit_pack_pattern;
    if (!explicit_args.empty()) {
      // Substitute in written order before deduction can fail ([temp.deduct.general]).
      if (!tmpl.has_trailing_return && !valid_return()) return std::nullopt;
      for (std::size_t i = 0; i < fixed; ++i) {
        auto p = substitute(f.params[i], given);
        if (!p || types().is_void(*p) || types().is_cvref_function(*p)) return std::nullopt;
        explicit_parameters.push_back(*p);
      }
      if (trailing) {
        explicit_pack_pattern = substitute(types().rec(f.params[n - 1]).inner, given);
        if (!explicit_pack_pattern) return std::nullopt;
      }
      if (tmpl.has_trailing_return && !valid_return()) return std::nullopt;
    }
    for (std::size_t i = 0; i < fixed && i < args.size(); ++i) {
      if (!args[i].deducible && args[i].overloads.empty()) continue;
      auto p0 = explicit_args.empty() ? substitute(f.params[i], given)
                                      : std::optional{explicit_parameters[i]};
      if (!p0) return std::nullopt;
      if (!mentions(*p0, depth)) continue;  // checked by overload resolution as a conversion
      if (!args[i].overloads.empty()) {
        if (!deduce_overload_argument(*p0, args[i], depth, out)) return std::nullopt;
        continue;
      }
      auto [p, a, allow] = call_pair(*p0, args[i], depth);
      if (!deduce(p, a, depth, out, allow)) return std::nullopt;
    }
    if (trailing) {
      auto p0 = explicit_args.empty() ? substitute(types().rec(f.params[n - 1]).inner, given)
                                      : explicit_pack_pattern;
      if (!p0) return std::nullopt;
      // The elements given explicitly take the first remaining arguments.
      std::size_t skip = 0;
      for (auto [d, k] : own_packs(type_arg(*p0)))
        if (d == depth && explicit_packs_.contains(k)) skip = std::max<std::size_t>(skip, ins().seqs.size(out[k]->elems));
      std::vector<TemplateArg> as;
      QualType p = *p0;
      Allowance allow;
      for (std::size_t i = fixed; i < args.size(); ++i) {
        if (i - fixed >= skip && !args[i].deducible) return std::nullopt;
        auto [pi, a, al] = call_pair(*p0, args[i], depth);
        p = pi;
        allow = al;
        as.push_back(type_arg(a));
      }
      if (!deduce_expansion(type_arg(p), as, depth, out, allow)) return std::nullopt;
    }
    // A value deduced from an array bound converts to its parameter's type
    // ([temp.deduct.type]/20).
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (!out[i] || params[i].kind != TemplateArgKind::value || params[i].pack) continue;
      auto pt = substitute(params[i].type, bindings(depth, out));
      auto v = pt ? converted_value(*out[i], *pt) : std::nullopt;
      if (!v) return std::nullopt;
      out[i] = *v;
    }
    return complete(tmpl, params, std::move(out));
  }
  // A call argument's P/A pair, adjusted as [temp.deduct.call]/2–4 say: a forwarding reference
  // from an lvalue deduces an lvalue reference; otherwise A decays and loses its top-level cv.
  struct CallPair {
    QualType p;
    QualType a;
    Allowance allow;
  };
  CallPair call_pair(QualType p, const ArgShape& arg, u16 depth) {
    QualType a = arg.type;
    Allowance allow;
    if (types().is_reference(p)) {
      TypeRec pr = types().rec(p);
      TypeRec inner = types().rec(pr.inner);
      if (pr.rvalue_ref && inner.kind == TypeKind::template_param && inner.depth == depth &&
          Types::cv(types().canonical(pr.inner)) == Cv::none && arg.lvalue)
        a = types().lvalue_ref(a);
      p = pr.inner;
      allow.more_cv = true;
      allow.function_conversion = types().is_function(p);
    } else {
      if (types().is_array(a)) a = types().pointer(types().rec(a).inner);
      else if (types().is_function(a)) a = types().pointer(a);
      else a = Types::unqualified(types().canonical(a));
      p = Types::unqualified(types().canonical(p));
      allow.qualification = types().is_pointer(p);
      allow.function_conversion = types().is_pointer(p) && types().is_function(types().pointee(p));
    }
    return {p, a, allow};
  }
  // [temp.deduct.call]/6: try each non-template overload independently. Deduction from other
  // parameters cannot disambiguate these trials, but it must agree with a unique result.
  bool deduce_overload_argument(QualType param, const ArgShape& arg, u16 depth,
                                std::vector<std::optional<TemplateArg>>& out) {
    QualType base = types().referee(param);
    if (types().is_pointer(base)) base = types().pointee(base);
    if (!types().is_function(base)) return true;
    std::optional<CallPair> chosen;
    const auto saved = explicit_packs_;
    for (QualType t : arg.overloads) {
      CallPair pair = call_pair(param, {t, arg.lvalue}, depth);
      std::vector<std::optional<TemplateArg>> trial(out.size());
      const bool matched = deduce(pair.p, pair.a, depth, trial, pair.allow);
      explicit_packs_ = saved;
      if (!matched) continue;
      if (chosen && types().canonical(chosen->a) != types().canonical(pair.a)) return true;
      chosen = pair;
    }
    return !chosen || deduce(chosen->p, chosen->a, depth, out, chosen->allow);
  }
  // Explicitly specified template arguments, by parameter ([temp.arg.explicit]): one each, and a
  // pack takes the rest; a value converted to its parameter's type with the arguments before it
  // substituted ([temp.arg.nontype]/1, [temp.deduct.general]/2). False if they do not fit.
  // When checking a replayable variable initializer, a value with no constant is checked for
  // kind and type but left unbound. The caller does not complete or use these bindings as a key.
  bool explicit_into(std::span<const ast::TemplateParamSema> params, std::span<const TemplateArg> written,
                     std::vector<std::optional<TemplateArg>>& out, bool defer_values = false) {
    u16 depth = params.empty() ? 0 : params[0].depth;
    auto deferred = [defer_values](const TemplateArg& a) {
      return defer_values && a.kind == TemplateArgKind::value && a.value == ConstId::none;
    };
    auto one = [&](const ast::TemplateParamSema& p, TemplateArg a) -> std::optional<TemplateArg> {
      // The injected-class-name names the class for a type parameter, the template for a template
      // template parameter ([temp.local]/1).
      if (a.kind == TemplateArgKind::template_ && p.kind == TemplateArgKind::type && a.templ != EntityId::none &&
          types().is_class(a.type))
        a = type_arg(a.type);
      if (a.kind != p.kind) return std::nullopt;
      if (p.kind == TemplateArgKind::type) return a;
      if (p.kind == TemplateArgKind::template_) return template_matches(p, a) ? std::optional(a) : std::nullopt;
      auto pt = substitute(p.type, bindings(depth, out));
      if (deferred(a)) return pt && (types().is_integral(*pt) || types().is_dependent(*pt)) ? std::optional(a) : std::nullopt;
      return pt ? converted_value(a, *pt) : std::nullopt;
    };
    std::size_t j = 0;
    for (std::size_t i = 0; i < params.size() && j < written.size(); ++i) {
      if (params[i].pack) {
        std::vector<TemplateArg> es;
        bool deferred_pack = false;
        for (; j < written.size(); ++j) {
          auto v = one(params[i], written[j]);
          if (!v) return false;
          if (deferred(*v)) deferred_pack = true;
          else es.push_back(*v);
        }
        if (!deferred_pack) {
          out[i] = pack(es);
          explicit_packs_.insert(u16(i));
        }
        break;
      }
      auto v = one(params[i], written[j++]);
      if (!v) return false;
      if (!deferred(*v)) out[i] = *v;
    }
    return j == written.size();
  }

  // Whether template `a`'s template-head is equivalent to template template parameter `p`'s:
  // parameters of the same kinds and packs, values of the same types position by position, nested
  // heads alike ([temp.over.link]/6).
  bool heads_equivalent(const ast::TemplateParamSema& p, EntityId a) {
    if (!template_decl) return false;
    auto d = template_decl(a);
    if (!d) return false;
    return heads_equivalent(ins().seqs.elements(p.params), ins().seqs.elements(d->template_params));
  }
  bool heads_equivalent(std::span<const ast::TemplateParamSema> ps, std::span<const ast::TemplateParamSema> as) {
    if (ps.size() != as.size()) return false;
    if (ps.empty()) return true;
    Bindings b = Bindings::of(as[0].depth, own_args(ps));
    for (std::size_t i = 0; i < ps.size(); ++i) {
      if (ps[i].kind != as[i].kind || ps[i].pack != as[i].pack) return false;
      if (ps[i].kind == TemplateArgKind::value) {
        auto t = substitute(as[i].type, b);
        if (!t || types().canonical(*t) != types().canonical(ps[i].type)) return false;
      }
      if (ps[i].kind == TemplateArgKind::template_ &&
          !heads_equivalent(ins().seqs.elements(ps[i].params), ins().seqs.elements(as[i].params)))
        return false;
    }
    return true;
  }

  // The type template `templ` names with `args`, as a template-id says it ([temp.names]): a class
  // template's specialization, an alias template's type with the arguments substituted
  // ([temp.alias]/2); none when the arguments do not fit ([temp.deduct.general]/8).
  std::optional<QualType> apply_template(EntityId templ, std::span<const TemplateArg> written) {
    if (!template_decl) return std::nullopt;
    std::vector<TemplateArg> args = flatten(written);  // `TT<args>` keeps its own head's packs
    auto decl = template_decl(templ);
    if (!decl) return std::nullopt;
    std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(decl->template_params);
    std::size_t first_pack = std::ranges::find_if(ps, [](const ast::TemplateParamSema& p) { return p.pack != 0; }) - ps.begin();
    for (std::size_t i = 0; i < args.size() && i < first_pack; ++i)
      if (is_expansion(args[i])) return std::nullopt;  // for a non-pack parameter: not supported yet (#210)
    std::vector<std::optional<TemplateArg>> out(ps.size());
    std::set<u16> saved = std::exchange(explicit_packs_, {});
    bool fits = explicit_into(ps, args, out);
    explicit_packs_ = std::move(saved);
    if (!fits) return std::nullopt;
    auto full = complete(*decl, ps, out);
    if (!full) return std::nullopt;
    if (decl->kind == ast::DeclKind::type_alias) {
      auto bindings = Bindings::of(ps.empty() ? 0 : ps[0].depth, *full);
      if (alias_type) return alias_type(*decl, *full, bindings);
      return substitute(decl->type, bindings);
    }
    return types().class_type(specialize_class(templ, *full));
  }

  // Whether template `a` is an argument template template parameter `p` takes ([temp.arg.template]/4,
  // P0522): p's own parameters, made unique, are arguments a's head takes, with a's defaults. A
  // template template parameter (dependent) is checked when it is bound.
  bool template_matches(const ast::TemplateParamSema& p, const TemplateArg& a) {
    if (a.templ == EntityId::none) return true;
    if (!template_decl) return false;
    auto decl = template_decl(a.templ);
    if (!decl) return false;
    std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(p.params);
    std::vector<ast::TemplateParamSema> as = ins().seqs.elements(decl->template_params);
    std::vector<TemplateArg> synth;
    if (!ps.empty()) {
      auto u = substitute_args(own_args(ps), unique_bindings(ps));
      if (!u) return false;
      synth = flatten(*u);
    }
    // A trailing pack of p's stands for any number of arguments of its kind
    // ([temp.arg.template]/3's example: `template<class...> class` takes `template<class, class>`):
    // a unique one for each of a's remaining parameters, an expansion for a's own pack.
    if (!ps.empty() && ps.back().pack && !synth.empty() && is_expansion(synth.back())) {
      TemplateArg pack = synth.back();
      synth.pop_back();
      for (std::size_t j = synth.size(); j < as.size(); ++j) {
        if (as[j].kind != ps.back().kind) return false;
        if (as[j].pack) {
          synth.push_back(pack);
          break;
        }
        u16 index = u16(ps.size() + j);  // past p's own unique parameters
        if (as[j].kind == TemplateArgKind::type) synth.push_back(type_arg(types().template_param(unique_depth, index)));
        else if (as[j].kind == TemplateArgKind::value)
          synth.push_back(value_arg(as[j].type, ins().intern_value(ast::VDepParam{unique_depth, index})));
        else synth.push_back({TemplateArgKind::template_, types().template_param(unique_depth, index), ConstId::none});
      }
    }
    std::vector<std::optional<TemplateArg>> out(as.size());
    std::set<u16> saved = std::exchange(explicit_packs_, {});
    bool ok = explicit_into(as, synth, out) && complete(*decl, as, out).has_value();
    explicit_packs_ = std::move(saved);
    return ok;
  }

  // A template argument's value as a converted constant expression of type `t` ([expr.const]):
  // an integral conversion that is not narrowing, or a null pointer of type std::nullptr_t to a
  // pointer. None when it does not convert.
  std::optional<TemplateArg> converted_value(const TemplateArg& a, QualType t) {
    t = types().canonical(Types::unqualified(t));
    // Predicate conversions to non-bool destinations need a dependent conversion (#209).
    // A dependent base alignment is checked again when the written argument is replayed.
    ConstId pattern = is_expansion(a) ? pattern_of(a).value : a.value;
    if (ins().values.is<ast::VDepOp>(pattern)) {
      if (ins().values.get<ast::VDepOp>(pattern).op == ast::DepOp::alignof_type) {
        if (!types().is_dependent(t) && !types().is_integral(t)) return std::nullopt;
      } else if (!types().is(t, Builtin::bool_)) {
        return std::nullopt;
      }
    }
    if (types().is_dependent(t) || ins().values.is_dependent(a.value)) return TemplateArg{TemplateArgKind::value, t, a.value};
    if (types().is_integral(t) && ins().values.is<ast::VInt>(a.value)) {
      // An unscoped enumeration converts through its underlying type ([conv.prom]); a scoped one
      // does not convert ([dcl.enum]).
      QualType from = a.type;
      if (types().is_enum(from)) {
        const EnumRec* e = classes_->find_enum(types().enum_of(types().canonical(from)));
        if (!e || e->scoped || !e->underlying) return std::nullopt;
        from = *e->underlying;
      }
      ConstId value = ast::convert_integral(ins(), w_->target, a.value, from);
      ConstId c = ast::convert_integral(ins(), w_->target, value, t);
      ConstId back = ast::convert_integral(ins(), w_->target, c, from);
      auto neg = [&](ConstId v, QualType of) {
        return ast::is_signed(w_->target, *types().builtin_of(of)) && ast::is_negative(ast::int_bits(ins(), v));
      };
      if (back != value || neg(c, t) != neg(value, from)) return std::nullopt;  // narrowing ([dcl.init.list]/7)
      return TemplateArg{TemplateArgKind::value, t, c};
    }
    if (types().is_pointer(t) && ins().values.is<ast::VNullptr>(a.value)) return TemplateArg{TemplateArgKind::value, t, a.value};
    if (types().canonical(a.type) == t) return TemplateArg{TemplateArgKind::value, t, a.value};
    return std::nullopt;
  }

  // Default template arguments for what deduction left ([temp.deduct.general]/5), then every
  // parameter must have an argument.
  std::optional<std::vector<TemplateArg>> complete(const ast::DeclSema&, std::span<const ast::TemplateParamSema> params,
                                                    std::vector<std::optional<TemplateArg>> out) {
    if (params.empty()) return std::vector<TemplateArg>{};
    u16 depth = params[0].depth;
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (out[i]) continue;
      // A pack neither given nor deduced is empty ([temp.arg.explicit]/4).
      if (params[i].pack) {
        out[i] = pack({});
        continue;
      }
      if (!params[i].has_default) return std::nullopt;
      TemplateArg d = params[i].default_;
      if (d.kind == TemplateArgKind::type) {
        auto s = substitute(d.type, bindings(depth, out));
        if (!s) return std::nullopt;
        d.type = types().canonical(*s);
      } else if (ins().values.is<ast::VDepOp>(d.value) || ins().values.is<ast::VDepVariable>(d.value) ||
                 ins().values.is<ast::VDepCall>(d.value)) {
        auto s = substitute_arg(d, bindings(depth, out));
        if (!s) return std::nullopt;
        d = *s;
      } else if (ins().values.is<ast::VDepParam>(d.value)) {
        // `int N = M`: an earlier parameter's argument, converted ([temp.arg.nontype]).
        auto p = ins().values.get<ast::VDepParam>(d.value);
        auto a = p.depth == depth && p.index < i ? out[p.index] : std::nullopt;
        auto pt = substitute(params[i].type, bindings(depth, out));
        auto v = a && pt ? converted_value(*a, *pt) : std::nullopt;
        if (!v) return std::nullopt;
        d = *v;
      }
      out[i] = d;
    }
    std::vector<TemplateArg> r;
    for (auto& a : out) r.push_back(*a);
    return r;
  }

  // The arguments for which `tmpl` is the function of type `type` ([temp.deduct.decl],
  // [temp.deduct.funcaddr]). A placeholder return in address deduction is checked after
  // instantiating the specialization.
  std::optional<std::vector<TemplateArg>> deduce_declaration(const ast::DeclSema& tmpl, QualType type,
                                                             std::span<const TemplateArg> explicit_args,
                                                             bool address = false) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    if (params.empty()) return std::nullopt;
    Deducing deducing(this, params);
    u16 depth = params[0].depth;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!explicit_into(params, explicit_args, out)) return std::nullopt;
    QualType pattern = tmpl.type;
    TypeRec f = types().rec(pattern);
    const bool deduced_result = address && types().has_placeholder(f.inner);
    if (deduced_result) pattern = types().function(types().rec(type).inner, f.params, f.variadic, f.linkage, f.fn_cv);
    if (!deduce(pattern, type, depth, out)) return std::nullopt;
    auto full = complete(tmpl, params, std::move(out));
    if (!full || deduced_result) return full;
    auto t = specialization_type(tmpl, *full);
    if (!t || types().canonical(*t) != types().canonical(type)) return std::nullopt;
    return full;
  }

  // The type of a specialization's declaration: the template's function type with `args`.
  std::optional<QualType> specialization_type(const ast::DeclSema& tmpl, std::span<const TemplateArg> args) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    return substitute(tmpl.type, Bindings::of(params.empty() ? 0 : params[0].depth, args), tmpl.has_trailing_return);
  }

  // ------------------------------------------------------------ partial specializations ([temp.spec.partial])

  // Deduce a partial specialization's parameters from `args`. Its pattern must still be substituted
  // with those bindings and compared with `args` before it matches ([temp.spec.partial.match]/2).
  std::optional<std::vector<TemplateArg>> deduce_partial(std::span<const ast::TemplateParamSema> params,
                                                        std::span<const TemplateArg> pattern, std::span<const TemplateArg> args) {
    if (params.empty()) return std::nullopt;
    Deducing deducing(this, params, true);
    u16 depth = params[0].depth;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!deduce_args(pattern, args, depth, out) || std::ranges::any_of(out, [](const auto& a) { return !a; })) return std::nullopt;
    std::vector<TemplateArg> r;
    for (const auto& a : out) r.push_back(*a);
    return r;
  }
  // Whether partial specialization 1 (`p1`, `a1`, constraints `c1`) is more specialized than 2
  // ([temp.spec.partial.order]): as the function templates with one parameter of type `X<a1>` and
  // `X<a2>` order ([temp.func.order]), 2's arguments deduced from 1's with a unique value for each of
  // 1's parameters, and not the converse. When each is at least as specialized as the other, with
  // equal parameter lists and positionally equal arguments, the more constrained one is
  // ([temp.func.order]/6.2, [temp.constr.order]); both normal forms then name the same positions.
  bool more_specialized_partial(std::span<const ast::TemplateParamSema> p1, std::span<const TemplateArg> a1,
                                std::span<const ast::TemplateParamSema> p2, std::span<const TemplateArg> a2,
                                ConstId c1 = ConstId::none, ConstId c2 = ConstId::none) {
    const bool forward = at_least_as_specialized(p1, a1, p2, a2), backward = at_least_as_specialized(p2, a2, p1, a1);
    if (forward != backward) return forward;
    if (!forward || !equivalent_heads(p1, p2) || !std::ranges::equal(a1, a2)) return false;
    return subsumes(c1, c2) && !subsumes(c2, c1);
  }
  // Whether two template-heads have the same parameters, by kind, packness and type
  // ([temp.over.link]/6), type-constraints apart: those count among the constraints.
  bool equivalent_heads(std::span<const ast::TemplateParamSema> p1, std::span<const ast::TemplateParamSema> p2) {
    if (p1.size() != p2.size()) return false;
    for (std::size_t i = 0; i < p1.size(); ++i)
      if (p1[i].kind != p2[i].kind || p1[i].pack != p2[i].pack ||
          (p1[i].kind == TemplateArgKind::value && types().canonical(p1[i].type) != types().canonical(p2[i].type)))
        return false;
    return true;
  }
  // Whether every parameter of `params` can be deduced from template arguments `args` that name
  // them, as a partial specialization's must be ([temp.spec.partial.general]): deduced from
  // `args` with a unique value for each.
  bool deducible(std::span<const ast::TemplateParamSema> params, std::span<const TemplateArg> args) {
    if (params.empty()) return false;
    Deducing deducing(this, params, true);
    auto unique = substitute_args(args, unique_bindings(params));
    std::vector<std::optional<TemplateArg>> out(params.size());
    return unique && deduce_args(args, *unique, params[0].depth, out) && std::ranges::all_of(out, [](const auto& a) { return a.has_value(); });
  }
  bool at_least_as_specialized(std::span<const ast::TemplateParamSema> p1, std::span<const TemplateArg> a1,
                               std::span<const ast::TemplateParamSema> p2, std::span<const TemplateArg> a2) {
    if (p1.empty() || p2.empty()) return false;
    auto unique = substitute_args(a1, unique_bindings(p1));
    if (!unique) return false;
    auto deduced = deduce_partial(p2, a2, *unique);
    if (!deduced) return false;
    auto back = substitute_args(a2, Bindings::of(p2[0].depth, *deduced));
    return back && std::ranges::equal(*back, *unique);
  }

  // ------------------------------------------------------------ partial ordering ([temp.func.order])

  // An address compares the full function types, including their return types
  // ([temp.deduct.partial]/3). A call compares only the parameters it supplies.
  std::pair<bool, bool> address_ordering(const ast::DeclSema& f1, const ast::DeclSema& f2) {
    auto at_least = [&](const ast::DeclSema& a, const ast::DeclSema& p) {
      std::vector<ast::TemplateParamSema> ap = ins().seqs.elements(a.template_params);
      std::vector<ast::TemplateParamSema> pp = ins().seqs.elements(p.template_params);
      if (ap.empty() || pp.empty()) return false;
      auto transformed = substitute(a.type, unique_bindings(ap));
      std::vector<std::optional<TemplateArg>> out(pp.size());
      Deducing deducing(this, pp);
      return transformed && deduce(p.type, *transformed, pp[0].depth, out);
    };
    return {at_least(f1, f2), at_least(f2, f1)};
  }

  // Partial ordering of function templates `f1` and `f2` for a call with `nargs` arguments
  // ([temp.deduct.partial]): whether `f1` is at least as specialized as `f2`, and the converse.
  // `f1` is when `f2`'s parameters can be deduced from `f1`'s function parameter types with a
  // unique type for each of `f1`'s template parameters.
  std::pair<bool, bool> ordering(const ast::DeclSema& f1, const ast::DeclSema& f2, std::size_t nargs) {
    std::vector<ast::TemplateParamSema> p1 = w_->ins.seqs.elements(f1.template_params);
    std::vector<ast::TemplateParamSema> p2 = w_->ins.seqs.elements(f2.template_params);
    if (p1.empty() || p2.empty()) return {false, false};
    Bindings b1 = unique_bindings(p1), b2 = unique_bindings(p2);
    TypeRec t1 = types().rec(f1.type), t2 = types().rec(f2.type);
    // The parameter a call's `i`-th argument meets: a trailing function parameter pack's pattern
    // for each argument past the others.
    struct At {
      QualType type;
      bool pack;
    };
    auto param_at = [&](const TypeRec& t, std::size_t i) -> std::optional<At> {
      std::size_t n = t.params.size();
      if (n > 0 && types().kind(t.params[n - 1]) == TypeKind::pack_expansion && i >= n - 1)
        return At{types().rec(t.params[n - 1]).inner, true};
      if (i < n) return At{t.params[i], false};
      return std::nullopt;
    };
    std::vector<std::optional<TemplateArg>> out12(p2.size()), out21(p1.size());
    std::set<u16> bound_types12, bound_types21;
    // From position `i` on, where one of them has a function parameter pack: P's pack's pattern
    // deduced from every remaining A at once; an A from a pack deduces no P that is not one
    // ([temp.deduct.partial]/8).
    auto rest = [&](const TypeRec& ta, const Bindings& ba, const TypeRec& tp, std::span<const ast::TemplateParamSema> pp,
                    std::vector<std::optional<TemplateArg>>& out, std::set<u16>& bound_types, std::size_t i) {
      auto xp = param_at(tp, i);
      if (!xp || !xp->pack) return false;
      std::vector<TemplateArg> as;
      for (std::size_t j = i; j < nargs; ++j) {
        auto y = param_at(ta, j);
        if (!y) break;
        auto a = substitute(y->type, ba);
        if (!a) return false;
        as.push_back(y->pack ? type_arg(types().pack_expansion(strip(*a))) : type_arg(strip(*a)));
        if (y->pack) break;
      }
      Deducing deducing(this, pp, false, &bound_types);
      return deduce_expansion(type_arg(strip(xp->type)), as, pp[0].depth, out);
    };
    bool all12 = true, all21 = true;
    for (std::size_t i = 0; i < nargs; ++i) {
      auto x1 = param_at(t1, i), x2 = param_at(t2, i);
      if (!x1 || !x2) break;
      if (x1->pack || x2->pack) {
        all12 = all12 && rest(t1, b1, t2, p2, out12, bound_types12, i);
        all21 = all21 && rest(t2, b2, t1, p1, out21, bound_types21, i);
        break;
      }
      auto a1 = substitute(x1->type, b1);
      auto a2 = substitute(x2->type, b2);
      // [temp.deduct.partial]/8: an A from a function parameter pack deduces no P that is not one.
      auto compare = [&](const At& p, const std::optional<QualType>& a,
                         std::span<const ast::TemplateParamSema> params,
                         std::vector<std::optional<TemplateArg>>& out, std::set<u16>& bound_types) {
        Deducing deducing(this, params, false, &bound_types);
        return a && deduce(strip(p.type), strip(*a), params.front().depth, out);
      };
      bool s12 = compare(*x2, a1, p2, out12, bound_types12);
      bool s21 = compare(*x1, a2, p1, out21, bound_types21);
      // /9: when both types were references and deduction succeeds both ways, an lvalue
      // reference beats an rvalue one, then the more cv-qualified referred-to type wins.
      if (s12 && s21 && types().is_reference(x1->type) && types().is_reference(x2->type)) {
        bool l1 = !types().rec(x1->type).rvalue_ref, l2 = !types().rec(x2->type).rvalue_ref;
        Cv c1 = Types::cv(types().canonical(types().referee(x1->type)));
        Cv c2 = Types::cv(types().canonical(types().referee(x2->type)));
        if (l1 != l2) {
          s21 = s21 && !l1;
          s12 = s12 && !l2;
        } else if (c1 != c2) {
          if (within(c2, c1)) s21 = false;
          if (within(c1, c2)) s12 = false;
        }
      }
      all12 = all12 && s12;
      all21 = all21 && s21;
    }
    // [temp.deduct.partial]/12 permits an undeduced template parameter only
    // when it is absent from the types compared for this call.
    auto used_parameters_deduced = [&](const TypeRec& pattern, u16 depth,
                                       const std::vector<std::optional<TemplateArg>>& deduced,
                                       const std::set<u16>& bound_types) {
      std::vector<std::pair<u16, u16>> used;
      for (std::size_t i = 0; i < std::min(nargs, pattern.params.size()); ++i)
        params_in(pattern.params[i], used);
      return std::ranges::all_of(used, [&](auto ref) {
        auto [parameter_depth, index] = ref;
        // [temp.deduct.type]/2 uses an array bound to deduce a type only
        // when the type was not deduced elsewhere.
        return parameter_depth != depth ||
               (index < deduced.size() && (deduced[index].has_value() || bound_types.contains(index)));
      });
    };
    all12 = all12 && used_parameters_deduced(t2, p2[0].depth, out12, bound_types12);
    all21 = all21 && used_parameters_deduced(t1, p1[0].depth, out21, bound_types21);
    // [temp.deduct.partial]/11: a trailing function parameter pack breaks a tie only if
    // the other template has no trailing pack and no declared parameter at that position.
    // A parameter with an omitted default argument still occupies its declared position.
    if (all12 && all21) {
      auto trailing = [&](const TypeRec& t) {
        return !t.params.empty() && types().kind(t.params.back()) == TypeKind::pack_expansion;
      };
      const bool pack1 = trailing(t1), pack2 = trailing(t2);
      if (pack2 && !pack1 && t1.params.size() < t2.params.size()) return {true, false};
      if (pack1 && !pack2 && t2.params.size() < t1.params.size()) return {false, true};
    }
    return {all12, all21};
  }
  bool more_specialized(const ast::DeclSema& f1, const ast::DeclSema& f2, std::size_t nargs) {
    auto [a, b] = ordering(f1, f2, nargs);
    return a && !b;
  }

private:
  static constexpr u16 unique_depth = 0x7fff;

  enum class BooleanSubstitution { evaluate, defer, discard };

  std::optional<TemplateArg> substitute_member_value(ConstId v, const Bindings& b) {
    auto member = ins().values.get<ast::VDepMember>(v);
    auto qualifier = substitute(member.qualifier, b);
    if (!qualifier) return std::nullopt;
    if (types().is_dependent(*qualifier)) {
      member.qualifier = types().canonical(*qualifier);
      return value_arg(types().builtin(Builtin::dependent), ins().intern_value(member));
    }
    return member_constant ? member_constant(*qualifier, member.name) : std::nullopt;
  }

  std::optional<ConstId> substitute_concept_value(ConstId v, const Bindings& b) {
    auto ref = ins().values.get<ast::VDepConcept>(v);
    auto args = substitute_args(ins().seqs.elements(ref.args), b);
    if (!args) return std::nullopt;
    if (std::ranges::any_of(*args, [&](const TemplateArg& a) { return w_->arg_dependent(a); })) {
      ref.args = ins().intern_seq(std::span<const TemplateArg>(*args));
      return ins().intern_value(ref);
    }
    return concept_constant ? concept_constant(ref.concept_, *args) : std::nullopt;
  }

  std::optional<TemplateArg> substitute_call_value(ConstId v, const Bindings& b) {
    auto call = ins().values.get<ast::VDepCall>(v);
    auto args = substitute_args(ins().seqs.elements(call.args), b);
    if (!args) return std::nullopt;
    if (std::ranges::any_of(*args, [&](const TemplateArg& a) { return w_->arg_dependent(a); })) {
      call.args = ins().intern_seq(std::span<const TemplateArg>(*args));
      return value_arg(types().builtin(Builtin::dependent), ins().intern_value(call));
    }
    return call_constant ? call_constant(call.callee, *args) : std::nullopt;
  }

  std::optional<ConstId> substitute_variable_value(ConstId v, const Bindings& b,
                                                  const std::function<std::optional<ConstId>(EntityId)>& read,
                                                  BooleanSubstitution mode = BooleanSubstitution::evaluate) {
    auto ref = ins().values.get<ast::VDepVariable>(v);
    auto args = substitute_args(ins().seqs.elements(ref.args), b);
    if (!args) return std::nullopt;
    if (std::ranges::any_of(*args, [&](const TemplateArg& a) { return w_->arg_dependent(a); })) {
      ref.args = ins().intern_seq(std::span<const TemplateArg>(*args));
      return ins().intern_value(ref);
    }
    const EntityId spec = specialize_variable(ref.templ, *args);
    if (mode != BooleanSubstitution::evaluate) {
      auto global = variable_global ? variable_global(spec) : std::nullopt;
      auto type = global ? std::optional(global->type) : declared_variable_type(spec, ref.templ, *args);
      if (!type || !types().is(types().referee(*type), Builtin::bool_)) return std::nullopt;
      if (mode == BooleanSubstitution::discard) {
        if (!global && demand_variable) demand_variable(spec, *type);
        return w_->values.integer(types().builtin(Builtin::bool_), false);
      }
      ref.args = ins().intern_seq(std::span<const TemplateArg>(*args));
      return ins().intern_value(ref);
    }
    return read ? read(spec) : std::nullopt;
  }

  std::optional<QualType> declared_variable_type(EntityId spec, EntityId templ, std::span<const TemplateArg> args) {
    if (variable_specialization_type)
      if (auto type = variable_specialization_type(spec)) return type;
    if (!template_decl) return std::nullopt;
    auto pattern = template_decl(templ);
    if (!pattern) return std::nullopt;
    auto params = ins().seqs.elements(pattern->template_params);
    return substitute(pattern->type, Bindings::of(params.empty() ? 0 : params.front().depth, args));
  }

  // The selected specialization supplies its own type and constant initializer. Check those
  // before the ordinary non-type argument conversion can change the value's type.
  std::optional<ConstId> bool_variable_value(EntityId spec) {
    if (!variable_global) return std::nullopt;
    auto g = variable_global(spec);
    if (!g || !g->usable || !g->value || !types().is(g->type, Builtin::bool_) ||
        Types::is_volatile(types().canonical(g->type)))
      return std::nullopt;
    if (!ins().values.is<ast::VInt>(*g->value) ||
        !types().is(ins().values.get<ast::VInt>(*g->value).type, Builtin::bool_))
      return std::nullopt;
    return g->value;
  }

  // Preserve the right operand without reading its specialization while the left is dependent.
  std::optional<ConstId> substitute_boolean_operand(ConstId v, const Bindings& b, BooleanSubstitution mode) {
    if (ins().values.is<ast::VDepVariable>(v))
      return substitute_variable_value(v, b, [this](EntityId spec) { return bool_variable_value(spec); }, mode);
    if (!ins().values.is<ast::VDepOp>(v)) return substitute_value(v, b);
    auto op = ins().values.get<ast::VDepOp>(v);
    if (op.op != ast::DepOp::logical_not && op.op != ast::DepOp::logical_and)
      return substitute_value(v, b);
    auto left = substitute_boolean_operand(ins().seqs.at(op.operands, 0).value, b, mode);
    if (!left || !structural_value(*left)) return std::nullopt;
    if (op.op == ast::DepOp::logical_not) return logical_not_value(*left);
    const bool left_dependent = ins().values.is_dependent(*left);
    const bool short_circuited = !left_dependent && ast::int_bits(ins(), *left) == ast::Bits128{};
    auto right_mode = mode;
    if (mode == BooleanSubstitution::evaluate) {
      if (left_dependent) right_mode = BooleanSubstitution::defer;
      else if (short_circuited) right_mode = BooleanSubstitution::discard;
    }
    auto right = substitute_boolean_operand(ins().seqs.at(op.operands, 1).value, b, right_mode);
    if (!right || !structural_value(*right)) return std::nullopt;
    if (mode == BooleanSubstitution::defer || left_dependent || ins().values.is_dependent(*right))
      return conjunction_value(*left, *right);
    if (short_circuited) return *left;
    return *right;
  }

  ConstId logical_not_value(ConstId v) {
    QualType type = types().builtin(Builtin::bool_);
    if (!ins().values.is_dependent(v)) return w_->values.integer(type, ast::int_bits(ins(), v) == ast::Bits128{});
    const std::array operands{value_arg(type, v)};
    return ins().intern_value(ast::VDepOp{ast::DepOp::logical_not, ins().intern_seq(std::span<const TemplateArg>(operands))});
  }

public:
  // Unsupported dependent constructs can still be re-parsed in a template body. Their
  // stand-ins carry no expression identity and must not become operands of an interned value.
  bool structural_operand(QualType t) {
    if (!types().is_dependent(t)) return true;
    TypeRec r = types().rec(t);
    auto all_args = [&](std::span<const TemplateArg> args) {
      return std::ranges::all_of(args, [&](const TemplateArg& a) { return structural_operand(a); });
    };
    switch (r.kind) {
    case TypeKind::builtin: return false;  // Builtin::dependent
    case TypeKind::class_: return all_args(w_->entities.rec(r.entity).args);
    case TypeKind::dep_spec:
    case TypeKind::builtin_spec: return all_args(ins().seqs.elements(r.args));
    case TypeKind::array: return structural_operand(r.inner) && structural_value(r.bound_value);
    case TypeKind::pack_index: return (!r.pack || all_args(ins().seqs.elements(r.args))) && structural_value(r.bound_value);
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::dep_name:
    case TypeKind::transform:
    case TypeKind::pack_expansion: return structural_operand(r.inner);
    case TypeKind::member_pointer: return structural_operand(r.member_owner) && structural_operand(r.inner);
    case TypeKind::function:
      return structural_operand(r.inner) && std::ranges::all_of(r.params, [&](QualType p) { return structural_operand(p); });
    default: return true;
    }
  }
  bool structural_operand(const TemplateArg& a) {
    if (a.kind == TemplateArgKind::pack)
      return std::ranges::all_of(elements(a), [&](const TemplateArg& e) { return structural_operand(e); });
    if (a.kind == TemplateArgKind::value) return structural_value(a.value) && structural_operand(a.type);
    return a.templ != EntityId::none || structural_operand(a.type);
  }
private:
  bool structural_value(ConstId v) {
    if (ins().values.is<ast::VDepParam>(v)) return ins().values.get<ast::VDepParam>(v).depth != dependent_depth;
    if (ins().values.is<ast::VDepExpansion>(v)) return structural_value(ins().values.get<ast::VDepExpansion>(v).pattern);
    if (ins().values.is<ast::VDepVariable>(v))
      return std::ranges::all_of(ins().seqs.elements(ins().values.get<ast::VDepVariable>(v).args),
                                 [&](const TemplateArg& a) { return structural_operand(a); });
    if (ins().values.is<ast::VDepMember>(v))
      return structural_operand(ins().values.get<ast::VDepMember>(v).qualifier);
    if (ins().values.is<ast::VDepConcept>(v))
      return std::ranges::all_of(ins().seqs.elements(ins().values.get<ast::VDepConcept>(v).args),
                                 [&](const TemplateArg& a) { return structural_operand(a); });
    if (ins().values.is<ast::VDepCall>(v))
      return std::ranges::all_of(ins().seqs.elements(ins().values.get<ast::VDepCall>(v).args),
                                 [&](const TemplateArg& a) { return structural_operand(a); });
    if (ins().values.is<ast::VDepOp>(v))
      return std::ranges::all_of(ins().seqs.elements(ins().values.get<ast::VDepOp>(v).operands),
                                 [&](const TemplateArg& a) { return structural_operand(a); });
    return true;
  }

  static bool within(Cv a, Cv b) { return (std::to_underlying(a) & ~std::to_underlying(b)) == 0; }

  bool bind(std::vector<std::optional<TemplateArg>>& out, u16 index, TemplateArg v) {
    if (index >= out.size()) return false;
    if (out[index]) return *out[index] == v;  // [temp.deduct.type]/2: one value for each
    out[index] = v;
    return true;
  }

  // A unique type or value for each of a template's parameters ([temp.func.order]/3): a
  // parameter of a depth no template-head has.
  Bindings unique_bindings(std::span<const ast::TemplateParamSema> ps) {
    std::vector<TemplateArg> unique;
    for (const ast::TemplateParamSema& p : ps) {
      if (p.kind == TemplateArgKind::type) unique.push_back(type_arg(types().template_param(unique_depth, p.index, p.pack != 0)));
      else if (p.kind == TemplateArgKind::template_)
        unique.push_back({TemplateArgKind::template_, types().template_param(unique_depth, p.index, p.pack != 0), ConstId::none});
      else unique.push_back(value_arg(p.type, w_->ins.intern_value(ast::VDepParam{unique_depth, p.index})));
    }
    return Bindings::of(ps[0].depth, unique);
  }

  // The member alias template `n` of class `c` ([temp.mem]), as lookup at a point finds it, or
  // in the unit's record of `c`.
  std::optional<EntityId> member_template_of(EntityId c, NameId n) {
    if (member_template) return member_template(c, n);
    const ClassRec* cr = classes_->find(c);
    if (!cr) return std::nullopt;
    for (const NestedType& t : cr->alias_templates)
      if (t.name == n) return t.entity;
    return std::nullopt;
  }

  Bindings bindings(u16 depth, const std::vector<std::optional<TemplateArg>>& out) {
    Bindings b;
    b.levels.resize(depth + 1);
    b.levels[depth] = out;
    return b;
  }


  QualType strip(QualType t) {
    t = types().referee(t);
    return Types::unqualified(types().canonical(t));
  }

  World* w_;
  const Classes* classes_;

public:
  // The type member of a class with a name, as lookup at a point finds it; set by whoever asks at
  // a point, which `classes_` alone does not know.
  std::function<std::optional<QualType>(EntityId, NameId)> member_type;
  // The member alias template of a class with a name, likewise (`Sema::member_alias_template`).
  std::function<std::optional<EntityId>(EntityId, NameId)> member_template;
  // The scopedness of an enumeration whose record has yet to be published.
  std::function<std::optional<bool>(EntityId)> defining_enum_is_scoped;
  // The declaration of template `templ` with its head, as lookup at a point finds it.
  std::function<std::optional<ast::DeclSema>(EntityId)> template_decl;
  std::function<std::optional<ConstId>(EntityId)> variable_constant;
  std::function<std::optional<TemplateArg>(QualType, NameId)> member_constant;
  std::function<std::optional<ConstId>(EntityId, std::span<const TemplateArg>)> concept_constant;
  std::function<std::optional<TemplateArg>(EntityId, std::span<const TemplateArg>)> call_constant;
  std::function<std::optional<QualType>(const ast::DeclSema&, std::span<const TemplateArg>, const Bindings&)> alias_type;
  std::function<std::optional<interp::GlobalVar>(EntityId)> variable_global;
  std::function<std::optional<QualType>(EntityId)> variable_specialization_type;
  std::function<void(EntityId, QualType)> demand_variable;

private:
  // The template-head whose parameters a deduction deduces, and its packs whose leading elements
  // were specified explicitly ([temp.arg.explicit]/9).
  std::vector<ast::TemplateParamSema> head_;
  std::set<u16> explicit_packs_;
  bool partial_deduction_ = false;
  std::set<u16>* array_bound_types_ = nullptr;
  // Sets `head_` (and clears the explicit packs) for one deduction.
  struct Deducing {
    Templates* t;
    std::vector<ast::TemplateParamSema> saved;
    std::set<u16> saved_explicit;
    bool saved_partial;
    std::set<u16>* saved_bound_types;
    Deducing(Templates* t, std::span<const ast::TemplateParamSema> head, bool partial = false,
             std::set<u16>* bound_types = nullptr)
        : t(t), saved(std::move(t->head_)), saved_explicit(std::move(t->explicit_packs_)),
          saved_partial(std::exchange(t->partial_deduction_, partial)),
          saved_bound_types(std::exchange(t->array_bound_types_, bound_types)) {
      t->head_.assign(head.begin(), head.end());
      t->explicit_packs_.clear();
    }
    ~Deducing() {
      t->head_ = std::move(saved);
      t->explicit_packs_ = std::move(saved_explicit);
      t->partial_deduction_ = saved_partial;
      t->array_bound_types_ = saved_bound_types;
    }
  };
};

}  // namespace ycxx::sema
