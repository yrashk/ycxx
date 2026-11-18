// STAND-IN for #33 (types and constants), isolated in this one partition: the type, constant and
// entity tables the α subset needs (#39), and the arithmetic conversion rules over them. #33 owns
// the real `types`, `values` and `entities` interners (docs/design/ast.md §4.3, §5); when it lands,
// this file is replaced by imports of it and the rest of sema keeps its calls. The names follow
// #33's draft (`Builtin`, `LangLinkage`) so that the swap is mechanical.
//
// Handles come from the AST core's fingerprint-only placeholders (`Interners::types`, `values`,
// `entities`), so they are the handles the rest of the front end already passes around; each
// table here keeps the structure behind a handle in a side vector indexed like the placeholder.
//
// Target: AArch64 Darwin, LP64, `char` signed ([basic.fundamental] leaves widths and the
// signedness of `char` to the implementation).
export module ycxx.sema:types;

import std;
import ycxx.ast;

export namespace ycxx::sema {

using ast::ConstId;
using ast::Cv;
using ast::DeclId;
using ast::EntityId;
using ast::Fp128;
using ast::NameId;
using ast::QualType;
using ast::u16;
using ast::u32;
using ast::u64;
using ast::u8;
using i64 = std::int64_t;

// The fundamental types ([basic.fundamental]), as #33 names them.
enum class Builtin : u8 {
  void_, nullptr_t,
  bool_,
  char_, signed_char, unsigned_char, wchar_t_, char8_t_, char16_t_, char32_t_,
  short_, unsigned_short, int_, unsigned_int, long_, unsigned_long, long_long, unsigned_long_long,
  float_, double_, long_double,
};

constexpr bool is_integral(Builtin b) { return b >= Builtin::bool_ && b <= Builtin::unsigned_long_long; }
constexpr bool is_floating(Builtin b) { return b >= Builtin::float_ && b <= Builtin::long_double; }
constexpr bool is_arithmetic(Builtin b) { return is_integral(b) || is_floating(b); }

// Language linkage is part of a function type ([dcl.link]).
enum class LangLinkage : u8 { cxx, c };

// Width in bits and signedness of each integral type on the target.
struct IntShape {
  u8 bits;
  bool is_signed;
};
constexpr IntShape shape(Builtin b) {
  switch (b) {
  case Builtin::bool_: return {8, false};
  case Builtin::char_: return {8, true};
  case Builtin::signed_char: return {8, true};
  case Builtin::unsigned_char: return {8, false};
  case Builtin::char8_t_: return {8, false};
  case Builtin::wchar_t_: return {32, true};
  case Builtin::char16_t_: return {16, false};
  case Builtin::char32_t_: return {32, false};
  case Builtin::short_: return {16, true};
  case Builtin::unsigned_short: return {16, false};
  case Builtin::int_: return {32, true};
  case Builtin::unsigned_int: return {32, false};
  case Builtin::long_: return {64, true};
  case Builtin::unsigned_long: return {64, false};
  case Builtin::long_long: return {64, true};
  case Builtin::unsigned_long_long: return {64, false};
  default: return {0, false};
  }
}

constexpr std::string_view spelling(Builtin b) {
  constexpr std::array<std::string_view, 21> names{
      "void", "std::nullptr_t", "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t",
      "char32_t", "short", "unsigned short", "int", "unsigned int", "long", "unsigned long", "long long",
      "unsigned long long", "float", "double", "long double"};
  return names[std::to_underlying(b)];
}

// [conv.rank]: the integer conversion rank of a signed or unsigned integer type; the character
// types and bool take their underlying type's.
constexpr int rank(Builtin b) {
  switch (b) {
  case Builtin::bool_: return 0;
  case Builtin::char_:
  case Builtin::signed_char:
  case Builtin::unsigned_char:
  case Builtin::char8_t_: return 1;
  case Builtin::short_:
  case Builtin::unsigned_short:
  case Builtin::char16_t_: return 2;
  case Builtin::int_:
  case Builtin::unsigned_int:
  case Builtin::wchar_t_:
  case Builtin::char32_t_: return 3;
  case Builtin::long_:
  case Builtin::unsigned_long: return 4;
  case Builtin::long_long:
  case Builtin::unsigned_long_long: return 5;
  default: return -1;
  }
}

// The signed or unsigned integer type of a rank ([basic.fundamental]).
constexpr Builtin integer_of_rank(int r, bool is_signed) {
  switch (r) {
  case 1: return is_signed ? Builtin::signed_char : Builtin::unsigned_char;
  case 2: return is_signed ? Builtin::short_ : Builtin::unsigned_short;
  case 3: return is_signed ? Builtin::int_ : Builtin::unsigned_int;
  case 4: return is_signed ? Builtin::long_ : Builtin::unsigned_long;
  default: return is_signed ? Builtin::long_long : Builtin::unsigned_long_long;
  }
}

// [conv.prom]: the type an integral prvalue promotes to. bool, the narrow character types and
// short promote to int (every value fits); char8_t, char16_t, char32_t and wchar_t promote to the
// first of int, unsigned int, long, unsigned long, long long, unsigned long long that holds all
// their values. Types of rank int or above do not promote.
constexpr Builtin promoted(Builtin b) {
  if (!is_integral(b)) return b;
  IntShape s = shape(b);
  if (rank(b) >= 3 && b != Builtin::wchar_t_ && b != Builtin::char32_t_) return b;
  constexpr std::array<Builtin, 6> order{Builtin::int_,  Builtin::unsigned_int, Builtin::long_,
                                         Builtin::unsigned_long, Builtin::long_long, Builtin::unsigned_long_long};
  for (Builtin t : order) {
    IntShape ts = shape(t);
    bool fits = ts.is_signed ? (s.is_signed ? ts.bits >= s.bits : ts.bits > s.bits) : (!s.is_signed && ts.bits >= s.bits);
    if (fits) return t;
  }
  return b;
}

// [expr.arith.conv] for two promoted integer types: the common type of the usual arithmetic
// conversions.
constexpr Builtin common_integer(Builtin a, Builtin b) {
  if (a == b) return a;
  IntShape sa = shape(a), sb = shape(b);
  if (sa.is_signed == sb.is_signed) return rank(a) >= rank(b) ? a : b;
  Builtin u = sa.is_signed ? b : a;
  Builtin s = sa.is_signed ? a : b;
  if (rank(u) >= rank(s)) return u;
  if (shape(s).bits > shape(u).bits) return s;
  return integer_of_rank(rank(s), false);
}

// ---------------------------------------------------------------- types

enum class TypeKind : u8 { builtin, pointer, array, function };

// The structure behind a type node (cv lives in the handle).
struct TypeRec {
  TypeKind kind = TypeKind::builtin;
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

