// Linkage names (Itanium C++ ABI §5.1, "External Names"), as the object file spells them: Mach-O
// prefixes every C-level name with `_`.
//
// α subset (#39): entities of the global namespace whose types are fundamental types, pointers,
// cv-qualifiers and global-namespace classes (δ, #86). `main`, functions with C language linkage and variables are not mangled
// ([dcl.link], Itanium §5.1.2: a global-namespace variable's name is its identifier); other
// functions are `_Z <source-name> <bare-function-type>`, with substitutions for repeated
// qualified and pointer types (§5.1.10).
export module ycxx.pipeline:mangle;

import std;
import ycxx.ast;
import ycxx.lex;

export namespace ycxx::pipeline {

namespace ast = ycxx::ast;

// A function template specialization (§5.1.2, §5.1.5): its <template-args>, then its template's
// signature, return type first, with each template parameter as a <template-param> (§5.1.5.8).
// `pattern` is the template's function type; `args` the specialization's arguments.
struct TemplateSpec {
  ast::QualType pattern{};
  std::vector<ast::TemplateArg> args;
  // The template's parameters, for Clang's <template-param-decl> prefixes (`Tn`, `Tt`); none
  // leaves them out.
  std::vector<ast::TemplateParamSema> params{};
  // By argument: whether a template argument's head is equivalent to its parameter's, so that
  // Clang writes no `Tt` for it.
  std::vector<std::uint8_t> same_heads{};
};
// A function a closure type is local to: what its <encoding> is mangled from, as for its symbol.
struct LocalFunction {
  std::string name;
  ast::QualType type{};
  ast::EntityId scope = ast::EntityId::none;
  bool special = false;
  std::optional<TemplateSpec> spec;
};

// Fails, naming the type, on what the α subset does not mangle yet.
// How a class, enumeration or namespace is named: its identifier ("(anonymous)" for an unnamed
// namespace), and the class or namespace it is in, or none for the global namespace.
struct ClassName {
  std::string name;
  ast::EntityId scope = ast::EntityId::none;
  // A class template specialization's template and arguments (§5.1.5.7).
  ast::EntityId templ = ast::EntityId::none;
  std::vector<ast::TemplateArg> args;
  bool is_class = false;  // a class, not a namespace: its member functions have `this`
  // A closure type (docs/design/closures.md): its function call operator's declared type, whose
  // parameters are its <lambda-sig>, its number among its context's closures of that signature,
  // and the function it is local to (`scope`), as a <local-name> (§5.1.6, §5.1.8).
  bool closure = false;
  ast::QualType closure_type{};
  std::vector<ast::TemplateParamSema> closure_head{};  // its explicit template parameters
  std::uint32_t closure_number = 0;
  std::shared_ptr<const LocalFunction> local{};
  std::optional<std::uint32_t> unnamed_type_ordinal{};
};
using ClassNames = std::function<ClassName(ast::EntityId)>;

// `scope` is the class or namespace a function is a member of, none for the global namespace.
// Direct members of ::std use an <unscoped-name> with St (§5.1.5 and §5.1.10).
std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes = {},
                                                         ast::EntityId scope = ast::EntityId::none,
                                                         bool special = false);

// `special`: `name` is a <ctor-dtor-name> (`C1`), a constructor template's specialization.
std::expected<std::string, std::string> mangled_specialization(const ast::Interners& in, std::string_view name,
                                                               const TemplateSpec& spec, const ClassNames& classes = {},
                                                               ast::EntityId scope = ast::EntityId::none, bool special = false);

// <substitution> for the candidate at `index`: `S_`, then `S0_` … `SZ_`, `S10_` … (§5.1.10).
std::string substitution(std::size_t index);

// The symbol of an entity: the mangled name or identifier with the platform prefix, if any.
std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes = {},
                                                    ast::EntityId scope = ast::EntityId::none,
                                                    bool special = false, const TemplateSpec* spec = nullptr,
                                                    bool leading_underscore = true);

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

// Itanium §5.1.5.2, <builtin-type>.
std::optional<std::string_view> builtin_code(ast::Builtin b) {
  using enum ast::Builtin;
  switch (b) {
    case void_: return "v";
    case nullptr_t: return "Dn";
    case bool_: return "b";
    case char_: return "c";
    case signed_char: return "a";
    case unsigned_char: return "h";
    case wchar_t_: return "w";
    case char8_t_: return "Du";
    case char16_t_: return "Ds";
    case char32_t_: return "Di";
    case short_: return "s";
    case unsigned_short: return "t";
    case int_: return "i";
    case unsigned_int: return "j";
    case long_: return "l";
    case unsigned_long: return "m";
    case long_long: return "x";
    case unsigned_long_long: return "y";
    case float_: return "f";
    case double_: return "d";
    case long_double: return "e";
    default: return std::nullopt;
  }
}

