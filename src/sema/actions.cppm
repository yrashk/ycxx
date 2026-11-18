// Semantic actions: what the parser calls as it parses (docs/design/ast.md §3.2), building the
// analysed tree in a frame builder (§9.3). Every expression node carries its type and value
// category (§3.3); implicit conversions are explicit nodes of the operand's width ([conv],
// [expr.arith.conv], [expr.context]).
//
// Positions: an `Ex` knows its token range within the part being analysed (an item's
// declaration part, or a function body, §3.4), so a parent stores each child at its offset
// relative to the parent. Diagnostics are located item-relative (§8.2).
//
// Lookups go through `Questions`, the stand-in for the engine context of query-engine.md §B1.4:
// once contextual queries exist (#16), each of its functions is a recorded question or a fetch.
export module ycxx.sema:actions;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.interp;
import ycxx.lex;
import :types;
import :diag;

export namespace ycxx::sema {

using ast::Child;
using ast::ExprInfo;
using ast::NodeId;
using ast::ValueCategory;

// What sema asks of its context (§B1.4, §B3). In the engine these are `visible_decls(::, name,
// at item)` and fetches of `decl_sema`.
class Questions {
public:
  // The declarations of `name` in the global namespace visible at the current point: those that
  // precede the current item, and, inside a function body, the item's own (§B2, [basic.scope.pdecl]).
  virtual std::vector<DeclId> visible_decls(NameId name) = 0;
  virtual ast::DeclSema decl_sema(DeclId d) = 0;
  // What a constant evaluation fetches (§B7): `function_ir` of a function defined before the
  // current point ([expr.const]), and a namespace-scope variable as the interpreter sees it.
  virtual interp::Callee function_ir(EntityId f) = 0;
  virtual std::optional<interp::GlobalVar> global(EntityId v) = 0;
  virtual bool is_constexpr(EntityId f) = 0;  // a constexpr or consteval function
  // Whether class `c` is complete at the current point ([class.mem.general]): its definition
  // precedes the point and does not enclose it.
  virtual bool is_complete(EntityId c) = 0;
  // The constant a variable usable in constant expressions holds ([expr.const]), if it is known.
  virtual std::optional<ConstId> constant_value(EntityId v) = 0;
  // In a member declaration of a class being defined: the member named `name` declared before it
  // ([basic.scope.class]; the class's member list is not complete yet).
  virtual std::optional<ast::DeclSema> earlier_member(NameId name) = 0;
  virtual ~Questions() = default;
};

// An analysed expression or statement under construction: a draft node and its token range in
// the part. `id` is none after an error that has been diagnosed; actions given such an operand
// give up silently, so one mistake gives one diagnostic.
struct Ex {
  NodeId id = NodeId::none;
  u32 begin = 0;
  u32 end = 0;
  ExprInfo info{};
  bool ok() const { return id != NodeId::none; }
  u32 width() const { return end - begin; }
  QualType type() const { return info.type; }
  ValueCategory cat() const { return info.category; }
  bool glvalue() const { return info.category != ValueCategory::prvalue; }
};

// A braced-init-list as written ([dcl.init.general]): each initializer-clause is an analysed
// expression or a nested list, with its designator (`.name =`, [dcl.init.aggr]) if it has one.
// Sema decides what it initializes once it knows the target type.
struct Braced {
  struct Clause {
    Ex expr;
    std::unique_ptr<Braced> list;
    NameId designator{};
    bool designated = false;
    u32 begin = 0;
  };
  u32 begin = 0;
  u32 end = 0;
  std::vector<Clause> clauses;
};

// Flags of `LocalRec::flags` and `DeclSema::flags`.
namespace local_flag {
inline constexpr u16 parameter = 1;
}
namespace decl_flag {
inline constexpr u16 definition = 1;
inline constexpr u16 inline_ = 2;
inline constexpr u16 constexpr_ = 4;
inline constexpr u16 variadic = 8;
inline constexpr u16 lang_c = 16;  // declared with C language linkage ([dcl.link])
inline constexpr u16 consteval_ = 32;  // an immediate function ([dcl.constexpr])
inline constexpr u16 member = 64;      // a member of a class ([class.mem])
inline constexpr u16 static_ = 128;    // a static member ([class.static])
inline constexpr u16 deleted = 256;    // a deleted definition ([dcl.fct.def.delete])
inline constexpr u16 constructor = 512;
inline constexpr u16 destructor = 1024;
inline constexpr u16 implicit = 2048;  // an implicitly declared special member ([special])
}

class Sema {
public:
  Sema(World& w, ast::Builder& b, Questions& q, std::vector<engine::Diagnostic>& diags, ast::ItemId item,
       u32 loc_base)
      : w_(&w), b_(&b), q_(&q), diags_(&diags), item_(item), loc_base_(loc_base) {}

  World& world() { return *w_; }
  ast::ItemId item() const { return item_; }
  Types& types() { return w_->types; }
  ast::Builder& builder() { return *b_; }
  ast::Interners& ins() { return w_->ins; }

  // ------------------------------------------------------------ diagnostics

  engine::DiagArg arg(NameId n) { return {std::to_underlying(n), ins().names.fp(n)}; }
  engine::DiagArg arg(QualType t) { return {std::to_underlying(t), types().fp(t)}; }
  engine::DiagArg arg(lex::Tk k) { return engine::diag_arg(std::to_underlying(k)); }
  engine::DiagArg arg(u64 v) { return engine::diag_arg(v); }
  engine::DiagArg arg(ConstId c) { return {std::to_underlying(c), ins().values.fp(c)}; }

  template <class... A>
  void diag(u32 at, Diag d, const A&... args) {
    diags_->push_back({engine::Loc{std::to_underlying(item_), loc_base_ + at}, diag_id(d), {arg(args)...}});
  }
  std::size_t diag_count() const { return diags_->size(); }
  // Drops the diagnostics after the first `n`: a part analysed again where it was diagnosed already.
  void truncate_diags(std::size_t n) { diags_->resize(n); }

  // ------------------------------------------------------------ building

  template <class T>
  Ex node(const T& v, u32 begin, u32 end, ExprInfo info = {}) {
    return {b_->add(v, end - begin), begin, end, info};
  }
  static Child at(const Ex& child, u32 parent_begin) {
    return {child.id, child.ok() ? child.begin - parent_begin : 0};
  }
  // A draft node read back, as the parser does while it builds.
  template <class T>
  std::optional<T> as(const Ex& e) const {
    if (!e.ok() || ast::kind_of(e.id) != ast::Nodes::index_of<T>()) return std::nullopt;
    return b_->get<T>(e.id);
  }

  QualType builtin(Builtin k) { return types().builtin(k); }

  // What constant evaluation answers for list-initialization's narrowing check ([dcl.init.list]);
  // set by the parser, which owns the evaluator.
  std::function<std::optional<ConstId>(Ex)> constant_of;

  // ------------------------------------------------------------ classes ([class], [expr.ref])

  // A type-name naming a class, found by unqualified lookup ([basic.lookup.unqual]): none when
  // the name is not declared, or a variable or function hides it ([basic.scope.hiding]). With
  // `elaborated`, non-type declarations are ignored ([basic.lookup.elab]).
  std::optional<QualType> class_name(NameId name, bool elaborated = false) {
    if (!elaborated)
      for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
        for (auto p = s->rbegin(); p != s->rend(); ++p)
          if (p->first == name) return std::nullopt;
    std::optional<ast::DeclSema> found;
    for (auto p = own.rbegin(); p != own.rend() && !found; ++p)
      if (p->name == name) found = p->sema;
    if (!found)
      for (DeclId d : q_->visible_decls(name)) {
        ast::DeclSema s = q_->decl_sema(d);
        if (s.entity == EntityId::none) continue;
        if (s.kind == ast::DeclKind::class_ || !elaborated) found = s;
      }
    if (!found || found->kind != ast::DeclKind::class_) return std::nullopt;
    return found->type;
  }

  // Whether `t` is complete at this point; a class needs its definition before the point.
  bool is_complete(QualType t) {
    if (types().is_void(t)) return false;
    if (types().is_array(t)) return is_complete(types().rec(t).inner);
    if (types().is_class(t)) return q_->is_complete(types().class_of(t));
    return true;
  }
  // Diagnoses an object of incomplete type, at `at`.
  bool require_complete(QualType t, u32 at) {
    if (is_complete(t)) return true;
    diag(at, Diag::incomplete_type, t);
    return false;
  }

  // [expr.ref]: `object.name`, or `object->name` with `arrow`.
  Ex member(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at) {
    u32 begin = object.begin;
    Ex fail{NodeId::none, begin, end};
    if (!object.ok()) return fail;
    QualType ct = object.type();
    if (arrow) {
      object = rvalue(object);
      if (!types().is_pointer(object.type()) || !types().is_class(types().pointee(object.type()))) {
        diag(op_at, Diag::arrow_of_non_pointer, object.type());
        return fail;
      }
      ct = types().pointee(object.type());
    } else if (!types().is_class(ct)) {
      diag(op_at, Diag::member_of_non_class, ct);
      return fail;
    } else if (!object.glvalue()) {
      object = materialize(object, ct, false);  // [expr.ref]: a class prvalue is materialized
    }
    if (!require_complete(ct, op_at)) return fail;
    EntityId c = types().class_of(ct);
    auto found = find_member(c, name);
    if (!found) {
      diag(name_at, Diag::no_member, name, Types::unqualified(ct));
      return fail;
    }
    if (found->member && found->member->is_function) return bound_member(c, name, object, arrow, begin, end);
    if (found->member) {
      // A static data member: the object expression is evaluated, then discarded ([expr.ref]).
      const MemberDeclRec& m = *found->member;
      ExprInfo info{m.type, ValueCategory::lvalue, 0};
      Ex ref = node(ast::DeclRef{info, m.entity}, name_at, end, info);
      Ex discarded = convert_node(object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(ref, begin)}, begin, end, info);
    }
    u32 index = found->index;
    const FieldRec& f = *found->field;
    // [expr.ref]/7.2: the member's type, with the object's cv-qualifiers added; the result is an
    // lvalue if the object is (or is reached through a pointer), else an xvalue.
    // A reference member gives the object it refers to, an lvalue.
    bool ref = types().is_reference(f.type);
    QualType t = ref ? types().referee(f.type) : types().add_cv(f.type, Types::cv(ct));
    ValueCategory cat = ref || arrow || object.cat() == ValueCategory::lvalue ? ValueCategory::lvalue : ValueCategory::xvalue;
    ExprInfo info{t, cat, 0};
    return node(ast::Member{info, f.entity, index, arrow ? 1u : 0u, ref ? 1u : 0u, at(object, begin)}, begin, end, info);
  }

  // ------------------------------------------------------------ initialization ([dcl.init])