  QualType builtin(Builtin b) {
    TypeRec r;
    r.builtin = b;
    return node(std::move(r));
  }
  QualType pointer(QualType pointee) {
    TypeRec r;
    r.kind = TypeKind::pointer;
    r.inner = pointee;
    return node(std::move(r));
  }
  // [basic.type.qualifier]: cv on an array type applies to its elements; give cv to the element.
  QualType array(QualType elem, u64 bound) {
    TypeRec r;
    r.kind = TypeKind::array;
    r.inner = elem;
    r.bound = bound;
    return node(std::move(r));
  }
  // `params` are the adjusted parameter types ([dcl.fct]): the caller has decayed arrays and
  // functions and dropped top-level cv.
  QualType function(QualType ret, std::span<const QualType> params, bool variadic, LangLinkage linkage) {
    TypeRec r;
    r.kind = TypeKind::function;
    r.inner = ret;
    r.params.assign(params.begin(), params.end());
    r.variadic = variadic;
    r.linkage = linkage;
    return node(std::move(r));
  }

  const TypeRec& rec(QualType q) const
    pre(ast::type_node_of(q) < recs_.size())
  {
    return recs_[ast::type_node_of(q)];
  }

  static Cv cv(QualType q) { return ast::cv_of(q); }
  static bool is_const(QualType q) { return (std::to_underlying(ast::cv_of(q)) & 1) != 0; }
  static QualType unqualified(QualType q) { return QualType(std::to_underlying(q) & ~7u); }
  // Adds cv; on an array it goes to the element ([basic.type.qualifier]).
  QualType add_cv(QualType q, Cv c) {
    if (c == Cv::none) return q;
    const TypeRec& r = rec(q);
    if (r.kind == TypeKind::array) {
      QualType e = add_cv(r.inner, c);
      return array(e, r.bound);
    }
    if (r.kind == TypeKind::function) return q;
    return ast::with_cv(q, c);
  }
  // The stand-in has no sugar, so every type is canonical.
  QualType canonical(QualType q) const { return q; }