// <seq-id>: base 36 with digits and upper-case letters.
std::string substitution(std::size_t index) {
  if (index == 0) return "S_";
  std::size_t n = index - 1;
  std::string digits;
  do {
    std::size_t d = n % 36;
    digits.insert(digits.begin(), char(d < 10 ? '0' + d : 'A' + d - 10));
    n /= 36;
  } while (n);
  return "S" + digits + "_";
}

std::optional<std::string_view> operator_code(std::string_view op, std::size_t arity);

class Mangler {
 public:
  Mangler(const ast::Interners& in, const ClassNames& classes) : in_(in), classes_(classes) {}
  // The depth of the head of the template whose specialization is mangled: its parameters are
  // `T_`, `T0_`, …
  ast::u16 own_depth = 0;

  // <type> (§5.1.5), recording substitution candidates as they complete.
  std::expected<std::string, std::string> type(ast::QualType q) {
    q = in_.canonical(q);
    ast::Cv cv = ast::cv_of(q);
    ast::QualType bare = ast::without_cv(q);
    std::string inner;
    if (auto b = ast::builtin_of(in_, bare)) {
      auto code = builtin_code(*b);
      if (!code) return std::unexpected("a parameter of an unsupported fundamental type");
      inner = *code;
      if (cv == ast::Cv::none) return inner;  // builtin types are never substitution candidates
    }
    std::string qualifiers;  // <CV-qualifiers>: r V K
    if (std::to_underlying(cv) & 2) qualifiers += "V";
    if (std::to_underlying(cv) & 1) qualifiers += "K";
    if (inner.empty()) {
      if (in_.types.is<ast::TPointer>(in_.types.canon(bare))) {
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto p = type(ast::pointee_of(in_, bare));
        if (!p) return p;
        inner = "P" + *p;
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TMemberPtr>(in_.types.canon(bare))) {
        // <pointer-to-member-type> ::= M <class type> <member type> (Itanium §5.1.5.7).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto p = in_.types.get<ast::TMemberPtr>(in_.types.canon(bare));
        auto owner = type(p.cls);
        if (!owner) return owner;
        auto member = type(p.member);
        if (!member) return member;
        inner = "M" + *owner + *member;
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TLRef>(in_.types.canon(bare)) || in_.types.is<ast::TRRef>(in_.types.canon(bare))) {
        // <type> ::= R <type> | O <type>, a substitution candidate (§5.1.5.4).
        if (auto s = find(full_key(bare))) return *s;
        ast::QualType c = in_.types.canon(bare);
        bool rv = in_.types.is<ast::TRRef>(c);
        auto r = type(rv ? in_.types.get<ast::TRRef>(c).referee : in_.types.get<ast::TLRef>(c).referee);
        if (!r) return r;
        candidates_.push_back(full_key(bare));
        return (rv ? "O" : "R") + *r;
      } else if (in_.types.is<ast::TArray>(in_.types.canon(bare))) {
        // <array-type> ::= A <dimension number> _ <element type>, a substitution candidate
        // (§5.1.5.6).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto a = in_.types.get<ast::TArray>(in_.types.canon(bare));
        // The dimension is a number, or a template parameter's <template-param>, which is an
        // <expression> here and not a substitution candidate.
        std::string dim;
        if (a.bound != ast::ConstId::none && in_.values.is<ast::VInt>(a.bound)) {
          dim = std::format("{}", in_.values.get<ast::VInt>(a.bound).bits.lo);
        } else if (a.bound != ast::ConstId::none && in_.values.is<ast::VDepParam>(a.bound) &&
                   in_.values.get<ast::VDepParam>(a.bound).depth == 0) {
          auto i = in_.values.get<ast::VDepParam>(a.bound).index;
          dim = i == 0 ? "T_" : std::format("T{}_", i - 1);
        } else {
          return std::unexpected("an array of unknown or dependent bound");
        }
        auto e = type(a.elem);
        if (!e) return e;
        inner = std::format("A{}_{}", dim, *e);
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TAuto>(in_.types.canon(bare))) {
        // A placeholder, as a template's declared return type keeps it: the builtin types `Da`
        // (auto) and `Dc` (decltype(auto)), not substitution candidates (§5.1.5.2).
        inner = in_.types.get<ast::TAuto>(in_.types.canon(bare)).decltype_auto ? "Dc" : "Da";
      } else if (in_.types.is<ast::TDepName>(in_.types.canon(bare))) {
        // `typename Q::n`: <nested-name> N <prefix> <source-name> E, a substitution candidate
        // (§5.1.5.3).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto d = in_.types.get<ast::TDepName>(in_.types.canon(bare));
        auto p = type(d.qualifier);
        if (!p) return p;
        inner = "N" + *p + source_name(in_.names.spelling(d.name)) + "E";
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TTransform>(in_.types.canon(bare))) {
        // A type-trait built-in of a dependent type: a vendor-extended type `u <source-name>` with
        // its argument, `u11__remove_cvIT_E`, as Clang spells it (docs/EXTENSIONS.md).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto x = in_.types.get<ast::TTransform>(in_.types.canon(bare));
        auto a = type(x.arg);
        if (!a) return a;
        std::string_view n = lex::type_trait_info(lex::TypeTrait(x.trait)).name;
        inner = std::format("u{}{}I{}E", n.size(), n, *a);
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TDepSpec>(in_.types.canon(bare))) {
        // `TT<args>`: <template-template-param> <template-args>, the parameter a candidate, then
        // the whole (§5.1.5.8, §5.1.10).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto d = in_.types.get<ast::TDepSpec>(in_.types.canon(bare));
        // The parameter's own type, which sema interned when the template was declared.
        auto p = type(const_cast<ast::Interners&>(in_).intern_type(ast::TTemplateParam{d.depth, d.index, d.pack}));
        if (!p) return p;
        auto a = template_args(in_.seqs.elements(d.args));
        if (!a) return a;
        inner = *p + *a;
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TBuiltinSpec>(in_.types.canon(bare))) {
        // `__make_integer_seq<args>` while dependent: as Clang, a global class template of that
        // name, the name and the whole candidates (docs/EXTENSIONS.md).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto d = in_.types.get<ast::TBuiltinSpec>(in_.types.canon(bare));
        constexpr std::uint64_t name_key = (std::uint64_t{3} << 32) | 0;
        std::string n;
        if (auto s = find(name_key)) {
          n = *s;
        } else {
          n = source_name("__make_integer_seq");
          candidates_.push_back(name_key);
        }
        auto a = template_args(in_.seqs.elements(d.args));
        if (!a) return a;
        inner = n + *a;
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TPackExpansion>(in_.types.canon(bare))) {
        // A pack expansion `Dp <type>`, a substitution candidate (§5.1.5).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto e = type(in_.types.get<ast::TPackExpansion>(in_.types.canon(bare)).pattern);
        if (!e) return e;
        inner = "Dp" + *e;
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TTemplateParam>(in_.types.canon(bare))) {
        // <template-param> ::= T_ | T <parameter-2 non-negative number> _, a substitution
        // candidate (§5.1.5.8, §5.1.10). Function templates of namespace scope have one level.
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto p = in_.types.get<ast::TTemplateParam>(in_.types.canon(bare));
        // A template template parameter's own parameter, in Clang's <template-param-decl>:
        // `TL <level-1> _ [<index-1>] _`.
        // Its depth relative to the mangled template's own head (a member template's is inside its
        // class's, whose parameters are substituted).
        if (p.depth < own_depth) return std::unexpected("a template parameter of an enclosing template-head");
        int level = p.depth - own_depth;
        if (level != 0) inner = p.index == 0 ? std::format("TL{}__", level - 1) : std::format("TL{}_{}_", level - 1, p.index - 1);
        else inner = p.index == 0 ? "T_" : std::format("T{}_", p.index - 1);
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TFunction>(in_.types.canon(bare))) {
        // Function type qualifiers and the non-throwing exception specification are encoded
        // before F, and the ref-qualifier before E (Itanium §5.1.5.3).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto f = in_.types.get<ast::TFunction>(in_.types.canon(bare));
        if (f.linkage != ast::LangLinkage::cxx) return std::unexpected("an extern \"C\" function type");
        auto sig = signature(f, true);
        if (!sig) return sig;
        if (std::to_underlying(f.cv) & 2) inner += "V";
        if (std::to_underlying(f.cv) & 1) inner += "K";
        if (f.noexcept_ != ast::ConstId::none && in_.values.is<ast::VInt>(f.noexcept_) &&
            in_.values.get<ast::VInt>(f.noexcept_).bits.lo != 0) inner += "Do";
        inner += "F" + *sig;
        if (f.ref != ast::RefQual::none) inner += f.ref == ast::RefQual::lvalue ? "R" : "O";
        inner += "E";
        candidates_.push_back(full_key(bare));
      } else if (in_.types.is<ast::TClass>(in_.types.canon(bare)) || in_.types.is<ast::TEnum>(in_.types.canon(bare))) {
        // <class-enum-type> ::= <name>: a global-namespace class is its <source-name>, a
        // substitution candidate (§5.1.5.3, §5.1.10).
        auto n = class_name(bare);
        if (!n) return n;
        inner = *n;
      } else {
        return std::unexpected("a parameter type other than a fundamental, pointer or class type");
      }
    }
    return qualify(qualifiers, inner, q);
  }

