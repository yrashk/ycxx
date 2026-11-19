// Sema's view of types, constants and entities. Types and constants are #33's (ycxx.ast: `types`,
// `values`, ycxx.ast.typeops) and this partition only shapes them for sema: by-value records of
// the canonical form, and the target fixed to AArch64 Darwin. Entities are still a STAND-IN for
// #33: a side table behind the AST core's fingerprint-only `entities` placeholder, isolated here.
export module ycxx.sema:types;

import std;
import ycxx.ast;
import ycxx.ast.store;
import ycxx.lex;

export namespace ycxx::sema {

using ast::Builtin;
using ast::ConstId;
using ast::Cv;
using ast::DeclId;
using ast::EntityId;
using ast::Fp128;
using ast::LangLinkage;
using ast::NameId;
using ast::QualType;
using ast::u16;
using ast::u32;
using ast::u64;
using ast::u8;
using i64 = std::int64_t;

// The target of the α slice (#39).
inline constexpr const ast::Target& target = ast::aarch64_darwin;

// Width in bits and signedness of an integral type on the target.
struct IntShape {
  u32 bits;
  bool is_signed;
};
inline IntShape shape(Builtin b) { return {ast::width_of(target, b), ast::is_signed(target, b)}; }

// [conv.prom] and [expr.arith.conv] on the target.
inline Builtin promoted(Builtin b) { return ast::is_integral(b) ? ast::promoted(target, b) : b; }
inline Builtin common_integer(Builtin a, Builtin b) { return *ast::usual_arithmetic(target, a, b); }

constexpr std::string_view spelling(Builtin b) {
  constexpr std::array<std::string_view, 21> names{
      "void", "std::nullptr_t", "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t",
      "char32_t", "short", "unsigned short", "int", "unsigned int", "long", "unsigned long", "long long",
      "unsigned long long", "float", "double", "long double"};
  auto i = std::to_underlying(b);
  return i < names.size() ? names[i] : "<builtin>";
}

// ---------------------------------------------------------------- types

enum class TypeKind : u8 {
  builtin, pointer, array, function, class_, reference, enum_, template_param, dep_name, pack_expansion, transform,
  placeholder, dep_spec, builtin_spec, other
};

// The canonical form of a type, read out by value.
struct TypeRec {
  TypeKind kind = TypeKind::other;
  Builtin builtin = Builtin::void_;
  QualType inner{};  // pointee, element or return type
  u64 bound = 0;     // an array's
  std::vector<QualType> params;
  bool variadic = false;
  LangLinkage linkage = LangLinkage::cxx;
  EntityId entity = EntityId::none;  // a class's or an enumeration's
  Cv fn_cv = Cv::none;               // a member function type's cv-qualifier-seq
  bool rvalue_ref = false;           // a reference's kind; `inner` is what it refers to
  ConstId bound_value = ConstId::none;  // an array's bound as a constant, possibly dependent
  NameId name{};                     // a dependent member type's name; `inner` is its qualifier
  u16 depth = 0;                     // a template parameter's position ([temp.param])
  u16 index = 0;
  bool pack = false;
  u8 trait = 0;  // a transform's `lex::TypeTrait`; a built-in specialization's `ast::BuiltinTemplate`
  ast::SeqId<ast::TemplateArg> args{};  // a dependent or built-in specialization's arguments
  bool decltype_auto = false;  // a placeholder's: `decltype(auto)`, not `auto`
};

class Types {
public:
  explicit Types(ast::Interners& ins) : ins_(&ins) {}

