// Sema's view of types, constants and entities. Types and constants are #33's (ycxx.ast: `types`,
// `values`, ycxx.ast.typeops) and this partition only shapes them for sema: by-value records of
// the canonical form, and the target fixed to AArch64 Darwin. Entities are still a STAND-IN for
// #33: a side table behind the AST core's fingerprint-only `entities` placeholder, isolated here.
export module ycxx.sema:types;

import std;
import ycxx.ast;

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

enum class TypeKind : u8 { builtin, pointer, array, function, other };

// The canonical form of a type, read out by value.
struct TypeRec {
  TypeKind kind = TypeKind::other;
  Builtin builtin = Builtin::void_;
  QualType inner{};  // pointee, element or return type
  u64 bound = 0;     // an array's
  std::vector<QualType> params;
  bool variadic = false;
  LangLinkage linkage = LangLinkage::cxx;
};

class Types {
public:
  explicit Types(ast::Interners& ins) : ins_(&ins) {}

  QualType builtin(Builtin b) { return ast::builtin(*ins_, b); }
  QualType pointer(QualType pointee) { return ast::pointer_to(*ins_, pointee); }
  QualType array(QualType elem, u64 bound) {
    return ast::array_of(*ins_, target, elem, ast::int_value(*ins_, target, Builtin::unsigned_long, i64(bound)));
  }
  // The parameters are adjusted by #33's `function_type` ([dcl.fct]).
  QualType function(QualType ret, std::span<const QualType> params, bool variadic, LangLinkage linkage) {
    return ast::function_type(*ins_, ret, params, {.variadic = variadic, .linkage = linkage});
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
      r.bound = a.bound == ConstId::none ? 0 : ast::int_bits(*ins_, a.bound).lo;
    } else if (t.is<ast::TFunction>(c)) {
      auto f = t.get<ast::TFunction>(c);
      r.kind = TypeKind::function;
      r.inner = f.ret;
      r.params = ins_->seqs.elements(f.params);
      r.variadic = f.variadic != 0;
      r.linkage = f.linkage;
    }
    return r;
  }

  static Cv cv(QualType q) { return ast::cv_of(q); }
  static bool is_const(QualType q) { return (std::to_underlying(ast::cv_of(q)) & 1) != 0; }
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
  // [basic.types.general]: an object type is not a function, reference or void type.
  bool is_object(QualType q) const { return !is_function(q) && !is_void(q) && !ast::is_reference(*ins_, q); }
  QualType pointee(QualType q) const
    pre(is_pointer(q))
  {
    return ast::pointee_of(*ins_, q);
  }

  // The type as it would be written, for diagnostics.
  std::string spell(QualType q) const { return spell(q, ""); }

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
    case TypeKind::array: return spell(r.inner, inner + "[" + std::to_string(r.bound) + "]");
    case TypeKind::function: {
      std::string ps;
      for (std::size_t i = 0; i < r.params.size(); ++i) ps += (i ? ", " : "") + spell(r.params[i]);
      if (r.variadic) ps += r.params.empty() ? "..." : ", ...";
      return spell(r.inner, inner + "(" + ps + ")");
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

enum class EntityKind : u8 { function, variable };

// The structure behind an entity id (§4.3): for a function `(scope, name, canonical type)`, with
// `::` as the scope of a function with C language linkage ([dcl.link]); for a variable with
// external linkage `(scope, name)`; for an entity with internal linkage, also its first
// declaration. The α subset has only the global namespace as a scope.
struct EntityRec {
  EntityKind kind = EntityKind::function;
  NameId name{};
  QualType type{};
  LangLinkage lang = LangLinkage::cxx;
  ast::Linkage linkage = ast::Linkage::external;
  DeclId first{};  // internal linkage only
};

class Entities {
public:
  explicit Entities(ast::Interners& ins, Types& types) : ins_(&ins), types_(&types) {}

  EntityId entity(EntityRec r) {
    Fp128 f = ast::seed("ycxx.sema.Entity");
    f = ast::mix(f, std::to_underlying(r.kind));
    f = ast::mix(f, ins_->names.fp(r.name));
    // A variable's type is not part of its identity; a function's is ([basic.link]).
    if (r.kind == EntityKind::function) f = ast::mix(f, types_->fp(types_->canonical(r.type)));
    f = ast::mix(f, std::to_underlying(r.linkage));
    if (r.linkage == ast::Linkage::internal)
      f = ast::mix(ast::mix(f, ins_->items.fp(r.first.item)), std::to_underlying(r.first.ord));
    EntityId e = ins_->entities.add<EntityId>(f);
    if (std::to_underlying(e) == recs_.size()) recs_.push_back(r);
    return e;
  }
  const EntityRec& rec(EntityId e) const
    pre(std::to_underlying(e) < recs_.size())
  {
    return recs_[std::to_underlying(e)];
  }

private:
  ast::Interners* ins_;
  Types* types_;
  std::deque<EntityRec> recs_;  // a deque: references to records stay valid as entities are added
};

// What the front end shares process-wide (§9.1): the AST core's interners, and sema's views.
struct World {
  ast::Interners& ins;
  Types types{ins};
  Values values{ins, types};
  Entities entities{ins, types};

  explicit World(ast::Interners& i) : ins(i) {}
  World(const World&) = delete;
};

}  // namespace ycxx::sema