  // A global-namespace class's <source-name>, or its substitution once it has been mangled.
  std::expected<std::string, std::string> class_name(ast::QualType cls) {
    ast::QualType c = in_.types.canon(cls);
    return entity_name(in_.types.is<ast::TEnum>(c) ? in_.types.get<ast::TEnum>(c).entity : in_.types.get<ast::TClass>(c).entity);
  }
  // A class's or enumeration's <name> as a type: its <source-name>, or, inside a class or a
  // namespace, a <nested-name> whose prefixes are candidates first (§5.1.5.3, §5.1.10). Classes
  // and namespaces are keyed by entity.
  std::expected<std::string, std::string> entity_name(ast::EntityId e) {
    std::uint64_t key = (std::uint64_t{1} << 32) | std::to_underlying(e);
    if (auto s = find(key)) return *s;
    ClassName n = classes_ ? classes_(e) : ClassName{};
    if (n.closure) {
      // A closure type as a type: the <local-name> `Z <encoding> E <closure-type-name>` (§5.1.6).
      if (!n.local) return std::unexpected("a closure type whose function is unknown");
      auto l = local_encoding(*n.local);
      if (!l) return l;
      auto sig = closure_sig(n);
      if (!sig) return sig;
      std::string r = "Z" + *l + "EUl" + *sig + "E" + (n.closure_number ? std::to_string(n.closure_number - 1) : "") + "_";
      candidates_.push_back(key);
      return r;
    }
    if (n.name.empty() && !n.unnamed_type_ordinal) return std::unexpected("a class whose name is unknown");
    if (n.templ != ast::EntityId::none) {
      // The template's name and the specialization are candidates (§5.1.5.3, §5.1.10).
      auto r = specialization_prefix(e, n);
      if (!r) return r;
      return delimit_name(*r, n.scope);
    }
    std::string r = class_component(n);
    if (n.scope != ast::EntityId::none) {
      auto p = prefix(n.scope);
      if (!p) return p;
      r = delimit_name(*p + r, n.scope);
    }
    candidates_.push_back(key);
    return r;
  }
  // The <prefix> naming class or namespace `e` inside a <nested-name>: its enclosing prefix, then
  // its own name, each a candidate as it completes.
  std::expected<std::string, std::string> prefix(ast::EntityId e) {
    std::uint64_t key = (std::uint64_t{1} << 32) | std::to_underlying(e);
    if (auto s = find(key)) return *s;
    ClassName n = classes_ ? classes_(e) : ClassName{};
    // The predefined abbreviation for ::std never gets a numbered substitution (§5.1.10).
    if (is_global_std(n)) return "St";
    if (n.closure) {
      // <closure-type-name> ::= Ul <lambda-sig> E [<number>] _, its parameter types or `v`; the
      // n-th closure of one signature in its context is numbered n − 2 (§5.1.8). Its context is
      // the <local-name> before the nested-name it begins.
      auto sig = closure_sig(n);
      if (!sig) return sig;
      std::string r = "Ul" + *sig + "E" + (n.closure_number ? std::to_string(n.closure_number - 1) : "") + "_";
      candidates_.push_back(key);
      return r;
    }
    if (n.name.empty() && !n.unnamed_type_ordinal) return std::unexpected("a scope whose name is unknown");
    if (n.templ != ast::EntityId::none) return specialization_prefix(e, n);
    std::string r;
    if (n.scope != ast::EntityId::none) {
      auto p = prefix(n.scope);
      if (!p) return p;
      r = *p;
    }
    r += class_component(n);
    candidates_.push_back(key);
    return r;
  }
  // Global names and direct members of ::std are <unscoped-name>s (§5.1.5).
  std::string delimit_name(std::string components, ast::EntityId scope) const {
    if (scope == ast::EntityId::none || (classes_ && is_global_std(classes_(scope)))) return components;
    return "N" + components + "E";
  }
  // <unnamed-type-name> ::= Ut [<number>] _ (Itanium ABI §5.1.5.5).
  static std::string class_component(const ClassName& n) {
    if (!n.unnamed_type_ordinal) return source_name(n.name);
    std::uint32_t ordinal = *n.unnamed_type_ordinal;
    return "Ut" + (ordinal ? std::to_string(ordinal - 1) : "") + "_";
  }
  // <lambda-sig> ::= <template-param-decl>* <parameter type>+: a generic lambda's explicit
  // template parameters, then its parameters' types (§5.1.8; `Ty` as Clang has it).
  std::expected<std::string, std::string> closure_sig(const ClassName& n) {
    std::string out;
    for (const ast::TemplateParamSema& p : n.closure_head) {
      auto d = param_decl(p);
      if (!d) return d;
      out += *d;
    }
    auto sig = signature(in_.types.get<ast::TFunction>(in_.types.canon(n.closure_type)), false);
    if (!sig) return sig;
    return out + *sig;
  }
  // A function's <encoding> (§5.1.2), without `_Z`: its <name>, a <nested-name> in a class or
  // namespace, then its parameter types. A member of a closure type is in a <local-name>, the
  // closure's function's <encoding> first (§5.1.6).
  std::expected<std::string, std::string> encoding(std::string_view name, ast::QualType type, ast::EntityId scope, bool special) {
    ast::QualType c = in_.types.canon(type);
    if (!in_.types.is<ast::TFunction>(c)) return std::unexpected("not a function type");
    auto f = in_.types.get<ast::TFunction>(c);
    // An <operator-name> (§5.1.5.1) or a <source-name>; `+`, `-`, `*` and `&` mangle by arity,
    // `this` counted for a member.
    ClassName sc = scope != ast::EntityId::none && classes_ ? classes_(scope) : ClassName{};
    std::size_t arity = in_.seqs.size(f.params) + (sc.is_class ? 1 : 0);
    std::string unqualified = std::format("{}{}", name.size(), name);
    if (name.starts_with("operator") && name.size() > 8) {
      auto code = operator_code(name.substr(8), arity);
      if (!code) return std::unexpected(std::format("the operator function '{}'", name));
      unqualified = *code;
    }
    std::string out = unqualified;
    if (scope != ast::EntityId::none) {
      std::string local;
      if (sc.closure) {
        if (!sc.local) return std::unexpected("a closure type whose function is unknown");
        auto l = local_encoding(*sc.local);
        if (!l) return l;
        local = "Z" + *l + "E";
      }
      // <nested-name> ::= N [<CV-qualifiers>] <prefix> <unqualified-name> E, the member function's
      // cv-qualifiers in the order r V K (§5.1.5.3).
      auto p = prefix(scope);
      if (!p) return p;
      std::string cv = std::string(std::to_underlying(f.cv) & 2 ? "V" : "") + (std::to_underlying(f.cv) & 1 ? "K" : "");
      // A <ctor-dtor-name> (`C1`, `D1`) stands as it is; other names are <source-name>s.
      out = local + delimit_name(cv + *p + (special ? std::string(name) : unqualified), scope);
    }
    auto sig = signature(f, false);
    if (!sig) return sig;
    return out + *sig;
  }
  // A function template specialization's <encoding>, without `_Z` (§5.1.2).
  // `special`: `name` is a <ctor-dtor-name> (`C1`), a constructor template's specialization.
  std::expected<std::string, std::string> specialization_encoding(std::string_view name, const TemplateSpec& spec, ast::EntityId scope,
                                                                  bool special = false) {
    // Template parameters are mangled relative to the specialization's own head (a member
    // template's is inside its class's), for its encoding only.
    ast::u16 saved = own_depth;
    if (!spec.params.empty()) own_depth = spec.params[0].depth;
    auto r = specialization_encoding_at(name, spec, scope, special);
    own_depth = saved;
    return r;
  }
  std::expected<std::string, std::string> specialization_encoding_at(std::string_view name, const TemplateSpec& spec, ast::EntityId scope,
                                                                     bool special) {
    ast::QualType c = in_.types.canon(spec.pattern);
    // A generic lambda's function call operator, a member function template of a closure type:
    // `N [<CV-qualifiers>] <prefix> <unqualified-name> <template-args> E` and its signature, in a
    // <local-name> when the class is a closure type (§5.1.5.3, §5.1.6).
    ClassName sc = scope != ast::EntityId::none && classes_ ? classes_(scope) : ClassName{};
    if (sc.closure && in_.types.is<ast::TFunction>(c)) {
      auto f = in_.types.get<ast::TFunction>(c);
      std::string local;
      if (sc.closure) {
        if (!sc.local) return std::unexpected("a closure type whose function is unknown");
        auto l = local_encoding(*sc.local);
        if (!l) return l;
        local = "Z" + *l + "E";
      }
      auto p = prefix(scope);
      if (!p) return p;
      std::string unqualified = std::format("{}{}", name.size(), name);
      if (name.starts_with("operator") && name.size() > 8) {
        auto code = operator_code(name.substr(8), in_.seqs.size(f.params) + 1);
        if (!code) return std::unexpected(std::format("the operator function '{}'", name));
        unqualified = *code;
      }
      candidates_.push_back((std::uint64_t{2} << 32) | candidates_.size());  // the <template-prefix>
      auto args = spec.params.empty() ? template_args(spec.args) : own_template_args(spec.args, spec.params, spec.same_heads);
      if (!args) return args;
      auto sig = signature(f, true);
      if (!sig) return sig;
      std::string cv = std::string(std::to_underlying(f.cv) & 2 ? "V" : "") + (std::to_underlying(f.cv) & 1 ? "K" : "");
      return local + "N" + cv + *p + unqualified + *args + "E" + *sig;
    }
    auto n = template_name(name, scope, special);
    if (!n) return n;
    auto args = spec.params.empty() ? template_args(spec.args) : own_template_args(spec.args, spec.params, spec.same_heads);
    if (!args) return args;
    // A variable template specialization: its name and arguments, no type (§5.1.2).
    if (!in_.types.is<ast::TFunction>(c)) return delimit_name(*n + *args, scope);
    auto sig = signature(in_.types.get<ast::TFunction>(c), !special);  // a constructor has no return type
    if (!sig) return sig;
    // In a <nested-name>, a member function template's cv-qualifiers follow the `N`.
    ast::Cv fcv = in_.types.get<ast::TFunction>(c).cv;
    std::string cv = std::string(std::to_underlying(fcv) & 2 ? "V" : "") + (std::to_underlying(fcv) & 1 ? "K" : "");
    return delimit_name(cv + *n + *args, scope) + *sig;
  }
  std::expected<std::string, std::string> local_encoding(const LocalFunction& f) {
    if (f.spec) return specialization_encoding(f.name, *f.spec, f.scope);
    return encoding(f.name, f.type, f.scope, f.special);
  }

