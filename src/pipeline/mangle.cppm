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
import ycxx.hash;
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
  ast::WrittenConstraint head{}, trailing{};
  ast::SeqId<ast::TemplateParamSema> generic_invoker_head{};
};
// The declaration data for a function's <encoding> nested inside another name.
struct LocalFunction {
  std::string name;
  ast::QualType type{};
  ast::EntityId scope = ast::EntityId::none;
  bool special = false;
  std::optional<TemplateSpec> spec;
  bool internal_name = false;
  bool c_name = false;
};

// The declaration named by a symbolic function template argument.
struct FunctionName {
  LocalFunction declaration;
  ast::LangLinkage lang = ast::LangLinkage::cxx;
};
using FunctionNames = std::function<std::optional<FunctionName>(ast::EntityId)>;

// An object named in a template argument or a pack index.
struct ObjectName {
  std::string name;
  ast::QualType type;
  ast::EntityId scope;
  bool internal;
};
using ObjectNames = std::function<std::optional<ObjectName>(ast::EntityId)>;

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
  ast::EntityId closure_variable = ast::EntityId::none;
  std::optional<ast::SeqId<ast::TemplateParamSema>> template_params{};
  // A local class ([class.local]): the function it is local to (`local`), and its number among
  // that function's local classes of its name (`closure_number`), or among its unnamed types
  // (`unnamed_type_ordinal`): a <local-name> with a discriminator (§5.1.6).
  bool local_class = false;
  std::optional<std::uint32_t> private_closure_number{};
};
using ClassNames = std::function<ClassName(ast::EntityId)>;

// A standalone <type> encoding, with an initially empty substitution table (§5.1.5).
std::expected<std::string, std::string> mangled_type(const ast::Interners& in, ast::QualType type, const ClassNames& classes,
                                                     ast::Compatibility compatibility);

// The Itanium <special-name> for a class-valued template parameter object.
std::expected<std::string, std::string> mangled_template_parameter_object(const ast::Interners& in,
                                                                        const ast::TemplateArg& argument,
                                                                        const ClassNames& classes,
                                                                        ast::Compatibility compatibility);

// `scope` is the class or namespace a function is a member of, none for the global namespace.
// Direct members of ::std use an <unscoped-name> with St (§5.1.5 and §5.1.10).
std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes = {},
                                                         ast::EntityId scope = ast::EntityId::none,
                                                         bool special = false, const FunctionNames& functions = {},
                                                         const ObjectNames& objects = {},
                                                    ast::Compatibility compatibility = ast::Compatibility::spec);

// `special`: `name` is a <ctor-dtor-name> (`C1`), a constructor template's specialization.
std::expected<std::string, std::string> mangled_specialization(const ast::Interners& in, std::string_view name,
                                                               const TemplateSpec& spec, const ClassNames& classes = {},
                                                               ast::EntityId scope = ast::EntityId::none, bool special = false,
                                                               const FunctionNames& functions = {}, const ObjectNames& objects = {},
                                                    ast::Compatibility compatibility = ast::Compatibility::spec);

// <substitution> for the candidate at `index`: `S_`, then `S0_` … `SZ_`, `S10_` … (§5.1.10).
std::string substitution(std::size_t index);