  QualType builtin(Builtin b) { return ast::builtin(*ins_, b); }
  QualType pointer(QualType pointee) { return ast::pointer_to(*ins_, pointee); }
  // An array whose bound is value-dependent ([temp.dep.constexpr]).
  QualType array_dependent(QualType elem, ConstId bound) { return ast::array_of(*ins_, target, elem, bound); }
  // An array of unknown bound, an incomplete type ([dcl.array]/6).
  QualType array_unknown(QualType elem) { return ast::array_of(*ins_, target, elem, ConstId::none); }
  QualType array(QualType elem, u64 bound) {
    return ast::array_of(*ins_, target, elem, ast::int_value(*ins_, target, Builtin::unsigned_long, i64(bound)));
  }
  // The parameters are adjusted by #33's `function_type` ([dcl.fct]).
  QualType function(QualType ret, std::span<const QualType> params, bool variadic, LangLinkage linkage,
                    Cv cv = Cv::none) {
    return ast::function_type(*ins_, ret, params, {.variadic = variadic, .cv = cv, .linkage = linkage});
  }
  QualType class_type(EntityId c) { return ins_->intern_type(ast::TClass{c}); }
  QualType enum_type(EntityId e) { return ins_->intern_type(ast::TEnum{e}); }
  QualType lvalue_ref(QualType t) { return ast::lvalue_ref_to(*ins_, t); }
  QualType rvalue_ref(QualType t) { return ast::rvalue_ref_to(*ins_, t); }
  QualType template_param(u16 depth, u16 index, bool pack = false) {
    return ins_->intern_type(ast::TTemplateParam{depth, index, u8(pack)});
  }
  // `pattern...` ([temp.variadic]).
  QualType pack_expansion(QualType pattern) { return ins_->intern_type(ast::TPackExpansion{pattern}); }
  // `__trait(arg)` of a dependent `arg` (docs/EXTENSIONS.md).
  QualType transform(u8 trait, QualType arg) { return ins_->intern_type(ast::TTransform{trait, arg}); }
  // `auto` or `decltype(auto)` ([dcl.spec.auto]).
  QualType placeholder(bool decltype_auto) { return ins_->intern_type(ast::TAuto{u8(decltype_auto)}); }
  // Whether a declared type holds a placeholder still to be deduced ([dcl.type.auto.deduct]): as
  // the type, or as what it points or refers to, an array's element or a function's return type.
  bool has_placeholder(QualType q) const {
    TypeRec r = rec(q);
    switch (r.kind) {
    case TypeKind::placeholder: return true;
    case TypeKind::pointer:
    case TypeKind::reference:
    case TypeKind::array:
    case TypeKind::function: return has_placeholder(r.inner);
    default: return false;
    }
  }
  // `TT<args>` of a template template parameter, and `__make_integer_seq<args>` while dependent.
  QualType dep_spec(u16 depth, u16 index, bool pack, std::span<const ast::TemplateArg> args) {
    return ins_->intern_type(ast::TDepSpec{depth, index, u8(pack), ins_->intern_seq(args)});
  }
  QualType builtin_spec(ast::BuiltinTemplate which, std::span<const ast::TemplateArg> args) {
    return ins_->intern_type(ast::TBuiltinSpec{u8(which), ins_->intern_seq(args)});
  }
  bool is_floating(QualType q) const {
    auto b = builtin_of(q);
    return b && ast::is_floating(*b);
  }

  TypeRec rec(QualType q) const {
    const ast::Types& t = ins_->types;
    QualType c = ins_->canonical(q);
    TypeRec r;
    if (t.is<ast::TBuiltin>(c)) {
      r.kind = TypeKind::builtin;
      r.builtin = t.get<ast::TBuiltin>(c).b;
    } else if (t.is<ast::TPointer>(c)) {
      r.kind = TypeKind::pointer;
      r.inner = t.get<ast::TPointer>(c).pointee;
    } else if (t.is<ast::TArray>(c)) {
      auto a = t.get<ast::TArray>(c);
      r.kind = TypeKind::array;
      r.inner = a.elem;
      r.bound = a.bound == ConstId::none || ins_->values.is_dependent(a.bound) ? 0 : ast::int_bits(*ins_, a.bound).lo;
      r.bound_value = a.bound;
    } else if (t.is<ast::TFunction>(c)) {
      auto f = t.get<ast::TFunction>(c);
      r.kind = TypeKind::function;
      r.inner = f.ret;
      r.params = ins_->seqs.elements(f.params);
      r.variadic = f.variadic != 0;
      r.linkage = f.linkage;
      r.fn_cv = f.cv;
    } else if (t.is<ast::TClass>(c)) {
      r.kind = TypeKind::class_;
      r.entity = t.get<ast::TClass>(c).entity;
    } else if (t.is<ast::TEnum>(c)) {
      r.kind = TypeKind::enum_;
      r.entity = t.get<ast::TEnum>(c).entity;
    } else if (t.is<ast::TLRef>(c) || t.is<ast::TRRef>(c)) {
      r.kind = TypeKind::reference;
      r.rvalue_ref = t.is<ast::TRRef>(c);
      r.inner = r.rvalue_ref ? t.get<ast::TRRef>(c).referee : t.get<ast::TLRef>(c).referee;
    } else if (t.is<ast::TDepName>(c)) {
      r.kind = TypeKind::dep_name;
      r.inner = t.get<ast::TDepName>(c).qualifier;
      r.name = t.get<ast::TDepName>(c).name;
    } else if (t.is<ast::TTransform>(c)) {
      r.kind = TypeKind::transform;
      r.inner = t.get<ast::TTransform>(c).arg;
      r.trait = t.get<ast::TTransform>(c).trait;
    } else if (t.is<ast::TAuto>(c)) {
      r.kind = TypeKind::placeholder;
      r.decltype_auto = t.get<ast::TAuto>(c).decltype_auto != 0;
    } else if (t.is<ast::TDepSpec>(c)) {
      auto d = t.get<ast::TDepSpec>(c);
      r.kind = TypeKind::dep_spec;
      r.depth = d.depth;
      r.index = d.index;
      r.pack = d.pack != 0;
      r.args = d.args;
    } else if (t.is<ast::TBuiltinSpec>(c)) {
      r.kind = TypeKind::builtin_spec;
      r.trait = t.get<ast::TBuiltinSpec>(c).which;
      r.args = t.get<ast::TBuiltinSpec>(c).args;
    } else if (t.is<ast::TPackExpansion>(c)) {
      r.kind = TypeKind::pack_expansion;
      r.inner = t.get<ast::TPackExpansion>(c).pattern;
    } else if (t.is<ast::TTemplateParam>(c)) {
      auto p = t.get<ast::TTemplateParam>(c);
      r.kind = TypeKind::template_param;
      r.depth = p.depth;
      r.index = p.index;
      r.pack = p.pack != 0;
    }
    return r;
  }