  // <bare-function-type>: the return type when `with_return`, then the parameter types, `v` for
  // none (§5.1.5.3).
  std::expected<std::string, std::string> signature(const ast::TFunction& f, bool with_return) {
    std::string out;
    if (with_return) {
      auto r = type(f.ret);
      if (!r) return r;
      out += *r;
    }
    auto params = in_.seqs.elements(f.params);
    if (params.empty() && !f.variadic) out += "v";
    for (ast::QualType p : params) {
      auto s = type(p);
      if (!s) return s;
      out += *s;
    }
    if (f.variadic) out += "z";
    return out;
  }

  // <template-args> ::= I <template-arg>+ E (§5.1.5.7): a type, or an integral value as an
  // <expr-primary> `L <type> <value> E`, `n` for a negative value.
  std::expected<std::string, std::string> template_args(std::span<const ast::TemplateArg> args) {
    std::string out = "I";
    for (const ast::TemplateArg& a : args) {
      auto m = template_arg(a);
      if (!m) return m;
      out += *m;
    }
    return out + "E";
  }
  // One <template-arg>: a type, an argument pack `J … E`, or a value (§5.1.5.7).
  std::expected<std::string, std::string> template_arg(const ast::TemplateArg& a) {
    std::string out;
    {
      if (a.kind == ast::TemplateArgKind::pack) {
        out = "J";
        for (const ast::TemplateArg& e : in_.seqs.elements(a.elems)) {
          auto m = template_arg(e);
          if (!m) return m;
          out += *m;
        }
        return out + "E";
      }
      if (a.kind == ast::TemplateArgKind::type) return type(a.type);
      // A template: its <name>, the name a candidate (§5.1.5.7); while dependent, its parameter.
      if (a.kind == ast::TemplateArgKind::template_) return a.templ != ast::EntityId::none ? template_entity_name(a.templ) : type(a.type);
      // A non-type pack expansion: `sp` and its pattern, an <expression> (§5.1.6).
      if (in_.values.is<ast::VDepExpansion>(a.value)) {
        ast::ConstId pat = in_.values.get<ast::VDepExpansion>(a.value).pattern;
        if (!in_.values.is<ast::VDepParam>(pat) || in_.values.get<ast::VDepParam>(pat).depth != 0)
          return std::unexpected("a pack expansion that is not of a template parameter");
        auto i = in_.values.get<ast::VDepParam>(pat).index;
        return i == 0 ? std::string("XspT_E") : std::format("XspT{}_E", i - 1);
      }
      // `sizeof...(P)` of a template parameter pack: `sZ <template-param>` (§5.1.6).
      if (in_.values.is<ast::VDepSizeofPack>(a.value) && in_.values.get<ast::VDepSizeofPack>(a.value).depth == 0) {
        auto i = in_.values.get<ast::VDepSizeofPack>(a.value).index;
        return i == 0 ? std::string("XsZT_E") : std::format("XsZT{}_E", i - 1);
      }
      // A template parameter as an argument: an <expression>, `X T_ E` (§5.1.5.7, §5.1.6).
      if (in_.values.is<ast::VDepParam>(a.value) && in_.values.get<ast::VDepParam>(a.value).depth == 0) {
        auto i = in_.values.get<ast::VDepParam>(a.value).index;
        return i == 0 ? std::string("XT_E") : std::format("XT{}_E", i - 1);
      }
      auto b = ast::builtin_of(in_, a.type);
      auto code = b ? builtin_code(*b) : std::nullopt;
      if (!code || !in_.values.is<ast::VInt>(a.value)) return std::unexpected("a template argument that is not a type or an integer");
      auto v = in_.values.get<ast::VInt>(a.value).bits;
      bool neg = ast::is_signed(ast::aarch64_darwin, *b) && ast::is_negative(v);
      // The fundamental integral types are at most 64 bits wide on the target.
      std::uint64_t mag = neg ? ~v.lo + 1 : v.lo;
      out += std::format("L{}{}{}E", *code, neg ? "n" : "", mag);
    }
    return out;
  }

