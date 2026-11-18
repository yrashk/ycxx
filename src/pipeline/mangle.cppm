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

export namespace ycxx::pipeline {

namespace ast = ycxx::ast;

// Fails, naming the type, on what the α subset does not mangle yet.
// How a class is named: its identifier (the global namespace only, so far).
// How a class or enumeration is named: its identifier, and the class it is nested in, or none.
struct ClassName {
  std::string name;
  ast::EntityId scope = ast::EntityId::none;
};
using ClassNames = std::function<ClassName(ast::EntityId)>;

// `scope` is the class type of a member, mangled as a <nested-name> (§5.1.5.3).
std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes = {},
                                                         std::optional<ast::QualType> scope = std::nullopt,
                                                         bool special = false);

// <substitution> for the candidate at `index`: `S_`, then `S0_` … `SZ_`, `S10_` … (§5.1.10).
std::string substitution(std::size_t index);

// The symbol of an entity: `_` and the mangled name or the identifier.
std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes = {},
                                                    std::optional<ast::QualType> scope = std::nullopt,
                                                    bool special = false);

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

class Mangler {
 public:
  Mangler(const ast::Interners& in, const ClassNames& classes) : in_(in), classes_(classes) {}

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
      } else if (in_.types.is<ast::TLRef>(in_.types.canon(bare)) || in_.types.is<ast::TRRef>(in_.types.canon(bare))) {
        // <type> ::= R <type> | O <type>, a substitution candidate (§5.1.5.4).
        if (auto s = find(full_key(bare))) return *s;
        ast::QualType c = in_.types.canon(bare);
        bool rv = in_.types.is<ast::TRRef>(c);
        auto r = type(rv ? in_.types.get<ast::TRRef>(c).referee : in_.types.get<ast::TLRef>(c).referee);
        if (!r) return r;
        candidates_.push_back(full_key(bare));
        return (rv ? "O" : "R") + *r;
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
  // A class's or enumeration's <name>: its <source-name>, or, nested in a class, a
  // <nested-name> whose prefix is a candidate first (§5.1.5.3). Classes are keyed by entity.
  std::expected<std::string, std::string> entity_name(ast::EntityId e) {
    std::uint32_t key = 0x80000000u | std::to_underlying(e);
    if (auto s = find(key)) return *s;
    ClassName n = classes_ ? classes_(e) : ClassName{};
    if (n.name.empty()) return std::unexpected("a class whose name is unknown");
    std::string r = std::format("{}{}", n.name.size(), n.name);
    if (n.scope != ast::EntityId::none) {
      auto prefix = entity_name(n.scope);
      if (!prefix) return prefix;
      r = "N" + *prefix + r + "E";
    }
    candidates_.push_back(key);
    return r;
  }

 private:

  // A qualified type is a candidate of its own, after its unqualified part.
  std::string qualify(const std::string& qualifiers, const std::string& inner, ast::QualType q) {
    if (qualifiers.empty()) return inner;
    if (auto s = find(full_key(q))) return *s;
    candidates_.push_back(full_key(q));
    return qualifiers + inner;
  }
  std::uint32_t full_key(ast::QualType q) const { return std::to_underlying(in_.canonical(q)); }
  std::optional<std::string> find(std::uint32_t key) const {
    for (std::size_t i = 0; i < candidates_.size(); ++i)
      if (candidates_[i] == key) return substitution(i);
    return std::nullopt;
  }

  const ast::Interners& in_;
  const ClassNames& classes_;
  std::vector<std::uint32_t> candidates_;  // canonical types, in order of completion
};

std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes,
                                                         std::optional<ast::QualType> scope, bool special) {
  ast::QualType c = in.types.canon(type);
  if (!in.types.is<ast::TFunction>(c)) return std::unexpected("not a function type");
  auto f = in.types.get<ast::TFunction>(c);
  Mangler m(in, classes);
  std::string out = std::format("_Z{}{}", name.size(), name);
  if (scope) {
    // <nested-name> ::= N [<CV-qualifiers>] <prefix> <unqualified-name> E, the member function's
    // cv-qualifiers in the order r V K (§5.1.5.3).
    auto prefix = m.class_name(*scope);
    if (!prefix) return prefix;
    std::string cv = std::string(std::to_underlying(f.cv) & 2 ? "V" : "") + (std::to_underlying(f.cv) & 1 ? "K" : "");
    // A <ctor-dtor-name> (`C1`, `D1`) stands as it is; other names are <source-name>s.
    out = special ? std::format("_ZN{}{}{}E", cv, *prefix, name) : std::format("_ZN{}{}{}{}E", cv, *prefix, name.size(), name);
  }
  auto params = in.seqs.elements(f.params);
  if (params.empty() && !f.variadic) out += "v";
  for (ast::QualType p : params) {
    auto s = m.type(p);
    if (!s) return s;
    out += *s;
  }
  if (f.variadic) out += "z";
  return out;
}

std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes, std::optional<ast::QualType> scope,
                                                    bool special) {
  if (scope && !is_function) {
    // A static data member: <nested-name> ([class.static.data], §5.1.2).
    std::string n = classes ? classes(in.types.get<ast::TClass>(in.types.canon(*scope)).entity).name : "";
    if (n.empty()) return std::unexpected("a class whose name is unknown");
    return std::format("__ZN{}{}{}{}E", n.size(), n, name.size(), name);
  }
  if (!is_function || name == "main" || lang == ast::LangLinkage::c) return "_" + std::string(name);
  auto m = mangled_function(in, name, type, classes, scope, special);
  if (!m) return m;
  return "_" + *m;
}

}  // namespace ycxx::pipeline