  // An aggregate ([dcl.init.aggr]): an array, or a class (every class the front end knows so far
  // is one: no constructors, bases or virtual functions yet).
  bool is_aggregate(QualType t) {
    if (types().is_array(t)) return true;
    if (!types().is_class(t)) return false;
    const ClassRec* c = w_->classes.find(types().class_of(t));
    return !c || !c->user_ctors;  // [dcl.init.aggr]/1: no user-declared constructors
  }
  bool has_user_ctors(QualType t) { return types().is_class(t) && !is_aggregate(t); }

  // The destructor that ends an object of type `t`, or none if it is trivial ([class.dtor]); for
  // an array, its elements'.
  EntityId destructor_of(QualType t) { return w_->classes.destructor_of(t); }

  // The elements of an aggregate, in order: an array's elements, a class's non-static data
  // members, each with the aggregate's cv added.
  struct Element {
    QualType type;
    NameId name;
    ConstId init = ConstId::none;  // a member's default member initializer, if constant
    bool dmi = false;              // it has one
  };
  std::vector<Element> elements(QualType t) {
    std::vector<Element> r;
    TypeRec tr = types().rec(t);
    if (tr.kind == TypeKind::array) {
      r.assign(tr.bound, Element{tr.inner, no_name()});
    } else if (const ClassRec* c = tr.kind == TypeKind::class_ ? w_->classes.find(tr.entity) : nullptr) {
      for (const FieldRec& f : c->fields) r.push_back({types().add_cv(f.type, Types::cv(t)), f.name, f.init, f.dmi});
    }
    return r;
  }