  // A class or alias template as a template argument: its <source-name>, in a namespace or class a
  // <nested-name>; keyed as a specialization's template-name is (§5.1.5.7, §5.1.10).
  std::expected<std::string, std::string> template_entity_name(ast::EntityId t) {
    std::uint64_t key = (std::uint64_t{1} << 32) | std::to_underlying(t);
    if (auto s = find(key)) return *s;
    ClassName n = classes_ ? classes_(t) : ClassName{};
    if (n.name.empty()) return std::unexpected("a template whose name is unknown");
    std::string r;
    if (n.scope != ast::EntityId::none) {
      auto p = prefix(n.scope);
      if (!p) return p;
      r = delimit_name(*p + source_name(n.name), n.scope);
    } else {
      r = source_name(n.name);
    }
    candidates_.push_back(key);
    return r;
  }
  // Whether a type names a template parameter.
  bool dependent(ast::QualType q) {
    ast::QualType c = in_.types.canon(ast::without_cv(in_.canonical(q)));
    if (in_.types.is<ast::TTemplateParam>(c) || in_.types.is<ast::TDepName>(c) || in_.types.is<ast::TPackExpansion>(c) ||
        in_.types.is<ast::TTransform>(c) || in_.types.is<ast::TDepSpec>(c) || in_.types.is<ast::TBuiltinSpec>(c))
      return true;
    if (in_.types.is<ast::TPointer>(c)) return dependent(in_.types.get<ast::TPointer>(c).pointee);
    if (in_.types.is<ast::TMemberPtr>(c)) {
      auto p = in_.types.get<ast::TMemberPtr>(c);
      return dependent(p.cls) || dependent(p.member);
    }
    if (in_.types.is<ast::TLRef>(c)) return dependent(in_.types.get<ast::TLRef>(c).referee);
    if (in_.types.is<ast::TRRef>(c)) return dependent(in_.types.get<ast::TRRef>(c).referee);
    if (in_.types.is<ast::TArray>(c)) return dependent(in_.types.get<ast::TArray>(c).elem);
    return false;
  }
  // Clang's <template-param-decl> for parameter `p` (an extension beyond the published ABI):
  // `Ty`, `Tn <type>`, `Tt <decl>* E`, `Tp <decl>` for a pack.
  std::expected<std::string, std::string> param_decl(const ast::TemplateParamSema& p) {
    std::string r = p.pack ? "Tp" : "";
    if (p.kind == ast::TemplateArgKind::type) return r + "Ty";
    if (p.kind == ast::TemplateArgKind::value) {
      auto t = type(p.type);
      if (!t) return t;
      return r + "Tn" + *t;
    }
    r += "Tt";
    for (const ast::TemplateParamSema& q : in_.seqs.elements(p.params)) {
      auto d = param_decl(q);
      if (!d) return d;
      r += *d;
    }
    return r + "E";
  }
  // A function template specialization's own <template-args>, with Clang's prefixes: `Tn <type>`
  // before a value whose parameter's type is dependent, `Tt <decl>* E` before a template whose head
  // is not the parameter's ([temp.arg.template]).
  std::expected<std::string, std::string> own_template_args(std::span<const ast::TemplateArg> args,
                                                            std::span<const ast::TemplateParamSema> params,
                                                            const std::vector<std::uint8_t>& same_heads) {
    std::string out = "I";
    for (std::size_t i = 0; i < args.size(); ++i) {
      const ast::TemplateArg& a = args[i];
      if (i < params.size()) {
        // Clang's choices, as its output has them: a value whose parameter's type is dependent, or
        // an empty pack of values; a template whose head is not the parameter's (for a pack, its
        // first element's), or an empty pack of templates. Type parameters get none.
        const ast::TemplateParamSema& p = params[i];
        bool empty = p.pack && a.kind == ast::TemplateArgKind::pack && in_.seqs.size(a.elems) == 0;
        bool same = i < same_heads.size() && same_heads[i];
        bool decl = p.kind == ast::TemplateArgKind::value    ? dependent(p.type) || empty
                    : p.kind == ast::TemplateArgKind::template_ ? empty || !same
                                                                : false;
        if (decl) {
          auto d = param_decl(p);
          if (!d) return d;
          out += *d;
        }
      }
      auto m = template_arg(a);
      if (!m) return m;
      out += *m;
    }
    return out + "E";
  }

