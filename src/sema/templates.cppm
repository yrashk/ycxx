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

// The depth of the `VDepParam` that stands for a value-dependent expression in a template's
// definition, which only an instantiation evaluates.
inline constexpr u16 dependent_depth = 0x7ffe;

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
  // `classes` is the unit's class records (#115), which a dependent name's lookup reads.
  Templates(World& w, const Classes& classes) : w_(&w), classes_(&classes) {}

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
      auto q = substitute(r.inner, b);
      if (!q) return std::nullopt;
      if (types().is_dependent(*q)) return types().add_cv(types().dep_name(*q, r.name), cv);
      if (!types().is_class(*q)) return std::nullopt;
      const ClassRec* c = classes_->find(types().class_of(*q));
      if (!c) return std::nullopt;
      for (const auto& [n, mt] : c->types)
        if (n == r.name) return types().add_cv(mt, cv);
      return std::nullopt;
    }
    case TypeKind::builtin:
    case TypeKind::enum_:
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
      if (r.bound_value != ConstId::none && ins().values.is_dependent(r.bound_value)) {
        // A bound that is a non-type parameter; one that is any other dependent expression is
        // known only to an instantiation of the body (#88).
        auto p = ins().values.get<ast::VDepParam>(r.bound_value);
        auto a = b.at(p.depth, p.index);
        if (!a) return p.depth == dependent_depth ? std::nullopt : std::optional(types().array_dependent(*e, r.bound_value));
        if (a->kind != TemplateArgKind::value) return std::nullopt;
        if (ins().values.is_dependent(a->value)) return types().array_dependent(*e, a->value);  // a unique value
        if (!ins().values.is<ast::VInt>(a->value)) return std::nullopt;
        // [dcl.array]/1: the bound's value is greater than zero.
        if (ast::is_signed(target, *types().builtin_of(a->type)) && ast::is_negative(ast::int_bits(ins(), a->value)))
          return std::nullopt;
        if (ast::int_bits(ins(), a->value) == ast::Bits128{}) return std::nullopt;
        return types().array_dependent(*e, a->value);
      }
      return types().array(*e, r.bound);
    }
    case TypeKind::function: {
      auto ret = substitute(r.inner, b);
      if (!ret || types().is_array(*ret) || types().is_function(*ret)) return std::nullopt;
      std::vector<QualType> ps;
      for (QualType p : r.params) {
        // A function parameter pack's type: one parameter for each element ([temp.variadic]/8).
        if (types().kind(p) == TypeKind::pack_expansion) {
          auto xs = substitute_expansion(type_arg(p), b);
          if (!xs) return std::nullopt;
          for (const TemplateArg& x : *xs) {
            if (types().is_void(x.type)) return std::nullopt;
            ps.push_back(x.type);
          }
          continue;
        }
        auto s = substitute(p, b);
        if (!s || types().is_void(*s)) return std::nullopt;
        ps.push_back(*s);
      }
      return types().function(*ret, ps, r.variadic, r.linkage, r.fn_cv);
    }
    case TypeKind::pack_expansion: return std::nullopt;  // only in a list, by `substitute_expansion`
    }
    return t;
  }

  // Template arguments `args` with the parameters `b` binds replaced by their arguments; a pack's
  // expansions expanded in place ([temp.variadic]/8).
  std::optional<std::vector<TemplateArg>> substitute_args(std::span<const TemplateArg> args, const Bindings& b) {
    std::vector<TemplateArg> out;
    for (const TemplateArg& a : args) {
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
    if (a.kind == TemplateArgKind::type) {
      auto s = substitute(a.type, b);
      if (!s) return std::nullopt;
      return type_arg(*s);
    }
    auto v = substitute_value(a.value, b);
    if (!v) return std::nullopt;
    QualType t = a.type;
    if (types().is_dependent(t))
      if (auto st = substitute(t, b)) t = types().canonical(Types::unqualified(*st));
    return TemplateArg{TemplateArgKind::value, t, *v};
  }
  // A value with the parameters `b` binds replaced: a non-type parameter by its argument,
  // `sizeof...` of a bound pack by its length ([expr.sizeof]).
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
    return v;
  }

  // ------------------------------------------------------------ packs ([temp.variadic])

  // A template-head's parameters as arguments, as the current instantiation and the primary
  // template's own argument list have them ([temp.dep.type]): a pack's is its expansion.
  std::vector<TemplateArg> own_args(std::span<const ast::TemplateParamSema> params) {
    std::vector<TemplateArg> r;
    for (const ast::TemplateParamSema& p : params) {
      TemplateArg a = p.kind == TemplateArgKind::type ? type_arg(p.type)
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
    return a.kind == TemplateArgKind::type ? types().kind(a.type) == TypeKind::pack_expansion
                                           : a.kind == TemplateArgKind::value && ins().values.is<ast::VDepExpansion>(a.value);
  }
  // The expansion of `pattern` as a pack element.
  TemplateArg expansion_of(const TemplateArg& pattern) {
    if (pattern.kind == TemplateArgKind::type) return type_arg(types().pack_expansion(pattern.type));
    return {TemplateArgKind::value, pattern.type, ins().intern_value(ast::VDepExpansion{pattern.value})};
  }
  // An expansion's pattern, as an argument of its kind.
  TemplateArg pattern_of(const TemplateArg& e) {
    if (e.kind == TemplateArgKind::type) return type_arg(types().rec(e.type).inner);
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
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::dep_name: params_in(r.inner, out, nested); return;
    case TypeKind::function:
      params_in(r.inner, out, nested);
      for (QualType p : r.params) params_in(p, out, nested);
      return;
    default: return;
    }
  }
  void params_in(ConstId v, std::vector<std::pair<u16, u16>>& out, bool nested = true) {
    if (ins().values.is<ast::VDepParam>(v)) out.push_back({ins().values.get<ast::VDepParam>(v).depth, ins().values.get<ast::VDepParam>(v).index});
    else if (nested && ins().values.is<ast::VDepExpansion>(v)) params_in(ins().values.get<ast::VDepExpansion>(v).pattern, out, nested);
  }
  void params_in(const TemplateArg& a, std::vector<std::pair<u16, u16>>& out, bool nested = true) {
    if (a.kind == TemplateArgKind::type) params_in(a.type, out, nested);
    else if (a.kind == TemplateArgKind::value) params_in(a.value, out, nested);
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
    case TypeKind::array:
      if (r.bound_value != ConstId::none && ins().values.is<ast::VDepParam>(r.bound_value) &&
          ins().values.get<ast::VDepParam>(r.bound_value).depth == depth)
        return true;
      return mentions(r.inner, depth);
    case TypeKind::pointer:
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
    // `typename Q::n` is a non-deduced context ([temp.deduct.type]/5.1).
    if (pr.kind == TypeKind::dep_name) return true;
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
    case TypeKind::dep_name: return types().canonical(pr.inner) == types().canonical(ar.inner) && pr.name == ar.name;
    case TypeKind::builtin: return pr.builtin == ar.builtin;
    case TypeKind::class_:
      if (pr.entity == ar.entity) return true;
      return deduce_specialization(pr.entity, ar.entity, depth, out);
    case TypeKind::enum_: return pr.entity == ar.entity;
    case TypeKind::pointer:
      // Under /4.2 the pointee may be less cv-qualified in A: a qualification conversion adds it,
      // at a deeper level only below a const one ([conv.qual]).
      return deduce(pr.inner, ar.inner, depth, out,
                    {.more_cv = allow.qualification, .qualification = allow.qualification && Types::is_const(types().canonical(pr.inner))});
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
          if (!bind(out, p.index, {TemplateArgKind::value, size, ar.bound_value})) return false;
          return deduce(pr.inner, ar.inner, depth, out, {.more_cv = allow.more_cv});
        }
      }
      // An array's cv-qualifiers are its elements' ([basic.type.qualifier]/3).
      return pr.bound == ar.bound && pr.bound_value == ar.bound_value &&
             deduce(pr.inner, ar.inner, depth, out, {.more_cv = allow.more_cv});
    case TypeKind::function: {
      if (pr.variadic != ar.variadic || pr.fn_cv != ar.fn_cv) return false;
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
  EntityId specialize_class(EntityId templ, std::span<const TemplateArg> args) {
    const EntityRec& t = w_->entities.rec(templ);
    EntityRec r{EntityKind::class_, t.name, {}, LangLinkage::cxx, t.linkage, t.first, t.scope};
    r.templ = templ;
    r.args.assign(args.begin(), args.end());
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
    if (ins().values.is<ast::VDepParam>(x.value) && ins().values.get<ast::VDepParam>(x.value).depth == depth)
      return bind(out, ins().values.get<ast::VDepParam>(x.value).index, y);
    if (is_expansion(x) && is_expansion(y)) return deduce_arg(pattern_of(x), pattern_of(y), depth, out);
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
        if (!tmp[k]) return false;
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
    case TypeKind::array:
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::dep_name:
    case TypeKind::pack_expansion: pack_types_in(r.inner, out); return;
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
  std::optional<std::vector<TemplateArg>> deduce_call(const ast::DeclSema& tmpl, std::span<const ArgShape> args,
                                                       std::span<const TemplateArg> explicit_args) {
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
    if (args.size() < fixed || (args.size() > n && !trailing && !f.variadic) || (!trailing && args.size() < n))
      return std::nullopt;
    // The explicitly specified arguments are substituted first ([temp.deduct.general]/2); packs
    // stay open, to be extended by deduction ([temp.arg.explicit]/9).
    Bindings given = bindings(depth, out);
    for (auto& a : given.levels[depth])
      if (a && a->kind == TemplateArgKind::pack) a = std::nullopt;
    for (std::size_t i = 0; i < fixed; ++i) {
      if (!args[i].deducible) continue;
      auto p0 = substitute(tmpl_param_type(tmpl, i), given);
      if (!p0) return std::nullopt;
      if (!mentions(*p0, depth)) continue;  // checked by overload resolution as a conversion
      auto [p, a, allow] = call_pair(*p0, args[i], depth);
      if (!deduce(p, a, depth, out, allow)) return std::nullopt;
    }
    if (trailing) {
      auto p0 = substitute(types().rec(f.params[n - 1]).inner, given);
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
    } else {
      if (types().is_array(a)) a = types().pointer(types().rec(a).inner);
      else if (types().is_function(a)) a = types().pointer(a);
      else a = Types::unqualified(types().canonical(a));
      p = Types::unqualified(types().canonical(p));
      allow.qualification = types().is_pointer(p);
    }
    return {p, a, allow};
  }
  // Explicitly specified template arguments, by parameter ([temp.arg.explicit]): one each, and a
  // pack takes the rest; a value converted to its parameter's type with the arguments before it
  // substituted ([temp.arg.nontype]/1, [temp.deduct.general]/2). False if they do not fit.
  bool explicit_into(std::span<const ast::TemplateParamSema> params, std::span<const TemplateArg> written,
                     std::vector<std::optional<TemplateArg>>& out) {
    u16 depth = params.empty() ? 0 : params[0].depth;
    auto one = [&](const ast::TemplateParamSema& p, const TemplateArg& a) -> std::optional<TemplateArg> {
      if (a.kind != p.kind) return std::nullopt;
      if (p.kind == TemplateArgKind::type) return a;
      auto pt = substitute(p.type, bindings(depth, out));
      return pt ? converted_value(a, *pt) : std::nullopt;
    };
    std::size_t j = 0;
    for (std::size_t i = 0; i < params.size() && j < written.size(); ++i) {
      if (params[i].pack) {
        std::vector<TemplateArg> es;
        for (; j < written.size(); ++j) {
          auto v = one(params[i], written[j]);
          if (!v) return false;
          es.push_back(*v);
        }
        out[i] = pack(es);
        explicit_packs_.insert(u16(i));
        break;
      }
      auto v = one(params[i], written[j++]);
      if (!v) return false;
      out[i] = *v;
    }
    return j == written.size();
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

  // The arguments for which function template `tmpl` is the function of type `type`, after the
  // explicitly specified `explicit_args` ([temp.deduct.decl]): what an explicit specialization
  // of it specializes; none if there are none.
  std::optional<std::vector<TemplateArg>> deduce_declaration(const ast::DeclSema& tmpl, QualType type,
                                                             std::span<const TemplateArg> explicit_args) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    if (params.empty()) return std::nullopt;
    Deducing deducing(this, params);
    u16 depth = params[0].depth;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!explicit_into(params, explicit_args, out)) return std::nullopt;
    if (!deduce(tmpl.type, type, depth, out)) return std::nullopt;
    auto full = complete(tmpl, params, std::move(out));
    if (!full) return std::nullopt;
    auto t = specialization_type(tmpl, *full);
    if (!t || types().canonical(*t) != types().canonical(type)) return std::nullopt;
    return full;
  }

  // The type of a specialization's declaration: the template's function type with `args`.
  std::optional<QualType> specialization_type(const ast::DeclSema& tmpl, std::span<const TemplateArg> args) {
    std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(tmpl.template_params);
    return substitute(tmpl.type, Bindings::of(params.empty() ? 0 : params[0].depth, args));
  }

  // ------------------------------------------------------------ partial specializations ([temp.spec.partial])

  // The arguments of a partial specialization's parameters `params`, whose template arguments are
  // `pattern`, for which it matches the specialization with `args` ([temp.spec.partial.match]/2):
  // deduced from them, and substituting back to them; none if it does not match.
  std::optional<std::vector<TemplateArg>> match_partial(std::span<const ast::TemplateParamSema> params,
                                                        std::span<const TemplateArg> pattern, std::span<const TemplateArg> args) {
    if (params.empty()) return std::nullopt;
    Deducing deducing(this, params);
    u16 depth = params[0].depth;
    std::vector<std::optional<TemplateArg>> out(params.size());
    if (!deduce_args(pattern, args, depth, out) || std::ranges::any_of(out, [](const auto& a) { return !a; })) return std::nullopt;
    std::vector<TemplateArg> r;
    for (const auto& a : out) r.push_back(*a);
    auto back = substitute_args(pattern, Bindings::of(depth, r));
    if (!back || !std::ranges::equal(*back, args)) return std::nullopt;
    return r;
  }
  // Whether partial specialization 1 (`p1`, `a1`) is more specialized than 2 ([temp.spec.partial.order]):
  // as the function templates with one parameter of type `X<a1>` and `X<a2>` order ([temp.func.order]),
  // 2's arguments deduced from 1's with a unique value for each of 1's parameters, and not the converse.
  bool more_specialized_partial(std::span<const ast::TemplateParamSema> p1, std::span<const TemplateArg> a1,
                                std::span<const ast::TemplateParamSema> p2, std::span<const TemplateArg> a2) {
    return at_least_as_specialized(p1, a1, p2, a2) && !at_least_as_specialized(p2, a2, p1, a1);
  }
  // Whether every parameter of `params` can be deduced from template arguments `args` that name
  // them, as a partial specialization's must be ([temp.spec.partial.general]): deduced from
  // `args` with a unique value for each.
  bool deducible(std::span<const ast::TemplateParamSema> params, std::span<const TemplateArg> args) {
    if (params.empty()) return false;
    Deducing deducing(this, params);
    auto unique = substitute_args(args, unique_bindings(params));
    std::vector<std::optional<TemplateArg>> out(params.size());
    return unique && deduce_args(args, *unique, params[0].depth, out) && std::ranges::all_of(out, [](const auto& a) { return a.has_value(); });
  }
  bool at_least_as_specialized(std::span<const ast::TemplateParamSema> p1, std::span<const TemplateArg> a1,
                               std::span<const ast::TemplateParamSema> p2, std::span<const TemplateArg> a2) {
    if (p1.empty() || p2.empty()) return false;
    Deducing deducing(this, p2);
    auto unique = substitute_args(a1, unique_bindings(p1));
    std::vector<std::optional<TemplateArg>> out(p2.size());
    return unique && deduce_args(a2, *unique, p2[0].depth, out);
  }

  // ------------------------------------------------------------ partial ordering ([temp.func.order])

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
    // From position `i` on, where one of them has a function parameter pack: P's pack's pattern
    // deduced from every remaining A at once; an A from a pack deduces no P that is not one
    // ([temp.deduct.partial]/8).
    auto rest = [&](const TypeRec& ta, const Bindings& ba, const TypeRec& tp, std::span<const ast::TemplateParamSema> pp,
                    std::vector<std::optional<TemplateArg>>& out, std::size_t i) {
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
      Deducing deducing(this, pp);
      return deduce_expansion(type_arg(strip(xp->type)), as, pp[0].depth, out);
    };
    bool all12 = true, all21 = true;
    for (std::size_t i = 0; i < nargs; ++i) {
      auto x1 = param_at(t1, i), x2 = param_at(t2, i);
      if (!x1 || !x2) break;
      if (x1->pack || x2->pack) {
        all12 = all12 && rest(t1, b1, t2, p2, out12, i);
        all21 = all21 && rest(t2, b2, t1, p1, out21, i);
        break;
      }
      auto a1 = substitute(x1->type, b1);
      auto a2 = substitute(x2->type, b2);
      // [temp.deduct.partial]/8: an A from a function parameter pack deduces no P that is not one.
      bool s12 = a1 && !(x1->pack && !x2->pack) && deduce(strip(x2->type), strip(*a1), p2[0].depth, out12);
      bool s21 = a2 && !(x2->pack && !x1->pack) && deduce(strip(x1->type), strip(*a2), p1[0].depth, out21);
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
    // [temp.deduct.partial]/11: otherwise equal, a template without a trailing function parameter
    // pack is more specialized than one with.
    if (all12 && all21) {
      auto trailing = [&](const TypeRec& t) {
        return !t.params.empty() && types().kind(t.params.back()) == TypeKind::pack_expansion;
      };
      if (trailing(t2) && !trailing(t1)) return {true, false};
      if (trailing(t1) && !trailing(t2)) return {false, true};
    }
    return {all12, all21};
  }
  bool more_specialized(const ast::DeclSema& f1, const ast::DeclSema& f2, std::size_t nargs) {
    auto [a, b] = ordering(f1, f2, nargs);
    return a && !b;
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

  // A unique type or value for each of a template's parameters ([temp.func.order]/3): a
  // parameter of a depth no template-head has.
  Bindings unique_bindings(std::span<const ast::TemplateParamSema> ps) {
    std::vector<TemplateArg> unique;
    for (const ast::TemplateParamSema& p : ps) {
      if (p.kind == TemplateArgKind::type) unique.push_back(type_arg(types().template_param(unique_depth, p.index, p.pack != 0)));
      else unique.push_back(value_arg(p.type, w_->ins.intern_value(ast::VDepParam{unique_depth, p.index})));
    }
    return Bindings::of(ps[0].depth, unique);
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
  const Classes* classes_;
  // The template-head whose parameters a deduction deduces, and its packs whose leading elements
  // were specified explicitly ([temp.arg.explicit]/9).
  std::vector<ast::TemplateParamSema> head_;
  std::set<u16> explicit_packs_;
  // Sets `head_` (and clears the explicit packs) for one deduction.
  struct Deducing {
    Templates* t;
    std::vector<ast::TemplateParamSema> saved;
    std::set<u16> saved_explicit;
    Deducing(Templates* t, std::span<const ast::TemplateParamSema> head)
        : t(t), saved(std::move(t->head_)), saved_explicit(std::move(t->explicit_packs_)) {
      t->head_.assign(head.begin(), head.end());
      t->explicit_packs_.clear();
    }
    ~Deducing() {
      t->head_ = std::move(saved);
      t->explicit_packs_ = std::move(saved_explicit);
    }
  };
};

}  // namespace ycxx::sema
