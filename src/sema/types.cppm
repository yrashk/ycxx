// Sema's view of types, constants and entities. Types and constants are #33's (ycxx.ast: `types`,
// `values`, ycxx.ast.typeops) and this partition only shapes them for sema: by-value records of
// the canonical form using the selected target. Entities are still a STAND-IN for
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

// Width in bits and signedness of an integral type on the target.
struct IntShape {
  u32 bits;
  bool is_signed;
};
inline IntShape shape(const ast::Target& target, Builtin b) { return {ast::width_of(target, b), ast::is_signed(target, b)}; }

// [conv.prom] and [expr.arith.conv] on the target.
inline Builtin promoted(const ast::Target& target, Builtin b, u8 bit_width = 0) { return ast::is_integral(b) ? ast::promoted(target, b, bit_width) : b; }
inline Builtin common_integer(const ast::Target& target, Builtin a, Builtin b) { return *ast::usual_arithmetic(target, a, b); }

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
  placeholder, dep_spec, builtin_spec, member_pointer, pack_index, other
};

// The canonical form of a type, read out by value.
struct TypeRec {
  TypeKind kind = TypeKind::other;
  Builtin builtin = Builtin::void_;
  QualType inner{};  // pointee, element or return type
  QualType member_owner{};  // a pointer-to-member's class; `inner` is the member type
  u64 bound = 0;     // an array's
  std::vector<QualType> params;
  bool variadic = false;
  LangLinkage linkage = LangLinkage::cxx;
  EntityId entity = EntityId::none;  // a class, enumeration or a placeholder's concept
  Cv fn_cv = Cv::none;               // a member function type's cv-qualifier-seq
  ast::RefQual fn_ref = ast::RefQual::none;
  bool fn_noexcept = false;
  bool rvalue_ref = false;           // a reference's kind; `inner` is what it refers to
  ConstId bound_value = ConstId::none;  // an array's bound or a pack index, possibly dependent
  NameId name{};                     // a dependent member type's name; `inner` is its qualifier
  u16 depth = 0;                     // a template parameter's position ([temp.param])
  u16 index = 0;
  bool pack = false;  // a template parameter's; a pack index's: its pack is bound to `args`
  u8 trait = 0;  // a transform's `lex::TypeTrait`; a built-in specialization's `ast::BuiltinTemplate`
  ast::SeqId<ast::TemplateArg> args{};  // a specialization's or a placeholder's written arguments
  bool decltype_auto = false;  // a placeholder's: `decltype(auto)`, not `auto`
};

class Types {
public:
  explicit Types(ast::Interners& ins, const ast::Target& target = ast::aarch64_darwin) : ins_(&ins), target_(&target) {}
  const ast::Target& target() const { return *target_; }