  // A dependent type ([temp.dep.type]): one that names a template parameter, or the type of a
  // type-dependent expression (`Builtin::dependent`).
  bool is_dependent(QualType q) const {
    TypeRec r = rec(q);
    switch (r.kind) {
    case TypeKind::template_param:
    case TypeKind::dep_name:
    case TypeKind::pack_expansion:
    case TypeKind::transform:
    case TypeKind::dep_spec:
    case TypeKind::builtin_spec: return true;
    case TypeKind::builtin: return r.builtin == Builtin::dependent;
    case TypeKind::array:
      return is_dependent(r.inner) || (r.bound_value != ConstId::none && ins_->values.is_dependent(r.bound_value));
    case TypeKind::pointer:
    case TypeKind::reference: return is_dependent(r.inner);
    case TypeKind::function:
      return is_dependent(r.inner) || std::ranges::any_of(r.params, [&](QualType p) { return is_dependent(p); });
    case TypeKind::class_: return class_dependent && class_dependent(r.entity);
    default: return false;
    }
  }
  // Whether a class is a specialization with a dependent argument (`X<T>`); set by the world that
  // knows the entities.
  std::function<bool(EntityId)> class_dependent;
  std::function<std::string(NameId)> dep_name_spelling;
  QualType dep_name(QualType qualifier, NameId name) { return ins_->intern_type(ast::TDepName{qualifier, name}); }

  static Cv cv(QualType q) { return ast::cv_of(q); }
  static bool is_const(QualType q) { return (std::to_underlying(ast::cv_of(q)) & 1) != 0; }
  static bool is_volatile(QualType q) { return (std::to_underlying(ast::cv_of(q)) & 2) != 0; }
  static QualType unqualified(QualType q) { return ast::without_cv(q); }
  // Adds cv; on an array it goes to the elements ([basic.type.qualifier]).
  QualType add_cv(QualType q, Cv c) { return c == Cv::none ? q : ins_->qualified(q, c); }
  QualType canonical(QualType q) const { return ins_->canonical(q); }

  bool is(QualType q, Builtin b) const { return ast::builtin_of(*ins_, q) == b; }
  TypeKind kind(QualType q) const { return rec(q).kind; }
  std::optional<Builtin> builtin_of(QualType q) const { return ast::builtin_of(*ins_, q); }
  bool is_void(QualType q) const { return ast::is_void(*ins_, q); }
  bool is_integral(QualType q) const { return builtin_of(q) && ast::is_integral(*builtin_of(q)); }
  bool is_arithmetic(QualType q) const {
    auto b = builtin_of(q);
    return b && ast::is_integral(*b);  // the α subset has no floating types
  }
  bool is_pointer(QualType q) const { return ast::is_pointer(*ins_, q); }
  bool is_function(QualType q) const { return ast::is_function(*ins_, q); }
  bool is_array(QualType q) const { return ast::is_array(*ins_, q); }
  bool is_class(QualType q) const { return ins_->types.is<ast::TClass>(ins_->types.canon(q)); }
  bool is_reference(QualType q) const { return ast::is_reference(*ins_, q); }
  bool is_enum(QualType q) const { return ins_->types.is<ast::TEnum>(ins_->types.canon(q)); }
  EntityId enum_of(QualType q) const
    pre(is_enum(q))
  {
    return ins_->types.get<ast::TEnum>(ins_->types.canon(q)).entity;
  }
  // The type an expression naming a reference has ([expr.type]): what it refers to.
  QualType referee(QualType q) const { return is_reference(q) ? rec(q).inner : q; }
  // The class of a class type.
  EntityId class_of(QualType q) const
    pre(is_class(q))
  {
    return ins_->types.get<ast::TClass>(ins_->types.canon(q)).entity;
  }
  // [basic.types.general]: arithmetic, enumeration, pointer, pointer-to-member and std::nullptr_t
  // types, cv-qualified or not.
  bool is_scalar(QualType q) const {
    return is_arithmetic(q) || is_pointer(q) || is(q, Builtin::nullptr_t) || is_enum(q);
  }
  // [basic.types.general]: an object type is not a function, reference or void type.
  bool is_object(QualType q) const { return !is_function(q) && !is_void(q) && !ast::is_reference(*ins_, q); }
  QualType pointee(QualType q) const
    pre(is_pointer(q))
  {
    return ast::pointee_of(*ins_, q);
  }

