// Templates (#88): substitution of template arguments into types ([temp.inst], docs/design/ast.md
// §6.3), template argument deduction from a call ([temp.deduct.call], [temp.deduct.type]) and the
// partial ordering of function templates ([temp.func.order], [temp.deduct.partial]).
//
// Template parameters are positional in types (`TTemplateParam{depth, index}`, ast.md §5.1), so
// these are functions of types and argument lists only; what a name means is the caller's.
export module ycxx.sema:templates;

import std;
import ycxx.ast;
import :types;

export namespace ycxx::sema {

using ast::TemplateArg;
using ast::TemplateArgKind;

// The arguments a substitution gives, per template-head depth then parameter index (ast.md §5.6);
// a parameter without one stays as it is.
struct Bindings {
  std::vector<std::vector<std::optional<TemplateArg>>> levels;

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
  bool deducible = true;  // false for a braced-init-list or an overload set: a non-deduced context
};

// How far a deduced A may differ from A at the top of a P/A pair ([temp.deduct.call]/4): with
// `more_cv` it may be more cv-qualified (P was a reference, /4.1); with `qualification` A may be
// a pointer that converts to the deduced A by a qualification conversion (/4.2).
struct Allowance {
  bool more_cv = false;
  bool qualification = false;
};

class Templates {
public:
  explicit Templates(World& w) : w_(&w) {}

  Types& types() { return w_->types; }
  ast::Interners& ins() { return w_->ins; }

  TemplateArg type_arg(QualType t) { return {TemplateArgKind::type, types().canonical(t), ConstId::none}; }
  TemplateArg value_arg(QualType t, ConstId v) { return {TemplateArgKind::value, types().canonical(Types::unqualified(t)), v}; }

  // ------------------------------------------------------------ substitution ([temp.inst])

  // `t` with the parameters `b` binds replaced by their arguments. None when that forms a type
  // that cannot exist ([temp.deduct.general]/11): a pointer to a reference, a reference to void, an
  // array of void, references or functions, a function returning an array or a function, a
  // parameter of type void. A type parameter's cv-qualifiers are added to its argument
  // ([dcl.type.cv]: on a reference they are dropped).
  std::optional<QualType> substitute(QualType t, const Bindings& b) {
    TypeRec r = types().rec(t);
    Cv cv = Types::cv(types().canonical(t));
    switch (r.kind) {
    case TypeKind::template_param: {
      auto a = b.at(r.depth, r.index);
      if (!a) return t;
      if (a->kind != TemplateArgKind::type) return std::nullopt;
      return types().add_cv(a->type, cv);
    }
    case TypeKind::builtin:
    case TypeKind::class_:
    case TypeKind::other: return t;
    case TypeKind::pointer: {
      auto p = substitute(r.inner, b);
      if (!p || types().is_reference(*p)) return std::nullopt;
      return types().add_cv(types().pointer(*p), cv);
    }
    case TypeKind::reference: {
      auto p = substitute(r.inner, b);
      if (!p || types().is_void(*p)) return std::nullopt;
      return r.rvalue_ref ? types().rvalue_ref(*p) : types().lvalue_ref(*p);  // collapsing ([dcl.ref])
    }
    case TypeKind::array: {
      auto e = substitute(r.inner, b);
      if (!e || !types().is_object(*e)) return std::nullopt;
      return types().array(*e, r.bound);
    }
    case TypeKind::function: {
      auto ret = substitute(r.inner, b);
      if (!ret || types().is_array(*ret) || types().is_function(*ret)) return std::nullopt;
      std::vector<QualType> ps;
      for (QualType p : r.params) {
        auto s = substitute(p, b);
        if (!s || types().is_void(*s)) return std::nullopt;
        ps.push_back(*s);
      }
      return types().function(*ret, ps, r.variadic, r.linkage, r.fn_cv);
    }
    }
    return t;
  }