  // A function template's name before its <template-args>: an <unscoped-template-name>, or in a
  // namespace a <template-prefix> after the namespace's <prefix>; a substitution candidate either
  // way (§5.1.5.3, §5.1.10).
  std::expected<std::string, std::string> template_name(std::string_view name, ast::EntityId scope, bool special = false) {
    std::string r;
    if (scope != ast::EntityId::none) {
      auto p = prefix(scope);
      if (!p) return p;
      r = *p;
    }
    r += special ? std::string(name) : source_name(name);
    candidates_.push_back((std::uint64_t{2} << 32) | candidates_.size());
    return r;
  }

  // A class template specialization `e` as a <prefix>: its scope's prefix, the template's name (a
  // candidate, keyed by the template), the <template-args>, and the whole (a candidate).
  std::expected<std::string, std::string> specialization_prefix(ast::EntityId e, const ClassName& n) {
    std::string r;
    std::uint64_t tkey = (std::uint64_t{1} << 32) | std::to_underlying(n.templ);
    if (auto s = find(tkey)) {
      r = *s;
    } else {
      if (n.scope != ast::EntityId::none) {
        auto p = prefix(n.scope);
        if (!p) return p;
        r = *p;
      }
      r += source_name(n.name);
      candidates_.push_back(tkey);
    }
    auto a = template_args(n.args);
    if (!a) return a;
    r += *a;
    candidates_.push_back((std::uint64_t{1} << 32) | std::to_underlying(e));
    return r;
  }