  // The type as it would be written, for diagnostics.
  std::string spell(QualType q) const { return spell(q, ""); }
  // How a class is named in diagnostics; set by the world that knows the entities.
  std::function<std::string(EntityId)> class_name;

  Fp128 fp(QualType q) const { return ins_->fp(q); }

private:
  std::string spell(QualType q, std::string inner) const {
    TypeRec r = rec(q);
    Cv c = cv(ins_->canonical(q));
    std::string cvs;
    if (std::to_underlying(c) & 1) cvs += "const ";
    if (std::to_underlying(c) & 2) cvs += "volatile ";
    switch (r.kind) {
    case TypeKind::builtin:
      return cvs + std::string(sema::spelling(r.builtin)) + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::pointer: {
      std::string p = "*" + (cvs.empty() ? "" : " " + cvs.substr(0, cvs.size() - 1)) + inner;
      TypeKind k = kind(r.inner);
      if (k == TypeKind::array || k == TypeKind::function) p = "(" + p + ")";
      return spell(r.inner, p);
    }
    case TypeKind::array:
      return spell(r.inner, inner + "[" + (r.bound_value == ConstId::none ? std::string() : std::to_string(r.bound)) + "]");
    case TypeKind::function: {
      std::string ps;
      for (std::size_t i = 0; i < r.params.size(); ++i) ps += (i ? ", " : "") + spell(r.params[i]);
      if (r.variadic) ps += r.params.empty() ? "..." : ", ...";
      std::string q = (std::to_underlying(r.fn_cv) & 1) ? " const" : "";
      if (std::to_underlying(r.fn_cv) & 2) q += " volatile";
      return spell(r.inner, inner + "(" + ps + ")" + q);
    }
    case TypeKind::reference: {
      std::string p = std::string(r.rvalue_ref ? "&&" : "&") + inner;
      TypeKind k = kind(r.inner);
      if (k == TypeKind::array || k == TypeKind::function) p = "(" + p + ")";
      return spell(r.inner, p);
    }
    case TypeKind::enum_:
    case TypeKind::class_:
      return cvs + (class_name ? class_name(r.entity) : "<class>") + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::dep_name:
      return cvs + "typename " + spell(r.inner) + "::" + (dep_name_spelling ? dep_name_spelling(r.name) : "<name>") +
             (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::template_param:
      // Parameters are positional in the type (ast.md §5.1).
      return cvs + std::format("type-parameter-{}-{}", r.depth, r.index) + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::pack_expansion: return spell(r.inner) + "..." + (inner.empty() ? "" : " " + inner);
    case TypeKind::transform:
      return cvs + std::string(lex::type_trait_info(lex::TypeTrait(r.trait)).name) + "(" + spell(r.inner) + ")" +
             (inner.empty() ? "" : " " + inner);
    case TypeKind::placeholder:
      return cvs + (r.decltype_auto ? "decltype(auto)" : "auto") + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::dep_spec:
    case TypeKind::builtin_spec: {
      std::string name = r.kind == TypeKind::builtin_spec ? std::string("__make_integer_seq")
                                                          : std::format("template-parameter-{}-{}", r.depth, r.index);
      return cvs + name + "<…>" + (inner.empty() ? "" : " " + inner);
    }
    case TypeKind::other: return "<type>";
    }
    return {};
  }

  ast::Interners* ins_;
};

// ---------------------------------------------------------------- constants

enum class ValueKind : u8 { integer, string, address, null_pointer, other };

// A constant read out by value (§5.5): an integer of an integral type (`VInt`, its low 64 bits),
// a string literal object's code units (`VString`), the address of one or of an entity
// (`VEntity`) plus an offset (`VLvalue`), or a null pointer (`VNullptr`).
struct ValueRec {
  ValueKind kind = ValueKind::other;
  QualType type{};
  u64 bits = 0;
  std::string units;
  ConstId base = ConstId::none;
  i64 offset = 0;
};

class Values {
public:
  explicit Values(ast::Interners& ins, Types&) : ins_(&ins) {}