  // Whether initializing a `t` runs a default member initializer.
  // (or a constructor).
  bool has_member_inits(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array) return has_member_inits(r.inner);
    if (r.kind != TypeKind::class_) return false;
    if (const ClassRec* c = w_->classes.find(r.entity); c && c->user_ctors) return true;
    if (const ClassRec* c = w_->classes.find(r.entity))
      for (const FieldRec& f : c->fields)
        if (f.init != ConstId::none || has_member_inits(f.type)) return true;
    return false;
  }

  // Value-initialization ([dcl.init.general]) of an object of type `t`. An aggregate whose
  // members have default member initializers gets them (the implicit default constructor's).
  Ex value_init(QualType t, u32 begin, u32 end) {
    if (has_user_ctors(t)) return construct(t, {}, begin, end);  // the default constructor
    if (has_member_inits(t)) return member_inits(t, true, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::ValueInit{info}, begin, end, info);
  }
  // Default-initialization ([dcl.init.general]/7): the default member initializers, and no
  // initialization for the rest.
  Ex default_init(QualType t, u32 begin, u32 end) {
    if (has_user_ctors(t)) return construct(t, {}, begin, end);
    if (has_member_inits(t)) return member_inits(t, false, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::DefaultInit{info}, begin, end, info);
  }
  // An aggregate's elements each from its default member initializer, else value- or
  // default-initialized.
  Ex member_inits(QualType t, bool value, u32 begin, u32 end) {
    std::vector<Ex> out;
    for (const Element& e : elements(t)) out.push_back(element_default(e, value, begin, end));
    return list_of(t, std::move(out), begin, end);
  }
  Ex element_default(const Element& e, bool value, u32 begin, u32 end) {
    if (e.init != ConstId::none) return literal(Types::unqualified(e.type), e.init, ValueCategory::prvalue, begin, end);
    if (e.dmi) {
      diag(begin, Diag::not_supported, lex::Tk::equal);  // a non-constant default member initializer in an aggregate (#118)
      return {NodeId::none, begin, end};
    }
    return value ? value_init(e.type, begin, end) : default_init(e.type, begin, end);
  }
  Ex list_of(QualType t, std::vector<Ex> out, u32 begin, u32 end) {
    std::vector<Child> cs;
    for (const Ex& e : out) cs.push_back({e.id, e.begin >= begin ? e.begin - begin : 0});
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    Ex r = node(ast::InitList{info, b_->list(cs)}, begin, end, info);
    list_elems_[std::to_underlying(r.id)] = std::move(out);
    return r;
  }

  // List-initialization of an object of type `t` from `b` ([dcl.init.list]/3).
  Ex list_init(QualType t, const Braced& b) {
    Ex fail{NodeId::none, b.begin, b.end};
    if (!require_complete(t, b.begin)) return fail;
    // [dcl.init.general]: an initializer-list or a designated-initializer-list, not a mix.
    if (std::ranges::any_of(b.clauses, &Braced::Clause::designated) &&
        !std::ranges::all_of(b.clauses, &Braced::Clause::designated)) {
      diag(b.begin, Diag::mixed_designators);
      return fail;
    }
    // [dcl.init.list]/3.2: a class from one element of its own class.
    if (types().is_class(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated &&
        initializes(t, b.clauses[0].expr))
      return convert(b.clauses[0].expr, t, b.clauses[0].begin);
    // [dcl.init.list]/3.7: a class with constructors: `{}` value-initializes it, else the clauses
    // are a constructor's arguments, without narrowing.
    if (has_user_ctors(t)) {
      if (b.clauses.empty()) return value_init(t, b.begin, b.end);
      std::vector<Ex> args;
      for (const Braced::Clause& c : b.clauses) {
        if (c.list || c.designated) {
          diag(c.begin, Diag::not_supported, lex::Tk::l_brace);  // nested lists as constructor arguments
          return fail;
        }
        args.push_back(c.expr);
      }
      return construct(t, std::move(args), b.begin, b.end, true);
    }
    // A reference from one element ([dcl.init.list]/3.10).
    if (types().is_reference(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
      return bind_reference(b.clauses[0].expr, t, b.clauses[0].begin);
    if (is_aggregate(t)) {
      std::size_t next = 0;
      Ex r = aggregate(t, b, next, b.begin, b.end);
      if (r.ok() && next < b.clauses.size()) {
        diag(b.clauses[next].begin, Diag::too_many_initializers, Types::unqualified(t));
        return fail;
      }
      return r;
    }
    // A scalar: `{}` value-initializes it, `{e}` copy-initializes it from `e`, without narrowing.
    if (b.clauses.empty()) return value_init(t, b.begin, b.end);
    if (b.clauses.size() > 1) {
      diag(b.clauses[1].begin, Diag::too_many_initializers, Types::unqualified(t));
      return fail;
    }
    const Braced::Clause& c = b.clauses[0];
    if (c.list || c.designated) {
      diag(c.begin, Diag::braces_for_scalar, Types::unqualified(t));
      return fail;
    }
    return list_element(t, c.expr);
  }

  // The elements of aggregate `t` from the clauses of `b` starting at `next`, which advances past
  // those used. Called for a braced list, and for brace elision ([dcl.init.aggr]/16), where the
  // sub-aggregate takes as many clauses as it has elements.
  Ex aggregate(QualType t, const Braced& b, std::size_t& next, u32 begin, u32 end) {
    Ex fail{NodeId::none, begin, end};
    std::vector<Element> elems = elements(t);
    std::vector<Ex> out;
    bool elided = begin != b.begin || end != b.end;
    for (std::size_t i = 0; i < elems.size(); ++i) {
      if (next == b.clauses.size()) {
        out.push_back(element_default(elems[i], true, end, end));
        continue;
      }
      const Braced::Clause& c = b.clauses[next];
      if (c.designated) {
        // [dcl.init.aggr]/3.1: designators name members in declaration order; the members
        // skipped are initialized as if there were no initializer.
        std::size_t j = i;
        while (j < elems.size() && elems[j].name != c.designator) ++j;
        if (elided || !types().is_class(t) || j == elems.size()) {
          diag(c.begin, Diag::bad_designator, c.designator, Types::unqualified(t));
          return fail;
        }
        for (; i < j; ++i) out.push_back(element_default(elems[i], true, c.begin, c.begin));
      }
      QualType et = elems[i].type;
      if (c.list) {
        ++next;
        out.push_back(list_init(et, *c.list));
      } else if (is_aggregate(et) && !c.designated && !initializes(et, c.expr)) {
        // Brace elision: the clause begins the sub-aggregate's own elements.
        if (!require_complete(et, c.begin)) return fail;
        out.push_back(aggregate(et, b, next, c.begin, c.begin));
        out.back().end = next > 0 ? b.clauses[next - 1].expr.end : c.begin;
      } else {
        ++next;
        out.push_back(list_element(et, c.expr));
      }
      if (!out.back().ok()) return fail;
    }
    return list_of(t, std::move(out), begin, end);
  }
  // The elements of an `InitList` this frame built, as analysed.
  std::span<const Ex> list_elements(const Ex& list) const {
    auto it = list_elems_.find(std::to_underlying(list.id));
    if (it == list_elems_.end()) return {};
    return it->second;
  }

  // The bound of an array of `elem` initialized by `b` ([dcl.array]/8): the number of elements
  // its clauses initialize, brace elision included.
  u64 deduce_bound(QualType elem, const Braced& b) {
    u64 n = 0;
    std::size_t next = 0;
    while (next < b.clauses.size()) {
      const Braced::Clause& c = b.clauses[next];
      if (c.list || c.designated || !is_aggregate(elem) || initializes(elem, c.expr) || !is_complete(elem)) ++next;
      else {
        std::size_t before = next;
        aggregate(elem, b, next, c.begin, c.begin);
        if (next == before) ++next;  // an element with no elements of its own takes the clause
      }
      ++n;
    }
    return n;
  }

  // Whether `e` initializes an object of aggregate type `t` by itself rather than starting its
  // elements: a class object from an expression of its own class ([dcl.init.aggr]/16).
  bool initializes(QualType t, const Ex& e) {
    return e.ok() && types().is_class(t) && types().is_class(e.type()) &&
           types().class_of(t) == types().class_of(e.type());
  }

  // An element copy-initialized from `e`, where a narrowing conversion is ill-formed
  // ([dcl.init.list]/7).
  Ex list_element(QualType t, Ex e) {
    if (!e.ok()) return e;
    Ex from = rvalue(e);
    Ex r = convert(e, t, e.begin);
    if (r.ok() && narrows(from, t)) {
      diag(e.begin, Diag::narrowing, from.type(), Types::unqualified(t));
      return {NodeId::none, e.begin, e.end};
    }
    return r;
  }

  // [dcl.init.list]/7: an integral conversion to a type that cannot represent every value of the
  // source type, unless the source is a constant whose value fits; a pointer to bool.
  bool narrows(const Ex& from, QualType to) {
    if (!from.ok()) return false;
    auto a = types().builtin_of(from.type()), b = types().builtin_of(to);
    if (types().is_pointer(from.type()) && b == Builtin::bool_) return true;
    if (!a || !b || !ast::is_integral(*a) || !ast::is_integral(*b) || *a == *b) return false;
    IntShape sa = shape(*a), sb = shape(*b);
    if (*b == Builtin::bool_) sb = {1, false};
    if (*a == Builtin::bool_) sa = {1, false};
    if (ast::represents(target, *b, sa.bits, sa.is_signed) || (*b == Builtin::bool_ && *a == Builtin::bool_)) return false;
    auto v = constant_of ? constant_of(from) : std::nullopt;
    if (!v || w_->values.rec(*v).kind != ValueKind::integer) return true;
    // The constant, read in the source type, must lie in the target type's range.
    u64 mask = sa.bits >= 64 ? ~u64{0} : (u64{1} << sa.bits) - 1;
    u64 raw = w_->values.rec(*v).bits & mask;
    bool negative = sa.is_signed && ((raw >> (sa.bits - 1)) & 1);
    u64 max = sb.is_signed ? (u64{1} << (sb.bits - 1)) - 1 : sb.bits >= 64 ? ~u64{0} : (u64{1} << sb.bits) - 1;
    if (!negative) return raw > max;
    if (!sb.is_signed) return true;
    std::int64_t value = std::int64_t(raw | ~mask);
    std::int64_t min = sb.bits >= 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t{1} << (sb.bits - 1));
    return value < min;
  }

  // ------------------------------------------------------------ scopes and locals (§4.4)

  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() {
    if (!guarded_.empty() && guarded_.back() == scopes_.size() - 1) guarded_.pop_back();
    scopes_.pop_back();
  }
  std::size_t scope_depth() const { return scopes_.size(); }

  // Declares a block-scope variable or a parameter in the innermost scope ([basic.scope.block]).
  // A name may not be redeclared in its scope, nor in the outermost block of a function body
  // when it names a parameter ([basic.scope.param]).
  // A name may not be redeclared in its scope, nor in the outermost block of a substatement whose
  // enclosing scope is guarded: a function's parameters, a for-init-statement's or a condition's
  // declarations ([basic.scope.block]/2). Parameters are checked by the parser (`check` false).
  u32 declare_local(NameId name, QualType type, u16 flags, u32 at, bool check = true) {
    if (name != no_name() && check) {
      bool clash = std::ranges::any_of(scopes_.back(), [&](auto& p) { return p.first == name; });
      if (scopes_.size() >= 2 && !guarded_.empty() && guarded_.back() == scopes_.size() - 2)
        clash = clash || std::ranges::any_of(scopes_[guarded_.back()], [&](auto& p) { return p.first == name; });
      if (clash) diag(at, Diag::redeclared_local, name);
    }
    u32 index = static_cast<u32>(locals_.size());
    locals_.push_back({name, type, EntityId::none, NodeId::none, flags});
    local_values_.push_back(std::nullopt);
    if (name != no_name()) scopes_.back().push_back({name, index});
    return index;
  }
  void set_local_decl(u32 local, NodeId decl) { locals_[local].decl = decl; }
  // An array of unknown bound gets its bound from its initializer ([dcl.array]/8).
  void set_local_type(u32 local, QualType t) { locals_[local].type = t; }
  // A local usable in constant expressions ([expr.const]): constexpr, or const integral with a
  // constant initializer; its value.
  void set_local_value(u32 local, ConstId v) { local_values_[local] = v; }
  std::span<const std::optional<ConstId>> local_values() const { return local_values_; }
  const ast::LocalRec& local(u32 i) const { return locals_[i]; }
  std::span<const ast::LocalRec> locals() const { return locals_; }
  // The function's parameters are in the scope at this depth; its body's outermost block is the
  // next one.
  // Marks the innermost scope as guarded (`declare_local`); it stays so until it is popped.
  void guard_scope() { guarded_.push_back(scopes_.size() - 1); }
  // Hides every block scope, for a part analysed in the class's scope inside a function (a
  // default member initializer in a constructor), and gives them back.
  using Scopes = std::vector<std::vector<std::pair<NameId, u32>>>;
  Scopes take_scopes() { return std::exchange(scopes_, {}); }
  void put_scopes(Scopes s) { scopes_ = std::move(s); }

  NameId no_name() { return ins().names.intern(""); }

  // ------------------------------------------------------------ the function being defined

  void set_return_type(QualType t) { return_type_ = t; }
  QualType return_type() const { return return_type_; }
  u32 loop_depth = 0;
  // Inside an immediate function context ([expr.const]): a consteval function's body, or the
  // consteval branch of `if consteval`. A call to a consteval function elsewhere is an immediate
  // invocation, evaluated where it appears.
  u32 immediate_context = 0;
  bool is_consteval(EntityId f) const { return consteval_.contains(std::to_underlying(f)); }

  // Names the current item has declared so far in its declaration part; visible after their
  // declarators ([basic.scope.pdecl]).
  struct OwnDecl {
    NameId name;
    ast::DeclSema sema;
  };
  std::vector<OwnDecl> own;

  // ------------------------------------------------------------ member functions ([class.mfct])

  // The class whose member function body is being analysed, and the type of `this` there: none
  // outside a non-static member function ([expr.prim.this]).
  EntityId current_class = EntityId::none;
  std::optional<QualType> this_type;

  Ex this_expr(u32 begin, u32 end) {
    if (!this_type) {
      diag(begin, Diag::this_outside_member);
      return {NodeId::none, begin, end};
    }
    ExprInfo info{*this_type, ValueCategory::prvalue, 0};
    return node(ast::This{info}, begin, end, info);
  }

  // A member of class `c` named `name` ([class.member.lookup], one class, no bases yet): a
  // non-static data member, or a member function or static data member.
  struct Found {
    const FieldRec* field = nullptr;
    u32 index = 0;
    const MemberDeclRec* member = nullptr;
  };
  std::optional<Found> find_member(EntityId c, NameId name) {
    const ClassRec* cr = w_->classes.find(c);
    if (!cr) return std::nullopt;
    for (u32 i = 0; i < cr->fields.size(); ++i)
      if (cr->fields[i].name == name) return Found{&cr->fields[i], i, nullptr};
    for (const MemberDeclRec& m : cr->members)
      if (m.name == name && !m.is_ctor && !m.is_dtor) return Found{nullptr, 0, &m};
    return std::nullopt;
  }
  // The member functions of `c` named `name`: an overload set ([over.match.funcs]).
  std::vector<const MemberDeclRec*> member_functions(EntityId c, NameId name) {
    std::vector<const MemberDeclRec*> r;
    if (const ClassRec* cr = w_->classes.find(c))
      for (const MemberDeclRec& m : cr->members)
        if (m.name == name && m.is_function && !m.is_ctor && !m.is_dtor) r.push_back(&m);
    return r;
  }
  const MemberDeclRec* member_decl(EntityId f) {
    EntityId c = w_->entities.rec(f).scope;
    if (const ClassRec* cr = c == EntityId::none ? nullptr : w_->classes.find(c))
      for (const MemberDeclRec& m : cr->members)
        if (m.entity == f) return &m;
    return nullptr;
  }

  // A member function named without a call yet: `a.f`, `p->f`, `f` or `S::f`, bound to its object
  // (none when there is none to bind, as in a static member function). Only a call may follow
  // ([expr.ref]/6.3.2).
  Ex bound_member(EntityId c, NameId name, std::optional<Ex> object, bool arrow, u32 begin, u32 end) {
    auto cands = member_functions(c, name);
    ExprInfo info{builtin(Builtin::bound_member), ValueCategory::prvalue, 0};
    Ex r = node(ast::Member{info, cands.front()->entity, ~u32{0}, arrow ? 1u : 0u, 0, object ? at(*object, begin) : Child{}},
                begin, end, info);
    bound_[std::to_underlying(r.id)] = {object, std::move(cands)};
    return r;
  }
  bool is_bound_member(const Ex& e) { return e.ok() && types().is(e.type(), Builtin::bound_member); }

  // The implicit member access `name` inside a member function of `c` ([class.mfct.non.static]),
  // or a qualified name `C::name`: none if `c` has no such member.
  std::optional<Ex> class_member(EntityId c, NameId name, u32 begin, u32 end) {
    auto f = find_member(c, name);
    if (!f) return std::nullopt;
    Ex fail{NodeId::none, begin, end};
    bool has_this = this_type && current_class == c;
    if (f->field) {
      if (!has_this) {
        diag(begin, Diag::member_without_object, name);
        return fail;
      }
      Ex self = this_expr(begin, begin);
      bool ref = types().is_reference(f->field->type);
      QualType t = ref ? types().referee(f->field->type) : types().add_cv(f->field->type, Types::cv(types().pointee(*this_type)));
      ExprInfo info{t, ValueCategory::lvalue, 0};
      return node(ast::Member{info, f->field->entity, f->index, 1, ref ? 1u : 0u, at(self, begin)}, begin, end, info);
    }
    const MemberDeclRec& m = *f->member;
    // A static member, data or function, is named like a namespace-scope entity
    // ([expr.prim.id.qual]): `S::f` can have its address taken or be called.
    if (!m.is_function || (m.is_static && member_functions(c, name).size() == 1)) {
      ExprInfo info{m.type, ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, m.entity}, begin, end, info);
    }
    std::optional<Ex> object;
    if (has_this) object = this_expr(begin, begin);
    return bound_member(c, name, object, true, begin, end);
  }

  // `C::name` ([expr.prim.id.qual]) for a class C.
  Ex qualified_name(NameId cls, NameId name, u32 begin, u32 name_at, u32 end) {
    Ex fail{NodeId::none, begin, end};
    auto t = class_name(cls, true);
    if (!t) {
      diag(begin, Diag::not_a_type, cls);
      return fail;
    }
    if (!require_complete(*t, begin)) return fail;
    if (auto r = class_member(types().class_of(*t), name, begin, end)) return *r;
    diag(name_at, Diag::no_member, name, *t);
    return fail;
  }

  // A call of a member function ([expr.call], [over.match.funcs]): a static one is an ordinary
  // call; a non-static one passes its object, which must be at least as cv-qualified as the
  // function ([class.this]).
  Ex member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at) {
    u32 begin = callee.begin;
    Ex fail{NodeId::none, begin, end};
    auto m = *as<ast::Member>(callee);
    const Bound& bound = bound_.at(std::to_underlying(callee.id));
    std::optional<Ex> object = bound.object;
    // The object's cv-qualifiers, which a non-static member function must have ([class.this]).
    std::optional<Cv> object_cv;
    if (object) object_cv = Types::cv(m.arrow ? types().pointee(object->type()) : object->type());
    std::vector<Candidate> cands;
    for (const MemberDeclRec* c : bound.cands)
      cands.push_back({c->entity, c->type, c->is_static, c->is_deleted});
    // [class.this]: a non-static member function needs the object's cv-qualifiers.
    if (object_cv && std::ranges::none_of(bound.cands, [&](const MemberDeclRec* c) {
          return c->is_static || cv_within(*object_cv, types().rec(c->type).fn_cv);
        })) {
      diag(begin, Diag::const_member_call, bound.cands.front()->name);
      return fail;
    }
    auto chosen = resolve(cands, args, begin, bound.cands.front()->name, object_cv);
    if (!chosen) return fail;
    const MemberDeclRec* d = bound.cands[*chosen];
    TypeRec fr = types().rec(d->type);
    auto children = call_args(fr, args, begin, paren_at);
    if (!children) return fail;
    ExprInfo info = result_info(fr.inner);
    if (d->is_static) {
      // The object expression is evaluated, then discarded ([expr.ref]).
      Ex call = node(ast::Call{info, d->entity, at(callee, begin), b_->list(*children)}, begin, end, info);
      if (!object) return call;
      Ex discarded = convert_node(*object, ast::ConversionKind::to_void, builtin(Builtin::void_));
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(call, begin)}, begin, end, info);
    }
    if (!object) {
      diag(begin, Diag::member_without_object, d->name);
      return fail;
    }
    Ex o = *object;
    if (!m.arrow) {
      if (!o.glvalue()) {
        diag(paren_at, Diag::not_supported, lex::Tk::l_paren);  // a member call on a temporary
        return fail;
      }
      ExprInfo pi{types().pointer(o.type()), ValueCategory::prvalue, 0};
      o = node(ast::Unary{pi, ast::UnaryOp::address_of, at(o, o.begin)}, o.begin, o.end, pi);
    }
    return node(ast::MemberCall{info, d->entity, at(o, begin), b_->list(*children)}, begin, end, info);
  }

  // ------------------------------------------------------------ overload resolution ([over.match])

  // A candidate function: its entity (none for an implicit trivial special member), its type,
  // whether it takes no implicit object (a non-member or static function), and whether it is
  // deleted.
  struct Candidate {
    EntityId entity;
    QualType type;
    bool no_object;
    bool deleted;
  };

  // An implicit conversion sequence's ranking ([over.ics.rank]): exact match, promotion,
  // conversion, then a tie-break among exact matches (an added qualification, or a reference
  // to a more cv-qualified type, is worse); `viable` false when there is none.
  struct Ics {
    bool viable = false;
    int rank = 0;
    int sub = 0;              // a qualification conversion among exact matches ([over.ics.rank]/3.2.5)
    bool ref = false;         // a reference binding: the tie-breaks of /3.2.3 and /3.2.6 apply
    bool rvalue_ref = false;  // binding an rvalue reference
    bool to_rvalue = false;   // to an rvalue
    int ref_cv = 0;           // the cv-qualifiers the referred-to type adds
    bool operator<(const Ics& o) const {
      if (rank != o.rank) return rank < o.rank;
      if (ref && o.ref) {
        if (to_rvalue && o.to_rvalue && rvalue_ref != o.rvalue_ref) return rvalue_ref;  // /3.2.3
        return ref_cv < o.ref_cv;                                                         // /3.2.6
      }
      if (!ref && !o.ref) return sub < o.sub;
      return false;
    }
  };
  static Ics binding(Ics v, bool rvalue_ref, bool to_rvalue, int cv) {
    v.ref = true;
    v.rvalue_ref = rvalue_ref;
    v.to_rvalue = to_rvalue;
    v.ref_cv = cv;
    return v;
  }
  static int cv_bits(Cv c) { return std::popcount(unsigned(std::to_underlying(c))); }
  static bool cv_within(Cv a, Cv b) { return (std::to_underlying(a) & ~std::to_underlying(b)) == 0; }
  bool same_unqualified(QualType a, QualType b) {
    return types().canonical(Types::unqualified(a)) == types().canonical(Types::unqualified(b));
  }

  // The type `a` has after the lvalue conversions ([conv.lval], [conv.array], [conv.func]).
  QualType value_type(const Ex& a) {
    QualType t = a.type();
    if (!a.glvalue()) return t;
    if (types().is_array(t)) return types().pointer(types().rec(t).inner);
    if (types().is_function(t)) return types().pointer(t);
    return Types::unqualified(t);
  }

  Ics ics(const Ex& a, QualType p) {
    if (!a.ok()) return {};
    if (types().is_reference(p)) {
      TypeRec r = types().rec(p);
      QualType t = r.inner;
      // [dcl.init.ref]: a direct binding to an lvalue (or, for an rvalue reference, an xvalue)
      // of the type, no less qualified.
      bool direct_cat = r.rvalue_ref ? a.cat() == ValueCategory::xvalue : a.cat() == ValueCategory::lvalue;
      if (direct_cat && same_unqualified(a.type(), t) && cv_within(Types::cv(a.type()), Types::cv(t)))
        return binding({true, 0, 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue,
                       cv_bits(Types::cv(t)) - cv_bits(Types::cv(a.type())));
      bool const_ref = Types::is_const(t) && !Types::is_volatile(t);
      if (!r.rvalue_ref && !const_ref) return {};
      if (r.rvalue_ref && a.cat() == ValueCategory::lvalue) return {};
      Ics v = ics(a, Types::unqualified(t));  // to a temporary
      if (!v.viable) return v;
      return binding(v, r.rvalue_ref, true, cv_bits(Types::cv(t)));
    }
    QualType to = Types::unqualified(p);
    if (types().is_class(to)) {
      if (initializes(to, a)) return {true, 0, 0};
      return {};
    }
    if (types().is_pointer(to) && is_null_pointer_constant(a)) return {true, 2, 0};
    QualType from = value_type(a);
    if (types().is_class(from) || types().is_void(from)) return {};
    if (types().canonical(from) == types().canonical(to)) return {true, 0, 0};
    if (types().is_pointer(from) && types().is_pointer(to))
    {
      if (qualification_convertible(from, to)) return {true, 0, 1};
      return pointer_convertible(from, to) ? Ics{true, 2, 0} : Ics{};
    }
    if (types().is(to, Builtin::bool_) && (types().is_arithmetic(from) || types().is_pointer(from))) return {true, 2, 0};
    if (types().is_integral(from) && types().is_integral(to)) {
      bool promotion = builtin(promoted(*types().builtin_of(from))) == to && !types().is(from, *types().builtin_of(to));
      return {true, promotion ? 1 : 2, 0};
    }
    return {};
  }

  // The best viable candidate for `args` ([over.match.best]), diagnosed at `at` when there is
  // none or more than one, or when it is deleted. `object_cv` is the implicit object's, for
  // non-static member functions.
  std::optional<std::size_t> resolve(std::span<const Candidate> cands, std::span<const Ex> args, u32 at, NameId name,
                                     std::optional<Cv> object_cv, std::optional<QualType> ctor_of = std::nullopt) {
    std::vector<std::pair<std::size_t, std::vector<Ics>>> viable;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      TypeRec fr = types().rec(cands[i].type);
      if (args.size() < fr.params.size() || (args.size() > fr.params.size() && !fr.variadic)) continue;
      std::vector<Ics> seq;
      // The implicit object parameter, first: the object must not be more cv-qualified.
      if (!cands[i].no_object && object_cv) {
        if (!cv_within(*object_cv, fr.fn_cv)) continue;
        seq.push_back(binding({true, 0, 0}, false, false, cv_bits(fr.fn_cv) - cv_bits(*object_cv)));
      } else {
        seq.push_back({true, 0, 0});
      }
      bool ok = true;
      for (std::size_t k = 0; k < args.size() && ok; ++k) {
        Ics c = k < fr.params.size() ? ics(args[k], fr.params[k]) : Ics{true, 3, 0};  // the ellipsis
        ok = c.viable;
        seq.push_back(c);
      }
      if (ok) viable.push_back({i, std::move(seq)});
    }
    auto report = [&](Diag fn, Diag ctor) {
      if (ctor_of) diag(at, ctor, Types::unqualified(*ctor_of));
      else diag(at, fn, name);
    };
    if (viable.empty()) {
      report(Diag::no_viable_function, Diag::no_viable_constructor);
      return std::nullopt;
    }
    // F1 is better than F2 if none of its conversions is worse and one is better.
    auto better = [](const std::vector<Ics>& x, const std::vector<Ics>& y) {
      bool some = false;
      for (std::size_t k = 0; k < x.size(); ++k) {
        if (y[k] < x[k]) return false;
        some = some || x[k] < y[k];
      }
      return some;
    };
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < viable.size(); ++i) {
      bool wins = true;
      for (std::size_t j = 0; j < viable.size() && wins; ++j)
        if (i != j) wins = better(viable[i].second, viable[j].second);
      if (wins) best = i;
    }
    if (!best) {
      report(Diag::ambiguous_call, Diag::ambiguous_constructor);
      return std::nullopt;
    }
    std::size_t c = viable[*best].first;
    if (cands[c].deleted) {
      report(Diag::deleted_function, Diag::deleted_constructor);
      return std::nullopt;
    }
    return c;
  }

  // What a call to a function returning `r` is ([expr.call]): an lvalue for an lvalue reference,
  // an xvalue for an rvalue reference, else a prvalue.
  ExprInfo result_info(QualType r) {
    if (!types().is_reference(r)) return {Types::unqualified(r), ValueCategory::prvalue, 0};
    return {types().referee(r), types().rec(r).rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue, 0};
  }

  // ------------------------------------------------------------ constructors and references

  // Initializes a `t` by a constructor from `args` ([over.match.ctor], [dcl.init.general]/17.6):
  // the class's constructors and, where they are implicit, its default and copy constructors.
  // A trivial copy is a copy; `list` forbids narrowing ([over.match.list]); `copy_init` takes only
  // one-argument constructors.
  Ex construct(QualType t, std::vector<Ex> args, u32 begin, u32 end, bool list = false, bool copy_init = false) {
    Ex fail{NodeId::none, begin, end};
    if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return fail;
    if (!require_complete(t, begin)) return fail;
    QualType ut = Types::unqualified(t);
    EntityId c = types().class_of(ut);
    const ClassRec* cr = w_->classes.find(c);
    if (!cr) return fail;
    std::vector<Candidate> cands;
    std::vector<EntityId> ents;
    bool user_copy = false;
    for (const MemberDeclRec& m : cr->members)
      if (m.is_ctor) {
        TypeRec fr = types().rec(m.type);
        if (copy_init && fr.params.size() != 1) continue;
        cands.push_back({m.entity, m.type, true, m.is_deleted});
        user_copy = user_copy || is_copy_ctor(fr, ut);
      }
    QualType void_ = builtin(Builtin::void_);
    if (!user_copy) {
      QualType p = types().lvalue_ref(types().add_cv(ut, Cv::const_));
      cands.push_back({cr->copy_ctor, types().function(void_, std::span(&p, 1), false, LangLinkage::cxx), true, cr->copy_deleted});
    }
    if (!cr->user_ctors && !copy_init) cands.push_back({EntityId::none, types().function(void_, {}, false, LangLinkage::cxx), true, false});
    auto chosen = resolve(cands, args, begin, no_name(), std::nullopt, ut);
    if (!chosen) return fail;
    const Candidate& k = cands[*chosen];
    TypeRec fr = types().rec(k.type);
    if (k.entity == EntityId::none) {
      // Trivial: a copy of the argument's object, or nothing to construct.
      if (fr.params.empty()) return value_init_aggregate(ut, begin, end);
      Ex a = args[0];
      return a.glvalue() ? convert_node(a, ast::ConversionKind::lvalue_to_rvalue, ut) : a;
    }
    std::vector<Child> cs;
    for (std::size_t i = 0; i < args.size(); ++i) {
      Ex a = i < fr.params.size() ? convert(args[i], fr.params[i], args[i].begin) : variadic_argument(args[i]);
      if (!a.ok()) return fail;
      if (list && i < fr.params.size() && narrows(rvalue(args[i]), fr.params[i])) {
        diag(args[i].begin, Diag::narrowing, value_type(args[i]), Types::unqualified(fr.params[i]));
        return fail;
      }
      cs.push_back({a.id, a.begin >= begin ? a.begin - begin : 0});
    }
    ExprInfo info{ut, ValueCategory::prvalue, 0};
    return node(ast::CtorCall{info, k.entity, 0, b_->list(cs)}, begin, end, info);
  }
  // [class.copy.ctor]/2: a constructor whose only parameter is `C&` or `cv C&`.
  bool is_copy_ctor(const TypeRec& fr, QualType c) {
    return fr.params.size() == 1 && types().is_reference(fr.params[0]) && !types().rec(fr.params[0]).rvalue_ref &&
           same_unqualified(types().referee(fr.params[0]), c);
  }
  // Value-initialization of an aggregate (no constructor to call).
  Ex value_init_aggregate(QualType t, u32 begin, u32 end) {
    if (has_member_inits(t)) return member_inits(t, true, begin, end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    return node(ast::ValueInit{info}, begin, end, info);
  }

  // `T(args)` and `T{…}` for a class T ([expr.type.conv]): a prvalue.
  Ex functional_cast(QualType t, std::vector<Ex> args, u32 begin, u32 end) {
    if (args.empty()) return value_init(t, begin, end);
    if (!has_user_ctors(t) && args.size() == 1) return convert(args[0], t, begin);
    if (!has_user_ctors(t)) {
      diag(begin, Diag::no_viable_constructor, Types::unqualified(t));  // parenthesized aggregate init is not supported
      return {NodeId::none, begin, end};
    }
    return construct(t, std::move(args), begin, end);
  }

  // Temporary materialization ([conv.rval]) of the prvalue `v` as an object of type `t`.
  Ex materialize(Ex v, QualType t, bool extended) {
    if (!v.ok()) return v;
    ExprInfo info{t, ValueCategory::xvalue, 0};
    return node(ast::Materialize{info, destructor_of(t), extended ? 1u : 0u, at(v, v.begin)}, v.begin, v.end, info);
  }

  // Binds a reference of type `r` to `e` ([dcl.init.ref]): directly to an lvalue (or, for an
  // rvalue reference, an xvalue) of its type, no less qualified; a const lvalue reference or an
  // rvalue reference also to a temporary initialized from `e`, which lives as long as the
  // reference when `extend` ([class.temporary]/6).
  Ex bind_reference(Ex e, QualType r, u32 at_, bool extend = false) {
    if (!e.ok()) return e;
    if (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)) return rvalue(e);
    TypeRec rr = types().rec(r);
    QualType t = rr.inner;
    Ex fail{NodeId::none, e.begin, e.end};
    bool direct_cat = rr.rvalue_ref ? e.cat() == ValueCategory::xvalue : e.cat() == ValueCategory::lvalue;
    if (direct_cat && same_unqualified(e.type(), t)) {
      if (cv_within(Types::cv(e.type()), Types::cv(t))) return extend ? extended(e) : e;
      diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
      return fail;
    }
    bool const_ref = Types::is_const(t) && !Types::is_volatile(t);
    if (!rr.rvalue_ref && !const_ref) {
      if (e.cat() == ValueCategory::lvalue && same_unqualified(e.type(), t)) diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
      else diag(at_, Diag::bind_to_rvalue, t);
      return fail;
    }
    // [dcl.init.ref]/5.4.4: an rvalue reference does not bind to an lvalue.
    if (rr.rvalue_ref && e.cat() == ValueCategory::lvalue) {
      diag(at_, Diag::bind_to_lvalue, t);
      return fail;
    }
    if (e.glvalue() && same_unqualified(e.type(), t) && cv_within(Types::cv(e.type()), Types::cv(t)))
      return extend ? extended(e) : e;
    Ex v = convert(e, Types::unqualified(t), at_);
    if (!v.ok() || v.glvalue()) return v;
    return materialize(v, t, extend);
  }

  // A temporary a reference is bound to through its subobject (`const int& m = A().x`) lives as
  // long as the reference too ([class.temporary]/6): the `Materialize` under `.` and parentheses,
  // rebuilt extended.
  Ex extended(Ex e) {
    if (auto m = as<ast::Materialize>(e))
      return node(ast::Materialize{m->info, m->destructor, 1, m->operand}, e.begin, e.end, m->info);
    if (auto p = as<ast::Paren>(e)) {
      Ex inner = extended({p->operand.id, e.begin + p->operand.at, e.end, {}});
      return node(ast::Paren{p->info, at(inner, e.begin)}, e.begin, e.end, p->info);
    }
    if (auto m = as<ast::Member>(e); m && !m->arrow && !m->reference) {
      Ex inner = extended({m->object.id, e.begin + m->object.at, e.end, {}});
      return node(ast::Member{m->info, m->member, m->field, 0, 0, at(inner, e.begin)}, e.begin, e.end, m->info);
    }
    return e;
  }

  // ------------------------------------------------------------ names ([basic.lookup.unqual])

  // An id-expression naming `name`, at `begin`: the innermost block-scope declaration, else the
  // namespace-scope ones.
  Ex name_expr(NameId name, u32 begin, u32 end) {
    for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
      for (auto p = s->rbegin(); p != s->rend(); ++p)
        if (p->first == name) {
          const ast::LocalRec& l = locals_[p->second];
          ExprInfo info{types().referee(l.type), ValueCategory::lvalue, 0};  // [expr.type]: a reference names its object
          return node(ast::LocalRef{info, p->second}, begin, end, info);
        }
    // The class's members, in a member function's body ([basic.scope.class]).
    if (current_class != EntityId::none) {
      if (auto r = class_member(current_class, name, begin, end)) return *r;
      if (!w_->classes.is_defined(current_class))
        if (auto m = q_->earlier_member(name)) {
          if (m->kind != ast::DeclKind::variable) {
            diag(begin, Diag::member_without_object, name);
            return {NodeId::none, begin, end};
          }
          ExprInfo info{m->type, ValueCategory::lvalue, 0};
          return node(ast::DeclRef{info, m->entity}, begin, end, info);
        }
    }
    std::vector<ast::DeclSema> found;
    for (auto p = own.rbegin(); p != own.rend(); ++p)
      if (p->name == name) found.push_back(p->sema);
    bool any = !found.empty();
    if (found.empty())
      for (DeclId d : q_->visible_decls(name)) {
        any = true;
        ast::DeclSema s = q_->decl_sema(d);
        if (s.entity != EntityId::none) found.push_back(s);  // else its item diagnosed it
      }
    // A class hidden by a variable or function of the same name ([basic.scope.hiding]/2).
    if (std::ranges::any_of(found, [](const ast::DeclSema& d) { return d.kind != ast::DeclKind::class_; }))
      std::erase_if(found, [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::class_; });
    if (found.empty()) {
      if (!any) diag(begin, Diag::undeclared, name);
      return {NodeId::none, begin, end};
    }
    // Several functions: an overload set, resolved by the call ([over.match]); the latest
    // declaration of each gives its type.
    const ast::DeclSema& d = found.back();
    std::vector<ast::DeclSema> set;
    for (auto o = found.rbegin(); o != found.rend(); ++o)
      if (!std::ranges::any_of(set, [&](const ast::DeclSema& x) { return x.entity == o->entity; })) set.push_back(*o);
    if (set.size() > 1) {
      if (!std::ranges::all_of(set, [](const ast::DeclSema& x) { return x.kind == ast::DeclKind::function; })) {
        diag(begin, Diag::overload_not_supported, name);
        return {NodeId::none, begin, end};
      }
      ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
      Ex r = node(ast::DeclRef{info, d.entity}, begin, end, info);
      overloads_[std::to_underlying(r.id)] = std::move(set);
      return r;
    }
    if (d.kind == ast::DeclKind::function && (d.flags & decl_flag::consteval_))
      consteval_.insert(std::to_underlying(d.entity));
    if (d.kind == ast::DeclKind::function && (d.flags & decl_flag::deleted)) deleted_.insert(std::to_underlying(d.entity));
    if (d.kind == ast::DeclKind::function && ins().names.spelling(name) == "main") {
      diag(begin, Diag::main_named);  // [basic.start.main]/3: main shall not be used
      return {NodeId::none, begin, end};
    }
    ExprInfo info{types().referee(d.type), ValueCategory::lvalue, 0};
    return node(ast::DeclRef{info, d.entity}, begin, end, info);
  }

  // ------------------------------------------------------------ literals ([lex.literal])

  Ex literal(QualType t, ConstId v, ValueCategory cat, u32 begin, u32 end) {
    ExprInfo info{t, cat, 0};
    return node(ast::Literal{info, v}, begin, end, info);
  }

  // [lex.icon]: the first type in the literal's list that can represent its value.
  Ex integer_literal(const lex::Literal& lit, u32 begin, u32 end) {
    bool decimal = lit.base == 10;
    std::vector<Builtin> list;
    auto add = [&](std::initializer_list<Builtin> bs) { list.insert(list.end(), bs); };
    switch (lit.int_suffix) {
    case lex::IntSuffix::none:
      if (decimal) add({Builtin::int_, Builtin::long_, Builtin::long_long});
      else add({Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long, Builtin::long_long,
                Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::u: add({Builtin::unsigned_int, Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::l:
      if (decimal) add({Builtin::long_, Builtin::long_long});
      else add({Builtin::long_, Builtin::unsigned_long, Builtin::long_long, Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::ul: add({Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::ll:
      if (decimal) add({Builtin::long_long});
      else add({Builtin::long_long, Builtin::unsigned_long_long});
      break;
    case lex::IntSuffix::ull: add({Builtin::unsigned_long_long}); break;
    case lex::IntSuffix::z:
    case lex::IntSuffix::uz: add({lit.int_suffix == lex::IntSuffix::z ? Builtin::long_ : Builtin::unsigned_long}); break;
    }
    Builtin chosen = Builtin::long_long;
    bool fits = false;
    for (Builtin k : list) {
      IntShape s = shape(k);
      u64 max = s.is_signed ? (u64{1} << (s.bits - 1)) - 1 : (s.bits == 64 ? ~u64{0} : (u64{1} << s.bits) - 1);
      if (!lit.overflow && lit.value <= max) {
        chosen = k;
        fits = true;
        break;
      }
    }
    // A value no listed type holds was diagnosed by the lexer (integer_too_large), or is
    // ill-formed here; `long long` keeps analysis going.
    if (!fits) {
      if (!lit.overflow) diag(begin, Diag::integer_literal_too_large);  // [lex.icon]/3
      chosen = list.back();
    }
    QualType t = builtin(chosen);
    return literal(t, w_->values.integer(t, lit.value), ValueCategory::prvalue, begin, end);
  }

  // [lex.ccon]: an ordinary character literal has type char; a multicharacter one, int.
  Ex char_literal(const lex::Literal& l, u32 begin, u32 end) {
    using enum lex::Encoding;
    Builtin k = Builtin::char_;
    if (l.multichar) k = Builtin::int_;
    else if (l.encoding == utf8) k = Builtin::char8_t_;
    else if (l.encoding == utf16) k = Builtin::char16_t_;
    else if (l.encoding == utf32) k = Builtin::char32_t_;
    else if (l.encoding == wide) k = Builtin::wchar_t_;
    QualType t = builtin(k);
    return literal(t, w_->values.integer(t, l.value), ValueCategory::prvalue, begin, end);
  }

  // [lex.string]: an lvalue of type array of N const char, N counting the terminating null.
  Ex string_literal(const lex::Literal& l, u32 begin, u32 end) {
    if (l.encoding != lex::Encoding::ordinary && l.encoding != lex::Encoding::utf8) {
      diag(begin, Diag::not_supported, lex::Tk::string_lit);
      return {NodeId::none, begin, end};
    }
    std::string units;
    for (u32 u : l.units) units.push_back(static_cast<char>(u));
    units.push_back('\0');
    Builtin elem = l.encoding == lex::Encoding::utf8 ? Builtin::char8_t_ : Builtin::char_;
    QualType t = types().array(types().add_cv(builtin(elem), Cv::const_), units.size());
    return literal(t, w_->values.string(t, std::move(units)), ValueCategory::lvalue, begin, end);
  }

  Ex bool_literal(bool v, u32 begin, u32 end) {
    QualType t = builtin(Builtin::bool_);
    return literal(t, w_->values.integer(t, v), ValueCategory::prvalue, begin, end);
  }

  Ex nullptr_literal(u32 begin, u32 end) {
    QualType t = builtin(Builtin::nullptr_t);
    return literal(t, w_->values.null_pointer(t), ValueCategory::prvalue, begin, end);
  }

  // The value of a static data member of integral type usable in constant expressions.
  std::optional<ConstId> static_constant(const Ex& e) {
    auto d = as<ast::DeclRef>(e);
    if (!d || !types().is_integral(e.type()) || Types::is_volatile(e.type()) ||
        w_->entities.rec(d->entity).scope == EntityId::none)
      return std::nullopt;
    return q_->constant_value(d->entity);
  }

  // An expression statement's expression, a discarded-value expression ([expr.context]).
  Ex discarded(Ex e) {
    if (is_bound_member(e)) {
      diag(e.begin, Diag::bound_member_not_called, w_->entities.rec(as<ast::Member>(e)->member).name);
      return {NodeId::none, e.begin, e.end};
    }
    return e;
  }

  Ex paren(Ex e, u32 begin, u32 end) {
    if (!e.ok()) return {NodeId::none, begin, end};
    // A parenthesized member function name is that name ([expr.prim.paren]): only a call may follow.
    if (is_bound_member(e)) return {e.id, begin, end, e.info};
    return node(ast::Paren{e.info, at(e, begin)}, begin, end, e.info);
  }

  // ------------------------------------------------------------ conversions ([conv])

  Ex convert_node(Ex e, ast::ConversionKind k, QualType to, ValueCategory cat = ValueCategory::prvalue) {
    ExprInfo info{to, cat, 0};
    return node(ast::ImplicitConversion{info, k, EntityId::none, at(e, e.begin)}, e.begin, e.end, info);
  }

  // The conversions every operand that needs a prvalue gets ([basic.lval], [conv.lval],
  // [conv.array], [conv.func]).
  Ex rvalue(Ex e) {
    if (e.ok() && types().is(e.type(), Builtin::overload_set)) {
      diag(e.begin, Diag::overload_address, w_->entities.rec(as<ast::DeclRef>(e)->entity).name);
      return {NodeId::none, e.begin, e.end};
    }
    if (is_bound_member(e)) {
      diag(e.begin, Diag::bound_member_not_called, w_->entities.rec(as<ast::Member>(e)->member).name);
      return {NodeId::none, e.begin, e.end};
    }
    if (!e.ok() || !e.glvalue()) return e;
    QualType t = e.type();
    // A static data member usable in constant expressions is read as its value, which is no
    // odr-use ([basic.def.odr]), so it needs no definition; through an object (`a.x`, the comma
    // `member` built), the object is still evaluated.
    if (auto v = static_constant(e)) return literal(Types::unqualified(t), *v, ValueCategory::prvalue, e.begin, e.end);
    if (auto c = as<ast::OrderedBinary>(e); c && c->op == ast::OrderedOp::comma && types().is_integral(t)) {
      Ex rhs{c->rhs.id, e.begin + c->rhs.at, e.end, c->info};
      if (auto v = static_constant(rhs)) {
        Ex lit = literal(Types::unqualified(t), *v, ValueCategory::prvalue, rhs.begin, rhs.end);
        ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
        return node(ast::OrderedBinary{info, ast::OrderedOp::comma, c->lhs, at(lit, e.begin)}, e.begin, e.end, info);
      }
    }
    if (types().is_array(t)) {
      QualType p = types().pointer(types().rec(t).inner);
      return convert_node(e, ast::ConversionKind::array_to_pointer, p);
    }
    if (types().is_function(t)) return convert_node(e, ast::ConversionKind::function_to_pointer, types().pointer(t));
    return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, Types::unqualified(t));
  }

  // [conv.prom], after the lvalue conversions.
  Ex promote(Ex e) {
    e = rvalue(e);
    if (!e.ok()) return e;
    auto b = types().builtin_of(e.type());
    if (!b || !is_integral(*b)) return e;
    Builtin p = promoted(*b);
    if (p == *b) return e;
    return convert_node(e, ast::ConversionKind::integral, builtin(p));
  }

  // [conv.ptr]: an integer literal with value zero, or a prvalue of type std::nullptr_t.
  bool is_null_pointer_constant(const Ex& e) {
    if (!e.ok() || e.glvalue()) return false;
    if (types().is(e.type(), Builtin::nullptr_t)) return true;
    NodeId n = e.id;
    while (ast::kind_of(n) == ast::Nodes::index_of<ast::Paren>()) n = b_->get<ast::Paren>(n).operand.id;
    if (ast::kind_of(n) != ast::Nodes::index_of<ast::Literal>()) return false;
    // An integer-literal's type is a signed or unsigned integer type of rank int or above
    // ([lex.icon]); a character or bool literal is not an integer-literal.
    auto b = types().builtin_of(e.type());
    if (!b || !ast::is_integral(*b) || *b == Builtin::bool_ || rank_below_int(*b) || is_character(*b)) return false;
    return w_->values.rec(b_->get<ast::Literal>(n).value).bits == 0;
  }
  static bool is_character(Builtin b) {
    return b == Builtin::char_ || b == Builtin::wchar_t_ || b == Builtin::char8_t_ || b == Builtin::char16_t_ ||
           b == Builtin::char32_t_;
  }
  static bool rank_below_int(Builtin b) {
    return b == Builtin::signed_char || b == Builtin::unsigned_char || b == Builtin::short_ ||
           b == Builtin::unsigned_short;
  }

  // An implicit conversion sequence to `to` ([over.best.ics], [dcl.init.general]): copy-
  // initialization of an object of type `to` from `e`. Standard conversions only.
  Ex convert(Ex e, QualType to, u32 at_) {
    if (!e.ok()) return e;
    QualType target = Types::unqualified(to);
    // A class object is copy-initialized from an expression of its own class: a glvalue through
    // the (trivial, implicit) copy constructor, a prvalue directly ([dcl.init.general]/17.6).
    if (types().is_reference(to)) return bind_reference(e, to, at_);
    if (types().is_class(target)) {
      if (!require_complete(target, at_)) return {NodeId::none, e.begin, e.end};
      if (initializes(target, e) && !e.glvalue()) return e;  // the prvalue initializes it directly
      const ClassRec* cr = w_->classes.find(types().class_of(target));
      // A copy through a constructor that is not trivial, or one converting another type
      // ([class.conv.ctor]).
      if (cr && (cr->user_ctors || cr->copy_ctor != EntityId::none || cr->copy_deleted))
        return construct(target, {e}, e.begin, e.end, false, true);
      if (!initializes(target, e)) {
        diag(at_, Diag::no_conversion, e.type(), to);
        return {NodeId::none, e.begin, e.end};
      }
      return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, target);
    }
    if (types().is_pointer(target) && is_null_pointer_constant(e))
      return convert_node(e, ast::ConversionKind::null_pointer, target);
    e = rvalue(e);
    QualType from = e.type();
    if (types().is_void(from)) {
      diag(at_, Diag::void_value);
      return {NodeId::none, e.begin, e.end};
    }
    if (from == target) return e;
    if (types().is_arithmetic(from) && types().is_arithmetic(target)) {
      if (types().is(target, Builtin::bool_)) return convert_node(e, ast::ConversionKind::boolean, target);
      return convert_node(e, ast::ConversionKind::integral, target);
    }
    if (types().is(target, Builtin::bool_) && (types().is_pointer(from) || types().is(from, Builtin::nullptr_t))) {
      if (types().is_pointer(from)) return convert_node(e, ast::ConversionKind::boolean, target);
    }
    if (types().is_pointer(from) && types().is_pointer(target) && qualification_convertible(from, target))
      return convert_node(e, ast::ConversionKind::qualification, target);
    if (types().is_pointer(from) && types().is_pointer(target) && pointer_convertible(from, target))
      return convert_node(e, ast::ConversionKind::pointer, target);
    diag(at_, Diag::no_conversion, from, to);
    return {NodeId::none, e.begin, e.end};
  }

  // [conv.ptr]/2: a pointer to an object type converts to a pointer to `void` at least as
  // cv-qualified.
  bool pointer_convertible(QualType from, QualType to) {
    QualType a = types().pointee(from), b = types().pointee(to);
    return types().is_void(b) && types().is_object(a) && cv_within(Types::cv(a), Types::cv(b));
  }

  // [conv.qual], for one level of pointers: `T cv1*` to `T cv2*` where cv2 holds cv1.
  bool qualification_convertible(QualType from, QualType to) {
    QualType a = types().pointee(from), b = types().pointee(to);
    if (Types::unqualified(a) != Types::unqualified(b)) return false;
    auto ca = std::to_underlying(Types::cv(a)), cb = std::to_underlying(Types::cv(b));
    return (ca & cb) == ca;
  }

  // Contextual conversion to bool ([conv.general], [conv.bool]).
  Ex to_bool(Ex e, u32 at_) {
    e = rvalue(e);
    if (!e.ok()) return e;
    QualType t = e.type();
    if (types().is(t, Builtin::bool_)) return e;
    if (types().is_arithmetic(t) || types().is_pointer(t) || types().is(t, Builtin::nullptr_t))
      return convert_node(e, ast::ConversionKind::boolean, builtin(Builtin::bool_));
    diag(at_, Diag::not_contextually_bool, t);
    return {NodeId::none, e.begin, e.end};
  }

  // [expr.arith.conv] for integral operands: both are promoted and converted to the common type.
  QualType arithmetic_conversions(Ex& a, Ex& c) {
    a = promote(a);
    c = promote(c);
    Builtin k = common_integer(*types().builtin_of(a.type()), *types().builtin_of(c.type()));
    QualType t = builtin(k);
    if (a.type() != t) a = convert_node(a, ast::ConversionKind::integral, t);
    if (c.type() != t) c = convert_node(c, ast::ConversionKind::integral, t);
    return t;
  }

  // ------------------------------------------------------------ operators ([expr.unary], [expr.compound])

  bool is_integral_or_unscoped(QualType t) { return types().is_integral(t); }
  // A pointer to a completely-defined object type, which [expr.add] and [expr.pre.incr] need.
  bool is_object_pointer(QualType t) {
    return types().is_pointer(t) && types().is_object(types().pointee(t)) && is_complete(types().pointee(t));
  }

  // [class.copy.assign]/7: the implicit copy assignment of a class with a const member (or one
  // whose own is deleted) is deleted.
  bool has_const_member(QualType t) {
    TypeRec r = types().rec(t);
    if (r.kind == TypeKind::array) return has_const_member(r.inner);
    if (r.kind != TypeKind::class_) return false;
    if (const ClassRec* c = w_->classes.find(r.entity))
      for (const FieldRec& f : c->fields)
        if (Types::is_const(types().canonical(f.type)) || has_const_member(f.type)) return true;
    return false;
  }
  bool check_class_assignable(const Ex& l, u32 at_) {
    if (!types().is_class(l.type()) || !has_const_member(l.type())) return true;
    diag(at_, Diag::const_member_assign, Types::unqualified(l.type()));
    return false;
  }

  // A modifiable lvalue ([basic.lval]): an lvalue of a non-const object type that is not an array.
  bool check_modifiable(const Ex& e, u32 at_) {
    if (!e.glvalue() || types().is_function(e.type()) || types().is_array(e.type())) {
      diag(at_, Diag::not_assignable);
      return false;
    }
    if (Types::is_const(e.type())) {
      diag(at_, Diag::assign_to_const, e.type());
      return false;
    }
    return true;
  }

  Ex unary(ast::UnaryOp op, Ex e, u32 begin, u32 end, u32 op_at) {
    using enum ast::UnaryOp;
    if (!e.ok()) return {NodeId::none, begin, end};
    auto make = [&](Ex operand, QualType t, ValueCategory cat) {
      ExprInfo info{t, cat, 0};
      return node(ast::Unary{info, op, at(operand, begin)}, begin, end, info);
    };
    switch (op) {
    case plus:
    case minus:
    case compl_: {
      Ex o = promote(e);
      QualType t = o.type();
      bool ok = op == compl_ ? types().is_integral(t) : types().is_arithmetic(t) || (op == plus && types().is_pointer(t));
      if (!ok) {
        diag(op_at, Diag::invalid_operand, e.type());
        return {NodeId::none, begin, end};
      }
      return make(o, t, ValueCategory::prvalue);
    }
    case not_: {
      Ex o = to_bool(e, e.begin);
      if (!o.ok()) return {NodeId::none, begin, end};
      return make(o, builtin(Builtin::bool_), ValueCategory::prvalue);
    }
    case deref: {
      Ex o = rvalue(e);
      if (!types().is_pointer(o.type()) || types().is_void(types().pointee(o.type()))) {
        diag(op_at, Diag::indirection_not_pointer, e.type());
        return {NodeId::none, begin, end};
      }
      return make(o, types().pointee(o.type()), ValueCategory::lvalue);
    }
    case address_of:
      if (!e.glvalue()) {
        diag(op_at, Diag::not_lvalue_address, e.type());
        return {NodeId::none, begin, end};
      }
      return make(e, types().pointer(e.type()), ValueCategory::prvalue);
    case pre_inc:
    case pre_dec:
    case post_inc:
    case post_dec: {
      // [expr.pre.incr], [expr.post.incr]: a modifiable lvalue of arithmetic type other than
      // bool, or a pointer to a complete object type.
      if (!check_modifiable(e, e.begin)) return {NodeId::none, begin, end};
      QualType t = e.type();
      if (types().is(t, Builtin::bool_)) {
        diag(op_at, Diag::increment_bool);
        return {NodeId::none, begin, end};
      }
      if (!types().is_arithmetic(t) && !is_object_pointer(t)) {
        diag(op_at, Diag::invalid_operand, t);
        return {NodeId::none, begin, end};
      }
      bool pre = op == pre_inc || op == pre_dec;
      return make(e, pre ? t : Types::unqualified(t), pre ? ValueCategory::lvalue : ValueCategory::prvalue);
    }
    }
    std::unreachable();
  }

  enum class BinOp : u8 {
    mul, div, rem, add, sub, shl, shr, lt, gt, le, ge, eq, ne, bit_and, bit_xor, bit_or, logical_and, logical_or, comma,
  };

  Ex binary(BinOp op, Ex l, Ex r, u32 op_at) {
    using enum BinOp;
    u32 begin = l.begin, end = r.end;
    if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
    auto bin = [&](ast::BinaryOp k, Ex a, Ex c, QualType t) {
      ExprInfo info{t, ValueCategory::prvalue, 0};
      return node(ast::Binary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
    };
    auto ordered = [&](ast::OrderedOp k, Ex a, Ex c, ExprInfo info) {
      return node(ast::OrderedBinary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
    };
    auto invalid = [&] {
      diag(op_at, Diag::invalid_operands, l.type(), r.type());
      return Ex{NodeId::none, begin, end};
    };
    switch (op) {
    case logical_and:
    case logical_or: {
      Ex a = to_bool(l, l.begin), c = to_bool(r, r.begin);
      if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
      return ordered(op == logical_and ? ast::OrderedOp::logical_and : ast::OrderedOp::logical_or, a, c,
                     {builtin(Builtin::bool_), ValueCategory::prvalue, 0});
    }
    case comma: return ordered(ast::OrderedOp::comma, l, r, r.info);
    case shl:
    case shr: {
      Ex a = promote(l), c = promote(r);
      if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
      if (!types().is_integral(a.type()) || !types().is_integral(c.type())) return invalid();
      return ordered(op == shl ? ast::OrderedOp::shl : ast::OrderedOp::shr, a, c, {a.type(), ValueCategory::prvalue, 0});
    }
    default: break;
    }
    Ex a = rvalue(l), c = rvalue(r);
    if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
    QualType ta = a.type(), tc = c.type();
    bool arith = types().is_arithmetic(ta) && types().is_arithmetic(tc);
    bool integral = types().is_integral(ta) && types().is_integral(tc);
    static constexpr std::array<ast::BinaryOp, 19> kinds{
        ast::BinaryOp::mul, ast::BinaryOp::div, ast::BinaryOp::rem, ast::BinaryOp::add, ast::BinaryOp::sub,
        ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::lt, ast::BinaryOp::gt, ast::BinaryOp::le,
        ast::BinaryOp::ge, ast::BinaryOp::eq, ast::BinaryOp::ne, ast::BinaryOp::bit_and, ast::BinaryOp::bit_xor,
        ast::BinaryOp::bit_or, ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::mul};
    ast::BinaryOp k = kinds[std::to_underlying(op)];
    switch (op) {
    case mul:
    case div:
      if (!arith) return invalid();
      return bin(k, a, c, arithmetic_conversions(a, c));
    case rem:
    case bit_and:
    case bit_xor:
    case bit_or:
      if (!integral) return invalid();
      return bin(k, a, c, arithmetic_conversions(a, c));
    case add:
    case sub:
      if (arith) return bin(k, a, c, arithmetic_conversions(a, c));
      // [expr.add]: a pointer to a complete object type and an integer; or, for `-`, two
      // pointers to the same type, giving std::ptrdiff_t (long).
      if (is_object_pointer(ta) && types().is_integral(tc)) return bin(k, a, promote(c), ta);
      if (op == add && types().is_integral(ta) && is_object_pointer(tc)) return bin(k, promote(a), c, tc);
      if (op == sub && is_object_pointer(ta) && is_object_pointer(tc) &&
          Types::unqualified(types().pointee(ta)) == Types::unqualified(types().pointee(tc)))
        return bin(k, a, c, builtin(Builtin::long_));
      if (types().is_pointer(ta) && types().is_integral(tc)) {
        diag(op_at, Diag::pointer_arithmetic_void, ta);
        return {NodeId::none, begin, end};
      }
      return invalid();
    case lt:
    case gt:
    case le:
    case ge:
    case eq:
    case ne: {
      QualType b = builtin(Builtin::bool_);
      if (arith) {
        arithmetic_conversions(a, c);
        return bin(k, a, c, b);
      }
      // [expr.rel], [expr.eq]: pointers converted to their composite pointer type; for the
      // equality operators also a pointer and a null pointer constant.
      bool equality = op == eq || op == ne;
      if (types().is_pointer(ta) && types().is_pointer(tc)) {
        QualType pa = types().pointee(ta), pc = types().pointee(tc);
        if (Types::unqualified(pa) != Types::unqualified(pc)) return invalid();
        QualType composite = types().pointer(
            types().add_cv(Types::unqualified(pa), Cv(std::to_underlying(Types::cv(pa)) | std::to_underlying(Types::cv(pc)))));
        if (ta != composite) a = convert_node(a, ast::ConversionKind::qualification, composite);
        if (tc != composite) c = convert_node(c, ast::ConversionKind::qualification, composite);
        return bin(k, a, c, b);
      }
      if (equality && types().is(ta, Builtin::nullptr_t) && types().is(tc, Builtin::nullptr_t)) return bin(k, a, c, b);
      if (equality && types().is_pointer(ta) && is_null_pointer_constant(r))
        return bin(k, a, convert_node(c, ast::ConversionKind::null_pointer, ta), b);
      if (equality && types().is_pointer(tc) && is_null_pointer_constant(l))
        return bin(k, convert_node(a, ast::ConversionKind::null_pointer, tc), c, b);
      return invalid();
    }
    default: break;
    }
    std::unreachable();
  }

  enum class AsgOp : u8 { assign, mul, div, rem, add, sub, shl, shr, bit_and, bit_xor, bit_or };

  // [expr.assign]
  Ex assign(AsgOp op, Ex l, Ex r, u32 op_at) {
    u32 begin = l.begin, end = r.end;
    if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
    if (!check_modifiable(l, op_at) || !check_class_assignable(l, op_at)) return {NodeId::none, begin, end};
    QualType lt = l.type();
    QualType computation = Types::unqualified(lt);
    Ex c = r;
    if (op == AsgOp::assign) {
      c = convert(r, lt, r.begin);
    } else {
      Ex lv = rvalue(l);  // only to read its type; the IR loads the left operand itself
      QualType tl = lv.type();
      Ex rv = rvalue(r);
      if (!rv.ok()) return {NodeId::none, begin, end};
      QualType tr = rv.type();
      bool shift = op == AsgOp::shl || op == AsgOp::shr;
      bool integral_only = shift || op == AsgOp::rem || op == AsgOp::bit_and || op == AsgOp::bit_xor || op == AsgOp::bit_or;
      if (types().is_arithmetic(tl) && types().is_arithmetic(tr) &&
          (!integral_only || (types().is_integral(tl) && types().is_integral(tr)))) {
        if (shift) {
          computation = builtin(promoted(*types().builtin_of(tl)));
          c = promote(rv);
        } else {
          Builtin common = common_integer(promoted(*types().builtin_of(tl)), promoted(*types().builtin_of(tr)));
          computation = builtin(common);
          c = convert(rv, computation, r.begin);
        }
      } else if ((op == AsgOp::add || op == AsgOp::sub) && is_object_pointer(tl) && types().is_integral(tr)) {
        computation = tl;
        c = promote(rv);
      } else {
        diag(op_at, Diag::invalid_operands, lt, r.type());
        return {NodeId::none, begin, end};
      }
    }
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{lt, ValueCategory::lvalue, 0};
    return node(ast::Assign{info, ast::AssignOp(std::to_underlying(op)), at(l, begin), at(c, begin), computation}, begin,
                end, info);
  }

  // [expr.assign]/9: `x = {…}` assigns `T{…}`, list-initialized, to an object of type T.
  Ex assign_list(Ex l, const Braced& b, u32 op_at) {
    u32 begin = l.begin, end = b.end;
    if (!l.ok() || !check_modifiable(l, op_at) || !check_class_assignable(l, op_at)) return {NodeId::none, begin, end};
    Ex c = list_init(Types::unqualified(l.type()), b);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info{l.type(), ValueCategory::lvalue, 0};
    return node(ast::Assign{info, ast::AssignOp::assign, at(l, begin), at(c, begin), Types::unqualified(l.type())},
                begin, end, info);
  }

  // [expr.cond]
  Ex conditional(Ex cond, Ex t, Ex f, u32 q_at) {
    u32 begin = cond.begin, end = f.end;
    if (!cond.ok() || !t.ok() || !f.ok()) return {NodeId::none, begin, end};
    Ex c = to_bool(cond, cond.begin);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info;
    if (t.glvalue() && t.cat() == f.cat() && t.type() == f.type()) {
      info = t.info;
    } else {
      Ex a = rvalue(t), b = rvalue(f);
      QualType ta = a.type(), tb = b.type();
      if (ta == tb) {
        info = {ta, ValueCategory::prvalue, 0};  // [expr.cond]/7.1: the same type, no conversion
      } else if (types().is_void(ta) && types().is_void(tb)) {
        info = {ta, ValueCategory::prvalue, 0};
      } else if (types().is_arithmetic(ta) && types().is_arithmetic(tb)) {
        info = {arithmetic_conversions(a, b), ValueCategory::prvalue, 0};
      } else if (types().is_pointer(ta) && (tb == ta || is_null_pointer_constant(f))) {
        if (tb != ta) b = convert_node(b, ast::ConversionKind::null_pointer, ta);
        info = {ta, ValueCategory::prvalue, 0};
      } else if (types().is_pointer(tb) && is_null_pointer_constant(t)) {
        a = convert_node(a, ast::ConversionKind::null_pointer, tb);
        info = {tb, ValueCategory::prvalue, 0};
      } else if (types().is_pointer(ta) && types().is_pointer(tb) &&
                 (qualification_convertible(ta, tb) || qualification_convertible(tb, ta))) {
        QualType to = qualification_convertible(ta, tb) ? tb : ta;
        if (ta != to) a = convert_node(a, ast::ConversionKind::qualification, to);
        if (tb != to) b = convert_node(b, ast::ConversionKind::qualification, to);
        info = {to, ValueCategory::prvalue, 0};
      } else {
        diag(q_at, Diag::incompatible_operands, t.type(), f.type());
        return {NodeId::none, begin, end};
      }
      t = a;
      f = b;
    }
    return node(ast::Conditional{info, at(c, begin), at(t, begin), at(f, begin)}, begin, end, info);
  }

  // [expr.call]: a call to a named function or through a pointer to function. Each argument
  // copy-initializes its parameter; arguments matching a C-style ellipsis get the default
  // argument promotions.
  Ex call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at) {
    u32 begin = callee.begin;
    if (!callee.ok() || std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
    if (is_bound_member(callee)) return member_call(callee, std::move(args), end, paren_at);
    if (types().is(callee.type(), Builtin::overload_set)) {
      const std::vector<ast::DeclSema>& set = overloads_.at(std::to_underlying(callee.id));
      std::vector<Candidate> cands;
      for (const ast::DeclSema& d : set)
        cands.push_back({d.entity, d.type, true, (d.flags & decl_flag::deleted) != 0});
      auto chosen = resolve(cands, args, callee.begin, w_->entities.rec(set.front().entity).name, std::nullopt);
      if (!chosen) return {NodeId::none, begin, end};
      const ast::DeclSema& d = set[*chosen];
      if (d.flags & decl_flag::consteval_) consteval_.insert(std::to_underlying(d.entity));
      ExprInfo ci{d.type, ValueCategory::lvalue, 0};
      callee = node(ast::DeclRef{ci, d.entity}, callee.begin, callee.end, ci);
    } else if (auto d = as<ast::DeclRef>(callee); d && deleted_.contains(std::to_underlying(d->entity))) {
      // One function: still not callable when deleted ([dcl.fct.def.delete]).
      diag(callee.begin, Diag::deleted_function, w_->entities.rec(d->entity).name);
      return {NodeId::none, begin, end};
    }
    EntityId function = EntityId::none;
    QualType ft = callee.type();
    if (auto d = as<ast::DeclRef>(callee); d && types().is_function(ft)) {
      function = d->entity;
    } else {
      callee = rvalue(callee);
      ft = callee.type();
      if (types().is_pointer(ft) && types().is_function(types().pointee(ft))) ft = types().pointee(ft);
    }
    if (!types().is_function(ft)) {
      diag(paren_at, Diag::not_callable, callee.type());
      return {NodeId::none, begin, end};
    }
    TypeRec fr = types().rec(ft);
    auto children = call_args(fr, args, begin, paren_at);
    if (!children) return {NodeId::none, begin, end};
    ExprInfo info = result_info(fr.inner);
    return node(ast::Call{info, function, at(callee, begin), b_->list(*children)}, begin, end, info);
  }

  // The arguments of a call to a function of type `fr`, each copy-initializing its parameter.
  std::optional<std::vector<Child>> call_args(const TypeRec& fr, std::span<const Ex> args, u32 begin, u32 paren_at) {
    std::vector<QualType> params = fr.params;  // copied: converting may intern new types
    if (args.size() < params.size()) {
      diag(paren_at, Diag::too_few_arguments, u64(params.size()), u64(args.size()));
      return std::nullopt;
    }
    if (args.size() > params.size() && !fr.variadic) {
      diag(args[params.size()].begin, Diag::too_many_arguments, u64(params.size()), u64(args.size()));
      return std::nullopt;
    }
    std::vector<Child> children;
    for (std::size_t i = 0; i < args.size(); ++i) {
      Ex a = i < params.size() ? convert(args[i], params[i], args[i].begin) : variadic_argument(args[i]);
      if (!a.ok()) return std::nullopt;
      children.push_back(at(a, begin));
    }
    return children;
  }

  // [expr.call]/12: an argument matched by the ellipsis gets the lvalue conversions and the
  // integral promotions; std::nullptr_t becomes void*; void is no value.
  Ex variadic_argument(Ex e) {
    e = promote(e);
    if (!e.ok()) return e;
    if (types().is_void(e.type())) {
      diag(e.begin, Diag::void_value);
      return {NodeId::none, e.begin, e.end};
    }
    if (types().is(e.type(), Builtin::nullptr_t))
      return convert_node(e, ast::ConversionKind::null_pointer, types().pointer(builtin(Builtin::void_)));
    return e;
  }

  // [expr.sub]: `E1[E2]` is `*(E1 + E2)`. The α subset takes the pointer on the left only.
  Ex subscript(Ex base, Ex index, u32 end, u32 at_) {
    u32 begin = base.begin;
    if (!base.ok() || !index.ok()) return {NodeId::none, begin, end};
    Ex a = rvalue(base), c = rvalue(index);
    if (!is_object_pointer(a.type()) || !types().is_integral(c.type())) {
      diag(at_, Diag::invalid_operands, base.type(), index.type());
      return {NodeId::none, begin, end};
    }
    ExprInfo info{types().pointee(a.type()), ValueCategory::lvalue, 0};
    return node(ast::Subscript{info, at(a, begin), at(promote(c), begin)}, begin, end, info);
  }

private:
  World* w_;
  ast::Builder* b_;
  Questions* q_;
  std::vector<engine::Diagnostic>* diags_;
  ast::ItemId item_;
  u32 loc_base_;
  std::vector<ast::LocalRec> locals_;
  std::vector<std::optional<ConstId>> local_values_;  // by local
  std::set<u32> consteval_;                           // consteval functions named so far
  std::map<u32, std::vector<Ex>> list_elems_;         // by draft InitList
  struct Bound {
    std::optional<Ex> object;
    std::vector<const MemberDeclRec*> cands;
  };
  std::map<u32, Bound> bound_;                        // by draft bound-member Member
  std::set<u32> deleted_;                             // deleted functions named so far
  std::map<u32, std::vector<ast::DeclSema>> overloads_;  // by draft overload-set DeclRef
  std::vector<std::vector<std::pair<NameId, u32>>> scopes_;
  std::vector<std::size_t> guarded_;
  QualType return_type_{};
};

}  // namespace ycxx::sema