  // Whether `t` names a parameter of the template-head at `depth`: whether deduction can learn
  // anything from it.
  bool mentions(QualType t, u16 depth) {
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::template_param: return r.depth == depth;
    case TypeKind::pointer:
    case TypeKind::array:
    case TypeKind::reference: return mentions(r.inner, depth);
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
    if (pr.kind == TypeKind::template_param && pr.depth == depth) {
      // `cv T` from `cv' A`: T is A without cv, or with the cv `cv'` has beyond `cv`.
      if (!within(cp, ca) && !allow.more_cv) return false;
      QualType v = types().add_cv(Types::unqualified(ac), Cv(std::to_underlying(ca) & ~std::to_underlying(cp)));
      return bind(out, pr.index, {TemplateArgKind::type, types().canonical(v), ConstId::none});
    }
    if (!(cp == ca || (allow.more_cv && within(ca, cp)))) return false;
    TypeRec ar = types().rec(ac);
    if (pr.kind != ar.kind) return false;
    switch (pr.kind) {
    case TypeKind::template_param: return pr.depth == ar.depth && pr.index == ar.index && pr.pack == ar.pack;
    case TypeKind::builtin: return pr.builtin == ar.builtin;
    case TypeKind::class_: return pr.entity == ar.entity;
    case TypeKind::pointer:
      // Under /4.2 the pointee may be less cv-qualified in A: a qualification conversion adds it.
      return deduce(pr.inner, ar.inner, depth, out, {.more_cv = allow.qualification, .qualification = false});
    case TypeKind::reference: return pr.rvalue_ref == ar.rvalue_ref && deduce(pr.inner, ar.inner, depth, out);
    case TypeKind::array: return pr.bound == ar.bound && deduce(pr.inner, ar.inner, depth, out);
    case TypeKind::function:
      if (pr.params.size() != ar.params.size() || pr.variadic != ar.variadic || pr.fn_cv != ar.fn_cv) return false;
      if (!deduce(pr.inner, ar.inner, depth, out)) return false;
      for (std::size_t i = 0; i < pr.params.size(); ++i)
        if (!deduce(pr.params[i], ar.params[i], depth, out)) return false;
      return true;
    case TypeKind::other: return false;
    }
    return false;
  }

  // ------------------------------------------------------------ calls ([temp.deduct.call])