  // An integer of type `t`, wrapped to its width ([conv.integral]).
  ConstId integer(QualType t, u64 bits) { return ast::int_value(*ins_, target, t, ast::Bits128{bits, 0}); }
  // A string literal object of array type `t` holding `units`, one byte per code unit.
  ConstId string(QualType t, std::string units) {
    std::vector<u8> us(units.begin(), units.end());
    return ins_->intern_value(ast::VString{t, ins_->intern_seq(std::span<const u8>(us))});
  }
  ConstId address(QualType, ConstId base, i64 offset) {
    return ins_->intern_value(ast::VLvalue{base, ins_->intern_seq(std::span<const u32>{}), offset});
  }
  ConstId null_pointer(QualType) { return ins_->intern_value(ast::VNullptr{}); }
  // A declared object or function as the base of an address (§5.5).
  ConstId entity(EntityId e) { return ins_->intern_value(ast::VEntity{e}); }
  // An array or class object, from its elements' values in order ([dcl.init.aggr]).
  ConstId aggregate(QualType t, std::span<const ConstId> elems) {
    return ins_->intern_value(ast::VAggregate{ast::without_cv(ins_->canonical(t)), ins_->intern_seq(elems)});
  }

  ValueRec rec(ConstId c) const {
    const ast::Values& v = ins_->values;
    ValueRec r;
    if (v.is<ast::VInt>(c)) {
      auto i = v.get<ast::VInt>(c);
      r.kind = ValueKind::integer;
      r.type = i.type;
      r.bits = i.bits.lo;
    } else if (v.is<ast::VString>(c)) {
      auto s = v.get<ast::VString>(c);
      r.kind = ValueKind::string;
      r.type = s.type;
      for (u8 u : ins_->seqs.elements(s.units)) r.units.push_back(static_cast<char>(u));
    } else if (v.is<ast::VLvalue>(c)) {
      auto l = v.get<ast::VLvalue>(c);
      r.kind = ValueKind::address;
      r.base = l.base;
      r.offset = l.offset;
    } else if (v.is<ast::VNullptr>(c)) {
      r.kind = ValueKind::null_pointer;
    }
    return r;
  }
  i64 as_signed(ConstId c) const { return i64(rec(c).bits); }

private:
  ast::Interners* ins_;
};

// ---------------------------------------------------------------- entities (stand-in for #33)

// `data_member` is a non-static data member ([class.mem]).
// A `function_template` is a function template ([temp.fct]); a specialization of one is a
// `function` whose `templ` is the template. A `class_template` likewise ([temp.class]); a
// specialization of it, the current instantiation included ([temp.dep.type]), is a `class_`.
enum class EntityKind : u8 {
  function, variable, class_, data_member, constructor, destructor, enum_, enumerator, namespace_, function_template,
  class_template,
  variable_template,  // [temp.pre]: `(scope, name, head)`, as a function template's
  alias_template,     // [temp.alias]: `(scope, name, head)`; its uses are the types it stands for
  concept_,           // [temp.concept]: `(scope, name, head)`
  type_alias  // a typedef-name or alias-declaration ([dcl.typedef]): `(scope, name)`
};

// The structure behind an entity id (§4.3): for a function `(scope, name, canonical type)`, with
// `::` as the scope of a function with C language linkage ([dcl.link]); for a variable
// `(scope, name, language linkage)`: redeclarations must agree on the language linkage
// ([dcl.link]/6), and so its record never holds a stale one (#115); for an entity with internal
// linkage, also its first
// declaration. `scope` is the enclosing class, or none for the global namespace. A function
// template's identity also has its template-head (`head`, a fingerprint of the parameters' kinds and
// types); a specialization's is its template and its canonical template arguments (§B1.5).
// An unnamed namespace's `head` is its unit's: each unit has its own ([namespace.unnamed]), so its
// members are distinct from another unit's of the same name.
struct EntityRec {
  EntityKind kind = EntityKind::function;
  NameId name{};
  QualType type{};
  LangLinkage lang = LangLinkage::cxx;
  ast::Linkage linkage = ast::Linkage::external;
  DeclId first{};  // internal linkage only
  EntityId scope = EntityId::none;
  Fp128 head{};                          // a template's, or an unnamed namespace's unit
  Fp128 constraints{};                   // a function template's constraints (ast.md §4.3)
  EntityId templ = EntityId::none;       // a specialization's template
  std::vector<ast::TemplateArg> args{};  // a specialization's template arguments
  // A closure type's number among its context's closures of its signature (ast.md §4.3); its
  // `type` is its function call operator's declared type (docs/design/closures.md).
  u32 ordinal = 0;
};

class Entities {
public:
  // A template argument into an entity's identity: a pack's elements in order ([temp.variadic]).
  Fp128 mix_arg(Fp128 f, const ast::TemplateArg& a) {
    f = ast::mix(f, std::to_underlying(a.kind));
    if (a.kind == ast::TemplateArgKind::pack) {
      f = ast::mix(f, u64(ins_->seqs.size(a.elems)));
      for (const ast::TemplateArg& e : ins_->seqs.elements(a.elems)) f = mix_arg(f, e);
      return f;
    }
    // A template: itself ([temp.type]/1: the same template), or while dependent its parameter.
    if (a.kind == ast::TemplateArgKind::template_ && a.templ != EntityId::none) return ast::mix(f, ins_->entities.fp(a.templ));
    f = ast::mix(f, types_->fp(types_->canonical(a.type)));
    if (a.value != ConstId::none) f = ast::mix(f, ins_->values.fp(a.value));
    return f;
  }
  explicit Entities(ast::Interners& ins, Types& types) : ins_(&ins), types_(&types) {}