  bool is(QualType q, Builtin b) const { return kind(q) == TypeKind::builtin && rec(q).builtin == b; }
  TypeKind kind(QualType q) const { return rec(q).kind; }
  std::optional<Builtin> builtin_of(QualType q) const {
    if (kind(q) != TypeKind::builtin) return std::nullopt;
    return rec(q).builtin;
  }
  bool is_void(QualType q) const { return is(q, Builtin::void_); }
  bool is_integral(QualType q) const { return builtin_of(q) && sema::is_integral(*builtin_of(q)); }
  bool is_arithmetic(QualType q) const { return builtin_of(q) && sema::is_arithmetic(*builtin_of(q)); }
  bool is_pointer(QualType q) const { return kind(q) == TypeKind::pointer; }
  bool is_scalar(QualType q) const { return is_arithmetic(q) || is_pointer(q) || is(q, Builtin::nullptr_t); }
  bool is_function(QualType q) const { return kind(q) == TypeKind::function; }
  bool is_array(QualType q) const { return kind(q) == TypeKind::array; }
  // [basic.types.general]: an object type is not a function, reference or void type.
  bool is_object(QualType q) const { return !is_function(q) && !is_void(q); }
  QualType pointee(QualType q) const
    pre(is_pointer(q))
  {
    return rec(q).inner;
  }
  // The size in bytes, for pointer arithmetic and layout.
  u64 size_of(QualType q) const {
    const TypeRec& r = rec(q);
    switch (r.kind) {
    case TypeKind::builtin: return r.builtin == Builtin::void_ ? 0 : std::max<u64>(1, shape(r.builtin).bits / 8);
    case TypeKind::pointer: return 8;
    case TypeKind::array: return r.bound * size_of(r.inner);
    case TypeKind::function: return 0;
    }
    return 0;
  }

  // The type as it would be written, for diagnostics.
  std::string spell(QualType q) const { return spell(q, ""); }

  Fp128 fp(QualType q) const { return ins_->types.fp(q); }

private:
  std::string spell(QualType q, std::string inner) const {
    const TypeRec& r = rec(q);
    std::string cv;
    if (is_const(q)) cv += "const ";
    if ((std::to_underlying(ast::cv_of(q)) & 2) != 0) cv += "volatile ";
    switch (r.kind) {
    case TypeKind::builtin:
      return cv + std::string(sema::spelling(r.builtin)) + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    case TypeKind::pointer: {
      std::string p = "*" + (cv.empty() ? "" : " " + cv.substr(0, cv.size() - 1)) + inner;
      const TypeRec& t = rec(r.inner);
      if (t.kind == TypeKind::array || t.kind == TypeKind::function) p = "(" + p + ")";
      return spell(r.inner, p);
    }
    case TypeKind::array: return spell(r.inner, inner + "[" + std::to_string(r.bound) + "]");
    case TypeKind::function: {
      std::string ps;
      for (std::size_t i = 0; i < r.params.size(); ++i) ps += (i ? ", " : "") + spell(r.params[i]);
      if (r.variadic) ps += r.params.empty() ? "..." : ", ...";
      return spell(r.inner, inner + "(" + ps + ")");
    }
    }
    return {};
  }