  // <source-name>; an unnamed namespace is `_GLOBAL__N_1`, unique to its translation unit
  // (§5.1.4.4).
  static std::string source_name(std::string_view n) {
    if (n == "(anonymous)") n = "_GLOBAL__N_1";
    return std::format("{}{}", n.size(), n);
  }

 private:

  static bool is_global_std(const ClassName& scope) {
    return !scope.is_class && scope.scope == ast::EntityId::none && scope.name == "std";
  }

  // A qualified type is a candidate of its own, after its unqualified part.
  std::string qualify(const std::string& qualifiers, const std::string& inner, ast::QualType q) {
    if (qualifiers.empty()) return inner;
    if (auto s = find(full_key(q))) return *s;
    candidates_.push_back(full_key(q));
    return qualifiers + inner;
  }
  // Keys: a type is its canonical handle, an entity (a class or namespace) and a template name
  // have bit 32 or 33 set.
  std::uint64_t full_key(ast::QualType q) const { return std::to_underlying(in_.canonical(q)); }
  std::optional<std::string> find(std::uint64_t key) const {
    for (std::size_t i = 0; i < candidates_.size(); ++i)
      if (candidates_[i] == key) return substitution(i);
    return std::nullopt;
  }

  const ast::Interners& in_;
  const ClassNames& classes_;
  std::vector<std::uint64_t> candidates_;  // in order of completion
};