  QualType builtin(Builtin b) { return ast::builtin(*ins_, b); }
  QualType pointer(QualType pointee) { return ast::pointer_to(*ins_, pointee); }
  bool valid_member_owner(QualType owner) const { return is_class(owner) || is_dependent(owner); }
  bool valid_member_type(QualType member) const { return !is_void(member) && !is_reference(member); }
  // [dcl.mptr]: the nested-name-specifier names a possibly incomplete class; no member of that
  // class need have this type. A typedef to a cv-qualified class still names that class.
  QualType member_pointer(QualType owner, QualType member) {
    QualType c = ins_->types.canon(member);
    if (ins_->types.is<ast::TFunction>(c)) {
      auto f = ins_->types.get<ast::TFunction>(c);
      if (f.linkage == LangLinkage::c) {
        // [dcl.link]: C language linkage is ignored for a class member function's type.
        f.linkage = LangLinkage::cxx;
        member = ins_->intern_type(f);
      }
    }
    return ins_->intern_type(ast::TMemberPtr{unqualified(canonical(owner)), member});
  }
  // An array whose bound is value-dependent ([temp.dep.constexpr]).
  QualType array_dependent(QualType elem, ConstId bound) { return ast::array_of(*ins_, *target_, elem, bound); }
  // An array of unknown bound, an incomplete type ([dcl.array]/6).
  QualType array_unknown(QualType elem) { return ast::array_of(*ins_, *target_, elem, ConstId::none); }
  QualType array(QualType elem, u64 bound) {
    return ast::array_of(*ins_, *target_, elem, ast::int_value(*ins_, *target_, Builtin::unsigned_long, i64(bound)));
  }
  // The parameters are adjusted by #33's `function_type` ([dcl.fct]).
  QualType function(QualType ret, std::span<const QualType> params, bool variadic, LangLinkage linkage,
                    Cv cv = Cv::none, ast::RefQual ref = ast::RefQual::none, bool nonthrowing = false) {
    return ast::function_type(*ins_, ret, params,
                              {.variadic = variadic, .cv = cv, .ref = ref, .noexcept_ = nonthrowing, .linkage = linkage});
  }
  QualType class_type(EntityId c) { return ins_->intern_type(ast::TClass{c}); }
  QualType enum_type(EntityId e) { return ins_->intern_type(ast::TEnum{e}); }
  QualType lvalue_ref(QualType t) { return ast::lvalue_ref_to(*ins_, t); }
  QualType rvalue_ref(QualType t) { return ast::rvalue_ref_to(*ins_, t); }
  // [conv.fctptr]: only a nonthrowing function can lose its exception specification.
  bool function_pointer_conversion(QualType from, QualType to) {
    from = canonical(from);
    to = canonical(to);
    if (!is_function(from) || !is_function(to) || !rec(from).fn_noexcept || rec(to).fn_noexcept) return false;
    auto source = ins_->types.get<ast::TFunction>(from);
    const auto target = ins_->types.get<ast::TFunction>(to);
    source.noexcept_ = target.noexcept_;
    return ins_->intern_type(source) == to;
  }
  QualType template_param(u16 depth, u16 index, bool pack = false) {
    return ins_->intern_type(ast::TTemplateParam{depth, index, u8(pack)});
  }
  // `pattern...` ([temp.variadic]).
  QualType pack_expansion(QualType pattern) { return ins_->intern_type(ast::TPackExpansion{pattern}); }
  // `__trait(arg)` of a dependent `arg` (docs/EXTENSIONS.md).
  QualType transform(u8 trait, QualType arg) { return ins_->intern_type(ast::TTransform{trait, arg}); }
  // `T...[I]` while dependent ([dcl.type.pack.index]): `pack` is the pack's own parameter type;
  // with `elems`, that pack is bound to them and `index` is the dependent part.
  QualType pack_index(QualType pack, ConstId index) { return ins_->intern_type(ast::TPackIndex{pack, index, {}, 0}); }
  QualType pack_index(QualType pack, ConstId index, ast::SeqId<ast::TemplateArg> elems) {
    return ins_->intern_type(ast::TPackIndex{pack, index, elems, 1});
  }
  // A placeholder ([dcl.spec.auto.general]); a bare type-constraint has no written arguments.
  QualType placeholder(bool decltype_auto, EntityId concept_ = EntityId::none) {
    return ins_->intern_type(ast::TAuto{u8(decltype_auto), concept_});
  }
  // Whether a declared type holds a placeholder still to be deduced ([dcl.type.auto.deduct]): as
  // the type, or as what it points or refers to, an array's element or a function's return type.
  bool has_placeholder(QualType q) const {
    TypeRec r = rec(q);
    switch (r.kind) {
    case TypeKind::placeholder: return true;
    case TypeKind::pointer:
    case TypeKind::member_pointer:
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
  // A floating-point type ([basic.fundamental]); `float`, `double` and `long double` have values.
  bool is_floating(QualType q) const {
    auto b = builtin_of(q);
    return b && ast::is_floating(*b);
  }
  bool is_standard_floating(QualType q) const {
    auto b = builtin_of(q);
    return b && ast::is_standard_floating(*b);
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
    } else if (t.is<ast::TMemberPtr>(c)) {
      auto p = t.get<ast::TMemberPtr>(c);
      r.kind = TypeKind::member_pointer;
      r.inner = p.member;
      r.member_owner = p.cls;
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
      r.fn_ref = f.ref;
      r.fn_noexcept = f.noexcept_ != ConstId::none && ins_->values.is<ast::VInt>(f.noexcept_) &&
                      ast::int_bits(*ins_, f.noexcept_).lo != 0;
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
      auto p = t.get<ast::TAuto>(c);
      r.kind = TypeKind::placeholder;
      r.decltype_auto = p.decltype_auto != 0;
      r.entity = p.concept_;
      r.args = p.args;
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
    } else if (t.is<ast::TPackIndex>(c)) {
      auto x = t.get<ast::TPackIndex>(c);
      r.kind = TypeKind::pack_index;
      r.inner = x.pack;
      r.bound_value = x.index;
      r.pack = x.bound != 0;  // the pack is bound to `args`
      r.args = x.elems;
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
    case TypeKind::pack_index:
    case TypeKind::transform:
    case TypeKind::dep_spec:
    case TypeKind::builtin_spec: return true;
    case TypeKind::builtin: return r.builtin == Builtin::dependent;
    case TypeKind::array:
      return is_dependent(r.inner) || (r.bound_value != ConstId::none && ins_->values.is_dependent(r.bound_value));
    case TypeKind::pointer:
    case TypeKind::reference: return is_dependent(r.inner);
    case TypeKind::member_pointer: return is_dependent(r.member_owner) || is_dependent(r.inner);
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
  // [basic.fundamental]: the integral and floating-point types. An extended floating-point type
  // ([basic.extended.fp]) has no values yet (#336), so it is not arithmetic here and its uses are
  // diagnosed.
  bool is_arithmetic(QualType q) const {
    auto b = builtin_of(q);
    return b && (ast::is_integral(*b) || ast::is_standard_floating(*b));
  }
  bool is_pointer(QualType q) const { return ast::is_pointer(*ins_, q); }
  bool is_member_pointer(QualType q) const { return ins_->types.is<ast::TMemberPtr>(ins_->types.canon(q)); }
  bool is_function(QualType q) const { return ast::is_function(*ins_, q); }
  bool is_cvref_function(QualType q) const {
    QualType c = ins_->types.canon(q);
    if (!ins_->types.is<ast::TFunction>(c)) return false;
    auto f = ins_->types.get<ast::TFunction>(c);
    return f.cv != Cv::none || f.ref != ast::RefQual::none;
  }
  bool is_array(QualType q) const { return ast::is_array(*ins_, q); }
  // Peel every array extent. A non-array type is returned as given.
  QualType innermost_array_element(QualType q) const {
    while (is_array(q)) q = canonical(rec(q).inner);
    return q;
  }
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
    return is_arithmetic(q) || is_pointer(q) || is_member_pointer(q) || is(q, Builtin::nullptr_t) || is_enum(q);
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
    case TypeKind::pointer:
    case TypeKind::member_pointer: {
      std::string p = r.kind == TypeKind::member_pointer ? spell(r.member_owner) + "::*" : "*";
      p += (cvs.empty() ? "" : " " + cvs.substr(0, cvs.size() - 1)) + inner;
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
      if (r.fn_ref != ast::RefQual::none) q += r.fn_ref == ast::RefQual::lvalue ? " &" : " &&";
      if (r.fn_noexcept) q += " noexcept";
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
    case TypeKind::pack_index: {
      std::string i = ins_->values.is<ast::VDepParam>(r.bound_value)
                          ? std::format("value-parameter-{}-{}", ins_->values.get<ast::VDepParam>(r.bound_value).depth,
                                        ins_->values.get<ast::VDepParam>(r.bound_value).index)
                      : ins_->values.is<ast::VInt>(r.bound_value) ? std::to_string(ast::int_bits(*ins_, r.bound_value).lo)
                                                                  : std::string("…");
      std::string p = r.pack ? std::string("<…>") : spell(r.inner);
      return cvs + p + "...[" + i + "]" + (inner.empty() || inner[0] == '[' ? "" : " ") + inner;
    }
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
  const ast::Target* target_;
};

// ---------------------------------------------------------------- constants

enum class ValueKind : u8 { integer, floating, string, address, null_pointer, other };

// A constant read out by value (§5.5): an integer of an integral type (`VInt`, its low 64 bits),
// a floating value (`VFloat`, its encoding in `bits` and its value in `real`),
// a string literal object's code units (`VString`), the address of one or of an entity
// (`VEntity`) plus an offset (`VLvalue`), or a null pointer (`VNullptr`).
struct ValueRec {
  ValueKind kind = ValueKind::other;
  QualType type{};
  u64 bits = 0;
  double real = 0;
  std::vector<u32> units;
  ConstId base = ConstId::none;
  i64 offset = 0;
};

class Values {
public:
  explicit Values(ast::Interners& ins, const Types& types) : ins_(&ins), target_(&types.target()) {}

  // An integer of type `t`, wrapped to its width ([conv.integral]).
  ConstId integer(QualType t, u64 bits) { return ast::int_value(*ins_, *target_, t, ast::Bits128{bits, 0}); }
  // A value of standard floating type `t`: `v` rounded to its format ([conv.double]).
  ConstId floating(QualType t, double v) { return ast::float_value(*ins_, *target_, t, v); }
  // A string literal object of array type `t`, including its terminating code unit.
  ConstId string(QualType t, std::span<const u32> units) {
    return ins_->intern_value(ast::VString{t, ins_->intern_seq(units)});
  }
  // The bytes of a narrow string for consumers that interpret it as text.
  std::optional<std::string> narrow_string(ConstId c) const {
    if (!ins_->values.is<ast::VString>(c)) return std::nullopt;
    auto s = ins_->values.get<ast::VString>(c);
    auto a = ins_->types.get<ast::TArray>(ins_->types.canon(s.type));
    auto b = ast::builtin_of(*ins_, a.elem);
    if (b != Builtin::char_ && b != Builtin::char8_t_) return std::nullopt;
    auto units = ins_->seqs.elements(s.units);
    std::string text;
    text.reserve(units.size());
    for (u32 unit : units) text.push_back(static_cast<char>(unit));
    return text;
  }
  ConstId address(QualType, ConstId base, i64 offset, std::span<const u32> path = {}) {
    return ins_->intern_value(ast::VLvalue{base, ins_->intern_seq(path), offset});
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
    } else if (v.is<ast::VFloat>(c)) {
      auto f = v.get<ast::VFloat>(c);
      r.kind = ValueKind::floating;
      r.type = f.type;
      r.bits = f.bits.lo;
      r.real = ast::float_double(*ins_, *target_, c);
    } else if (v.is<ast::VString>(c)) {
      auto s = v.get<ast::VString>(c);
      r.kind = ValueKind::string;
      r.type = s.type;
      r.units = ins_->seqs.elements(s.units);
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
  const ast::Target* target_;
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
  type_alias,  // a typedef-name or alias-declaration ([dcl.typedef]): `(scope, name)`
  virtual_table,  // an external Itanium vtable, with its class as `scope`
  deleting_destructor,  // an Itanium D0 entry, with its class as `scope`
  type_info,  // an external Itanium type information object, with its class as `scope`
  base_object_destructor  // an Itanium D2 entry, with its class as `scope`
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
  Fp128 head{};                          // a template head, scope discriminator or builtin class identity
  Fp128 constraints{};                   // a function template's constraints (ast.md §4.3)
  EntityId templ = EntityId::none;       // a specialization's template
  std::vector<ast::TemplateArg> args{};  // a specialization's template arguments
  // A closure type's number among its context's closures of its signature (ast.md §4.3); its
  // `type` is its function call operator's declared type (docs/design/closures.md).
  u32 ordinal = 0;
  // A generic closure's explicit template parameters, which its name declares (`Ty`, `Tn`).
  std::vector<ast::TemplateParamSema> closure_head{};
  // An unnamed type in a class has a lexical ordinal in that scope (Itanium ABI §5.1.5.5).
  std::optional<u32> unnamed_type_ordinal{};
  // An unnamed enumeration's first enumerator and canonical underlying `type` denote it for
  // linkage purposes ([dcl.enum]/12). Neither binds the type's name in source lookup.
  std::optional<NameId> enum_linkage_name{};
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

  // A variable initializer can name the declarator as a closure owner before its type is known.
  EntityId identity(const EntityRec& r) {
    Fp128 f = ast::seed("ycxx.sema.Entity");
    f = ast::mix(f, std::to_underlying(r.kind));
    f = ast::mix(f, ins_->names.fp(r.name));
    if (r.scope != EntityId::none) f = ast::mix(f, ins_->entities.fp(r.scope));
    // A variable's type is not part of its identity; a function's is ([basic.link]).
    if (r.kind == EntityKind::function || r.kind == EntityKind::constructor || r.kind == EntityKind::destructor ||
        r.kind == EntityKind::function_template)
      f = ast::mix(f, types_->fp(types_->canonical(r.type)));
    if (r.kind == EntityKind::variable) f = ast::mix(f, std::to_underlying(r.lang));
    // A static local's number among its function's statics of its name ([stmt.dcl]/3).
    if (r.kind == EntityKind::variable && r.head != Fp128{}) f = ast::mix(f, r.head);
    if (r.kind == EntityKind::function_template || r.kind == EntityKind::class_template ||
        r.kind == EntityKind::variable_template || r.kind == EntityKind::alias_template || r.kind == EntityKind::concept_ ||
        r.kind == EntityKind::namespace_)
      f = ast::mix(f, r.head);
    if (r.kind == EntityKind::function_template) f = ast::mix(f, r.constraints);
    // A closure's `head` includes its signature and ordinal in its scope (docs/design/closures.md).
    // A compiler-owned class uses `head` to distinguish it from declarations of the same name.
    if (r.kind == EntityKind::class_) f = ast::mix(f, r.head);
    if (r.unnamed_type_ordinal)
      f = ast::mix(ast::mix(f, ast::seed("ycxx.sema.unnamed-type-in-class")), *r.unnamed_type_ordinal);
    if (r.enum_linkage_name) {
      f = ast::mix(f, ins_->names.fp(*r.enum_linkage_name));
      f = ast::mix(f, types_->fp(r.type));
    }
    if (r.templ != EntityId::none) {
      f = ast::mix(f, ins_->entities.fp(r.templ));
      for (const ast::TemplateArg& a : r.args) f = mix_arg(f, a);
      // A constrained partial specialization's pattern: its constraints' token fingerprint, so it
      // differs from the specialization with its arguments and from partials constrained otherwise
      // (Fable's decision on #234). Zero, and not mixed, for every other specialization.
      if ((r.kind == EntityKind::class_ || r.kind == EntityKind::variable) && r.constraints != Fp128{})
        f = ast::mix(ast::mix(f, ast::seed("ycxx.sema.constrained-partial")), r.constraints);
    }
    f = ast::mix(f, std::to_underlying(r.linkage));
    if (r.linkage == ast::Linkage::internal)
      f = ast::mix(ast::mix(f, ins_->items.fp(r.first.item)), std::to_underlying(r.first.ord));
    return ins_->entities.add<EntityId>(f);
  }

  EntityId entity(EntityRec r) {
    EntityId e = identity(r);
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
  u8 bit_width = 0;
  bool mutable_ = false;
  bool no_unique_address = false;  // the member is potentially overlapping ([intro.object])
};

// Classification of a defaulted copy affected by an explicit in-class definition. Deletion
// takes precedence over an unsupported or nontrivial subobject operation.
enum class CopyState : u8 { ordinary, trivial, nontrivial, unsupported, deleted };

// What `class_def(C)` (ast.md §3.6) says of a class that member lookup and the layout need, kept
// by class entity. A class is complete once its definition has been analysed
// ([class.mem.general]); completeness at a point is the parser's question (`Program`).
// A member function ([class.mfct]) or static data member ([class.static.data]), which a
// qualified definition outside the class redeclares. Its declaration is an item of each unit that
// defines the class, so the unit keeps it (`Program::member_decl`), not the shared record (#156).
enum class CopyAssignment : u8 { none, trivial, memberwise, deleted, const_member, unsupported };

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
  // A generic lambda's function call operator's template-head ([expr.prim.lambda.closure]/7):
  // its definition is its lambda's, not an item's.
  std::vector<ast::TemplateParamSema> head{};
  std::vector<ast::NodeId> defaults;  // a function's default arguments by parameter, or empty
  CopyAssignment copy_assignment = CopyAssignment::none;
  bool explicit_defaulted_assignment = false;
  bool is_virtual = false;
  bool is_explicit = false;
  EntityId overridden_virtual = EntityId::none;
  bool is_final = false;
  bool is_pure = false;
  bool is_consteval = false;  // an immediate function ([dcl.constexpr])
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
  bool dependent_in_pattern = false;
  // Named by a virtual base-specifier ([class.mi]). The base-clause parser admits none yet (#310).
  bool virtual_ = false;
};

// Layout for access to a peer-owned object. A primary base already supplies the vptr at offset zero.
enum class PeerVirtualLayout : u8 { none, root, primary_base };

// A slot of a primary virtual table (Itanium C++ ABI §2.5.2): a virtual function's final
// overrider, or one of the two entries a virtual destructor occupies (§3.2.3).
enum class VirtualSlotKind : u8 { function, complete_destructor, deleting_destructor };
struct VirtualSlot {
  EntityId function = EntityId::none;  // the final overrider in this class; a destructor's complete entry
  VirtualSlotKind kind = VirtualSlotKind::function;
  bool pure = false;  // [class.abstract]: the slot holds the pure virtual handler
};

// A dynamic class laid out by the general single-inheritance rules (#309): a root whose vptr is
// at offset zero, or a class whose single public nonvirtual base is dynamic and primary. Its
// table extends the primary base's: overriders replace inherited slots, and the declarations
// that override nothing follow in declaration order (Itanium C++ ABI §2.5.2).
struct DynamicClass {
  bool primary_base = false;  // the single base supplies the vptr at offset zero
  // The first non-pure virtual function not inline at the end of the class definition
  // (Itanium C++ ABI §5.2.3): the unit defining it owns the table. Else every user emits it.
  EntityId key_function = EntityId::none;
  std::vector<VirtualSlot> slots;
  std::optional<u32> slot_of(EntityId function, VirtualSlotKind kind = VirtualSlotKind::function) const {
    for (u32 i = 0; i < slots.size(); ++i)
      if (slots[i].function == function && slots[i].kind == kind) return i;
    return std::nullopt;
  }
};

// Eligibility and triviality for [class.prop] and [special]. An absent or deleted operation is
// ineligible. Unknown marks a record whose declarations or subobjects do not establish the fact.
enum class CopyOperation : u8 { unknown, ineligible, eligible_trivial, eligible_nontrivial };

struct TrivialCopyability {
  CopyOperation copy_constructor = CopyOperation::unknown;
  CopyOperation move_constructor = CopyOperation::unknown;
  CopyOperation copy_assignment = CopyOperation::unknown;
  CopyOperation move_assignment = CopyOperation::unknown;
  CopyOperation destructor = CopyOperation::unknown;
  std::optional<bool> destructor_user_provided;  // the selected destructor's first declaration

  std::optional<bool> result() const {
    using enum CopyOperation;
    const std::array operations{copy_constructor, move_constructor, copy_assignment, move_assignment};
    if (destructor == unknown || std::ranges::contains(operations, unknown)) return std::nullopt;
    return destructor == eligible_trivial && std::ranges::contains(operations, eligible_trivial) &&
           !std::ranges::contains(operations, eligible_nontrivial);
  }
};

struct ClassRec {
  bool defined = false;
  bool member_declarations_valid = false;  // no member declaration was lost to an error
  bool is_final = false;  // the class property's value from the definition ([class.pre])
  u8 explicit_alignment = 0;  // a validated class-head requirement stricter than the natural alignment
  bool unapplied_layout = false;  // unresolved layout attributes on this definition or its data members
  // Its direct base classes, in declaration order ([class.derived.general]).
  std::vector<BaseRec> bases;
  // POD for the purposes of layout, as Clang has it (`ast::record_layout`): trivial and
  // standard-layout ([class.prop]).
  bool pod = true;
  bool union_ = false;  // a union ([class.union]): its members share its storage
  bool polymorphic = false;
  bool virtual_destructor = false;
  std::optional<bool> destructor_noexcept;  // the effective specification, when known
  bool destructor_usable = false;          // its declaration and subobject destruction are known to be usable
  bool destructor_dependent = false;       // a closure pattern's captures leave usability unresolved
  bool layout_requires_virtual_support = false;  // it or a subobject is polymorphic
  // It or a subobject is polymorphic outside the general dynamic layout (`dynamic`).
  bool virtual_layout_unsupported = false;
  PeerVirtualLayout peer_virtual_layout = PeerVirtualLayout::none;
  // The sole abstract primary base of a class admitted for plain automatic default initialization.
  EntityId peer_automatic_primary_base = EntityId::none;
  // A polymorphic class outside the peer profiles above, with its own table and constructors.
  std::optional<DynamicClass> dynamic;
  // The implicitly defined default constructor of a dynamic class without user-declared ones.
  EntityId implicit_default_ctor = EntityId::none;
  std::vector<FieldRec> fields;        // in declaration order
  std::vector<MemberDeclRec> members;  // member functions and static data members
  std::vector<EntityId> pure_final_overriders;
  bool is_abstract() const { return !pure_final_overriders.empty(); }
  // Special members ([special]): whether a constructor is user-declared; the destructor and the
  // copy constructor when they are not trivial (user-declared, or implicit and synthesized),
  // else none; whether the copy constructor is deleted.
  bool user_ctors = false;
  // A closure type ([expr.prim.lambda.closure]): not an aggregate, its captures' members unnamed
  // (/1); with a lambda-capture, its default constructor and copy assignment are deleted (/13).
  bool closure = false;
  bool default_deleted = false;
  // Written zero-parameter defaulting, with the completed class's supported no-op proof.
  EntityId defaulted_default_ctor = EntityId::none;
  bool trivial_defaulted_default_ctor = false;
  EntityId declared_destructor = EntityId::none;  // retains the access of a written trivial destructor
  EntityId destructor = EntityId::none;
  EntityId destructor_declaration() const {
    return declared_destructor == EntityId::none ? destructor : declared_destructor;
  }
  EntityId copy_ctor = EntityId::none;
  bool copy_deleted = false;
  std::vector<EntityId> defaulted_copy_declarations;
  CopyState defaulted_copy = CopyState::ordinary;
  bool defaulted_copy_const = true;  // the implicit parameter when it depends on a defaulted copy
  // The move constructor ([class.copy.ctor]/8): whether one is declared and not deleted, and
  // whether it is then not trivial (user-provided, or implicit and a member's move is not).
  bool move_usable = false;
  bool move_nontrivial = false;
  bool copy_nontrivial() const {
    return (!copy_deleted && layout_requires_virtual_support) || copy_ctor != EntityId::none ||
           defaulted_copy == CopyState::nontrivial || defaulted_copy == CopyState::unsupported;
  }

  TrivialCopyability trivial_copyability;
  std::optional<bool> trivially_copyable() const {
    return defined ? trivial_copyability.result() : std::nullopt;
  }

  // [class.prop]. The recorded special-member facts cover the supported classes.
  std::optional<bool> implicit_lifetime() const {
    if (!defined || !member_declarations_valid) return std::nullopt;
    const auto& facts = trivial_copyability;
    if (facts.destructor_user_provided == true) return false;
    using enum CopyOperation;
    if (facts.destructor == eligible_trivial &&
        (facts.copy_constructor == eligible_trivial || facts.move_constructor == eligible_trivial ||
         trivial_defaulted_default_ctor)) return true;

    if (facts.destructor_user_provided == false && bases.empty()) {
      const bool aggregate = !user_ctors && !closure && !polymorphic &&
          std::ranges::all_of(fields, [&](const FieldRec& field) { return access_of(field.entity) == ast::Access::public_; });
      if (aggregate) return true;
      if (facts.destructor == ineligible) return false;
    }
    return std::nullopt;
  }

  // Nested enumerations and type aliases ([class.mem]): their types by name, with their entities;
  // the unscoped enumerations' enumerators are members of the class.
  std::vector<NestedType> types;
  // Each member's access ([class.access.spec]), by entity: data members, member functions,
  // constructors, static data members and nested types.
  std::vector<std::pair<EntityId, ast::Access>> access;
  // Its friends ([class.friend]): functions, classes and class templates, by entity.
  std::vector<EntityId> friends;
  // The unqualified friend function declarations ADL can find through this class.
  std::vector<ast::MemberRec> adl_friends;

  // Whether `e` is a friend: one of `friends`, or a specialization of a friend class template,
  // whichever of its partial specializations it was instantiated from ([temp.friend]/1).
  bool befriends(EntityId e, const Entities& entities) const {
    if (e == EntityId::none) return false;
    if (std::ranges::contains(friends, e)) return true;
    const EntityId templ = entities.rec(e).templ;
    return templ != EntityId::none && std::ranges::contains(friends, templ);
  }

  const MemberDeclRec* implicit_copy_assignment() const {
    const auto copy = std::ranges::find_if(members, [](const MemberDeclRec& m) {
      return m.copy_assignment != CopyAssignment::none && !m.explicit_defaulted_assignment;
    });
    return copy == members.end() ? nullptr : &*copy;
  }

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
  bool defined = false;  // whether a definition supplied the enumerator list
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
// its declaration and layout information is complete. Member defaults can finish against that record.
class Classes {
public:
  explicit Classes(ast::Interners& ins, Types& types) : ins_(&ins), types_(&types) {}
  Classes(const Classes&) = delete;

  // A source declaration's class-key is available before a definition is complete.
  void record_class_key(EntityId c, bool is_union) {
    class_keys_.try_emplace(std::to_underlying(c), is_union);
  }
  std::optional<bool> is_union_class(EntityId c) const {
    if (auto it = class_keys_.find(std::to_underlying(c)); it != class_keys_.end()) return it->second;
    if (const ClassRec* r = find(c)) return r->union_;  // a synthesized class
    return std::nullopt;
  }

  // Keep incomplete recovery records available to sema without exporting them as definitions.
  const ClassRec& publish(EntityId c, ClassRec r, const std::function<void(ClassRec&)>& finish = {}) {
    auto [it, fresh] = recs_.try_emplace(std::to_underlying(c), std::move(r));
    if (fresh && it->second.defined) {
      class_order_.push_back(c);
      has_virtual_layouts_ |= it->second.layout_requires_virtual_support;
      if (finish) finish(it->second);
    }
    return it->second;
  }
  // The opaque form supplies the type's shape. A definition also supplies its enumerators.
  const EnumRec& publish_enum(EntityId e, EnumRec r) {
    auto [it, fresh] = enums_.try_emplace(std::to_underlying(e), std::move(r));
    if (fresh) enum_order_.push_back(e);
    else if (!it->second.defined && r.defined) it->second = std::move(r);
    return it->second;
  }
  // Defined classes and recorded enumerations, in publication order.
  std::span<const EntityId> classes() const { return class_order_; }
  std::span<const EntityId> enums() const { return enum_order_; }
  // A class record, including one retained for error recovery.
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

  // [class.prop]: compute the source type property from complete declarations in this unit.
  // Stored members and bases need complete types; a missing record cannot supply a layout fact.
  std::optional<bool> standard_layout(EntityId root) const {
    struct Facts {
      std::optional<bool> standard_layout;
      std::set<EntityId> bases;
      EntityId declaring_class = EntityId::none;
      // The class types in M(X), with true for a certain member. A non-first potentially
      // overlapping member of unproved size might also contribute.
      std::map<EntityId, bool> initial_member_types;
      bool has_nonzero_subobject = false;

      bool accept_declaring_class(EntityId owner) {
        if (owner == EntityId::none) return true;
        if (declaring_class == EntityId::none) declaring_class = owner;
        return declaring_class == owner;
      }

      void include_initial_members(const Facts& from, bool certain) {
        for (const auto& [type, present] : from.initial_member_types)
          initial_member_types[type] |= certain && present;
      }
    };

    // In-progress entries have no proof. A re-entrant or malformed subobject graph cannot
    // turn a partly assembled record into a positive classification.
    std::map<EntityId, Facts> memo;
    const auto facts_of = [&](this auto&& visit, EntityId cls) -> const Facts& {
      const auto [entry, inserted] = memo.try_emplace(cls);
      if (!inserted) return entry->second;
      const ClassRec* record = find(cls);
      if (!record || !record->defined || !record->member_declarations_valid) return entry->second;
      const auto reject = [&]() -> const Facts& {
        entry->second.standard_layout = false;
        return entry->second;
      };
      // BaseRec contains only nonvirtual edges. The base-clause parser rejects virtual bases.
      if (record->polymorphic) return reject();

      Facts facts;
      bool proved = true;
      if (!record->fields.empty()) facts.declaring_class = cls;
      for (const BaseRec& base : record->bases) {
        const Facts& subobject = visit(base.cls);
        if (subobject.standard_layout == false || !facts.accept_declaring_class(subobject.declaring_class))
          return reject();
        if (!facts.bases.insert(base.cls).second) return reject();
        for (EntityId indirect : subobject.bases)
          if (!facts.bases.insert(indirect).second) return reject();
        proved &= subobject.standard_layout.has_value();
        facts.has_nonzero_subobject |= subobject.has_nonzero_subobject;
        // The first data member can be inherited. The base type itself is not in M(X).
        facts.include_initial_members(subobject, true);
      }

      const auto& fields = record->fields;
      if (!fields.empty()) {
        const ast::Access access = record->access_of(fields.front().entity);
        for (const FieldRec& field : fields) {
          if (record->access_of(field.entity) != access) return reject();
          const QualType element = types_->innermost_array_element(field.type);
          if (types_->is_reference(element)) return reject();
          if (types_->is_dependent(element)) {
            proved = false;
            continue;
          }
          if (!types_->is_class(element)) {
            const bool scalar = types_->is_scalar(element) || types_->is_floating(element);
            proved &= scalar;
            facts.has_nonzero_subobject |= scalar;
            continue;
          }
          const EntityId type = types_->class_of(element);
          const Facts& member = visit(type);
          if (member.standard_layout == false) return reject();
          proved &= member.standard_layout.has_value();
          // Arrays and non-overlapping members have nonzero size. A class member can also
          // prove nonzero size through its subobjects ([intro.object]).
          const bool nonzero = !field.no_unique_address || types_->is_array(field.type) || member.has_nonzero_subobject;
          facts.has_nonzero_subobject |= nonzero;
          const bool certain = record->union_ || &field == &fields.front();
          if (certain || !nonzero) {
            // Only class types can collide with a base. Skip the array types along the path
            // to their element. Extend M with the member's M, which excludes its base types.
            facts.initial_member_types[type] |= certain;
            facts.include_initial_members(member, certain);
          }
        }
      }

      for (const auto& [type, certain] : facts.initial_member_types) {
        if (!facts.bases.contains(type)) continue;
        if (certain) return reject();
        proved = false;
      }
      if (proved) facts.standard_layout = true;
      entry->second = std::move(facts);
      return entry->second;
    };
    return facts_of(root).standard_layout;
  }

  bool has_virtual_layouts() const { return has_virtual_layouts_; }
  bool virtual_layout_unsupported(QualType t) const {
    if (!has_virtual_layouts()) return false;
    auto r = types_->rec(t);
    while (r.kind == TypeKind::array) r = types_->rec(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    return c && c->virtual_layout_unsupported;
  }
  bool layout_requires_virtual_support(QualType t) const {
    if (!has_virtual_layouts()) return false;
    auto r = types_->rec(t);
    while (r.kind == TypeKind::array) r = types_->rec(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    return c && c->layout_requires_virtual_support;
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
    return {c->copy_nontrivial(), c->copy_deleted};
  }
  CopyState defaulted_copy_of(QualType t) const {
    const TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return defaulted_copy_of(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    return c ? c->defaulted_copy : CopyState::ordinary;
  }
  // Whether moving a `t` calls a constructor that is not trivial: its move constructor, or its
  // copy constructor when it has none ([over.match.ctor]).
  bool move_nontrivial(QualType t) const {
    TypeRec r = types_->rec(t);
    if (r.kind == TypeKind::array) return move_nontrivial(r.inner);
    const ClassRec* c = r.kind == TypeKind::class_ ? find(r.entity) : nullptr;
    if (!c) return false;
    return c->move_usable ? c->move_nontrivial : c->copy_nontrivial();
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
    if (c->layout_requires_virtual_support || c->destructor != EntityId::none || c->copy_nontrivial()) return false;
    if (rule == ast::CallTriviality::apple) return true;
    if (c->move_usable && c->move_nontrivial) return false;
    return !c->copy_deleted || c->move_usable;
  }

private:
  ast::Interners* ins_;
  Types* types_;
  std::map<u32, bool> class_keys_;  // whether the parsed class-key names a union
  std::map<u32, ClassRec> recs_;  // node-based: a published record never moves
  std::map<u32, EnumRec> enums_;
  std::vector<EntityId> class_order_, enum_order_;
  bool has_virtual_layouts_ = false;
};

// The C library functions builtins call (`lex::BuiltinFunction::library`, `World::string_function`),
// with the C library's signatures. The search functions return `char*` from a `const char*`, as in
// C: the C++ library's const-correct overloads are its own functions ([cstring.syn]).
enum class CType : u8 { int_, size, void_pointer, const_void_pointer, char_pointer, const_char_pointer };
// `constant`: what constant evaluation computes for the builtin's call (`ast::Call::builtin`); a
// function without one is not a constant expression.
struct StringFunction {
  std::string_view name;
  CType result;
  std::array<CType, 3> params;
  u8 arity = 3;
  ast::LibraryBuiltin constant = ast::LibraryBuiltin::none;
};
inline constexpr std::array string_functions{
    StringFunction{"memchr", CType::void_pointer, {CType::const_void_pointer, CType::int_, CType::size}, 3,
                   ast::LibraryBuiltin::memchr},
    StringFunction{"memcmp", CType::int_, {CType::const_void_pointer, CType::const_void_pointer, CType::size}, 3,
                   ast::LibraryBuiltin::memcmp},
    StringFunction{"memcpy", CType::void_pointer, {CType::void_pointer, CType::const_void_pointer, CType::size}},
    StringFunction{"memmove", CType::void_pointer, {CType::void_pointer, CType::const_void_pointer, CType::size}},
    StringFunction{"memset", CType::void_pointer, {CType::void_pointer, CType::int_, CType::size}},
    StringFunction{"strchr", CType::char_pointer, {CType::const_char_pointer, CType::int_}, 2},
    StringFunction{"strcmp", CType::int_, {CType::const_char_pointer, CType::const_char_pointer}, 2},
    StringFunction{"strpbrk", CType::char_pointer, {CType::const_char_pointer, CType::const_char_pointer}, 2},
    StringFunction{"strrchr", CType::char_pointer, {CType::const_char_pointer, CType::int_}, 2},
    StringFunction{"strstr", CType::char_pointer, {CType::const_char_pointer, CType::const_char_pointer}, 2},
};
constexpr const StringFunction* string_function_named(std::string_view name) {
  for (const StringFunction& f : string_functions)
    if (f.name == name) return &f;
  return nullptr;
}
// Every library builtin has a signature.
static_assert(std::ranges::all_of(lex::builtin_functions, [](const lex::BuiltinFunctionInfo& b) {
  return b.function != lex::BuiltinFunction::library || string_function_named(lex::library_function_name(b));
}));

// The ordinary implicit allocation signatures at global scope ([basic.stc.dynamic]).
// The single-object and array operators share these shapes and the target's size_t.
enum class AllocationFunction : u8 {
  allocate, deallocate, deallocate_sized, allocate_aligned, deallocate_aligned, deallocate_sized_aligned
};

constexpr bool allocates(AllocationFunction f) {
  return f == AllocationFunction::allocate || f == AllocationFunction::allocate_aligned;
}

constexpr std::string_view allocation_name(AllocationFunction f, bool array = false) {
  if (allocates(f)) return array ? "operator new[]" : "operator new";
  return array ? "operator delete[]" : "operator delete";
}

constexpr bool is_deallocation_name(std::string_view n) {
  return n == allocation_name(AllocationFunction::deallocate) || n == allocation_name(AllocationFunction::deallocate, true);
}

constexpr bool is_allocating_name(std::string_view n) {
  return n == allocation_name(AllocationFunction::allocate) || n == allocation_name(AllocationFunction::allocate, true);
}

constexpr bool is_allocation_name(std::string_view n) {
  return is_allocating_name(n) || is_deallocation_name(n);
}

constexpr bool has_deallocation_size(AllocationFunction f) {
  return f == AllocationFunction::deallocate_sized || f == AllocationFunction::deallocate_sized_aligned;
}

constexpr bool allocation_has_alignment(AllocationFunction f) {
  return f == AllocationFunction::allocate_aligned || f == AllocationFunction::deallocate_aligned ||
         f == AllocationFunction::deallocate_sized_aligned;
}

// What the front end shares process-wide (§9.1): the AST core's interners, and sema's views.
// Class and enumeration records are each unit's (`Classes`).
struct World {
  ast::Interners& ins;
  const ast::Target& target;
  Types types{ins, target};
  Values values{ins, types};
  Entities entities{ins, types};

  explicit World(ast::Interners& i, const ast::Target& t = ast::aarch64_darwin) : ins(i), target(t) {
    if (target.va_list == ast::VaList::sysv_x86_64)
      sysv_va_list_tag_ = entities.entity({.kind = EntityKind::class_, .name = ins.names.intern("__va_list_tag"),
                                          .head = ast::seed("ycxx.sema.builtin.sysv_va_list_tag")});
    types.class_name = [this](EntityId e) {
      const EntityRec& r = entities.rec(e);
      if (r.unnamed_type_ordinal) return std::string("(unnamed union)");
      if (r.enum_linkage_name) return std::string("(unnamed enum)");
      std::string n(ins.names.spelling(r.name));
      return n.empty() ? std::string("(lambda)") : n;  // a closure type ([expr.prim.lambda.closure])
    };
    types.dep_name_spelling = [this](NameId n) { return std::string(ins.names.spelling(n)); };
    types.class_dependent = [this](EntityId e) {
      return std::ranges::any_of(entities.rec(e).args, [&](const ast::TemplateArg& a) { return arg_dependent(a); });
    };
  }
  World(const World&) = delete;

  // The interners give repeated requests and written declarations the same identity. Construct
  // the implicit declaration on demand, so a unit that never names it needs no shared state.
  ast::DeclSema allocation_function(AllocationFunction f, bool array = false) {
    QualType size = types.builtin(target.size_type);
    QualType void_type = types.builtin(Builtin::void_);
    QualType void_pointer = types.pointer(void_type);
    const bool allocating = allocates(f);
    QualType params[3] = {allocating ? size : void_pointer};
    unsigned count = 1;
    if (has_deallocation_size(f)) params[count++] = size;
    if (allocation_has_alignment(f)) params[count++] = types.enum_type(alignment_enum());
    QualType type = types.function(allocating ? void_pointer : void_type, std::span(params).first(count),
                                   false, LangLinkage::cxx, Cv::none, ast::RefQual::none, !allocating);
    return implicit_function(entities.entity({.kind = EntityKind::function, .name = ins.names.intern(allocation_name(f, array)),
                                              .type = type}),
                             type);
  }

  // The C library function a builtin calls (docs/EXTENSIONS.md), declared at global scope with
  // C language linkage and the C library's signature (`string_functions`). Its exception
  // specification is the target's `<string.h>`'s, so a written declaration or definition of the
  // function is the same entity, with one symbol ([dcl.link]): glibc's `__THROW` is
  // `noexcept(true)` in C++, the macOS SDK's declarations have none.
  ast::DeclSema string_function(const StringFunction& f) {
    const auto c_type = [&](CType t) {
      const QualType void_type = types.builtin(Builtin::void_);
      const QualType char_type = types.builtin(Builtin::char_);
      switch (t) {
        case CType::int_: return types.builtin(Builtin::int_);
        case CType::size: return types.builtin(target.size_type);
        case CType::void_pointer: return types.pointer(void_type);
        case CType::const_void_pointer: return types.pointer(types.add_cv(void_type, Cv::const_));
        case CType::char_pointer: return types.pointer(char_type);
        case CType::const_char_pointer: return types.pointer(types.add_cv(char_type, Cv::const_));
      }
      std::unreachable();
    };
    std::vector<QualType> params;
    for (CType t : std::span(f.params).first(f.arity)) params.push_back(c_type(t));
    const bool nonthrowing = target.operating_system == ast::OperatingSystem::linux_;
    const QualType type = types.function(c_type(f.result), params, false, LangLinkage::c, Cv::none,
                                         ast::RefQual::none, nonthrowing);
    ast::DeclSema d = implicit_function(
        entities.entity({.kind = EntityKind::function, .name = ins.names.intern(f.name), .type = type, .lang = LangLinkage::c}),
        type);
    d.has_written_noexcept = nonthrowing;
    return d;
  }

  ast::DeclSema implicit_function(EntityId entity, QualType type) {
    ast::DeclSema d{};
    d.entity = entity;
    d.type = type;
    d.kind = ast::DeclKind::function;
    d.linkage = ast::Linkage::external;
    d.attributes = ins.intern_seq(std::span<const ast::NodeId>{});
    d.constraint = ast::NodeId::none;
    d.constraints = ConstId::none;
    d.contracts = ast::NodeId::none;
    d.params = ins.intern_seq(std::span<const ast::ParamSema>{});
    d.template_params = ins.intern_seq(std::span<const ast::TemplateParamSema>{});
    return d;
  }

  static constexpr std::array allocation_functions{
      AllocationFunction::allocate, AllocationFunction::deallocate, AllocationFunction::deallocate_sized,
      AllocationFunction::allocate_aligned, AllocationFunction::deallocate_aligned, AllocationFunction::deallocate_sized_aligned,
  };

  bool matches_allocation_function(std::string_view name, QualType written) {
    const bool array = name.ends_with("[]");
    const auto arity = types.rec(written).params.size();
    return std::ranges::any_of(allocation_functions, [&](AllocationFunction f) {
      return allocation_name(f, array) == name && arity == 1u + has_deallocation_size(f) + allocation_has_alignment(f) &&
             types.canonical(allocation_function(f, array).type) == types.canonical(written);
    });
  }

  // Written overloads with a final tag are separate from the usual implicit signatures
  // ([basic.stc.dynamic.allocation], [basic.stc.dynamic.deallocation]). A reference does not
  // require a definition of the class or union it names.
  bool matches_tagged_allocation_function(std::string_view name, QualType written, bool has_noexcept) {
    const bool allocating = is_allocating_name(name);
    if (!allocating && !is_deallocation_name(name)) return false;
    const TypeRec fn = types.rec(written);
    if (fn.kind != TypeKind::function || fn.variadic || fn.linkage != LangLinkage::cxx ||
        fn.fn_cv != Cv::none || fn.fn_ref != ast::RefQual::none || !fn.fn_noexcept ||
        (allocating && !has_noexcept) || (fn.params.size() != 2 && fn.params.size() != 3))
      return false;

    const QualType void_type = types.builtin(Builtin::void_);
    const QualType void_pointer = types.pointer(void_type);
    if (fn.inner != (allocating ? void_pointer : void_type) ||
        fn.params.front() != (allocating ? types.builtin(target.size_type) : void_pointer))
      return false;

    const TypeRec ref = types.rec(fn.params.back());
    if (ref.kind != TypeKind::reference || ref.rvalue_ref || Types::cv(ref.inner) != Cv::const_) return false;
    const TypeRec tag = types.rec(ref.inner);
    if (tag.kind != TypeKind::class_ || types.is_dependent(ref.inner) ||
        ins.names.spelling(entities.rec(tag.entity).name).empty())
      return false;
    return fn.params.size() == 2 || fn.params[1] == types.enum_type(alignment_enum());
  }

  // [new.delete.placement]: the written non-allocating library signatures. They are separate
  // from the implicit, replaceable allocation functions.
  bool matches_reserved_placement_function(std::string_view name, QualType written) {
    if (!is_allocation_name(name)) return false;
    const TypeRec fn = types.rec(types.canonical(written));
    if (fn.kind != TypeKind::function || fn.variadic || fn.linkage != LangLinkage::cxx ||
        fn.fn_cv != Cv::none || fn.fn_ref != ast::RefQual::none || !fn.fn_noexcept || fn.params.size() != 2)
      return false;

    const bool allocating = !is_deallocation_name(name);
    const QualType void_type = types.builtin(Builtin::void_);
    const QualType void_pointer = types.pointer(void_type);
    return fn.inner == (allocating ? void_pointer : void_type) &&
           fn.params.front() == (allocating ? types.builtin(target.size_type) : void_pointer) &&
           fn.params.back() == void_pointer;
  }

  void install_alignment_enum(Classes& classes) {
    // This opaque shape supports implicit users of the type. Source lookup still needs a written
    // declaration, and a written definition supplies its own record.
    classes.publish_enum(alignment_enum(), {.scoped = true, .fixed = true, .underlying = types.builtin(target.size_type),
                                           .promotion = std::nullopt, .enumerators = {}});
  }

  QualType builtin_va_list() {
    switch (target.va_list) {
    case ast::VaList::char_pointer: return types.pointer(types.builtin(Builtin::char_));
    case ast::VaList::sysv_x86_64: return types.array(types.class_type(sysv_va_list_tag_), 1);
    }
    std::unreachable();
  }

  void install_builtin_classes(Classes& classes) {
    if (sysv_va_list_tag_ == EntityId::none) return;
    // System V AMD64 psABI §3.5.7. The record is complete in every Linux translation unit and
    // has no namespace declaration. Its ordinary class layout gives the array's size and alignment.
    ClassRec rec;
    rec.defined = true;
    rec.move_usable = true;
    rec.destructor_noexcept = true;
    rec.destructor_usable = true;
    QualType offset = types.builtin(Builtin::unsigned_int);
    QualType area = types.pointer(types.builtin(Builtin::void_));
    auto field = [&](std::string_view spelling, QualType type) {
      NameId name = ins.names.intern(spelling);
      EntityId entity = entities.entity({.kind = EntityKind::data_member, .name = name, .type = type,
                                         .scope = sysv_va_list_tag_});
      rec.fields.push_back({name, type, entity});
    };
    field("gp_offset", offset);
    field("fp_offset", offset);
    field("overflow_arg_area", area);
    field("reg_save_area", area);
    rec.member_declarations_valid = true;
    classes.publish(sysv_va_list_tag_, std::move(rec));
  }

  // Whether a template argument depends on a template parameter; a pack, whether an element does.
  bool arg_dependent(const ast::TemplateArg& a) const {
    if (a.kind == ast::TemplateArgKind::type) return types.is_dependent(a.type);
    if (a.kind == ast::TemplateArgKind::value) return ins.values.is_dependent(a.value);
    if (a.kind == ast::TemplateArgKind::template_) return a.templ == EntityId::none;
    return std::ranges::any_of(ins.seqs.elements(a.elems), [&](const ast::TemplateArg& x) { return arg_dependent(x); });
  }

private:
  EntityId alignment_enum() {
    // These are the ordinary named namespace and enum identities, shared with source declarations.
    const auto ns = entities.entity({.kind = EntityKind::namespace_, .name = ins.names.intern("std")});
    return entities.entity({.kind = EntityKind::enum_, .name = ins.names.intern("align_val_t"), .scope = ns});
  }

  EntityId sysv_va_list_tag_ = EntityId::none;
};

}  // namespace ycxx::sema