  EntityId entity(EntityRec r) {
    Fp128 f = ast::seed("ycxx.sema.Entity");
    f = ast::mix(f, std::to_underlying(r.kind));
    f = ast::mix(f, ins_->names.fp(r.name));
    if (r.scope != EntityId::none) f = ast::mix(f, ins_->entities.fp(r.scope));
    // A variable's type is not part of its identity; a function's is ([basic.link]).
    if (r.kind == EntityKind::function || r.kind == EntityKind::constructor || r.kind == EntityKind::destructor ||
        r.kind == EntityKind::function_template)
      f = ast::mix(f, types_->fp(types_->canonical(r.type)));
    if (r.kind == EntityKind::variable) f = ast::mix(f, std::to_underlying(r.lang));
    if (r.kind == EntityKind::function_template || r.kind == EntityKind::class_template ||
        r.kind == EntityKind::variable_template || r.kind == EntityKind::alias_template || r.kind == EntityKind::concept_ ||
        r.kind == EntityKind::namespace_)
      f = ast::mix(f, r.head);
    if (r.kind == EntityKind::function_template) f = ast::mix(f, r.constraints);
    // A closure type, unnamed, has its signature and ordinal in its scope as its `head`
    // (docs/design/closures.md); other classes have none.
    if (r.kind == EntityKind::class_) f = ast::mix(f, r.head);
    if (r.templ != EntityId::none) {
      f = ast::mix(f, ins_->entities.fp(r.templ));
      for (const ast::TemplateArg& a : r.args) f = mix_arg(f, a);
    }
    f = ast::mix(f, std::to_underlying(r.linkage));
    if (r.linkage == ast::Linkage::internal)
      f = ast::mix(ast::mix(f, ins_->items.fp(r.first.item)), std::to_underlying(r.first.ord));
    EntityId e = ins_->entities.add<EntityId>(f);
    // The interner is shared: the back end adds entities of its own (string literal objects), which
    // have no record here, so records are indexed by handle with gaps. Equal identities have equal
    // records, so the first publication is as good as any (#156).
    recs_.publish(std::to_underlying(e), std::move(r));
    return e;
  }
  const EntityRec& rec(EntityId e) const
    pre(recs_.find(std::to_underlying(e)) != nullptr)
  {
    return *recs_.find(std::to_underlying(e));
  }
  // The record, if sema made the entity (the back end's string literal objects have none).
  const EntityRec* find(EntityId e) const { return recs_.find(std::to_underlying(e)); }

private:
  ast::Interners* ins_;
  Types* types_;
  ast::Published<EntityRec> recs_;
};

// ---------------------------------------------------------------- classes (stand-in for #33)

// A non-static data member ([class.mem]).
struct FieldRec {
  NameId name{};
  QualType type{};
  EntityId entity = EntityId::none;
  // Its default member initializer's value ([class.mem.general]), or none. The front end keeps
  // only those that are constants of scalar type so far (#86).
  ConstId init = ConstId::none;
  bool dmi = false;  // it has a default member initializer, constant or not
};

// What `class_def(C)` (ast.md §3.6) says of a class that member lookup and the layout need, kept
// by class entity. A class is complete once its definition has been analysed
// ([class.mem.general]); completeness at a point is the parser's question (`Program`).
// A member function ([class.mfct]) or static data member ([class.static.data]), which a
// qualified definition outside the class redeclares. Its declaration is an item of each unit that
// defines the class, so the unit keeps it (`Program::member_decl`), not the shared record (#156).
struct MemberDeclRec {
  NameId name{};
  EntityId entity = EntityId::none;
  QualType type{};
  bool is_function = false;
  bool is_static = false;
  bool is_ctor = false;
  bool is_dtor = false;
  bool is_deleted = false;
  bool is_template = false;  // a member function template ([temp.mem])
};

struct NestedType {
  NameId name{};
  QualType type{};
  EntityId entity = EntityId::none;
};

// A direct base class ([class.derived]): non-virtual so far.
struct BaseRec {
  EntityId cls = EntityId::none;
  ast::Access access = ast::Access::public_;
};

struct ClassRec {
  bool defined = false;
  // Its direct base classes, in declaration order ([class.derived.general]).
  std::vector<BaseRec> bases;
  // POD for the purposes of layout, as Clang has it (`ast::record_layout`): trivial and
  // standard-layout ([class.prop]).
  bool pod = true;
  bool union_ = false;  // a union ([class.union]): its members share its storage
  std::vector<FieldRec> fields;        // in declaration order
  std::vector<MemberDeclRec> members;  // member functions and static data members
  // Special members ([special]): whether a constructor is user-declared; the destructor and the
  // copy constructor when they are not trivial (user-declared, or implicit and synthesized),
  // else none; whether the copy constructor is deleted.
  bool user_ctors = false;
  // A closure type ([expr.prim.lambda.closure]): not an aggregate, its captures' members unnamed
  // (/1); with a lambda-capture, its default constructor and copy assignment are deleted (/13).
  bool closure = false;
  bool default_deleted = false;
  EntityId destructor = EntityId::none;
  EntityId copy_ctor = EntityId::none;
  bool copy_deleted = false;
  // The move constructor ([class.copy.ctor]/8): whether one is declared and not deleted, and
  // whether it is then not trivial (user-provided, or implicit and a member's move is not).
  bool move_usable = false;
  bool move_nontrivial = false;
  // Nested enumerations and type aliases ([class.mem]): their types by name, with their entities;
  // the unscoped enumerations' enumerators are members of the class.
  std::vector<NestedType> types;
  // Each member's access ([class.access.spec]), by entity: data members, member functions,
  // constructors, static data members and nested types.
  std::vector<std::pair<EntityId, ast::Access>> access;
  // Its friends ([class.friend]): functions and classes, by entity.
  std::vector<EntityId> friends;