// Itanium §5.1.5.1 <operator-name>: the code of `operator<op>` taking `arity` operands.
std::optional<std::string_view> operator_code(std::string_view op, std::size_t arity) {
  if (op == " new") return "nw";
  if (op == " delete") return "dl";
  static constexpr std::pair<std::string_view, std::string_view> binary[] = {
      {"+", "pl"},  {"-", "mi"},   {"*", "ml"},   {"&", "an"},  {"/", "dv"},  {"%", "rm"},  {"|", "or"},
      {"^", "eo"},  {"=", "aS"},   {"+=", "pL"},  {"-=", "mI"}, {"*=", "mL"}, {"/=", "dV"}, {"%=", "rM"},
      {"&=", "aN"}, {"|=", "oR"},  {"^=", "eO"},  {"<<", "ls"}, {">>", "rs"}, {"<<=", "lS"}, {">>=", "rS"},
      {"==", "eq"}, {"!=", "ne"},  {"<", "lt"},   {">", "gt"},  {"<=", "le"}, {">=", "ge"}, {"<=>", "ss"},
      {"&&", "aa"}, {"||", "oo"},  {",", "cm"},   {"->*", "pm"}, {"->", "pt"}, {"()", "cl"}, {"[]", "ix"},
      {"!", "nt"},  {"~", "co"},   {"++", "pp"},  {"--", "mm"}};
  if (arity == 1) {
    if (op == "+") return "ps";
    if (op == "-") return "ng";
    if (op == "*") return "de";
    if (op == "&") return "ad";
  }
  for (auto [s, code] : binary)
    if (s == op) return code;
  return std::nullopt;
}

std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes,
                                                         ast::EntityId scope, bool special) {
  Mangler m(in, classes);
  auto e = m.encoding(name, type, scope, special);
  if (!e) return e;
  return "_Z" + *e;
}

std::expected<std::string, std::string> mangled_specialization(const ast::Interners& in, std::string_view name,
                                                               const TemplateSpec& spec, const ClassNames& classes,
                                                               ast::EntityId scope, bool special) {
  Mangler m(in, classes);
  auto e = m.specialization_encoding(name, spec, scope, special);
  if (!e) return e;
  return "_Z" + *e;
}

std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes, ast::EntityId scope,
                                                    bool special, const TemplateSpec* spec, bool leading_underscore) {
  const std::string prefix = leading_underscore ? "_" : "";
  if (spec) {
    auto m = mangled_specialization(in, name, *spec, classes, scope, special);
    if (!m) return m;
    return prefix + *m;
  }
  if (scope != ast::EntityId::none && !is_function) {
    // A static data member or a namespace's variable (§5.1.2).
    Mangler m(in, classes);
    auto scope_name = m.prefix(scope);
    if (!scope_name) return scope_name;
    return prefix + "_Z" + m.delimit_name(*scope_name + Mangler::source_name(name), scope);
  }
  // [basic.start.main]: only the global namespace's `main` is the program's.
  if (!is_function || (name == "main" && scope == ast::EntityId::none) || lang == ast::LangLinkage::c)
    return prefix + std::string(name);
  auto m = mangled_function(in, name, type, classes, scope, special);
  if (!m) return m;
  return prefix + *m;
}

}  // namespace ycxx::pipeline