  QualType node(TypeRec r) {
    Fp128 f = ast::seed("ycxx.sema.Type");
    f = ast::mix(f, std::to_underlying(r.kind));
    f = ast::mix(f, std::to_underlying(r.builtin));
    if (r.kind != TypeKind::builtin) f = ast::mix(f, fp(r.inner));
    f = ast::mix(f, r.bound);
    f = ast::mix(f, r.params.size());
    for (QualType p : r.params) f = ast::mix(f, fp(p));
    f = ast::mix(f, u64(r.variadic) | u64(std::to_underlying(r.linkage)) << 1);
    QualType q = ins_->types.add<QualType>(f);
    if (ast::type_node_of(q) == recs_.size()) recs_.push_back(std::move(r));
    return q;
  }

  ast::Interners* ins_;
  std::deque<TypeRec> recs_;  // a deque: references to records stay valid as types are added
};

// ---------------------------------------------------------------- constants

enum class ValueKind : u8 { integer, string, address, null_pointer };

// The structure behind a constant (§5.5): an integer of an integral type, normalized to the type's
// width and signedness; a string literal object's code units (`VString`); the address of a string
// literal object plus an offset (`VLvalue`); or a null pointer.
struct ValueRec {
  ValueKind kind = ValueKind::integer;
  QualType type{};
  u64 bits = 0;  // integer: two's complement, sign- or zero-extended from the type's width
  std::string units;
  ConstId base = ConstId::none;
  i64 offset = 0;
};

class Values {
public:
  explicit Values(ast::Interners& ins, Types& types) : ins_(&ins), types_(&types) {}

  // An integer of type `t`, wrapped to its width ([conv.integral]).
  ConstId integer(QualType t, u64 bits) {
    t = Types::unqualified(t);
    Builtin b = *types_->builtin_of(t);
    if (b == Builtin::bool_) bits = bits != 0;
    else bits = wrap(bits, shape(b));
    ValueRec r;
    r.type = t;
    r.bits = bits;
    return node(std::move(r));
  }
  // A string literal object of type `t` (an array) holding `units`, one byte per code unit.
  ConstId string(QualType t, std::string units) {
    ValueRec r;
    r.kind = ValueKind::string;
    r.type = t;
    r.units = std::move(units);
    return node(std::move(r));
  }
  ConstId address(QualType pointer, ConstId base, i64 offset) {
    ValueRec r;
    r.kind = ValueKind::address;
    r.type = Types::unqualified(pointer);
    r.base = base;
    r.offset = offset;
    return node(std::move(r));
  }
  ConstId null_pointer(QualType pointer) {
    ValueRec r;
    r.kind = ValueKind::null_pointer;
    r.type = Types::unqualified(pointer);
    return node(std::move(r));
  }

  const ValueRec& rec(ConstId c) const
    pre(std::to_underlying(c) < recs_.size())
  {
    return recs_[std::to_underlying(c)];
  }
  i64 as_signed(ConstId c) const { return i64(rec(c).bits); }

  static u64 wrap(u64 v, IntShape s) {
    if (s.bits >= 64) return v;
    u64 mask = (u64{1} << s.bits) - 1;
    v &= mask;
    if (s.is_signed && (v >> (s.bits - 1)) & 1) v |= ~mask;
    return v;
  }

private:
  ConstId node(ValueRec r) {
    Fp128 f = ast::seed("ycxx.sema.Value");
    f = ast::mix(f, std::to_underlying(r.kind));
    f = ast::mix(f, types_->fp(r.type));
    f = ast::mix(f, r.bits);
    f = ast::mix_bytes(f, r.units);
    f = ast::mix(f, r.base == ConstId::none ? ast::none_fp : ins_->values.fp(r.base));
    f = ast::mix(f, u64(r.offset));
    ConstId c = ins_->values.add<ConstId>(f);
    if (std::to_underlying(c) == recs_.size()) recs_.push_back(std::move(r));
    return c;
  }

  ast::Interners* ins_;
  Types* types_;
  std::deque<ValueRec> recs_;
};

// ---------------------------------------------------------------- entities

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
  std::deque<EntityRec> recs_;
};

// What the front end shares process-wide (§9.1): the AST core's interners and, until #33, these
// tables.
struct World {
  ast::Interners& ins;
  Types types{ins};
  Values values{ins, types};
  Entities entities{ins, types};

  explicit World(ast::Interners& i) : ins(i) {}
  World(const World&) = delete;
};

}  // namespace ycxx::sema