  ast::Access access_of(EntityId e) const {
    for (const auto& [m, a] : access)
      if (m == e) return a;
    return ast::Access::public_;
  }
};

// An enumeration ([dcl.enum]): scoped or not, its underlying type (fixed, or chosen to hold its
// values), and its enumerators with their values, in order.
struct EnumeratorRec {
  NameId name{};
  EntityId entity = EntityId::none;
  ConstId value = ConstId::none;  // of the underlying type
};
struct EnumRec {
  bool scoped = false;
  bool fixed = false;
  std::optional<QualType> underlying;
  // Without a fixed type, what it promotes to ([conv.prom]/3): the first of int, unsigned int,
  // long, unsigned long that holds all its values, which may differ from `underlying`.
  std::optional<QualType> promotion;
  std::vector<EnumeratorRec> enumerators;
};

// A unit's class and enumeration records (#115). Entities are program-wide (§4.3), but what
// `class_def` says of one is a question of the unit that asks (query-engine.md §B1.4): units may
// see different definitions (one session serving several programs, or an edit), so each unit has
// its own table, built as it analyses the definitions, and gives it to the back end with its
// definitions (`pipeline::FrontResult::shapes`). A record is built privately and published once
// complete; until then a class has no record.
class Classes {
public:
  explicit Classes(ast::Interners& ins, Types& types) : ins_(&ins), types_(&types) {}
  Classes(const Classes&) = delete;