// The symbol of an entity: the mangled name or identifier with the platform prefix, if any.
std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes = {},
                                                    ast::EntityId scope = ast::EntityId::none,
                                                    bool special = false, const TemplateSpec* spec = nullptr,
                                                    bool leading_underscore = true, const FunctionNames& functions = {},
                                                    const ObjectNames& objects = {},
                                                    ast::Compatibility compatibility = ast::Compatibility::spec);

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
    case int128: return "n";
    case unsigned_int128: return "o";
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
  Mangler(const ast::Interners& in, const ClassNames& classes, const FunctionNames& functions, const ObjectNames& objects,
          ast::Compatibility compatibility)
      : in_(in), classes_(classes), functions_(functions), objects_(objects), compatibility_(compatibility) {}
  // The depth of the head of the template whose specialization is mangled: its parameters are
  // `T_`, `T0_`, …
  ast::u16 own_depth = 0;

  // <type> (§5.1.5), recording substitution candidates as they complete.
  std::expected<std::string, std::string> type(ast::QualType q) {
    q = in_.canonical(q);
    ast::Cv cv = ast::cv_of(q);
    ast::QualType bare = ast::without_cv(q);
    if (in_.types.is<ast::TDeferredAlias>(bare)) {
      const auto alias = in_.types.get<ast::TDeferredAlias>(bare);
      return type(const_cast<ast::Interners&>(in_).qualified(alias.result, cv));
    }
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
        } else if (a.bound != ast::ConstId::none) {
          return std::unexpected("an array of unknown or dependent bound");
        }
        // An incomplete array omits the dimension. It still has the `A_` prefix (§5.1.5.6).
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
        const auto d = in_.types.get<ast::TDepName>(bare);
        if (compatibility_ == ast::Compatibility::clang && ast::DepNameForm(d.form) == ast::DepNameForm::template_name) {
          auto p = type(d.qualifier);
          if (!p) return p;
          inner = "sr" + *p + "E" + source_name(in_.names.spelling(d.name));
          candidates_.push_back(full_key(bare));
        } else {
          auto p = dep_prefix(bare);
          if (!p) return p;
          inner = "N" + *p + "E";
        }
      } else if (in_.types.is<ast::TTransform>(in_.types.canon(bare))) {
        // A type-trait built-in of a dependent type: a vendor-extended type `u <source-name>` with
        // its argument, `u11__remove_cvIT_E`, as Clang spells it (docs/EXTENSIONS.md).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto x = in_.types.get<ast::TTransform>(in_.types.canon(bare));
        const std::string_view n = lex::type_trait_info(lex::TypeTrait(x.trait)).name;
        if (compatibility_ == ast::Compatibility::gcc)
          return std::unexpected(std::format("a {}built-in type trait '{}' in the signature",
                                             dependent(x.arg) ? "dependent " : "", n));
        auto a = type(x.arg);
        if (!a) return a;
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
      } else if (in_.types.is<ast::TPackIndex>(in_.types.canon(bare))) {
        // `T...[I]`: `Dy <type> <expression>`, a substitution candidate (§5.1.5); a pack bound to
        // its elements is the argument pack `J <template-arg>* E`, as GCC writes it. A constant
        // index is a literal of its own type: std::size_t when written, a parameter's type when
        // substituted (`Lm0E`, `Li1E`; DECISIONS §1.7).
        if (auto s = find(full_key(bare))) return qualify(qualifiers, *s, q);
        auto x = in_.types.get<ast::TPackIndex>(in_.types.canon(bare));
        std::string pack;
        if (x.bound && compatibility_ == ast::Compatibility::clang) {
          pack = "_SUBSTPACK_";
        } else if (x.bound) {
          pack = "J";
          for (const ast::TemplateArg& e : in_.seqs.elements(x.elems)) {
            auto m = template_arg(e);
            if (!m) return m;
            pack += *m;
          }
          pack += "E";
        } else {
          auto p = type(x.pack);
          if (!p) return p;
          pack = *p;
        }
        std::string index;
        if (x.written != ast::ConstId::none) {
          auto written = expression(x.written);
          if (!written) return written;
          index = *written;
        } else if (in_.values.is<ast::VDepParam>(x.index) && in_.values.get<ast::VDepParam>(x.index).depth == own_depth) {
          auto i = in_.values.get<ast::VDepParam>(x.index).index;
          index = i == 0 ? "T_" : std::format("T{}_", i - 1);
        } else if (in_.values.is<ast::VInt>(x.index)) {
          auto b = ast::builtin_of(in_, in_.values.get<ast::VInt>(x.index).type);
          auto code = b ? builtin_code(*b) : std::nullopt;
          if (!code) return std::unexpected("a pack index of a type other than a fundamental integer");
          index = std::format("L{}{}E", *code, in_.values.get<ast::VInt>(x.index).bits.lo);
        } else {
          return std::unexpected("a pack index that is not a constant or a template parameter");
        }
        inner = "Dy" + pack + index;
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
        auto sig = signature(f, true);
        if (!sig) return sig;
        if (std::to_underlying(f.cv) & 2) inner += "V";
        if (std::to_underlying(f.cv) & 1) inner += "K";
        if (f.noexcept_ != ast::ConstId::none && in_.values.is<ast::VInt>(f.noexcept_) &&
            in_.values.get<ast::VInt>(f.noexcept_).bits.lo != 0) inner += "Do";
        inner += f.linkage == ast::LangLinkage::c ? "FY" : "F";
        inner += *sig;
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
    if (in_.types.is<ast::TEnum>(c)) return entity_name(in_.types.get<ast::TEnum>(c).entity, true);
    return entity_name(in_.types.get<ast::TClass>(c).entity, false);
  }
  // A class's or enumeration's <name> as a type: its <source-name>, or, inside a class or a
  // namespace, a <nested-name> whose prefixes are candidates first (§5.1.5.3, §5.1.10). Classes
  // and namespaces are keyed by entity.
  std::expected<std::string, std::string> entity_name(ast::EntityId e, bool enumeration) {
    std::uint64_t key = (std::uint64_t{1} << 32) | std::to_underlying(e);
    if (auto s = find(key)) return *s;
    ClassName n = classes_ ? classes_(e) : ClassName{};
    if (n.closure) {
      if (n.closure_variable != ast::EntityId::none)
        return prefix(e).transform([](std::string p) { return "N" + p + "E"; });
      // A closure type as a type: the <local-name> `Z <encoding> E <closure-type-name>` (§5.1.6).
      if (!n.local) return std::unexpected("a closure type whose function is unknown");
      auto l = local_encoding(*n.local);
      if (!l) return l;
      auto component = closure_component(n);
      if (!component) return component;
      std::string r = "Z" + *l + "E" + *component;
      candidates_.push_back(key);
      return r;
    }
    if (n.name.empty() && !n.unnamed_type_ordinal)
      return std::unexpected(enumeration ? "an unnamed enumeration in a mangled type" : "a class whose name is unknown");
    // A local class, or a class nested in one: `Z <encoding> E <entity name> [<discriminator>]`.
    if (auto root = local_root(e)) {
      auto l = local_encoding(*root->second.local);
      if (!l) return l;
      std::string r = class_component(n);
      if (e != root->first) {
        auto p = prefix(n.scope);
        if (!p) return p;
        r = "N" + *p + r + "E";
      }
      r = "Z" + *l + "E" + r + discriminator(root->second);
      candidates_.push_back(key);
      return r;
    }
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
      std::string r;
      if (n.closure_variable != ast::EntityId::none) {
        auto owner = prefix(n.closure_variable);
        if (!owner) return owner;
        r = *owner;
        // <closure-prefix> ::= <name> M (§5.1.8). GCC omits M for a variable template.
        if (compatibility_ != ast::Compatibility::gcc || classes_(n.closure_variable).templ == ast::EntityId::none)
          r += "M";
      }
      auto component = closure_component(n);
      if (!component) return component;
      r += *component;
      candidates_.push_back(key);
      return r;
    }
    if (n.name.empty() && !n.unnamed_type_ordinal) return std::unexpected("a scope whose name is unknown");
    if (n.templ != ast::EntityId::none) return specialization_prefix(e, n);
    if (n.local_class) {  // the <local-name>'s function comes before the nested-name it begins
      std::string r = class_component(n);
      candidates_.push_back(key);
      return r;
    }
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
  // Whether a name in `scope` is a <nested-name>, not an <unscoped-name> (global or `St`).
  bool nested_name(ast::EntityId scope) const {
    return scope != ast::EntityId::none && !(classes_ && is_global_std(classes_(scope)));
  }
  std::string delimit_name(std::string components, ast::EntityId scope) const {
    if (!nested_name(scope)) return components;
    return "N" + components + "E";
  }
  // The <data name> part of an <encoding> (§5.1.2), sharing substitutions with its use.
  std::expected<std::string, std::string> object_encoding(std::string_view name, ast::EntityId scope, bool internal = false) {
    std::string component = (internal ? "L" : "") + source_name(name);
    if (scope == ast::EntityId::none) return component;
    auto p = prefix(scope);
    if (!p) return p;
    return delimit_name(*p + component, scope);
  }
  // The local class that is class `e` or encloses it, with its name, if there is one.
  std::optional<std::pair<ast::EntityId, ClassName>> local_root(ast::EntityId e) const {
    for (int depth = 0; e != ast::EntityId::none && classes_ && depth < 64; ++depth) {
      ClassName n = classes_(e);
      if (!n.is_class || n.closure) return std::nullopt;
      if (n.local_class) {
        if (!n.local) return std::nullopt;
        return std::pair{e, std::move(n)};
      }
      e = n.scope;
    }
    return std::nullopt;
  }
  // <discriminator> ::= _ <digit> | __ <number> _: the n-th local class of one name in its function
  // is numbered n − 2 (§5.1.6). Unnamed types are numbered by their <unnamed-type-name>.
  static std::string discriminator(const ClassName& local) {
    if (local.unnamed_type_ordinal || local.closure_number == 0) return "";
    const std::uint32_t d = local.closure_number - 1;
    return d < 10 ? "_" + std::to_string(d) : "__" + std::to_string(d) + "_";
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
  std::expected<std::string, std::string> closure_component(const ClassName& n) {
    if (n.private_closure_number) return source_name("$_" + std::to_string(*n.private_closure_number));
    return closure_sig(n).transform([&](const std::string& signature) {
      return "Ul" + signature + "E" + (n.closure_number ? std::to_string(n.closure_number - 1) : "") + "_";
    });
  }
  // A function's <encoding> (§5.1.2), without `_Z`: its <name>, a <nested-name> in a class or
  // namespace, then its parameter types. A member of a closure type is in a <local-name>, the
  // closure's function's <encoding> first (§5.1.6).
  std::expected<std::string, std::string> encoding(std::string_view name, ast::QualType type, ast::EntityId scope,
                                                  bool special, bool internal_name = false) {
    ast::QualType c = in_.types.canon(type);
    if (!in_.types.is<ast::TFunction>(c)) return std::unexpected("not a function type");
    auto f = in_.types.get<ast::TFunction>(c);
    // An <operator-name> (§5.1.5.1) or a <source-name>; `+`, `-`, `*` and `&` mangle by arity,
    // `this` counted for a member.
    ClassName sc = scope != ast::EntityId::none && classes_ ? classes_(scope) : ClassName{};
    if (sc.closure && name == "__invoke" && compatibility_ == ast::Compatibility::gcc) name = "_FUN";
    std::size_t arity = in_.seqs.size(f.params) + (sc.is_class ? 1 : 0);
    bool conversion = name == lex::conversion_function_name;
    std::string unqualified = std::format("{}{}", name.size(), name);
    if (name.starts_with(lex::literal_operator_prefix)) {
      unqualified = "li" + source_name(name.substr(lex::literal_operator_prefix.size()));
    } else if (!conversion && name.starts_with("operator") && name.size() > 8) {
      auto code = operator_code(name.substr(8), arity);
      if (!code) return std::unexpected(std::format("the operator function '{}'", name));
      unqualified = *code;
    }
    if (internal_name) unqualified = "L" + unqualified;
    std::string out = unqualified;
    if (scope != ast::EntityId::none) {
      std::string local;
      std::string disc;  // a local class's discriminator, after the whole nested-name (§5.1.6)
      if (sc.closure && sc.closure_variable == ast::EntityId::none) {
        if (!sc.local) return std::unexpected("a closure type whose function is unknown");
        auto l = local_encoding(*sc.local);
        if (!l) return l;
        local = "Z" + *l + "E";
      } else if (auto root = local_root(scope)) {
        auto l = local_encoding(*root->second.local);
        if (!l) return l;
        local = "Z" + *l + "E";
        disc = discriminator(root->second);
      }
      // <nested-name> ::= N [<CV-qualifiers>] <prefix> <unqualified-name> E, the member function's
      // cv-qualifiers in the order r V K (§5.1.5.3).
      auto p = prefix(scope);
      if (!p) return p;
      if (conversion) {
        auto target = this->type(f.ret);
        if (!target) return target;
        unqualified = "cv" + *target;
      }
      const std::string cv = member_qualifiers(f);
      // A <ctor-dtor-name> (`C1`, `D1`) stands as it is; other names are <source-name>s.
      out = local + delimit_name(cv + *p + (special ? std::string(name) : unqualified), scope) + disc;
    }
    auto sig = signature(f, false);
    if (!sig) return sig;
    return out + *sig;
  }
  // A function template specialization's <encoding>, without `_Z` (§5.1.2).
  // `special`: `name` is a <ctor-dtor-name> (`C1`), a constructor template's specialization.
  std::expected<std::string, std::string> specialization_encoding(std::string_view name, const TemplateSpec& spec, ast::EntityId scope,
                                                                  bool special = false, bool internal_name = false) {
    // Template parameters are mangled relative to the specialization's own head (a member
    // template's is inside its class's), for its encoding only.
    ast::u16 saved = own_depth;
    if (!spec.params.empty()) own_depth = spec.params[0].depth;
    auto r = specialization_encoding_at(name, spec, scope, special, internal_name);
    own_depth = saved;
    return r;
  }
  std::expected<std::string, std::string> specialization_encoding_at(std::string_view name, const TemplateSpec& spec, ast::EntityId scope,
                                                                     bool special, bool internal_name) {
    ast::QualType c = in_.types.canon(spec.pattern);
    // A generic lambda's function call operator, a member function template of a closure type:
    // `N [<CV-qualifiers>] <prefix> <unqualified-name> <template-args> E` and its signature, in a
    // <local-name> when the class is a closure type (§5.1.5.3, §5.1.6).
    ClassName sc = scope != ast::EntityId::none && classes_ ? classes_(scope) : ClassName{};
    if (sc.closure && in_.types.is<ast::TFunction>(c)) {
      auto f = in_.types.get<ast::TFunction>(c);
      const bool gcc_invoker = compatibility_ == ast::Compatibility::gcc && in_.seqs.size(spec.generic_invoker_head) != 0;
      if (gcc_invoker) name = "_FUN";
      std::string local;
      if (sc.closure_variable == ast::EntityId::none) {  // else the variable is its <closure-prefix>
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
      auto args = specialization_args(spec, /*nested=*/true);
      if (!args) return args;
      const bool deduced_result = in_.types.is<ast::TAuto>(in_.types.canon(f.ret));
      auto sig = signature_for_closure(f, sc, spec, scope, gcc_invoker && deduced_result);
      if (!sig) return sig;
      *sig += constraint_suffix(spec.trailing, false);
      return local + "N" + member_qualifiers(f) + *p + unqualified + *args + "E" + *sig;
    }
    auto n = template_name(name, scope, special, internal_name);
    if (!n) return n;
    auto args = specialization_args(spec, nested_name(scope));
    if (!args) return args;
    // A variable template specialization: its name and arguments, no type (§5.1.2).
    if (!in_.types.is<ast::TFunction>(c)) return delimit_name(*n + *args, scope);
    auto sig = signature(in_.types.get<ast::TFunction>(c), !special);  // a constructor has no return type
    if (!sig) return sig;
    *sig += constraint_suffix(spec.trailing, false);
    // In a <nested-name>, a member function template's qualifiers follow the `N`.
    return delimit_name(member_qualifiers(in_.types.get<ast::TFunction>(c)) + *n + *args, scope) + *sig;
  }
  // A member function's <CV-qualifiers> then its <ref-qualifier> in a <nested-name>, in the order
  // V K, then R or O (§5.1.5.3).
  static std::string member_qualifiers(const ast::TFunction& f) {
    std::string out = std::string(std::to_underlying(f.cv) & 2 ? "V" : "") + (std::to_underlying(f.cv) & 1 ? "K" : "");
    if (f.ref != ast::RefQual::none) out += f.ref == ast::RefQual::lvalue ? "R" : "O";
    return out;
  }
  std::expected<std::string, std::string> signature_for_closure(const ast::TFunction& invoker, const ClassName& closure,
                                                               const TemplateSpec& spec, ast::EntityId scope,
                                                               bool gcc_deduced_invoker) {
    if (!gcc_deduced_invoker) return signature(invoker, true);
    // GCC's current primary signature describes the deduced result by calling
    // operator() through a null closure pointer with the forwarded parameters.
    auto& in = const_cast<ast::Interners&>(in_);
    const auto member = in_.types.get<ast::TFunction>(in_.types.canon(closure.closure_type));
    auto object = ast::with_cv(in.intern_type(ast::TClass{scope}), member.cv);
    auto pointer = type(ast::pointer_to(in, object, ast::Cv::const_));
    if (!pointer) return pointer;
    std::string out = "DTcldtdeL" + *pointer + "0EonclI";
    for (const auto& parameter : in_.seqs.elements(spec.generic_invoker_head)) {
      if (parameter.pack) out += 'J';
      if (parameter.kind == ast::TemplateArgKind::value) {
        auto value = in.intern_value(ast::VDepParam{parameter.depth, parameter.index});
        if (parameter.pack) value = in.intern_value(ast::VDepExpansion{value});
        auto a = template_arg({ast::TemplateArgKind::value, parameter.type, value});
        if (!a) return a;
        out += *a;
      } else {
        auto t = in.intern_type(ast::TTemplateParam{parameter.depth, parameter.index, parameter.pack});
        if (parameter.pack) t = in.intern_type(ast::TPackExpansion{t});
        auto a = type(t);
        if (!a) return a;
        out += *a;
      }
      if (parameter.pack) out += 'E';
    }
    out += 'E';
    std::size_t index = 0;
    for (auto p : in_.seqs.elements(invoker.params)) {
      if (in_.types.is<ast::TPackExpansion>(in_.types.canon(p))) {
        out += "sp";
        p = in_.types.get<ast::TPackExpansion>(in_.types.canon(p)).pattern;
      }
      auto forwarded = type(ast::rvalue_ref_to(in, p));
      if (!forwarded) return forwarded;
      out += "sc" + *forwarded;
      out += index == 0 ? "fp_" : std::format("fp{}_", index - 1);
      ++index;
    }
    auto parameters = signature(invoker, false);
    if (!parameters) return parameters;
    return out + "EE" + *parameters;
  }
  std::expected<std::string, std::string> local_encoding(const LocalFunction& f) {
    if (f.c_name) return source_name(f.name);
    if (f.spec) return specialization_encoding(f.name, *f.spec, f.scope, f.special, f.internal_name);
    return encoding(f.name, f.type, f.scope, f.special, f.internal_name);
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

  // <template-args> ::= I <template-arg>+ E (§5.1.5.7).
  std::expected<std::string, std::string> template_args(
      std::span<const ast::TemplateArg> args, std::optional<ast::SeqId<ast::TemplateParamSema>> class_params = {}) {
    std::string out = "I";
    if (class_params && in_.seqs.size(*class_params) != args.size()) class_params.reset();
    for (std::size_t i = 0; i < args.size(); ++i) {
      const auto& a = args[i];
      std::optional<ast::QualType> class_parameter_type;
      if (class_params) {
        const auto parameter = in_.seqs.at(*class_params, i);
        // Only a direct argument-to-parameter match can certify the declared type.
        if (parameter.pack || a.kind == ast::TemplateArgKind::pack) class_params.reset();
        else if (parameter.kind == ast::TemplateArgKind::value && a.kind == ast::TemplateArgKind::value)
          class_parameter_type = parameter.type;
      }
      auto m = template_arg(a, class_parameter_type);
      if (!m) return m;
      out += *m;
    }
    return out + "E";
  }
  // One <template-arg>: a type, an argument pack `J … E`, or a value (§5.1.5.7).
  std::expected<std::string, std::string> template_arg(
      const ast::TemplateArg& a, std::optional<ast::QualType> class_parameter_type = {}) {
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
      // Any other value-dependent argument: `X <expression> E` (§5.1.5.7, §5.1.6).
      if (in_.values.is<ast::VDepOp>(a.value) || in_.values.is<ast::VDepVariable>(a.value) ||
          in_.values.is<ast::VDepCall>(a.value)) {
        auto e = expression(a.value);
        if (!e) return e;
        return "X" + *e + "E";
      }
      if (in_.values.is<ast::VAggregate>(a.value)) {
        const auto& aggregate = in_.values.get<ast::VAggregate>(a.value);
        auto t = type(aggregate.type);
        if (!t) return t;
        auto fields = in_.seqs.elements(aggregate.elems);
        for (ast::ConstId field : fields)
          if (!in_.values.is<ast::VInt>(field))
            return std::unexpected("a non-integral template argument field");
        // Itanium's typed braced-init-list omits trailing zero initializers. The
        // aggregate constant itself still contains every field, including those zeroes.
        std::size_t count = fields.size();
        while (count && in_.values.get<ast::VInt>(fields[count - 1]).bits == ast::Bits128{}) --count;
        out = "Xtl" + *t;
        for (std::size_t i = 0; i < count; ++i) {
          const auto& field = in_.values.get<ast::VInt>(fields[i]);
          auto value = template_arg({ast::TemplateArgKind::value, field.type, fields[i]});
          if (!value) return value;
          out += *value;
        }
        return out + "EE";
      }
      if (ast::is_pointer(in_, a.type) && ast::is_function(in_, ast::pointee_of(in_, a.type)))
        return function_pointer_argument(a);
      if (ast::is_pointer(in_, a.type) && in_.values.is<ast::VLvalue>(a.value))
        return object_pointer_argument(a, class_parameter_type && in_.same_type(a.type, *class_parameter_type));
      // A value of an enumeration type: `L <type> <value> E`, the type as any <type> is, the value
      // its integer, `n` before a negative one (§5.1.5.7).
      if (in_.types.is<ast::TEnum>(in_.types.canon(ast::without_cv(in_.canonical(a.type)))) && in_.values.is<ast::VInt>(a.value)) {
        auto t = type(a.type);
        if (!t) return t;
        ast::Bits128 v = in_.values.get<ast::VInt>(a.value).bits;
        bool neg = ast::is_negative(v);
        return std::format("L{}{}{}E", *t, neg ? "n" : "", ast::wide::decimal(neg ? ast::wide::neg(v) : v));
      }
      auto b = ast::builtin_of(in_, a.type);
      auto code = b ? builtin_code(*b) : std::nullopt;
      if (!code || !in_.values.is<ast::VInt>(a.value)) return std::unexpected("a template argument that is not a type or an integer");
      auto v = in_.values.get<ast::VInt>(a.value).bits;
      bool neg = ast::is_signed(ast::aarch64_darwin, *b) && ast::is_negative(v);
      out += std::format("L{}{}{}E", *code, neg ? "n" : "", ast::wide::decimal(neg ? ast::wide::neg(v) : v));
    }
    return out;
  }

  // The <expression> of a template argument or a written dependent type (§5.1.6), #773.
  std::expected<std::string, std::string> expression(ast::ConstId v) {
    if (in_.values.is<ast::VEntity>(v)) {
      const auto entity = in_.values.get<ast::VEntity>(v).entity;
      const auto name = objects_ ? objects_(entity) : std::nullopt;
      if (!name) return std::unexpected("a value expression whose external name is unknown");
      auto encoded = object_encoding(name->name, name->scope, name->internal);
      if (!encoded) return encoded;
      return "L_Z" + *encoded + "E";
    }
    if (in_.values.is<ast::VWrittenOperator>(v)) {
      const auto op = in_.values.get<ast::VWrittenOperator>(v);
      const auto operands = in_.seqs.elements(op.operands);
      const auto token = lex::Tk(op.token);
      if (!operands.empty() && in_.values.is<ast::VType>(operands.front())) {
        const auto target = in_.values.get<ast::VType>(operands.front());
        auto cast = type(ast::with_cv(target.type, ast::Cv(target.cv)));
        if (!cast) return cast;
        const bool braced = token == lex::Tk::l_brace;
        const bool static_cast_ = token == lex::Tk::kw_static_cast;
        const bool multiple = !braced && !static_cast_ && operands.size() != 2;
        std::string result = (braced ? "tl" : static_cast_ ? "sc" : "cv") + *cast;
        if (multiple) result += '_';
        for (ast::ConstId operand : std::span(operands).subspan(1)) {
          auto child = expression(operand);
          if (!child) return child;
          result += *child;
        }
        if (braced || multiple) result += 'E';
        return result;
      }
      std::optional<std::string_view> code;
      switch (token) {
      case lex::Tk::l_paren: code = "cl"; break;
      case lex::Tk::l_square: code = "ix"; break;
      case lex::Tk::question: code = "qu"; break;
      case lex::Tk::kw_sizeof: code = "sz"; break;
      case lex::Tk::kw_noexcept: code = "nx"; break;
      case lex::Tk::plusplus: code = operands.size() == 1 ? "pp_" : "pp"; break;
      case lex::Tk::minusminus: code = operands.size() == 1 ? "mm_" : "mm"; break;
      default: code = operator_code(lex::spelling(token), operands.size()); break;
      }
      if (!code) return std::unexpected("an unsupported written operator");
      std::string encoded(*code);
      for (ast::ConstId operand : operands) {
        if (operand == ast::ConstId::none) continue;
        auto child = expression(operand);
        if (!child) return child;
        encoded += *child;
      }
      if (token == lex::Tk::l_paren) encoded += 'E';
      return encoded;
    }
    if (in_.values.is<ast::VWrittenMember>(v)) {
      const auto member = in_.values.get<ast::VWrittenMember>(v);
      auto object = expression(member.object);
      if (!object) return object;
      std::string result = (member.arrow ? "pt" : "dt") + *object + source_name(in_.names.spelling(member.name));
      if (!member.template_id) return result;
      auto args = template_args(in_.seqs.elements(member.arguments));
      if (!args) return args;
      return result + *args;
    }
    if (in_.values.is<ast::VWrittenCallee>(v)) {
      const auto callee = in_.values.get<ast::VWrittenCallee>(v);
      std::string result;
      if (compatibility_ != ast::Compatibility::gcc) {
        if (callee.resolved != ast::EntityId::none && functions_) {
          if (const auto function = functions_(callee.resolved)) {
            if (function->lang == ast::LangLinkage::c) return "L" + source_name(function->declaration.name) + "E";
            auto name = local_encoding(function->declaration);
            if (!name) return name;
            return "L_Z" + *name + "E";
          }
        }
        if (callee.global) result = "gs";
        const auto qualifier = in_.seqs.elements(callee.qualifier);
        if (!qualifier.empty()) {
          result += "sr";
          for (ast::NameId name : qualifier) result += source_name(in_.names.spelling(name));
          result += 'E';
        }
      }
      result += source_name(in_.names.spelling(callee.name));
      if (!callee.template_id) return result;
      auto args = template_args(in_.seqs.elements(callee.arguments));
      if (!args) return args;
      return result + *args;
    }
    if (in_.values.is<ast::VDepMember>(v)) {
      const auto member = in_.values.get<ast::VDepMember>(v);
      const auto qualifier = in_.types.canon(member.qualifier);
      if (compatibility_ != ast::Compatibility::gcc && classes_ && in_.types.is<ast::TClass>(qualifier)) {
        const auto scope = classes_(in_.types.get<ast::TClass>(qualifier).entity);
        if (scope.templ != ast::EntityId::none) {
          auto args = template_args(scope.args);
          if (!args) return args;
          std::string prefix = "sr";
          if (scope.scope != ast::EntityId::none) {
            prefix = unresolved_qualifier(scope.scope, true, false);
            prefix.pop_back();
          }
          return prefix + source_name(scope.name) + *args + "E" + source_name(in_.names.spelling(member.name));
        }
      }
      auto scope = type(qualifier);
      if (!scope) return scope;
      return "sr" + *scope + source_name(in_.names.spelling(member.name));
    }
    if (in_.values.is<ast::VConstraint>(v)) {
      const auto c = in_.values.get<ast::VConstraint>(v);
      std::string r = c.op == 0 ? "aa" : "oo";
      for (ast::ConstId value : in_.seqs.elements(c.operands)) {
        auto operand = expression(value);
        if (!operand) return operand;
        r += *operand;
      }
      return r;
    }
    if (in_.values.is<ast::VDepConcept>(v)) {
      const auto c = in_.values.get<ast::VDepConcept>(v);
      ClassName n = classes_ ? classes_(c.concept_) : ClassName{};
      if (n.name.empty()) return std::unexpected("a concept whose name is unknown");
      auto args = template_args(in_.seqs.elements(c.args));
      if (!args) return args;
      return unresolved_qualifier(n.scope, c.qualified, c.global) + source_name(n.name) + *args;
    }
    if (in_.values.is<ast::VRequiresExpression>(v))
      return requires_expression(in_.values.get<ast::VRequiresExpression>(v));
    if (in_.values.is<ast::VRequiresParameter>(v))
      return requires_parameter(in_.values.get<ast::VRequiresParameter>(v));
    if (in_.values.is<ast::VDepFnParam>(v)) {
      const auto parameter = in_.values.get<ast::VDepFnParam>(v);
      const ast::u64 level = requires_depth_ + parameter.depth + (compatibility_ == ast::Compatibility::gcc);
      return parameter_encoding(level, parameter.index, ast::Cv(parameter.cv));
    }
    if (in_.values.is<ast::VInt>(v)) {
      const auto value = in_.values.get<ast::VInt>(v);
      return template_arg({ast::TemplateArgKind::value, value.type, v});
    }
    if (in_.values.is<ast::VDepParam>(v)) {
      const auto parameter = in_.values.get<ast::VDepParam>(v);
      if (parameter.depth != own_depth) return std::unexpected("an enclosing value parameter");
      return parameter.index == 0 ? std::string("T_") : std::format("T{}_", parameter.index - 1);
    }
    if (in_.values.is<ast::VDepCall>(v)) {
      const auto& call = in_.values.get<ast::VDepCall>(v);
      if (in_.types.is<ast::TClass>(call.result_type))
        return std::unexpected("a class-valued dependent call in a template argument");
      const auto operands = in_.seqs.elements(call.args);
      if (operands.empty() || operands.front().kind == ast::TemplateArgKind::type) {
        const auto name = classes_ ? classes_(call.callee) : ClassName{};
        if (name.name.empty()) return std::unexpected("a dependent call whose function name is unknown");
        auto args = template_args(operands);
        if (!args) return args;
        return "cl" + source_name(name.name) + *args + "E";
      }
      const auto target = functions_ ? functions_(call.callee) : std::nullopt;
      if (!target) return std::unexpected("a dependent call whose function name is unknown");
      auto callee = local_encoding(target->declaration);
      if (!callee) return callee;
      std::string result = "clL_Z" + *callee + "E";
      for (const auto& operand : operands) {
        if (in_.values.is<ast::VDepParam>(operand.value)) {
          const auto& parameter = in_.values.get<ast::VDepParam>(operand.value);
          if (parameter.depth != own_depth) return std::unexpected("a dependent call with an enclosing template parameter");
          result += parameter.index == 0 ? "T_" : std::format("T{}_", parameter.index - 1);
          continue;
        }
        auto value = template_arg(operand);
        if (!value) return value;
        result += *value;
      }
      return result + "E";
    }
    if (in_.values.is<ast::VDepVariable>(v)) {
      const ast::VDepVariable& d = in_.values.get<ast::VDepVariable>(v);
      ClassName n = classes_ ? classes_(d.templ) : ClassName{};
      if (n.name.empty()) return std::unexpected("a dependent variable template whose name is unknown");
      if (n.is_class) return std::unexpected("a dependent member variable template as a template argument");
      const bool gcc = compatibility_ == ast::Compatibility::gcc;
      auto args = template_args(in_.seqs.elements(gcc ? d.args : d.written_args));
      if (!args) return args;
      const auto written = in_.seqs.elements(d.qualifier);
      if (!gcc && !written.empty()) {
        std::string qualifier = d.global ? "gssr" : "sr";
        for (ast::NameId component : written) qualifier += source_name(in_.names.spelling(component));
        return qualifier + "E" + source_name(n.name) + *args;
      }
      return unresolved_qualifier(n.scope, d.qualified, d.global) + source_name(n.name) + *args;
    }
    if (in_.values.is<ast::VDepOp>(v)) {
      const ast::VDepOp& op = in_.values.get<ast::VDepOp>(v);
      auto operands = in_.seqs.elements(op.operands);
      if ((op.op == ast::DepOp::sizeof_type || op.op == ast::DepOp::alignof_type) && operands.size() == 1 &&
          operands.front().kind == ast::TemplateArgKind::type) {
        auto operand = type(operands.front().type);
        if (!operand) return operand;
        return (op.op == ast::DepOp::sizeof_type ? "st" : "at") + *operand;
      }
      if (op.op == ast::DepOp::logical_not && operands.size() == 1) {
        if (operands[0].kind != ast::TemplateArgKind::value) return std::unexpected("a type operand of '!'");
        auto e = expression(operands[0].value);
        if (!e) return e;
        return "nt" + *e;
      }
      return std::unexpected("a dependent type-trait operation as a template argument");
    }
    return std::unexpected("a dependent operation as a template argument");
  }

  std::string unresolved_qualifier(ast::EntityId scope, bool qualified, bool global) {
    if (!qualified || compatibility_ == ast::Compatibility::gcc) return {};
    std::string qualifier = global ? "gs" : "";
    if (scope == ast::EntityId::none) return qualifier;
    std::vector<std::string> scopes;
    for (; scope != ast::EntityId::none;) {
      ClassName ns = classes_(scope);
      scopes.push_back(source_name(ns.name));
      scope = ns.scope;
    }
    qualifier += "sr";
    for (const auto& name : scopes | std::views::reverse) qualifier += name;
    return qualifier + "E";
  }

  // Proposed in Itanium ABI issue #24: rq for an omitted list, rQ for an explicit list.
  std::expected<std::string, std::string> requires_expression(const ast::VRequiresExpression& clause) {
    const auto parameters = in_.seqs.elements(clause.parameters);
    const bool list = clause.parameter_list && (!parameters.empty() || compatibility_ != ast::Compatibility::gcc);
    std::string encoded = list ? "rQ" : "rq";
    if (list) {
      if (parameters.empty()) encoded += 'v';
      for (const auto& parameter : parameters) {
        auto p = type(parameter);
        if (!p) return p;
        encoded += *p;
      }
      encoded += '_';
    }
    struct RestoreDepth {
      ast::u32& depth;
      ast::u32 saved;
      ~RestoreDepth() { depth = saved; }
    } restore{requires_depth_, requires_depth_};
    if (list) ++requires_depth_;
    for (const auto& requirement : in_.seqs.elements(clause.requirements)) {
      if (requirement.kind == ast::RequirementKind::type) {
        const auto target = in_.values.get<ast::VType>(requirement.operand);
        auto t = type(ast::with_cv(target.type, ast::Cv(target.cv)));
        if (!t) return t;
        encoded += 'T' + *t;
        continue;
      }
      auto operand = expression(requirement.operand);
      if (!operand) return operand;
      encoded += (requirement.kind == ast::RequirementKind::nested ? 'Q' : 'X') + *operand;
      if (requirement.noexcept_) encoded += 'N';
      if (requirement.result_constraint != ast::ConstId::none) {
        auto result = requirement_constraint(in_.values.get<ast::VDepConcept>(requirement.result_constraint));
        if (!result) return result;
        encoded += 'R' + *result;
      }
    }
    return encoded + 'E';
  }

  std::expected<std::string, std::string> requirement_constraint(const ast::VDepConcept& constraint) {
    const auto name = classes_ ? classes_(constraint.concept_) : ClassName{};
    if (name.name.empty()) return std::unexpected("a result constraint whose concept name is unknown");
    auto encoded = object_encoding(name.name, name.scope);
    if (!encoded) return encoded;
    const auto arguments = in_.seqs.elements(constraint.args);
    if (arguments.empty()) return encoded;
    auto args = template_args(arguments);
    if (!args) return args;
    if (compatibility_ != ast::Compatibility::gcc && encoded->starts_with('N'))
      encoded->insert(encoded->size() - 1, *args);
    else
      *encoded += *args;
    return encoded;
  }

  std::expected<std::string, std::string> requires_parameter(const ast::VRequiresParameter& parameter) const {
    if (parameter.scope >= requires_depth_) return std::unexpected("a requires-expression parameter outside its scope");
    ast::u64 level = requires_depth_ - parameter.scope - 1;
    if (compatibility_ == ast::Compatibility::clang)
      level = requires_depth_ - !template_head_constraint_;
    else if (compatibility_ == ast::Compatibility::gcc)
      level = ast::u64{0} - parameter.scope;
    return parameter_encoding(level, parameter.index, compatibility_ == ast::Compatibility::gcc ? ast::Cv::none : parameter.cv);
  }

  std::string parameter_encoding(ast::u64 level, ast::u16 index, ast::Cv cv) const {
    std::string encoded = level == 0 ? "fp" : std::format("fL{}p", level - 1);
    if (std::to_underlying(cv) & 2) encoded += 'V';
    if (std::to_underlying(cv) & 1) encoded += 'K';
    if (index) encoded += std::to_string(index - 1);
    return encoded + '_';
  }

  std::string constraint_suffix(const ast::WrittenConstraint& clause, bool template_head) {
    if (clause.tokens == Fp128{}) return {};
    if (clause.expression != ast::ConstId::none) {
      if (compatibility_ == ast::Compatibility::gcc && in_.values.is<ast::VDepParam>(clause.expression))
        return {};  // GCC 16 uses the unadorned symbol for a bare boolean template parameter
      const auto checkpoint = candidates_.size();
      const bool old_template_head = std::exchange(template_head_constraint_, template_head);
      const auto outside_requires_depth = std::exchange(requires_depth_, 0);
      auto expr = expression(clause.expression);
      template_head_constraint_ = old_template_head;
      requires_depth_ = outside_requires_depth;
      if (expr) return "Q" + *expr;
      candidates_.resize(checkpoint);
    }
    // Keep unsupported written expressions distinct from each other and from unconstrained
    // functions. Use a vendor encoding until the expression has an interoperable mangling.
    return "Qu" + source_name(std::format("__ycxx_constraint_{:016x}{:016x}", clause.tokens.hi, clause.tokens.lo)) + "E";
  }

  std::expected<std::string, std::string> function_pointer_argument(const ast::TemplateArg& arg) {
    if (in_.values.is<ast::VNullptr>(arg.value)) {
      auto pointer = type(arg.type);
      if (!pointer) return pointer;
      return "L" + *pointer + "0E";
    }
    if (!in_.values.is<ast::VLvalue>(arg.value)) return std::unexpected("a non-address function pointer argument");
    const auto address = in_.values.get<ast::VLvalue>(arg.value);
    if (address.offset || in_.seqs.size(address.path) || !in_.values.is<ast::VEntity>(address.base))
      return std::unexpected("a function pointer argument without a function entity");
    const auto entity = in_.values.get<ast::VEntity>(address.base).entity;
    const auto target = functions_ ? functions_(entity) : std::nullopt;
    if (!target) return std::unexpected("a function pointer argument without an external name");
    // [conv.fctptr]: Clang encodes a conversion to a potentially throwing pointer before the
    // address expression. Its type participates in the substitution table before the name.
    std::string expression = "X";
    if (compatibility_ != ast::Compatibility::gcc &&
        in_.canonical(ast::pointee_of(in_, arg.type)) != in_.canonical(target->declaration.type)) {
      auto pointer = type(arg.type);
      if (!pointer) return pointer;
      expression += "cv" + *pointer;
    }
    // Itanium §5.1.6.2: the address-of expression contains the entity literal. A nested
    // C++ encoding uses this mangler so the entity and its enclosing name share substitutions.
    auto name = target->lang == ast::LangLinkage::c ? source_name(target->declaration.name)
                                                  : local_encoding(target->declaration);
    if (!name) return name;
    return expression + "adL_Z" + *name + "EE";
  }

  bool object_pointer_type_matches(ast::QualType pointee, ast::QualType object, bool fixed_class_parameter) const {
    pointee = in_.canonical(pointee);
    object = in_.canonical(object);
    if (ast::without_cv(pointee) == ast::without_cv(object)) return true;
    // The bare entity literal is shared by both peers for fixed class parameters. In other
    // template contexts the argument's pointer type can distinguish specializations (#1073).
    if (!fixed_class_parameter) return false;

    // [conv.ptr]: erasing an object pointer to cv void preserves the address and can add cv.
    if (ast::is_void(in_, pointee)) {
      if (ast::is_reference(in_, object) || ast::is_function(in_, object)) return false;
      // The canonical array type represents its cv-qualifiers on the elements.
      while (in_.types.is<ast::TArray>(object))
        object = in_.types.get<ast::TArray>(object).elem;
      const auto object_cv = std::to_underlying(ast::cv_of(object));
      const auto pointee_cv = std::to_underlying(ast::cv_of(pointee));
      return (pointee_cv & object_cv) == object_cv;
    }

    // [basic.link]: the named object's first declaration can lack the bound seen by the argument.
    if (!in_.types.is<ast::TArray>(object) || !in_.types.is<ast::TArray>(pointee)) return false;
    const auto declared = in_.types.get<ast::TArray>(object);
    const auto addressed = in_.types.get<ast::TArray>(pointee);
    return declared.bound == ast::ConstId::none && in_.values.is<ast::VInt>(addressed.bound) &&
           declared.elem == addressed.elem;
  }

  std::expected<std::string, std::string> object_pointer_argument(const ast::TemplateArg& arg, bool fixed_class_parameter) {
    const auto address = in_.values.get<ast::VLvalue>(arg.value);
    if (address.offset || in_.seqs.size(address.path) || !in_.values.is<ast::VEntity>(address.base))
      return std::unexpected("an object pointer argument without a whole named object");
    const auto entity = in_.values.get<ast::VEntity>(address.base).entity;
    const auto target = objects_ ? objects_(entity) : std::nullopt;
    if (!target) return std::unexpected("an object pointer argument without a supported object name");
    if (!object_pointer_type_matches(ast::pointee_of(in_, arg.type), target->type, fixed_class_parameter))
      return std::unexpected("an object pointer argument with a different pointee type");
    // Itanium §5.1.6.2: the address-of an entity literal. The name's prefix participates in the
    // enclosing symbol's substitution table, and qualification conversions keep the same value.
    auto name = object_encoding(target->name, target->scope, target->internal);
    if (!name) return name;
    return "XadL_Z" + *name + "EE";
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
    if (in_.types.is<ast::TDeferredAlias>(c)) return dependent(in_.types.get<ast::TDeferredAlias>(c).result);
    if (in_.types.is<ast::TTemplateParam>(c) || in_.types.is<ast::TDepName>(c) || in_.types.is<ast::TPackExpansion>(c) ||
        in_.types.is<ast::TTransform>(c) || in_.types.is<ast::TDepSpec>(c) || in_.types.is<ast::TBuiltinSpec>(c) ||
        in_.types.is<ast::TPackIndex>(c))
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
    if (p.kind == ast::TemplateArgKind::type) {
      if (p.type_constraint == ast::ConstId::none) return r + "Ty";
      const auto c = in_.values.get<ast::VDepConcept>(p.type_constraint);
      const ClassName n = classes_ ? classes_(c.concept_) : ClassName{};
      if (n.name.empty()) return std::unexpected("a constrained template parameter whose concept is unknown");
      std::string name = source_name(n.name);
      if (n.scope != ast::EntityId::none) {
        auto scope = prefix(n.scope);
        if (!scope) return scope;
        name = *scope + name;
      }
      std::string args;
      if (in_.seqs.size(c.args) != 0) {
        auto a = template_args(in_.seqs.elements(c.args));
        if (!a) return a;
        args = *a;
      }
      if (compatibility_ == ast::Compatibility::gcc)
        return r + "Tk" + delimit_name(name, n.scope) + args;
      return r + "Tk" + delimit_name(name + args, n.scope);
    }
    if (p.kind == ast::TemplateArgKind::value) {
      if (p.placeholder != ast::Placeholder::none) return r + (p.placeholder == ast::Placeholder::decltype_auto ? "TnDc" : "TnDa");
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
  // is not the parameter's ([temp.arg.template]). GCC writes its prefixes only after an
  // <unscoped-name>: a template in a namespace other than `std`, or a member, has none (`nested`).
  std::expected<std::string, std::string> own_template_args(std::span<const ast::TemplateArg> args,
                                                            std::span<const ast::TemplateParamSema> params,
                                                            const std::vector<std::uint8_t>& same_heads,
                                                            bool nested) {
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
        const bool gcc = compatibility_ == ast::Compatibility::gcc;
        // A placeholder parameter's `TnDa` both write, except GCC before an empty pack.
        const bool placeholder = p.placeholder != ast::Placeholder::none;
        const bool decl = p.kind == ast::TemplateArgKind::value    ? (placeholder ? !(gcc && empty)
                                                                                  : !gcc && (dependent(p.type) || empty))
                          : p.kind == ast::TemplateArgKind::template_ ? (empty ? !gcc : !same || (gcc && placeholder_head(p)))
                                                                      : p.type_constraint != ast::ConstId::none;
        if (decl && !(gcc && nested)) {
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

  // Whether a template template parameter's own head declares a placeholder parameter: GCC then
  // writes its `Tt` prefix even for an argument whose head is the same.
  bool placeholder_head(const ast::TemplateParamSema& p) const {
    return std::ranges::any_of(in_.seqs.elements(p.params),
                               [](const ast::TemplateParamSema& q) { return q.placeholder != ast::Placeholder::none; });
  }
  std::expected<std::string, std::string> specialization_args(const TemplateSpec& spec, bool nested) {
    auto args = spec.params.empty() ? template_args(spec.args)
                                    : own_template_args(spec.args, spec.params, spec.same_heads, nested);
    if (!args || spec.head.tokens == Fp128{}) return args;
    args->pop_back();
    *args += constraint_suffix(spec.head, true) + "E";
    return args;
  }

  // A function template's name before its <template-args>: an <unscoped-template-name>, or in a
  // namespace a <template-prefix> after the namespace's <prefix>; a substitution candidate either
  // way (§5.1.5.3, §5.1.10).
  std::expected<std::string, std::string> template_name(std::string_view name, ast::EntityId scope,
                                                       bool special, bool internal_name) {
    std::string r;
    if (scope != ast::EntityId::none) {
      auto p = prefix(scope);
      if (!p) return p;
      r = *p;
    }
    if (internal_name) r += "L";
    if (special) r += name;
    else if (name.starts_with(lex::literal_operator_prefix)) r += "li" + source_name(name.substr(lex::literal_operator_prefix.size()));
    else r += source_name(name);
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
    auto a = template_args(n.args, n.template_params);
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
  // The <prefix> a dependent member name `Q::n` or `Q::template n<args>` stands for inside its
  // `N … E` (§5.1.5.3): Q's own prefix when Q is one too, not a nested `N … E`; then the name, and
  // the arguments after the <template-prefix> `Q::n`. Each prefix is a candidate, the template
  // prefix keyed apart from the template-name argument `Q::template n`, which is a <type>.
  std::expected<std::string, std::string> dep_prefix(ast::QualType q) {
    ast::QualType bare = ast::without_cv(in_.canonical(q));
    if (!in_.types.is<ast::TDepName>(in_.types.canon(bare)) || ast::Cv(std::to_underlying(in_.canonical(q)) & 3) != ast::Cv::none)
      return type(q);
    if (auto s = find(full_key(bare))) return *s;
    auto d = in_.types.get<ast::TDepName>(in_.types.canon(bare));
    std::string r;
    auto prefix_and_name = [&]() -> std::expected<std::string, std::string> {
      auto p = dep_prefix(d.qualifier);
      if (!p) return p;
      return *p + source_name(in_.names.spelling(d.name));
    };
    if (ast::DepNameForm(d.form) == ast::DepNameForm::template_id) {
      auto& ins = const_cast<ast::Interners&>(in_);
      const std::uint64_t key = (std::uint64_t{4} << 32) |
          full_key(ins.intern_type(ast::TDepName{d.qualifier, d.name, std::uint8_t(ast::DepNameForm::template_name), {}}));
      if (auto s = find(key)) {
        r = *s;
      } else {
        auto p = prefix_and_name();
        if (!p) return p;
        r = *p;
        candidates_.push_back(key);
      }
      auto a = template_args(in_.seqs.elements(d.args));
      if (!a) return a;
      r += *a;
    } else {
      auto p = prefix_and_name();
      if (!p) return p;
      r = *p;
    }
    candidates_.push_back(full_key(bare));
    return r;
  }
  std::optional<std::string> find(std::uint64_t key) const {
    for (std::size_t i = 0; i < candidates_.size(); ++i)
      if (candidates_[i] == key) return substitution(i);
    return std::nullopt;
  }

  const ast::Interners& in_;
  const ClassNames& classes_;
  const FunctionNames& functions_;
  const ObjectNames& objects_;
  ast::Compatibility compatibility_;
  ast::u32 requires_depth_ = 0;
  bool template_head_constraint_ = false;
  std::vector<std::uint64_t> candidates_;  // in order of completion
};

// Itanium §5.1.5.1 <operator-name>: the code of `operator<op>` taking `arity` operands.
std::optional<std::string_view> operator_code(std::string_view op, std::size_t arity) {
  if (op == " new") return "nw";
  if (op == " new[]") return "na";
  if (op == " delete") return "dl";
  if (op == " delete[]") return "da";
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

std::expected<std::string, std::string> mangled_type(const ast::Interners& in, ast::QualType type, const ClassNames& classes,
                                                     ast::Compatibility compatibility) {
  return Mangler(in, classes, {}, {}, compatibility).type(type);
}

std::expected<std::string, std::string> mangled_template_parameter_object(const ast::Interners& in,
                                                                        const ast::TemplateArg& argument,
                                                                        const ClassNames& classes,
                                                                        ast::Compatibility compatibility) {
  auto encoding = Mangler(in, classes, {}, {}, compatibility).template_arg(argument);
  if (!encoding) return encoding;
  return "_ZTA" + *encoding;
}

std::expected<std::string, std::string> mangled_function(const ast::Interners& in, std::string_view name,
                                                         ast::QualType type, const ClassNames& classes,
                                                         ast::EntityId scope, bool special, const FunctionNames& functions,
                                                         const ObjectNames& objects, ast::Compatibility compatibility) {
  Mangler m(in, classes, functions, objects, compatibility);
  auto e = m.encoding(name, type, scope, special);
  if (!e) return e;
  return "_Z" + *e;
}

std::expected<std::string, std::string> mangled_specialization(const ast::Interners& in, std::string_view name,
                                                               const TemplateSpec& spec, const ClassNames& classes,
                                                               ast::EntityId scope, bool special, const FunctionNames& functions,
                                                               const ObjectNames& objects, ast::Compatibility compatibility) {
  Mangler m(in, classes, functions, objects, compatibility);
  auto e = m.specialization_encoding(name, spec, scope, special);
  if (!e) return e;
  return "_Z" + *e;
}

std::expected<std::string, std::string> symbol_name(const ast::Interners& in, std::string_view name, ast::QualType type,
                                                    bool is_function, ast::LangLinkage lang,
                                                    const ClassNames& classes, ast::EntityId scope,
                                                    bool special, const TemplateSpec* spec, bool leading_underscore,
                                                    const FunctionNames& functions, const ObjectNames& objects, ast::Compatibility compatibility) {
  const std::string prefix = leading_underscore ? "_" : "";
  if (spec) {
    auto m = mangled_specialization(in, name, *spec, classes, scope, special, functions, objects, compatibility);
    if (!m) return m;
    return prefix + *m;
  }
  if (scope != ast::EntityId::none && !is_function) {
    // A static data member or a namespace's variable (§5.1.2).
    Mangler m(in, classes, functions, objects, compatibility);
    auto encoding = m.object_encoding(name, scope);
    if (!encoding) return encoding;
    return prefix + "_Z" + *encoding;
  }
  // [basic.start.main]: only the global namespace's `main` is the program's.
  if (!is_function || (name == "main" && scope == ast::EntityId::none) || lang == ast::LangLinkage::c)
    return prefix + std::string(name);
  auto m = mangled_function(in, name, type, classes, scope, special, functions, objects, compatibility);
  if (!m) return m;
  return prefix + *m;
}

}  // namespace ycxx::pipeline