  // The template arguments of the function template `tmpl` for a call with `args`, after the
  // explicitly specified `explicit_args` ([temp.arg.explicit]); none when deduction fails
  // ([temp.deduct.general]).
  std::optional<std::vector<TemplateArg>> deduce_call(const ast::DeclSema& tmpl, std::span<const ArgShape> args,
                                                       std::span<const TemplateArg> explicit_args) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    if (params.empty()) return std::nullopt;
    u16 depth = params[0].depth;
    if (explicit_args.size() > params.size()) return std::nullopt;
    std::vector<std::optional<TemplateArg>> out(params.size());
    for (std::size_t i = 0; i < explicit_args.size(); ++i) {
      if (explicit_args[i].kind != params[i].kind) return std::nullopt;
      out[i] = explicit_args[i];
      // A value is converted to its parameter's type, with the arguments before it substituted
      // ([temp.arg.nontype]/1, [temp.deduct.general]/2).
      if (params[i].kind == TemplateArgKind::value) {
        auto pt = substitute(params[i].type, bindings(depth, out));
        if (!pt) return std::nullopt;
        auto v = converted_value(explicit_args[i], *pt);
        if (!v) return std::nullopt;
        out[i] = *v;
      }
    }
    TypeRec f = types().rec(tmpl.type);
    if (args.size() < f.params.size() || (args.size() > f.params.size() && !f.variadic)) return std::nullopt;
    // The explicitly specified arguments are substituted first ([temp.deduct.general]/2).
    const Bindings given = bindings(depth, out);
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      if (!args[i].deducible) continue;
      auto p0 = substitute(tmpl_param_type(tmpl, i), given);
      if (!p0) return std::nullopt;
      QualType p = *p0;
      if (!mentions(p, depth)) continue;  // checked by overload resolution as a conversion
      QualType a = args[i].type;
      Allowance allow;
      if (types().is_reference(p)) {
        // A forwarding reference from an lvalue deduces an lvalue reference ([temp.deduct.call]/3).
        TypeRec pr = types().rec(p);
        TypeRec inner = types().rec(pr.inner);
        if (pr.rvalue_ref && inner.kind == TypeKind::template_param && inner.depth == depth &&
            Types::cv(types().canonical(pr.inner)) == Cv::none && args[i].lvalue)
          a = types().lvalue_ref(a);
        else
          p = pr.inner;
        allow.more_cv = true;
      } else {
        // [temp.deduct.call]/2: A decays, and top-level cv is dropped from both.
        if (types().is_array(a)) a = types().pointer(types().rec(a).inner);
        else if (types().is_function(a)) a = types().pointer(a);
        else a = Types::unqualified(types().canonical(a));
        p = Types::unqualified(types().canonical(p));
        allow.qualification = types().is_pointer(p);
      }
      if (!deduce(p, a, depth, out, allow)) return std::nullopt;
    }
    return complete(tmpl, params, std::move(out));
  }

  // A template argument's value as a converted constant expression of type `t` ([expr.const]):
  // an integral conversion that is not narrowing, or a null pointer of type std::nullptr_t to a
  // pointer. None when it does not convert.
  std::optional<TemplateArg> converted_value(const TemplateArg& a, QualType t) {
    t = types().canonical(Types::unqualified(t));
    if (types().is_dependent(t) || ins().values.is_dependent(a.value)) return TemplateArg{TemplateArgKind::value, t, a.value};
    if (types().is_integral(t) && ins().values.is<ast::VInt>(a.value)) {
      ConstId c = ast::convert_integral(ins(), target, a.value, t);
      ConstId back = ast::convert_integral(ins(), target, c, a.type);
      auto neg = [&](ConstId v, QualType of) {
        return ast::is_signed(target, *types().builtin_of(of)) && ast::is_negative(ast::int_bits(ins(), v));
      };
      if (back != a.value || neg(c, t) != neg(a.value, a.type)) return std::nullopt;  // narrowing ([dcl.init.list]/7)
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
      if (!params[i].has_default) return std::nullopt;
      TemplateArg d = params[i].default_;
      if (d.kind == TemplateArgKind::type) {
        auto s = substitute(d.type, bindings(depth, out));
        if (!s) return std::nullopt;
        d.type = types().canonical(*s);
      }
      out[i] = d;
    }
    std::vector<TemplateArg> r;
    for (auto& a : out) r.push_back(*a);
    return r;
  }

  // The type of a specialization's declaration: the template's function type with `args`.
  std::optional<QualType> specialization_type(const ast::DeclSema& tmpl, std::span<const TemplateArg> args) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    return substitute(tmpl.type, Bindings::of(params.empty() ? 0 : params[0].depth, args));
  }

  // ------------------------------------------------------------ partial ordering ([temp.func.order])

  // Whether function template `f1` is at least as specialized as `f2` for a call with `nargs`
  // arguments ([temp.deduct.partial]): `f2`'s parameters can be deduced from `f1`'s function
  // parameter types with a unique type for each of `f1`'s template parameters.
  bool at_least_as_specialized(const ast::DeclSema& f1, const ast::DeclSema& f2, std::size_t nargs) {
    std::vector<ast::TemplateParamSema> p1 = w_->ins.seqs.elements(f1.template_params);
    std::vector<ast::TemplateParamSema> p2 = w_->ins.seqs.elements(f2.template_params);
    if (p1.empty() || p2.empty()) return false;
    // Unique types and values: parameters of a depth no template-head has.
    std::vector<TemplateArg> unique;
    for (const ast::TemplateParamSema& p : p1) {
      if (p.kind == TemplateArgKind::type) unique.push_back(type_arg(types().template_param(unique_depth, p.index)));
      else unique.push_back(value_arg(p.type, w_->ins.intern_value(ast::VDepParam{unique_depth, p.index})));
    }
    Bindings b = Bindings::of(p1[0].depth, unique);
    TypeRec t1 = types().rec(f1.type), t2 = types().rec(f2.type);
    std::size_t n = std::min({nargs, t1.params.size(), t2.params.size()});
    std::vector<std::optional<TemplateArg>> out(p2.size());
    for (std::size_t i = 0; i < n; ++i) {
      auto a = substitute(t1.params[i], b);
      if (!a) return false;
      // [temp.deduct.partial]/5–7: references are replaced by what they refer to, then top-level
      // cv-qualifiers are dropped.
      QualType p = strip(t2.params[i]), aa = strip(*a);
      if (!deduce(p, aa, p2[0].depth, out)) return false;
    }
    return true;
  }
  bool more_specialized(const ast::DeclSema& f1, const ast::DeclSema& f2, std::size_t nargs) {
    return at_least_as_specialized(f1, f2, nargs) && !at_least_as_specialized(f2, f1, nargs);
  }

private:
  static constexpr u16 unique_depth = 0x7fff;

  static bool within(Cv a, Cv b) { return (std::to_underlying(a) & ~std::to_underlying(b)) == 0; }

  bool bind(std::vector<std::optional<TemplateArg>>& out, u16 index, TemplateArg v) {
    if (index >= out.size()) return false;
    if (out[index]) return *out[index] == v;  // [temp.deduct.type]/2: one value for each
    out[index] = v;
    return true;
  }

  Bindings bindings(u16 depth, const std::vector<std::optional<TemplateArg>>& out) {
    Bindings b;
    b.levels.resize(depth + 1);
    b.levels[depth] = out;
    return b;
  }

  QualType tmpl_param_type(const ast::DeclSema& tmpl, std::size_t i) { return types().rec(tmpl.type).params[i]; }

  QualType strip(QualType t) {
    t = types().referee(t);
    return Types::unqualified(types().canonical(t));
  }

  World* w_;
};

}  // namespace ycxx::sema