  // `r`, a definition's record, unless one is published already; the published one.
  const ClassRec& publish(EntityId c, ClassRec r) {
    auto [it, fresh] = recs_.try_emplace(std::to_underlying(c), std::move(r));
    if (fresh) class_order_.push_back(c);
    return it->second;
  }
  const EnumRec& publish_enum(EntityId e, EnumRec r) {
    auto [it, fresh] = enums_.try_emplace(std::to_underlying(e), std::move(r));
    if (fresh) enum_order_.push_back(e);
    return it->second;
  }
  // The classes and enumerations with records, in the order they were published.
  std::span<const EntityId> classes() const { return class_order_; }
  std::span<const EntityId> enums() const { return enum_order_; }
  // A defined class's record.
  const ClassRec& rec(EntityId c) const
    pre(find(c) != nullptr)
  {
    return *find(c);
  }
  const EnumRec* find_enum(EntityId e) const {
    auto it = enums_.find(std::to_underlying(e));
    return it == enums_.end() ? nullptr : &it->second;
  }
  // An enumeration's underlying type; `int` until it is known.
  QualType underlying(EntityId e) const {
    const EnumRec* r = find_enum(e);
    return r && r->underlying ? *r->underlying : types_->builtin(Builtin::int_);
  }
  const ClassRec* find(EntityId c) const {
    auto it = recs_.find(std::to_underlying(c));
    return it == recs_.end() ? nullptr : &it->second;
  }
  bool is_defined(EntityId c) const {
    const ClassRec* r = find(c);
    return r && r->defined;
  }

  // The destructor that ends a `t`, none if trivial; for an array, its elements' ([class.dtor]).
  EntityId destructor_of(QualType t) const {
    TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return destructor_of(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    return c ? c->destructor : EntityId::none;
  }
  // Whether a `t`'s copy constructor is not trivial ([class.copy.ctor]/11), and whether it is
  // deleted.
  std::pair<bool, bool> copy_of(QualType t) const {
    TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return copy_of(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    if (!c) return {false, false};
    return {c->copy_ctor != EntityId::none, c->copy_deleted};
  }
  // Whether moving a `t` calls a constructor that is not trivial: its move constructor, or its
  // copy constructor when it has none ([over.match.ctor]).
  bool move_nontrivial(QualType t) const {
    TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return move_nontrivial(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    if (!c) return false;
    return c->move_usable ? c->move_nontrivial : c->copy_ctor != EntityId::none;
  }
  // Whether a `t` can be neither copied nor moved: its copy constructor deleted and no move
  // constructor usable; for an array, its elements'.
  bool unmovable(QualType t) const {
    TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return unmovable(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    return c && c->copy_deleted && !c->move_usable;
  }
  // Whether a `cls` is trivially copyable for the purposes of calls, by `rule` (ast::Target,
  // DECISIONS §1.6). Itanium ([class.temporary]/3, Itanium C++ ABI §3.1.2.3): its destructor and
  // every copy and move constructor trivial or deleted, and at least one of those constructors
  // not deleted. Apple: its destructor trivial, its copy constructor trivial or deleted.
  bool trivial_for_calls(EntityId cls, ast::CallTriviality rule) const {
    const ClassRec* c = find(cls);
    if (!c) return true;
    if (c->destructor != EntityId::none || c->copy_ctor != EntityId::none) return false;
    if (rule == ast::CallTriviality::apple) return true;
    if (c->move_usable && c->move_nontrivial) return false;
    return !c->copy_deleted || c->move_usable;
  }

private:
  ast::Interners* ins_;
  Types* types_;
  std::map<u32, ClassRec> recs_;  // node-based: a published record never moves
  std::map<u32, EnumRec> enums_;
  std::vector<EntityId> class_order_, enum_order_;
};

// What the front end shares process-wide (§9.1): the AST core's interners, and sema's views.
// Class and enumeration records are each unit's (`Classes`).
struct World {
  ast::Interners& ins;
  Types types{ins};
  Values values{ins, types};
  Entities entities{ins, types};

  explicit World(ast::Interners& i) : ins(i) {
    types.class_name = [this](EntityId e) {
      std::string n(ins.names.spelling(entities.rec(e).name));
      return n.empty() ? std::string("(lambda)") : n;  // a closure type ([expr.prim.lambda.closure])
    };
    types.dep_name_spelling = [this](NameId n) { return std::string(ins.names.spelling(n)); };
    types.class_dependent = [this](EntityId e) {
      return std::ranges::any_of(entities.rec(e).args, [&](const ast::TemplateArg& a) { return arg_dependent(a); });
    };
  }
  World(const World&) = delete;
  // Whether a template argument depends on a template parameter; a pack, whether an element does.
  bool arg_dependent(const ast::TemplateArg& a) const {
    if (a.kind == ast::TemplateArgKind::type) return types.is_dependent(a.type);
    if (a.kind == ast::TemplateArgKind::value) return ins.values.is_dependent(a.value);
    if (a.kind == ast::TemplateArgKind::template_) return a.templ == EntityId::none;
    return std::ranges::any_of(ins.seqs.elements(a.elems), [&](const ast::TemplateArg& x) { return arg_dependent(x); });
  }
};

}  // namespace ycxx::sema
