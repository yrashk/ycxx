// The parser: recursive descent over one part of one item (docs/design/ast.md §3.2, §3.4), with
// a function per grammar production of [gram], calling sema's actions as it goes. It reads the
// region's tokens in [begin, end) and never past them, so a part is analysed alone, as the
// per-item queries require.
//
// The α subset (#39) is parsed; what lies beyond it is diagnosed as not supported where the
// grammar meets it, so M2 widens these functions rather than replacing them.
export module ycxx.parse:parser;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pp;
import ycxx.sema;

export namespace ycxx::parse {

using lex::Tk;
using lex::Tok;
using sema::Builtin;
using sema::Diag;
using sema::Ex;
using sema::LangLinkage;
using sema::QualType;
using sema::u16;
using sema::u32;
using sema::u64;
using sema::u8;
using ast::NameId;
using ast::NodeId;

// [dcl.spec]: what a decl-specifier-seq says.
struct DeclSpec {
  u32 begin = 0;
  QualType type{};
  bool has_type = false;
  bool is_static = false;
  bool is_extern = false;
  bool is_inline = false;
  bool is_constexpr = false;
  bool is_consteval = false;
  bool is_constinit = false;
  bool is_typedef = false;  // [dcl.typedef]: the declarators declare typedef-names
  // An elaborated-type-specifier ([dcl.type.elab]): the class it names, and where its name is.
  bool elaborated = false;
  NameId class_name{};
  u32 class_at = 0;
  bool ok = true;
};

// A parameter of a function declarator ([dcl.fct]), its type adjusted.
struct Param {
  QualType type{};
  NameId name{};
  u32 at = 0;
  // A function parameter pack ([dcl.fct]/23): `type` is its pack expansion; in an instantiation,
  // one element of it (`element`).
  bool pack = false;
  bool element = false;
};

// What a declarator declares ([dcl.decl]): its name and type, and, for a function declarator,
// the parameters of the function type closest to the name.
struct Declarator {
  NameId name{};
  bool named = false;
  u32 name_at = 0;
  // `N::C::name`: the namespace or class the nested-name-specifier names ([dcl.meaning]).
  bool qualified = false;
  NameId qualifier{};  // its last component
  u32 qualifier_at = 0;
  sema::Sema::Scope scope{};
  u32 begin = 0;
  QualType type{};
  std::vector<Param> params;
  bool variadic = false;
  // `T x[] = {…}`: the outermost array bound is left to the initializer ([dcl.array]/8); `type`
  // is then the element type.
  bool unknown_bound = false;
  bool function = false;  // it has a parameter list of its own, not a function type from a name
  // `f<args>`: a declarator-id that is a template-id, which an explicit specialization declares
  // ([temp.expl.spec]/1); `f<>` has none.
  std::optional<std::vector<ast::TemplateArg>> template_args;
  u32 template_args_at = 0;
  // A GNU asm label, `__asm("_fopen")`: the symbol as written (docs/EXTENSIONS.md); empty if none.
  std::string asm_label;
  bool pack = false;  // `T... x`: a declarator-id after an ellipsis ([dcl.fct]/23, [temp.param]/17)
  bool ok = true;
};

class Parser {
public:
  // `part_begin` and `part_end` delimit the part in the region; offsets in the tree count from
  // `part_begin`.
  Parser(const pp::Region& r, u32 part_begin, u32 part_end, sema::Sema& s)
      : r_(&r), begin_(part_begin), end_(part_end), i_(part_begin), s_(&s) {}

  sema::Sema& sema() { return *s_; }
  bool at_end() const { return i_ >= end_; }

  // ------------------------------------------------------------ tokens

  Tk kind(u32 ahead = 0) const {
    if (ahead == 0 && half_) return Tk::greater;  // the second `>` of a `>>` ([temp.names]/4)
    return i_ + ahead < end_ ? r_->tokens[i_ + ahead].kind : Tk::eof;
  }
  const Tok& tok(u32 ahead = 0) const { return r_->tokens[i_ + ahead]; }
  bool is(Tk k) const { return kind() == k; }
  u32 pos() const { return i_ - begin_; }  // the current token's offset in the part
  void bump() {
    half_ = false;
    if (i_ < end_) ++i_;
  }
  bool accept(Tk k) {
    if (!is(k)) return false;
    bump();
    return true;
  }
  // A missing token is reported where it was expected: at the token before, when the next one
  // starts a new line.
  bool expect(Tk k) {
    if (accept(k)) return true;
    bool new_line = i_ >= end_ || r_->tokens[i_].has(lex::tok_flag::line_start);
    s_->diag(new_line && i_ > begin_ ? pos() - 1 : pos(), Diag::expected_token, k);
    return false;
  }
  NameId name_of(const Tok& t) { return s_->ins().names.intern(r_->name(t)); }
  // Moves to the region's token `i`, inside the part (past a class's member-specification).
  void seek(u32 i) { i_ = std::min(i, end_); }
  const lex::Literal& literal() const { return r_->literal(tok()); }
  const lex::Literal& literal_at(u32 ahead) const { return r_->literal(tok(ahead)); }

  // Skips to just after the next `;` at bracket depth 0, or to a `}` that closes the enclosing
  // block (not consumed).
  void skip_to_semi() {
    int depth = 0;
    while (!at_end()) {
      Tk k = kind();
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) ++depth;
      if (k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace) {
        if (depth == 0) return;
        --depth;
      }
      bump();
      if (k == Tk::semi && depth == 0) return;
    }
  }

  // ------------------------------------------------------------ [dcl.spec]

  static bool starts_decl_specifier(Tk k) {
    switch (k) {
    case Tk::kw_void: case Tk::kw_bool: case Tk::kw_char: case Tk::kw_short: case Tk::kw_int: case Tk::kw_long:
    case Tk::kw_signed: case Tk::kw_unsigned: case Tk::kw_const: case Tk::kw_volatile: case Tk::kw_static:
    case Tk::kw_extern: case Tk::kw_inline: case Tk::kw_constexpr: case Tk::kw_char8_t: case Tk::kw_char16_t:
    case Tk::kw_char32_t: case Tk::kw_wchar_t: case Tk::kw_float: case Tk::kw_double: case Tk::kw_auto:
    case Tk::kw_typedef: case Tk::kw_struct: case Tk::kw_class: case Tk::kw_union: case Tk::kw_enum:
    case Tk::kw_thread_local: case Tk::kw_constinit: case Tk::kw_consteval: case Tk::kw_register:
    case Tk::kw_mutable: case Tk::kw_typename: case Tk::kw_decltype:
      return true;
    default: return false;
    }
  }

  // A type-transforming built-in here, `__remove_cv(`: its entry.
  const lex::TypeTraitInfo* transform_trait_ahead(u32 j = 0) {
    if (kind(j) != Tk::identifier || kind(j + 1) != Tk::l_paren) return nullptr;
    const lex::TypeTraitInfo* t = s_->type_trait(name_of(tok(j)));
    return t && t->transforms ? t : nullptr;
  }

  // Whether a declaration starts here: a decl-specifier keyword, or a name that denotes a class
  // ([stmt.ambig]: what can be a declaration is one).
  bool starts_declaration() {
    if (transform_trait_ahead()) return true;
    // [stmt.ambig]: `int(x) + 1;`, `int(3);` and `void();` are expressions; `int(x);`, `int(x) = 1;`
    // and `int(*f)(int);` declare.
    if (simple_type_keyword(kind()) && kind(1) == Tk::l_paren) {
      Tk inner = kind(2);
      if (inner == Tk::r_paren || lex::is_literal(inner)) return false;
      u32 j = 1, depth = 0;
      for (; i_ + j < end_; ++j) {
        if (kind(j) == Tk::l_paren) ++depth;
        if (kind(j) == Tk::r_paren && --depth == 0) break;
      }
      Tk after = kind(j + 1);
      return after == Tk::semi || after == Tk::equal || after == Tk::comma || after == Tk::l_square ||
             after == Tk::l_paren || after == Tk::l_brace;
    }
    if (starts_decl_specifier(kind())) return true;
    if (qualified_ahead()) {
      auto q = qualified_type(0);
      return q && kind(q->second) != Tk::l_paren && kind(q->second) != Tk::period && kind(q->second) != Tk::l_brace;
    }
    u32 next = 1;  // the token after the type's name: after `X<…>` for a class template
    if (is(Tk::identifier) && kind(1) == Tk::less && s_->type_template(name_of(tok()))) next = past_template_args(1);
    else if (!is(Tk::identifier) || kind(1) == Tk::coloncolon || !s_->class_name(name_of(tok()))) return false;
    // `T(args).f()` and `T{…}.m` are expressions; `T (x);` declares x ([stmt.ambig]).
    if (kind(next) == Tk::l_brace) return false;
    if (kind(next) != Tk::l_paren) return true;
    u32 j = next, depth = 0;
    for (; i_ + j < end_; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren) ++depth;
      if (k == Tk::r_paren && --depth == 0) break;
    }
    Tk after = kind(j + 1);
    // `T (*p)(args);` and `T (&r)[3];` declare: a ptr-operator begins a nested declarator, which a
    // parameter list or a bound may follow; `T(&x).m` is an expression.
    if (kind(next + 1) == Tk::star || kind(next + 1) == Tk::amp || kind(next + 1) == Tk::ampamp)
      return after != Tk::period && after != Tk::arrow;
    return after != Tk::period && after != Tk::arrow && after != Tk::l_paren && after != Tk::l_square;
  }

  // Whether a qualified name starts here: `::`, `N::`, or `X<…>::` ([basic.lookup.qual]).
  bool qualified_ahead() {
    if (is(Tk::coloncolon)) return true;
    if (!is(Tk::identifier)) return false;
    if (kind(1) == Tk::coloncolon) return true;
    return kind(1) == Tk::less && kind(past_template_args(1)) == Tk::coloncolon;
  }

  // The offset of the token after the template argument list whose `<` is at `j` ahead: angle
  // brackets balanced outside parentheses and brackets, `>>` closing two ([temp.names]/4).
  u32 past_template_args(u32 j) {
    int angles = 0, parens = 0;
    for (; i_ + j < end_; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) ++parens;
      else if ((k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace) && parens > 0) --parens;
      else if (parens == 0 && k == Tk::less) ++angles;
      else if (parens == 0 && k == Tk::greater && --angles == 0) return j + 1;
      else if (parens == 0 && k == Tk::greatergreater) {
        // `>>` closes this list in its first half when only this list is open ([temp.names]/4).
        if (angles == 1) return j;
        if ((angles -= 2) == 0) return j + 1;
      }
      else if (parens == 0 && (k == Tk::semi || k == Tk::l_brace)) return j;
    }
    return j;
  }

  // `X<args>` where X is a class template ([temp.names]): the specialization's type, from the
  // name; the arguments are consumed.
  std::optional<QualType> class_template_type() {
    u32 at = pos();
    auto tmpl = s_->type_template(name_of(tok()));
    bump();
    auto args = template_argument_list();
    if (!tmpl || !args) return std::nullopt;
    return s_->class_template_id(*tmpl, *args, at);
  }

  // What an elaborated-type-specifier naming no declared class does: at namespace scope it
  // declares the class ([basic.scope.pdecl]); set by the owner where that applies.
  std::function<std::optional<QualType>(NameId)> declare_class;

  // decl-specifier-seq ([dcl.spec.general]) with the simple-type-specifiers of [dcl.type.simple],
  // class names and elaborated-type-specifiers ([dcl.type.elab]), and the cv-qualifiers.
  DeclSpec decl_specifiers() {
    DeclSpec d;
    d.begin = pos();
    int n_char = 0, n_short = 0, n_int = 0, n_long = 0, n_signed = 0, n_unsigned = 0, n_void = 0, n_bool = 0;
    int n_char8 = 0, n_char16 = 0, n_char32 = 0, n_wchar = 0, n_float = 0, n_double = 0;
    bool c = false, v = false;
    std::optional<QualType> cls;  // a class-name or elaborated-type-specifier
    auto any_type = [&] {
      return cls || n_char + n_short + n_int + n_long + n_signed + n_unsigned + n_void + n_bool + n_char8 + n_char16 +
                        n_char32 + n_wchar + n_float + n_double > 0;
    };
    auto once = [&](bool& flag, Tk k) {
      if (flag) s_->diag(pos(), Diag::duplicate_specifier, k);
      flag = true;
    };
    for (;;) {
      if (skip_attributes()) continue;
      Tk k = kind();
      switch (k) {
      case Tk::kw_void: ++n_void; break;
      case Tk::kw_bool: ++n_bool; break;
      case Tk::kw_char: ++n_char; break;
      case Tk::kw_char8_t: ++n_char8; break;
      case Tk::kw_char16_t: ++n_char16; break;
      case Tk::kw_char32_t: ++n_char32; break;
      case Tk::kw_wchar_t: ++n_wchar; break;
      case Tk::kw_float: ++n_float; break;  // types only: their values are #201
      case Tk::kw_double: ++n_double; break;
      case Tk::kw_short: ++n_short; break;
      case Tk::kw_int: ++n_int; break;
      case Tk::kw_long: ++n_long; break;
      case Tk::kw_signed: ++n_signed; break;
      case Tk::kw_unsigned: ++n_unsigned; break;
      case Tk::kw_const: once(c, k); break;
      case Tk::kw_volatile: once(v, k); break;
      case Tk::kw_static: once(d.is_static, k); break;
      case Tk::kw_extern:
        if (kind(1) == Tk::string_lit) goto done;  // a linkage-specification, not a specifier
        once(d.is_extern, k);
        break;
      case Tk::kw_inline: once(d.is_inline, k); break;
      case Tk::kw_decltype: {
        // `decltype(e)` ([dcl.type.decltype]); `decltype(auto)` is not supported yet.
        if (any_type() || kind(1) != Tk::l_paren || kind(2) == Tk::kw_auto) {
          s_->diag(pos(), Diag::not_supported, k);
          d.ok = false;
          goto done;
        }
        bump();
        bump();
        int saved = std::exchange(angle_, 0);
        Ex e = expression();
        angle_ = saved;
        if (!e.ok() || !expect(Tk::r_paren)) {
          d.ok = false;
          goto done;
        }
        cls = s_->decltype_of(e, s_->as<ast::Paren>(e).has_value());
        continue;
      }
      case Tk::kw_constexpr: once(d.is_constexpr, k); break;
      case Tk::kw_consteval: once(d.is_consteval, k); break;
      case Tk::kw_constinit: once(d.is_constinit, k); break;
      case Tk::kw_typedef: once(d.is_typedef, k); break;
      case Tk::kw_enum:
        if (kind(1) == Tk::identifier && kind(2) != Tk::l_brace && kind(2) != Tk::colon) {
          // An elaborated-type-specifier naming an enumeration ([dcl.type.elab]).
          bump();
          auto t = s_->class_name(name_of(tok()), true, false);
          if (!t && s_->class_name(name_of(tok()), true)) {  // [dcl.type.elab]/2
            s_->diag(pos(), Diag::elaborated_typedef, name_of(tok()));
            d.ok = false;
          } else if (!t || !s_->types().is_enum(*t)) {
            s_->diag(pos(), Diag::not_a_type, name_of(tok()));
            d.ok = false;
          } else {
            cls = t;
          }
          break;
        }
        s_->diag(pos(), Diag::not_supported, k);  // an enum-specifier where a type is named
        d.ok = false;
        goto done;
      case Tk::kw_typename: {
        // `typename nested-name-specifier identifier` ([temp.res.general]/5): a type, dependent
        // until the template is instantiated.
        u32 at = pos();
        bump();
        auto q = qualified_ahead() ? qualified_type(0) : std::nullopt;
        if (!q || any_type()) {
          s_->diag(at, Diag::expected_type_specifier);
          d.ok = false;
          goto done;
        }
        cls = q->first;
        take_qualified(q->second);
        continue;
      }
      case Tk::kw_struct:
      case Tk::kw_class:
      case Tk::kw_union: {
        u32 at = pos();
        // A class-specifier where a type is named (a local or unnamed class) is not supported yet.
        if (kind(1) != Tk::identifier || kind(2) == Tk::l_brace || kind(2) == Tk::colon || kind(2) == Tk::coloncolon) {
          s_->diag(at, Diag::not_supported, k);
          d.ok = false;
          goto done;
        }
        bump();
        NameId n = name_of(tok());
        bump();
        auto t = s_->class_name(n, true, false);
        if (!t && s_->class_name(n, true)) {  // [dcl.type.elab]/2
          s_->diag(at + 1, Diag::elaborated_typedef, n);
          d.ok = false;
          continue;
        }
        if (!t && declare_class) t = declare_class(n);
        if (!t) {
          s_->diag(at + 1, Diag::not_a_type, n);
          d.ok = false;
        } else if (any_type()) {
          s_->diag(at, Diag::conflicting_specifiers, k);
          d.ok = false;
        } else {
          cls = t;
          d.elaborated = true;
          d.class_name = n;
          d.class_at = at + 1;
        }
        continue;
      }
      case Tk::coloncolon:
      case Tk::identifier:
        if (!any_type()) {
          // A type-transforming built-in, `__remove_cv(T)` (docs/EXTENSIONS.md).
          if (const lex::TypeTraitInfo* tr = transform_trait_ahead()) {
            u32 at = pos();
            bump();
            bump();
            auto t = type_id();
            if (!t || !expect(Tk::r_paren)) {
              d.ok = false;
              goto done;
            }
            cls = s_->trait_type(tr->trait, *t, at);
            if (!cls) {
              d.ok = false;
              goto done;
            }
            continue;
          }
          if (qualified_ahead()) {
            // `N::C`, `::C`, `C::E`, `X<T>::E` ([dcl.type.simple]).
            if (auto q = qualified_type(0); q && kind(q->second) != Tk::l_paren) {
              cls = q->first;
              take_qualified(q->second);
              continue;
            }
            goto done;
          }
          if (kind(1) == Tk::less && s_->type_template(name_of(tok()))) {
            auto t = class_template_type();
            if (!t) {
              d.ok = false;
              goto done;
            }
            cls = t;
            continue;  // the arguments are consumed
          }
          if (kind(1) != Tk::coloncolon)
            if (auto t = s_->class_name(name_of(tok()))) {
              cls = t;
              break;
            }
        }
        goto done;
      default:
        if (starts_decl_specifier(k)) {
          s_->diag(pos(), Diag::not_supported, k);
          d.ok = false;
          break;
        }
        goto done;
      }
      bump();
    }
  done:
    if (d.is_static && d.is_extern) {  // [dcl.stc]/1
      s_->diag(d.begin, Diag::storage_class_conflict);
      d.ok = false;
    }
    // [dcl.typedef]/1: `typedef` is combined only with type-specifiers.
    if (d.is_typedef && (d.is_static || d.is_extern || d.is_inline || d.is_constexpr || d.is_consteval || d.is_constinit)) {
      s_->diag(d.begin, Diag::conflicting_specifiers, Tk::kw_typedef);
      d.ok = false;
    }
    if (int(d.is_constexpr) + int(d.is_consteval) + int(d.is_constinit) > 1) {  // [dcl.spec]/2
      s_->diag(d.begin, Diag::constexpr_specifiers);
      d.ok = false;
    }
    // [dcl.type.general]: the combinations of simple-type-specifiers, as in its table.
    int total = n_char + n_short + n_int + n_long + n_signed + n_unsigned + n_void + n_bool + n_char8 + n_char16 +
                n_char32 + n_wchar + n_float + n_double;
    if (cls) {
      if (total > 0) {
        s_->diag(d.begin, Diag::conflicting_specifiers, Tk::kw_int);
        d.ok = false;
      }
      d.has_type = true;
      d.type = s_->types().add_cv(*cls, sema::Cv((c ? 1 : 0) | (v ? 2 : 0)));
      return d;
    }
    if (total == 0) {
      if (d.ok) s_->diag(d.begin, Diag::missing_type_specifier);
      d.ok = false;
      d.type = s_->builtin(Builtin::int_);
      return d;
    }
    std::optional<Builtin> b;
    int sign = n_signed + n_unsigned;
    int others = n_void + n_bool + n_char8 + n_char16 + n_char32 + n_wchar;
    if (n_signed > 1 || n_unsigned > 1 || sign > 1 || n_long > 2 || n_short > 1 || n_int > 1 || n_char > 1 || others > 1) {
    } else if (n_float + n_double > 0) {
      // `float`, `double`, `long double` alone ([dcl.type.general]).
      if (n_float + n_double == 1 && others + n_char + n_short + n_int + sign == 0 && n_long <= n_double)
        b = n_float ? Builtin::float_ : n_long ? Builtin::long_double : Builtin::double_;
    } else if (others == 1) {
      if (n_char + n_short + n_int + n_long + sign == 0)
        b = n_void ? Builtin::void_ : n_bool ? Builtin::bool_ : n_char8 ? Builtin::char8_t_ : n_char16 ? Builtin::char16_t_
          : n_char32 ? Builtin::char32_t_ : Builtin::wchar_t_;
    } else if (n_char == 1) {
      if (n_short + n_int + n_long == 0)
        b = n_signed ? Builtin::signed_char : n_unsigned ? Builtin::unsigned_char : Builtin::char_;
    } else if (n_short == 1) {
      if (n_long == 0) b = n_unsigned ? Builtin::unsigned_short : Builtin::short_;
    } else if (n_long == 1) {
      b = n_unsigned ? Builtin::unsigned_long : Builtin::long_;
    } else if (n_long == 2) {
      b = n_unsigned ? Builtin::unsigned_long_long : Builtin::long_long;
    } else {
      b = n_unsigned ? Builtin::unsigned_int : Builtin::int_;
    }
    if (!b) {
      s_->diag(d.begin, Diag::conflicting_specifiers, Tk::kw_int);
      d.ok = false;
      b = Builtin::int_;
    }
    d.has_type = true;
    d.type = s_->types().add_cv(s_->builtin(*b), sema::Cv((c ? 1 : 0) | (v ? 2 : 0)));
    return d;
  }

  // ------------------------------------------------------------ [dcl.decl]

  // One level of a declarator: its ptr-operators, then either a declarator-id or a parenthesized
  // declarator, then its function and array suffixes.
  struct Suffix {
    bool function = false;
    std::vector<Param> params;
    std::vector<NameId> empty_packs;  // function parameter packs instantiated with no element
    bool variadic = false;
    sema::Cv cv = sema::Cv::none;  // a member function's cv-qualifier-seq ([dcl.fct])
    u64 bound = 0;
    bool unknown_bound = false;  // `[]`: in a parameter, or the outermost bound of a variable
    // A value-dependent bound ([temp.dep.constexpr]): a template parameter's `VDepParam`.
    std::optional<sema::ConstId> dependent_bound;
  };
  struct Level {
    // ptr-operators, outermost first ([dcl.ptr], [dcl.ref]): `*` with its cv, `&` or `&&`.
    struct PtrOp {
      Tk op;
      sema::Cv cv;
    };
    std::vector<PtrOp> pointers;
    std::unique_ptr<Level> inner;
    NameId name{};
    bool named = false;
    u32 name_at = 0;
    bool qualified = false;
    NameId qualifier{};
    u32 qualifier_at = 0;
    sema::Sema::Scope scope{};
    std::optional<std::vector<ast::TemplateArg>> template_args;
    u32 template_args_at = 0;
    bool pack = false;
    std::vector<Suffix> suffixes;
  };

  // declarator, or abstract-declarator when `abstract` ([dcl.decl.general], [dcl.name]).
  Declarator declarator(QualType base, bool abstract) {
    Declarator d;
    d.begin = pos();
    Level top = level(abstract, d.ok);
    top_ = &top;
    const Suffix* fn = nullptr;
    d.type = build(top, base, fn, d);
    if (fn) {
      d.function = true;
      d.params = fn->params;
      d.variadic = fn->variadic;
    }
    if (d.ok) {
      skip_attributes();
      if (!abstract) {
        d.asm_label = asm_label(d.ok);
        skip_attributes();
      }
    }
    return d;
  }

  // Attribute-specifiers ([dcl.attr.grammar]), and the GNU `__attribute__((…))` the SDK's
  // headers use (docs/EXTENSIONS.md): skipped, as no attribute ycxx knows changes what it does
  // yet ([dcl.attr.grammar]/6 lets an implementation ignore one it does not know). Whether any
  // was there: `attr_std` for an attribute-specifier, `attr_gnu` for a GNU attribute.
  static constexpr int attr_std = 1, attr_gnu = 2;
  int skip_attributes() {
    int any = 0;
    for (;;) {
      bool std_attr = is(Tk::l_square) && kind(1) == Tk::l_square;
      bool gnu = is(Tk::identifier) && name_spelling() == "__attribute__" && kind(1) == Tk::l_paren;
      if (!std_attr && !gnu) return any;
      any |= std_attr ? attr_std : attr_gnu;
      if (gnu) bump();
      Tk open = kind(), close = open == Tk::l_square ? Tk::r_square : Tk::r_paren;
      u32 depth = 0;
      do {
        if (is(open)) ++depth;
        else if (is(close)) --depth;
        bump();
      } while (depth > 0 && !at_end());
    }
  }
  // The token index `ahead` moves to past any attributes that start there.
  u32 past_attributes(u32 ahead) const {
    for (;;) {
      bool std_attr = kind(ahead) == Tk::l_square && kind(ahead + 1) == Tk::l_square;
      bool gnu = kind(ahead) == Tk::identifier && r_->name(tok(ahead)) == "__attribute__" && kind(ahead + 1) == Tk::l_paren;
      if (!std_attr && !gnu) return ahead;
      if (gnu) ++ahead;
      Tk open = kind(ahead), close = open == Tk::l_square ? Tk::r_square : Tk::r_paren;
      u32 depth = 0;
      do {
        if (kind(ahead) == open) ++depth;
        else if (kind(ahead) == close) --depth;
        ++ahead;
      } while (depth > 0 && i_ + ahead < end_);
    }
  }
  // [over.oper.general]/1: the punctuators an operator function may be named by.
  static bool overloadable(Tk k) {
    switch (k) {
    case Tk::l_brace: case Tk::r_brace: case Tk::l_paren: case Tk::r_paren: case Tk::l_square: case Tk::r_square:
    case Tk::semi: case Tk::colon: case Tk::coloncolon: case Tk::period: case Tk::question: case Tk::ellipsis:
    case Tk::hash: case Tk::hashhash: case Tk::periodstar:
      return false;
    default: return true;
    }
  }
  // A GNU asm label after a declarator, `asm("…")`, `__asm("…")` or `__asm__("…")`: the
  // string, the symbol's name as the assembler sees it.
  std::string asm_label(bool& ok) {
    bool keyword = is(Tk::kw_asm) || (is(Tk::identifier) && (name_spelling() == "__asm" || name_spelling() == "__asm__"));
    if (!keyword || kind(1) != Tk::l_paren) return {};
    bump();
    bump();
    std::string label;
    if (!is(Tk::string_lit) || literal().encoding != lex::Encoding::ordinary) {  // an ordinary string literal
      s_->diag(pos(), Diag::asm_label_string);
      ok = false;
      return {};
    }
    for (u32 u : literal().units) label.push_back(static_cast<char>(u));
    bump();
    while (is(Tk::string_lit) && tok().has(lex::tok_flag::continued)) bump();  // phase 5 joined them
    ok = expect(Tk::r_paren) && ok;
    return label;
  }
  std::string_view name_spelling() const { return r_->name(tok()); }

  Level level(bool abstract, bool& ok) {
    Level l;
    while (is(Tk::star) || is(Tk::amp) || is(Tk::ampamp)) {
      Tk op = kind();
      bump();
      int cv = 0;
      while (op == Tk::star && (is(Tk::kw_const) || is(Tk::kw_volatile))) {
        cv |= is(Tk::kw_const) ? 1 : 2;
        bump();
      }
      l.pointers.push_back({op, sema::Cv(cv)});
    }
    // A parameter pack's declarator ([dcl.fct]/23, [temp.param]/17): `T... x`, `Ts&&...`.
    if (abstract && packs_in_declarators_ && is(Tk::ellipsis)) {
      bump();
      l.pack = true;
    }
    if (!abstract && qualified_ahead()) {
      // A qualified declarator-id `N::C::name`, defining a member of namespace N or class C
      // ([namespace.memdef], [class.mfct], [class.static.data]). What follows it is looked up in
      // that scope.
      u32 next = 0;
      l.qualifier_at = pos();
      auto sc = nested_name(0, next);
      if (!sc) {
        s_->diag(pos(), Diag::not_a_namespace, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
        ok = false;
        return l;
      }
      l.qualified = true;
      l.qualifier = next >= 2 && kind(next - 2) == Tk::identifier ? name_of(tok(next - 2)) : s_->no_name();
      l.scope = *sc;
      for (u32 j = 0; j < next; ++j) bump();
      if (qualified_hook) qualified_hook(*sc);
    }
    if (!abstract && is(Tk::kw_operator)) {
      // An operator-function-id ([over.oper.general]): `operator@`, named as the splitter spells
      // it. Conversion functions, `new`, `delete` and `co_await` are not supported yet.
      l.name_at = pos();
      bump();
      std::string spelled = "operator";
      if ((is(Tk::l_paren) && kind(1) == Tk::r_paren) || (is(Tk::l_square) && kind(1) == Tk::r_square)) {
        spelled += is(Tk::l_paren) ? "()" : "[]";
        bump();
        bump();
      } else if (lex::is_punctuator(kind()) && overloadable(kind())) {
        spelled += lex::spelling(kind());
        bump();
      } else {
        s_->diag(pos(), Diag::not_supported, Tk::kw_operator);
        ok = false;
        return l;
      }
      l.name = s_->world().ins.names.intern(spelled);
      l.named = true;
      skip_attributes();
    } else if (is(Tk::identifier)) {
      l.name = name_of(tok());
      l.named = true;
      l.name_at = pos();
      bump();
      // A template-id: the declarator of an explicit specialization ([temp.expl.spec]).
      if (!abstract && is(Tk::less) && template_ids_in_declarators && s_->is_template_name(l.name)) {
        l.template_args_at = pos();
        l.template_args = template_argument_list();
        if (!l.template_args) ok = false;
      }
      skip_attributes();  // appertaining to the declarator-id ([dcl.meaning.general]/1)
    } else if (is(Tk::l_paren) && (kind(1) == Tk::star || kind(1) == Tk::amp || kind(1) == Tk::ampamp ||
                                   kind(1) == Tk::l_paren || kind(1) == Tk::identifier)) {
      // [dcl.ambig.res]: `(` begins a nested declarator when a parameter list cannot start here.
      bump();
      l.inner = std::make_unique<Level>(level(abstract, ok));
      ok = expect(Tk::r_paren) && ok;
    } else if (!abstract) {
      s_->diag(pos(), Diag::expected_unqualified_id);
      ok = false;
    }
    for (;;) {
      if (is(Tk::l_paren) && !abstract && in_params_ == 0 && !params_ahead()) break;  // `T x(args)`: an initializer
      if (is(Tk::l_paren)) {
        bump();
        l.suffixes.push_back(parameters(ok));
        // A member function's cv-qualifier-seq ([dcl.fct]).
        int cv = 0;
        while (is(Tk::kw_const) || is(Tk::kw_volatile)) {
          cv |= is(Tk::kw_const) ? 1 : 2;
          bump();
        }
        l.suffixes.back().cv = sema::Cv(cv);
        // A noexcept-specifier ([except.spec]): accepted, not yet part of the function type (#219).
        if (is(Tk::kw_noexcept)) {
          bump();
          if (is(Tk::l_paren)) {
            int saved = std::exchange(angle_, 0);
            bump();
            Ex e = constant_expression();
            angle_ = saved;
            ok = e.ok() && expect(Tk::r_paren) && ok;
          }
        }
        // ref-qualifiers, dynamic exception specifications and trailing return types are not
        // supported yet.
        if (is(Tk::amp) || is(Tk::ampamp) || is(Tk::kw_noexcept) || is(Tk::kw_throw) || is(Tk::arrow)) {
          s_->diag(pos(), Diag::not_supported, kind());
          ok = false;
          return l;
        }
      } else if (is(Tk::l_square)) {
        u32 at = pos();
        bump();
        if (is(Tk::l_square)) {
          s_->diag(at, Diag::not_supported, Tk::l_square);  // an attribute-specifier
          ok = false;
          return l;
        }
        if (is(Tk::r_square)) {
          bump();
          Suffix s;
          s.unknown_bound = true;
          l.suffixes.push_back(std::move(s));
          continue;
        }
        Suffix s;
        // [dcl.array]: a converted constant expression of integral type.
        Ex e = s_->promote(constant_expression());
        sema::Evaluated ev = sema::Evaluator(*s_, questions()).evaluate(e);
        auto v = ev.value;
        if (ev.dependent) {
          s.dependent_bound = template_value(e, s_->types().canonical(e.type()));
          ok = expect(Tk::r_square) && ok && s.dependent_bound.has_value();
          l.suffixes.push_back(std::move(s));
          continue;
        }
        if (!v || s_->world().values.rec(*v).kind != sema::ValueKind::integer || i64_of(*v) <= 0) {
          if (e.ok()) s_->diag(at + 1, Diag::bound_not_constant);
          ok = false;
        } else {
          s.bound = s_->world().values.rec(*v).bits;
        }
        ok = expect(Tk::r_square) && ok;
        l.suffixes.push_back(std::move(s));
      } else {
        break;
      }
    }
    return l;
  }

  // A nested-name-specifier ([expr.prim.id.qual]) at token `start` ahead: `::`, `N::`, `N::C::`,
  // resolved as it is read, without consuming it. `next` is where the name after it is.
  std::optional<sema::Sema::Scope> nested_name(u32 start, u32& next) {
    u32 j = start;
    std::optional<sema::Sema::Scope> s;
    if (kind(j) == Tk::coloncolon) {
      s = sema::Sema::global_scope();
      ++j;
    } else if (kind(j) == Tk::identifier && kind(j + 1) == Tk::coloncolon) {
      s = s_->scope_named(name_of(tok(j)));
      if (!s) return std::nullopt;
      j += 2;
    } else if (auto t = template_id_scope(std::nullopt, j)) {
      s = t;
    } else {
      return std::nullopt;
    }
    for (;;) {
      if (kind(j) == Tk::identifier && kind(j + 1) == Tk::coloncolon) {
        auto m = s_->scope_member(*s, name_of(tok(j)));
        if (!m) return std::nullopt;
        s = m;
        j += 2;
      } else if (auto t = template_id_scope(s, j)) {
        s = t;
      } else {
        break;
      }
    }
    if (kind(j) != Tk::identifier && kind(j) != Tk::tilde && kind(j) != Tk::kw_operator) return std::nullopt;
    next = j;
    return s;
  }
  // A component `X<args>::` at `j` ahead naming a class template specialization, found by
  // unqualified lookup or in namespace `in` ([temp.names]); `j` moves past the `::`. The arguments
  // are parsed ahead of the tokens being taken and then left, with what they said dropped:
  // nested names are read ahead before the parse commits to them.
  std::optional<sema::Sema::Scope> template_id_scope(std::optional<sema::Sema::Scope> in, u32& j, bool scope = true) {
    if (kind(j) != Tk::identifier || kind(j + 1) != Tk::less) return std::nullopt;
    if (in && !in->ns) return std::nullopt;  // member templates of classes come later (#88)
    NameId n = name_of(tok(j));
    auto tmpl = in ? s_->namespace_type_template(*in->ns, n) : s_->type_template(n);
    if (!tmpl) return std::nullopt;
    u32 saved = i_;
    bool saved_half = half_;
    std::size_t diags = s_->diag_count();
    i_ += j + 1;
    auto args = template_argument_list();
    u32 after = i_ - saved;
    bool closed = scope ? !half_ && kind() == Tk::coloncolon : true;
    bool ended_half = half_;  // the list ended in the first half of a `>>`
    std::optional<QualType> t;
    if (args && closed) t = s_->class_template_id(*tmpl, *args, 0);
    i_ = saved;
    half_ = saved_half;
    if (!scope) qualified_half_ = ended_half;
    s_->truncate_diags(diags);
    if (!t) return std::nullopt;
    j = scope ? after + 1 : after;
    return sema::Sema::Scope{std::nullopt, *t};
  }

  // Whether the qualified type name `qualified_type` read last ended in the first half of a `>>`:
  // the tokens it takes then stop at that `>>`, whose second half its taker keeps.
  bool qualified_half_ = false;
  // Takes the `n` tokens of a qualified type name `qualified_type` read.
  void take_qualified(u32 n) {
    bool half = qualified_half_;
    for (u32 j = 0; j < n; ++j) bump();
    if (half) half_ = true;
  }

  // A qualified type name at token `start`: its type and the tokens it takes.
  std::optional<std::pair<QualType, u32>> qualified_type(u32 start) {
    u32 next = 0;
    auto s = nested_name(start, next);
    if (!s || kind(next) != Tk::identifier || kind(next + 1) == Tk::coloncolon) return std::nullopt;
    // `N::X<args>`: a class template specialization named in a namespace ([temp.names]).
    qualified_half_ = false;
    if (kind(next + 1) == Tk::less && s->ns) {
      u32 j = next;
      if (auto t = template_id_scope(s, j, false)) return std::pair{*t->type, j - start};
    }
    auto t = s_->scope_type(*s, name_of(tok(next)));
    if (!t) return std::nullopt;
    return std::pair{*t, next + 1 - start};
  }
  // Called once a qualified declarator-id is read, before its parameters, so they are looked up
  // in the scope it names ([basic.scope.namespace], [class.mfct]).
  std::function<void(const sema::Sema::Scope&)> qualified_hook;
  // In an explicit specialization, a declarator-id may be a template-id ([temp.expl.spec]/1).
  bool template_ids_in_declarators = false;
  // In a parameter-declaration or a template-parameter, an ellipsis may begin a pack's declarator.
  bool packs_in_declarators_ = false;

  // Whether the `(` here begins a parameter-declaration-clause rather than an initializer
  // ([dcl.ambig.res]: what can be a declaration is one).
  bool params_ahead() {
    if (past_attributes(1) != 1) return true;  // a parameter's attributes ([dcl.fct]/3)
    Tk k = kind(1);
    if (k == Tk::r_paren || k == Tk::ellipsis || starts_decl_specifier(k)) return true;
    if ((k == Tk::identifier && kind(2) == Tk::coloncolon) || k == Tk::coloncolon)
      return qualified_type(1).has_value();  // `N::S`, `C::E`, `::S`
    if (k == Tk::identifier && kind(2) == Tk::less && s_->type_template(name_of(tok(1)))) return true;
    if (transform_trait_ahead(1)) return true;
    return k == Tk::identifier && kind(2) != Tk::coloncolon && s_->class_name(name_of(tok(1))).has_value();
  }

  std::int64_t i64_of(sema::ConstId c) { return s_->world().values.as_signed(c); }

  // The type a declarator gives its name, built from the base type outwards ([dcl.meaning]).
  QualType build(const Level& l, QualType t, const Suffix*& fn, Declarator& d) {
    for (const Level::PtrOp& p : l.pointers) {
      // [dcl.ref]: no pointer to a reference, no reference to void.
      if (s_->types().is_reference(t) && p.op == Tk::star) {
        s_->diag(d.begin, Diag::invalid_array_element, t);
        d.ok = false;
        return t;
      }
      if (s_->types().is_void(t) && p.op != Tk::star) {
        s_->diag(d.begin, Diag::void_variable, s_->no_name());
        d.ok = false;
        return t;
      }
      if (p.op == Tk::star) t = s_->types().add_cv(s_->types().pointer(t), p.cv);
      else t = p.op == Tk::amp ? s_->types().lvalue_ref(t) : s_->types().rvalue_ref(t);
    }
    for (auto s = l.suffixes.rbegin(); s != l.suffixes.rend(); ++s) {
      if (s->function) {
        // [dcl.fct]: a function cannot return a function or an array.
        if (s_->types().is_function(t) || s_->types().is_array(t)) {
          s_->diag(d.begin, Diag::function_returns_function);
          d.ok = false;
        }
        std::vector<QualType> ps;
        for (const Param& p : s->params) ps.push_back(p.type);
        t = s_->types().function(t, ps, s->variadic, linkage_, s->cv);
        fn = &*s;
      } else if (s->unknown_bound && in_params_ > 0) {
        // In a parameter, [dcl.fct]/5 adjusts the array to a pointer: build the pointer at once.
        t = s_->types().pointer(t);
      } else if (s->unknown_bound) {
        // The outermost bound of a variable, which its initializer gives or which stays unknown
        // (`extern T* a[];`: this level's pointers are the element's); any other is not
        // supported yet.
        if (&*s != &l.suffixes.front() || d.unknown_bound || &l != top_ || l.inner) {  // `(&r)[]`, `(*p)[]`
          s_->diag(d.begin, Diag::not_supported, Tk::l_square);
          d.ok = false;
        }
        d.unknown_bound = true;
      } else if (s->dependent_bound && s_->types().is_object(t)) {
        t = s_->types().array_dependent(t, *s->dependent_bound);
      } else if (!s_->types().is_object(t) || s->bound == 0) {
        // [dcl.array]: the element type is an object type; an array of functions or of void is
        // ill-formed.
        if (s->bound != 0) s_->diag(d.begin, Diag::invalid_array_element, t);
        d.ok = false;
      } else {
        t = s_->types().array(t, s->bound);
      }
    }
    if (l.inner) return build(*l.inner, t, fn, d);
    d.name = l.name;
    d.named = l.named;
    d.name_at = l.name_at;
    d.qualified = l.qualified;
    d.qualifier = l.qualifier;
    d.qualifier_at = l.qualifier_at;
    d.scope = l.scope;
    d.template_args = l.template_args;
    d.template_args_at = l.template_args_at;
    d.pack = l.pack;
    return t;
  }

  // `( parameter-declaration-clause ) cv-qualifier-seq` of a constructor or destructor, whose
  // declarator-id the caller has read ([class.ctor], [class.dtor]).
  Suffix function_suffix(bool& ok) {
    if (!expect(Tk::l_paren)) {
      ok = false;
      return {};
    }
    Suffix s = parameters(ok);
    int cv = 0;
    while (is(Tk::kw_const) || is(Tk::kw_volatile)) {
      cv |= is(Tk::kw_const) ? 1 : 2;
      bump();
    }
    s.cv = sema::Cv(cv);
    return s;
  }

  // parameter-declaration-clause ([dcl.fct]), after its `(`.
  Suffix parameters(bool& ok) {
    ++in_params_;
    Suffix s = parameter_list(ok);
    --in_params_;
    // [basic.scope.param]: parameter names are distinct.
    for (std::size_t i = 0; i < s.params.size(); ++i)
      for (std::size_t j = 0; j < i; ++j)
        if (s.params[i].name == s.params[j].name && s.params[i].name != s_->no_name() &&
            !(s.params[i].element && s.params[j].element))  // a pack's elements share its name
          s_->diag(s.params[i].at, Diag::redeclared_local, s.params[i].name);
    return s;
  }
  Suffix parameter_list(bool& ok) {
    Suffix s;
    s.function = true;
    if (is(Tk::kw_void) && kind(1) == Tk::r_paren) {
      bump();
      bump();
      return s;
    }
    while (!is(Tk::r_paren) && !at_end()) {
      if (accept(Tk::ellipsis)) {
        s.variadic = true;
        break;
      }
      u32 at = pos();
      DeclSpec spec = decl_specifiers();
      if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_constexpr) {
        s_->diag(at, Diag::not_supported, Tk::kw_static);
        ok = false;
      }
      bool saved = std::exchange(packs_in_declarators_, true);
      Declarator d = declarator(spec.type, true);
      packs_in_declarators_ = saved;
      ok = ok && spec.ok && d.ok;
      QualType t = d.type;
      // [dcl.fct]/5: arrays and functions decay; top-level cv is dropped.
      if (s_->types().is_array(t)) t = s_->types().pointer(s_->types().rec(t).inner);
      else if (s_->types().is_function(t)) t = s_->types().pointer(t);
      if (s_->types().is_void(t)) {
        s_->diag(at, Diag::void_parameter);
        ok = false;
      }
      // `T...` with no pack in T is `T, ...` ([dcl.fct]/3).
      if (d.pack && !s_->names_pack(t)) {
        if (d.named) {
          s_->diag(d.name_at, Diag::expansion_without_pack);
          ok = false;
        }
        s.params.push_back({t, s_->no_name(), at});
        s.variadic = true;
        break;
      }
      if (d.pack) {
        // A function parameter pack; in an instantiation its elements are the parameters
        // ([temp.variadic]/8).
        QualType e = s_->types().pack_expansion(t);
        std::array<ast::TemplateArg, 1> one{s_->templates().type_arg(e)};
        if (auto xs = s_->expand_bound(one)) {
          for (const ast::TemplateArg& x : *xs)
            s.params.push_back({x.type, d.named ? d.name : s_->no_name(), d.named ? d.name_at : at, false, true});
          if (xs->empty() && d.named) s.empty_packs.push_back(d.name);
        } else {
          s.params.push_back({e, d.named ? d.name : s_->no_name(), d.named ? d.name_at : at, true, false});
        }
        if (accept(Tk::ellipsis)) {  // `Ts......`: a pack, then a C ellipsis ([dcl.fct]/3)
          s.variadic = true;
          break;
        }
        if (!accept(Tk::comma)) break;
        continue;
      }
      // The parameter keeps its top-level cv as a variable of the body; the function type drops
      // it (`function_type` adjusts).
      s.params.push_back({t, d.named ? d.name : s_->no_name(), d.named ? d.name_at : at});
      if (accept(Tk::ellipsis)) {  // `int x...` is `int x, ...` ([dcl.fct]/3)
        s.variadic = true;
        break;
      }
      if (is(Tk::equal)) {
        s_->diag(pos(), Diag::not_supported, Tk::equal);  // default arguments
        ok = false;
        skip_param();
      }
      if (!accept(Tk::comma)) break;
      if (is(Tk::ellipsis)) {
        bump();
        s.variadic = true;
        break;
      }
    }
    ok = expect(Tk::r_paren) && ok;
    return s;
  }
  void skip_param() {
    int depth = 0;
    while (!at_end()) {
      Tk k = kind();
      if (depth == 0 && (k == Tk::comma || k == Tk::r_paren)) return;
      if (k == Tk::l_paren) ++depth;
      if (k == Tk::r_paren) --depth;
      bump();
    }
  }

  // ------------------------------------------------------------ [temp]

  // A template-head ([temp.pre]): `template < template-parameter-list >`, whose parameters belong
  // to the head at `depth`. The head's scope is pushed on sema's template scopes, for the caller
  // to pop at the end of the declaration; each parameter is in scope from the end of its own
  // declaration ([basic.scope.pdecl]), so later ones and their defaults may name it. Not yet:
  // template template parameters, packs and type-constraints.
  bool template_head(u16 depth) {
    s_->template_scopes.emplace_back();
    if (!expect(Tk::kw_template) || !expect(Tk::less)) return false;
    bool ok = true;
    ++angle_;
    if (!is(Tk::greater)) {
      do {
        auto p = template_parameter(depth, u16(s_->template_scopes.back().size()), ok);
        if (!p) break;
        // [temp.param]: the names of a head's parameters are distinct.
        for (const ast::TemplateParamSema& q : s_->template_scopes.back())
          if (q.name == p->name && p->name != s_->no_name()) {
            s_->diag(pos() - 1, Diag::redeclared_local, p->name);
            ok = false;
          }
        s_->template_scopes.back().push_back(*p);
      } while (accept(Tk::comma));
    }
    --angle_;
    return expect(Tk::greater) && ok;
  }

  std::optional<ast::TemplateParamSema> template_parameter(u16 depth, u16 index, bool& ok) {
    ast::TemplateParamSema p{};
    p.depth = depth;
    p.index = index;
    p.name = s_->no_name();
    p.default_ = {ast::TemplateArgKind::type, s_->builtin(Builtin::void_), sema::ConstId::none};
    bool type_param = (is(Tk::kw_class) || is(Tk::kw_typename)) &&
                      (kind(1) == Tk::comma || kind(1) == Tk::greater || kind(1) == Tk::equal || kind(1) == Tk::ellipsis ||
                       (kind(1) == Tk::identifier && kind(2) != Tk::coloncolon));
    if (is(Tk::kw_template)) {
      s_->diag(pos(), Diag::not_supported, Tk::kw_template);  // template template parameters
      ok = false;
      return std::nullopt;
    }
    if (type_param) {
      // A type-parameter ([temp.param]/1), with a default type-id; `class... Ts` is a pack.
      bump();
      if (accept(Tk::ellipsis)) {
        p.pack = 1;
        p.kind = ast::TemplateArgKind::type;
        p.type = s_->types().template_param(depth, index, true);
        if (is(Tk::identifier)) {
          p.name = name_of(tok());
          bump();
        }
        return p;
      }
      if (is(Tk::identifier)) {
        p.name = name_of(tok());
        bump();
      }
      p.kind = ast::TemplateArgKind::type;
      p.type = s_->types().template_param(depth, index);
      if (accept(Tk::equal)) {
        auto t = type_id();
        if (!t) {
          ok = false;
          return std::nullopt;
        }
        p.has_default = 1;
        p.default_ = {ast::TemplateArgKind::type, s_->types().canonical(*t), sema::ConstId::none};
      }
      return p;
    }
    // A non-type template parameter: a parameter-declaration, whose array or function type is
    // adjusted to a pointer and whose top-level cv-qualifiers are ignored ([temp.param]/10, /6).
    u32 at = pos();
    DeclSpec spec = decl_specifiers();
    bool saved = std::exchange(packs_in_declarators_, true);
    Declarator d = declarator(spec.type, true);
    packs_in_declarators_ = saved;
    if (!spec.ok || !d.ok) {
      ok = false;
      return std::nullopt;
    }
    p.pack = d.pack;
    QualType t = d.type;
    if (s_->types().is_array(t)) t = s_->types().pointer(s_->types().rec(t).inner);
    else if (s_->types().is_function(t)) t = s_->types().pointer(t);
    t = sema::Types::unqualified(t);
    // [temp.param]/7: a structural type; class types are not supported yet.
    if (!s_->types().is_integral(t) && !s_->types().is_pointer(t) && !s_->types().is_reference(t) &&
        !s_->types().is(t, Builtin::nullptr_t) && !s_->types().is_dependent(t)) {
      bool later = s_->types().is_class(t) || s_->types().kind(t) == sema::TypeKind::enum_;  // structural, not supported yet
      s_->diag(at, later ? Diag::not_supported : Diag::invalid_template_parameter_type,
               s_->types().is_class(t) ? Tk::kw_class : Tk::kw_enum);
      ok = false;
      return std::nullopt;
    }
    if (d.named) p.name = d.name;
    p.kind = ast::TemplateArgKind::value;
    p.type = t;
    if (!p.pack && accept(Tk::equal)) {
      Ex e = conditional_expression();
      auto v = template_value(e, t);
      if (!v) {
        ok = false;
        return std::nullopt;
      }
      p.has_default = 1;
      p.default_ = {ast::TemplateArgKind::value, s_->types().canonical(t), *v};
    }
    return p;
  }

  // A type-id ([dcl.name]): a decl-specifier-seq and an abstract declarator.
  std::optional<QualType> type_id() {
    DeclSpec spec = decl_specifiers();
    if (!spec.ok) return std::nullopt;
    Declarator d = declarator(spec.type, true);
    if (!d.ok) return std::nullopt;
    if (d.named) {
      s_->diag(d.name_at, Diag::unexpected_identifier, d.name);
      return std::nullopt;
    }
    return d.type;
  }

  // The value of a template argument for a non-type parameter of type `t`: a converted constant
  // expression ([temp.arg.nontype]/1). One that names a template parameter is value-dependent and
  // stays a `VDep*` value ([temp.dep.constexpr]).
  std::optional<sema::ConstId> template_value(Ex e, QualType t) {
    if (!e.ok()) return std::nullopt;
    if (auto lit = s_->as<ast::Literal>(e); lit && s_->world().ins.values.is_dependent(lit->value)) return lit->value;
    // Value-dependent: in a body its value is known when the body is instantiated; in a declaration
    // it is part of the template's signature, which needs the expression's `VDep*` form (#88).
    if (s_->dependent(e)) {
      if (body) return s_->world().ins.intern_value(ast::VDepParam{sema::dependent_depth, 0});
      s_->diag(e.begin, Diag::not_supported, Tk::kw_template);
      return std::nullopt;
    }
    Ex c = s_->convert(e, t, e.begin);
    if (!c.ok()) return std::nullopt;
    sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(c);
    if (!v.value) {
      s_->diag(e.begin, Diag::not_constant, v.reason);
      return std::nullopt;
    }
    return *v.value;
  }

  // A template-argument-list ([temp.arg.general]) after a template-name, from its `<`. An argument
  // that can be a type-id is one ([temp.arg.general]/2).
  std::optional<std::vector<ast::TemplateArg>> template_argument_list() {
    if (!expect(Tk::less)) return std::nullopt;
    std::vector<ast::TemplateArg> args;
    bool ok = true;
    ++angle_;
    if (!is(Tk::greater) && !is(Tk::greatergreater)) {
      do {
        u32 arg_at = pos();
        struct Unframe {
          sema::Sema* s;
          decltype(s->hide_frames()) saved;
          bool on;
          ~Unframe() {
            if (on) s->restore_frames(std::move(saved));
          }
        } unframe{s_, {}, s_->in_expansion() && argument_expands()};
        if (unframe.on) unframe.saved = s_->hide_frames();
        s_->begin_pattern();
        struct EndPattern {
          sema::Sema* s;
          bool expanded = false;
          u32 at;
          ~EndPattern() { s->end_pattern(expanded, at, at, false); }
        } end_pattern{s_, false, arg_at};
        if (starts_type_id()) {
          auto t = type_id();
          if (!t) ok = false;
          else args.push_back({ast::TemplateArgKind::type, s_->types().canonical(*t), sema::ConstId::none});
        } else {
          Ex e = conditional_expression();
          if (!e.ok()) {
            ok = false;
            continue;
          }
          // Converted to its parameter's type when the template is known ([temp.arg.nontype]).
          auto v = template_value(e, sema::Types::unqualified(s_->rvalue(e).type()));
          if (!v) ok = false;
          else args.push_back({ast::TemplateArgKind::value, s_->types().canonical(sema::Types::unqualified(s_->rvalue(e).type())), *v});
        }
        // `pattern...`: a pack expansion ([temp.variadic]/5), expanded where its packs are bound.
        if (ok && is(Tk::ellipsis)) {
          u32 at = pos();
          bump();
          if (!s_->names_pack(args.back())) {
            s_->diag(at, Diag::expansion_without_pack);
            ok = false;
            continue;
          }
          args.back() = s_->templates().expansion_of(args.back());
          end_pattern.expanded = true;
        }
      } while (accept(Tk::comma));
    }
    if (ok)
      if (auto xs = s_->expand_bound(args)) args = *xs;
    --angle_;
    // [temp.names]/4: `>>` closes this list and the enclosing one.
    if (is(Tk::greatergreater) && !half_) half_ = true;
    else if (!expect(Tk::greater)) return std::nullopt;
    if (!ok) return std::nullopt;
    return args;
  }
  bool starts_type_id() {
    if (transform_trait_ahead()) return true;
    if (starts_decl_specifier(kind())) return !is(Tk::kw_static) && !is(Tk::kw_extern);
    if (is(Tk::identifier) && kind(1) == Tk::less && s_->type_template(name_of(tok()))) return true;
    if (qualified_ahead()) return qualified_type(0).has_value();
    return is(Tk::identifier) && kind(1) != Tk::coloncolon && s_->class_name(name_of(tok())).has_value();
  }

  // The language linkage function types get: from an enclosing linkage-specification.
  void set_linkage(LangLinkage l) { linkage_ = l; }
  LangLinkage linkage() const { return linkage_; }

  // ------------------------------------------------------------ pack expansions ([temp.variadic])

  // An operand that may be the pattern of a pack expansion `pattern...` ([temp.variadic]/5);
  // `read` reads one. In a definition the expansion is recorded and is one dependent operand; in
  // an instantiation its pattern is read once for each element, with the packs it names bound
  // to that element (ast.md §6.4).
  template <class F>
  std::vector<Ex> expandable(F read) {
    u32 begin = pos();
    if (const sema::Expansion* x = s_->expansion_at(begin)) {
      sema::Expansion e = *x;
      auto n = s_->expansion_length(e, begin);
      std::vector<Ex> out;
      if (!n) {
        // A pack not bound yet (an enclosing template's): the expansion stays dependent.
        s_->begin_pattern();
        Ex p = read();
        s_->end_pattern(true, begin, e.end);
        accept(Tk::ellipsis);
        out.push_back(s_->dependent_node({p}, begin, pos()));
        return out;
      }
      for (std::size_t i = 0; i < *n; ++i) {
        i_ = begin_ + begin;
        s_->push_frame(e.packs, u32(i));
        out.push_back(read());
        s_->pop_frame();
      }
      i_ = begin_ + e.end;
      expect(Tk::ellipsis);
      return out;
    }
    s_->begin_pattern();
    Ex p = read();
    if (is(Tk::ellipsis)) {
      u32 end = pos();
      bump();
      if (s_->end_pattern(true, begin, end).empty()) s_->diag(end, Diag::expansion_without_pack);
      return {s_->dependent_node({p}, begin, pos())};
    }
    s_->end_pattern(false, begin, 0);
    return {p};
  }

  // ------------------------------------------------------------ fold expressions ([expr.prim.fold])

  static bool fold_operator(Tk k) {
    switch (k) {
    case Tk::plus: case Tk::minus: case Tk::star: case Tk::slash: case Tk::percent: case Tk::caret: case Tk::amp:
    case Tk::pipe: case Tk::lessless: case Tk::greatergreater: case Tk::plusequal: case Tk::minusequal:
    case Tk::starequal: case Tk::slashequal: case Tk::percentequal: case Tk::caretequal: case Tk::ampequal:
    case Tk::pipeequal: case Tk::lesslessequal: case Tk::greatergreaterequal: case Tk::equal: case Tk::equalequal:
    case Tk::exclaimequal: case Tk::less: case Tk::greater: case Tk::lessequal: case Tk::greaterequal:
    case Tk::ampamp: case Tk::pipepipe: case Tk::comma: case Tk::periodstar: case Tk::arrowstar: return true;
    default: return false;
    }
  }
  // Whether the parenthesis here begins a fold-expression: `( ... op`, or at its top level
  // `op ...` followed by `)` or the same operator. Another `...` there (`sizeof...`, an expansion
  // in a template argument list) is not a fold's.
  bool fold_ahead() {
    if (kind(1) == Tk::ellipsis) return fold_operator(kind(2));
    int depth = 0;
    for (u32 j = 1; i_ + j < end_; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) ++depth;
      else if (k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace) {
        if (depth == 0) return false;
        --depth;
      } else if (k == Tk::ellipsis && depth == 0 && kind(j - 1) != Tk::kw_sizeof && fold_operator(kind(j - 1)) &&
                 (kind(j + 1) == Tk::r_paren || kind(j + 1) == kind(j - 1))) {
        return true;
      }
    }
    return false;
  }
  // Whether the template argument here is a pack expansion: `...` ends it before its `,` or `>`.
  bool argument_expands() {
    int depth = 0;
    for (u32 j = 0; i_ + j < end_; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace || k == Tk::less) ++depth;
      else if (k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace || k == Tk::greater || k == Tk::greatergreater) {
        if (depth == 0) return j > 0 && kind(j - 1) == Tk::ellipsis;
        depth -= k == Tk::greatergreater ? 2 : 1;
        if (depth < 0) return j > 0 && kind(j - 1) == Tk::ellipsis;
      } else if (k == Tk::comma && depth == 0) {
        return j > 0 && kind(j - 1) == Tk::ellipsis;
      }
    }
    return false;
  }
  // A fold-expression ([expr.prim.fold]): in a definition one dependent operand; in an
  // instantiation the elements of its pattern nested as [temp.variadic]/11 says, an empty one as
  // /12 says.
  Ex fold_expression(u32 begin) {
    bump();
    int saved = std::exchange(angle_, 0);
    struct Restore {
      int& a;
      int s;
      ~Restore() { a = s; }
    } restore{angle_, saved};
    bool left = false;
    Tk op{};
    std::optional<Ex> init;
    std::optional<std::vector<Ex>> elems;
    if (is(Tk::ellipsis)) {  // ( ... op pattern ), a unary left fold
      bump();
      op = kind();
      bump();
      left = true;
      elems = fold_pattern();
    } else {
      // ( pattern op ... ), ( pattern op ... op init ), or ( init op ... op pattern ).
      // Which operand is the pattern: the one that names a pack, as the definition found.
      u32 first = pos();
      bool a_is_pattern = s_->expansion_at(first) != nullptr;
      if (!a_is_pattern && !s_->expansion_at_any()) {
        std::size_t n = s_->diag_count();
        s_->begin_pattern();
        cast_expression();
        a_is_pattern = !s_->end_pattern(false, first, 0).empty();
        s_->truncate_diags(n);
        i_ = begin_ + first;
      }
      if (a_is_pattern) {
        elems = fold_pattern();
      } else {
        init = cast_expression();
        left = true;
      }
      op = kind();
      bump();
      if (!expect(Tk::ellipsis)) return {NodeId::none, begin, pos()};
      if (!is(Tk::r_paren)) {
        if (kind() != op) {
          s_->diag(pos(), Diag::unexpected_token, kind());
          return {NodeId::none, begin, pos()};
        }
        bump();
        if (left) elems = fold_pattern();
        else init = cast_expression();
      }
    }
    if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
    if (!elems) return s_->dependent_node({}, begin, pos());
    u32 at = begin;
    auto apply = [&](Ex l, Ex r) -> Ex {
      if (op == Tk::comma) return s_->binary(sema::Sema::BinOp::comma, l, r, at);
      if (auto info = binary_operator(op)) return s_->binary(info->op, l, r, at);
      if (auto a = assignment_operator(op)) return s_->assign(*a, l, r, at);
      s_->diag(at, Diag::not_supported, op);
      return {NodeId::none, l.begin, r.end};
    };
    std::vector<Ex>& es = *elems;
    if (es.empty() && !init) {
      if (op == Tk::ampamp) return s_->bool_literal(true, begin, pos());
      if (op == Tk::pipepipe) return s_->bool_literal(false, begin, pos());
      if (op == Tk::comma) return s_->void_prvalue(begin, pos());
      s_->diag(begin, Diag::empty_fold);
      return {NodeId::none, begin, pos()};
    }
    Ex r;
    if (left) {
      std::size_t i = 0;
      r = init ? *init : es[i++];
      for (; i < es.size(); ++i) r = apply(r, es[i]);
    } else {
      std::size_t i = es.size();
      r = init ? *init : es[--i];
      while (i-- > 0) r = apply(es[i], r);
    }
    return {r.id, begin, pos(), r.info};
  }
  // A fold's pattern, a cast-expression: its elements in an instantiation; none in a definition,
  // where the expansion is recorded and the fold is dependent.
  std::optional<std::vector<Ex>> fold_pattern() {
    u32 begin = pos();
    if (const sema::Expansion* x = s_->expansion_at(begin)) {
      sema::Expansion e = *x;
      auto n = s_->expansion_length(e, begin);
      std::vector<Ex> out;
      if (n)
        for (std::size_t i = 0; i < *n; ++i) {
          i_ = begin_ + begin;
          s_->push_frame(e.packs, u32(i));
          out.push_back(cast_expression());
          s_->pop_frame();
        }
      i_ = begin_ + e.end;
      if (!n) return std::nullopt;
      return out;
    }
    s_->begin_pattern();
    cast_expression();
    if (s_->end_pattern(true, begin, pos()).empty()) s_->diag(begin, Diag::expansion_without_pack);
    return std::nullopt;
  }

  // ------------------------------------------------------------ [expr]

  Ex expression() {
    Ex e = assignment_expression();
    while (is(Tk::comma)) {
      u32 at = pos();
      bump();
      Ex r = assignment_expression();
      e = s_->binary(sema::Sema::BinOp::comma, e, r, at);
    }
    return e;
  }

  Ex constant_expression() { return conditional_expression(); }

  static std::optional<sema::Sema::AsgOp> assignment_operator(Tk k) {
    using A = sema::Sema::AsgOp;
    switch (k) {
    case Tk::equal: return A::assign;
    case Tk::starequal: return A::mul;
    case Tk::slashequal: return A::div;
    case Tk::percentequal: return A::rem;
    case Tk::plusequal: return A::add;
    case Tk::minusequal: return A::sub;
    case Tk::lesslessequal: return A::shl;
    case Tk::greatergreaterequal: return A::shr;
    case Tk::ampequal: return A::bit_and;
    case Tk::caretequal: return A::bit_xor;
    case Tk::pipeequal: return A::bit_or;
    default: return std::nullopt;
    }
  }

  // assignment-expression ([expr.assign]): right-associative; its left operand is a
  // logical-or-expression.
  Ex assignment_expression() {
    Ex l = binary_expression(1);
    if (is(Tk::question)) return conditional_rest(l);
    if (auto op = assignment_operator(kind())) {
      u32 at = pos();
      bump();
      if (is(Tk::l_brace)) {
        // [expr.assign]/9: `x = {…}`.
        if (*op != sema::Sema::AsgOp::assign) {
          s_->diag(pos(), Diag::not_supported, Tk::l_brace);
          return {NodeId::none, l.begin, pos()};
        }
        auto b = braced_init_list();
        return s_->assign_list(l, *b, at);
      }
      Ex r = assignment_expression();
      return s_->assign(*op, l, r, at);
    }
    return l;
  }

  Ex conditional_expression() {
    Ex l = binary_expression(1);
    if (is(Tk::question)) return conditional_rest(l);
    return l;
  }

  // [expr.cond]: `logical-or-expression ? expression : assignment-expression`.
  Ex conditional_rest(Ex cond) {
    u32 at = pos();
    bump();
    Ex t = expression();
    expect(Tk::colon);
    Ex f = assignment_expression();
    return s_->conditional(cond, t, f, at);
  }

  // The binary operators from `||` (precedence 1) to the multiplicative ones (10).
  struct BinInfo {
    int prec;
    sema::Sema::BinOp op;
  };
  static std::optional<BinInfo> binary_operator(Tk k) {
    using B = sema::Sema::BinOp;
    switch (k) {
    case Tk::pipepipe: return BinInfo{1, B::logical_or};
    case Tk::ampamp: return BinInfo{2, B::logical_and};
    case Tk::pipe: return BinInfo{3, B::bit_or};
    case Tk::caret: return BinInfo{4, B::bit_xor};
    case Tk::amp: return BinInfo{5, B::bit_and};
    case Tk::equalequal: return BinInfo{6, B::eq};
    case Tk::exclaimequal: return BinInfo{6, B::ne};
    case Tk::less: return BinInfo{7, B::lt};
    case Tk::greater: return BinInfo{7, B::gt};
    case Tk::lessequal: return BinInfo{7, B::le};
    case Tk::greaterequal: return BinInfo{7, B::ge};
    case Tk::lessless: return BinInfo{8, B::shl};
    case Tk::greatergreater: return BinInfo{8, B::shr};
    case Tk::plus: return BinInfo{9, B::add};
    case Tk::minus: return BinInfo{9, B::sub};
    case Tk::star: return BinInfo{10, B::mul};
    case Tk::slash: return BinInfo{10, B::div};
    case Tk::percent: return BinInfo{10, B::rem};
    default: return std::nullopt;
    }
  }

  // logical-or-expression down to multiplicative-expression ([expr.mul]–[expr.log.or]), all
  // left-associative, by precedence climbing.
  Ex binary_expression(int min_prec) {
    Ex l = cast_expression();
    for (;;) {
      if (is(Tk::spaceship) || is(Tk::periodstar) || is(Tk::arrowstar)) {
        s_->diag(pos(), Diag::not_supported, kind());
        bump();
        cast_expression();
        l = {NodeId::none, l.begin, pos()};
        continue;
      }
      auto info = binary_operator(kind());
      if (!info || info->prec < min_prec) return l;
      // [temp.names]/4: in a template argument list the first `>` outside parentheses ends it.
      if (angle_ > 0 && (is(Tk::greater) || is(Tk::greatergreater))) return l;
      u32 at = pos();
      bump();
      Ex r = binary_expression(info->prec + 1);
      l = s_->binary(info->op, l, r, at);
    }
  }

  // cast-expression ([expr.cast]); explicit type conversion is beyond the α subset.
  Ex cast_expression() {
    if (is(Tk::l_paren)) {
      // `(T)e` ([expr.cast]), as static_cast performs it; a parenthesized expression otherwise.
      u32 back = i_, begin = pos();
      bump();
      if (starts_type_id()) {
        // Tentatively: `(T(1)).m` begins with a type too.
        std::size_t n = s_->diag_count();
        auto t = type_id();
        if (t && accept(Tk::r_paren) && !is(Tk::l_brace) && !is(Tk::period) && !is(Tk::arrow)) {
          Ex e = cast_expression();
          Ex r = s_->static_cast_to(e, *t, begin);
          return {r.id, begin, pos(), r.info};
        }
        s_->truncate_diags(n);
      }
      i_ = back;
    }
    return unary_expression();
  }

  // unary-expression ([expr.unary]).
  Ex unary_expression() {
    u32 begin = pos();
    std::optional<ast::UnaryOp> op;
    switch (kind()) {
    case Tk::plusplus: op = ast::UnaryOp::pre_inc; break;
    case Tk::minusminus: op = ast::UnaryOp::pre_dec; break;
    case Tk::star: op = ast::UnaryOp::deref; break;
    case Tk::amp: op = ast::UnaryOp::address_of; break;
    case Tk::plus: op = ast::UnaryOp::plus; break;
    case Tk::minus: op = ast::UnaryOp::minus; break;
    case Tk::exclaim: op = ast::UnaryOp::not_; break;
    case Tk::tilde: op = ast::UnaryOp::compl_; break;
    case Tk::kw_sizeof:
      // `sizeof...(P)` ([expr.sizeof]/5).
      if (kind(1) == Tk::ellipsis && kind(2) == Tk::l_paren && kind(3) == Tk::identifier && kind(4) == Tk::r_paren) {
        NameId n = name_of(tok(3));
        for (int i = 0; i < 5; ++i) bump();
        return s_->sizeof_pack(n, begin, pos());
      }
      [[fallthrough]];
    case Tk::kw_alignof: {
      // `sizeof(T)`, `sizeof e`, `alignof(T)` ([expr.sizeof], [expr.alignof]); the expression is
      // an unevaluated operand; `sizeof...` is above, and another use of `...` here is not supported.
      bool align = is(Tk::kw_alignof);
      bump();
      if (is(Tk::ellipsis)) {
        s_->diag(pos(), Diag::not_supported, Tk::ellipsis);
        return {NodeId::none, begin, pos()};
      }
      if (is(Tk::l_paren)) {
        // Tentatively a type-id: `sizeof(int(1))` and `sizeof(T{})` are expressions.
        u32 back = i_;
        bump();
        if (starts_type_id()) {
          std::size_t n = s_->diag_count();
          auto t = type_id();
          if (t && accept(Tk::r_paren)) {
            Ex r = s_->size_of(*t, align, begin, pos());
            return {r.id, begin, pos(), r.info};
          }
          s_->truncate_diags(n);
        }
        i_ = back;
      }
      if (align) {  // [expr.alignof]/1: a type-id only
        s_->diag(pos(), Diag::expected_type_specifier);
        return {NodeId::none, begin, pos()};
      }
      Ex e = unary_expression();
      if (!e.ok()) return {NodeId::none, begin, pos()};
      Ex r = s_->size_of(e.type(), false, begin, pos());
      return {r.id, begin, pos(), r.info};
    }
    case Tk::kw_new:
    case Tk::kw_delete:
    case Tk::kw_co_await:
    case Tk::kw_noexcept:
    case Tk::caretcaret: {
      s_->diag(begin, Diag::not_supported, kind());
      bump();
      cast_expression();
      return {NodeId::none, begin, pos()};
    }
    default: return postfix_expression();
    }
    bump();
    bool is_incdec = *op == ast::UnaryOp::pre_inc || *op == ast::UnaryOp::pre_dec;
    Ex e = is_incdec ? unary_expression() : cast_expression();
    return s_->unary(*op, e, begin, e.end > begin ? e.end : pos(), begin);
  }

  // postfix-expression ([expr.post]).
  Ex postfix_expression() {
    Ex e = primary_expression();
    // Inside brackets a `>` is an operator again ([temp.names]/4).
    struct Unangle {
      int& a;
      int saved;
      ~Unangle() { a = saved; }
    };
    for (;;) {
      u32 at = pos();
      switch (kind()) {
      case Tk::l_paren: {
        bump();
        Unangle u{angle_, std::exchange(angle_, 0)};
        std::vector<Ex> args;
        if (!is(Tk::r_paren)) {
          do {
            if (is(Tk::l_brace)) {  // copy-list-initializes its parameter ([dcl.init.list]/1)
              args.push_back(s_->braced_argument(braced_init_list()));
              continue;
            }
            for (const Ex& x : expandable([&] { return assignment_expression(); })) args.push_back(x);
          } while (accept(Tk::comma));
        }
        if (!expect(Tk::r_paren)) return {NodeId::none, e.begin, pos()};
        e = immediate(s_->call(e, std::move(args), pos(), at));
        break;
      }
      case Tk::l_square: {
        bump();
        Unangle u{angle_, std::exchange(angle_, 0)};
        Ex index = expression();
        if (!expect(Tk::r_square)) return {NodeId::none, e.begin, pos()};
        e = s_->subscript(e, index, pos(), at);
        break;
      }
      case Tk::plusplus:
      case Tk::minusminus: {
        auto op = is(Tk::plusplus) ? ast::UnaryOp::post_inc : ast::UnaryOp::post_dec;
        bump();
        e = s_->unary(op, e, e.begin, pos(), at);
        break;
      }
      case Tk::period:
      case Tk::arrow: {
        // [expr.ref]: `E1.id-expression` and `E1->id-expression`.
        bool arrow = is(Tk::arrow);
        bump();
        if (!is(Tk::identifier)) {
          if (is(Tk::kw_template) || is(Tk::tilde) || is(Tk::coloncolon) || is(Tk::kw_operator))
            s_->diag(pos(), Diag::not_supported, kind());
          else
            s_->diag(pos(), Diag::expected_unqualified_id);
          if (!at_end()) bump();
          return {NodeId::none, e.begin, pos()};
        }
        u32 name_at = pos();
        NameId n = name_of(tok());
        bump();
        e = s_->member(e, arrow, n, pos(), at, name_at);
        break;
      }
      default: return e;
      }
    }
  }

  // primary-expression ([expr.prim]).
  Ex primary_expression() {
    u32 begin = pos();
    switch (kind()) {
    case Tk::int_lit: {
      const lex::Literal& l = literal();
      bump();
      if (l.ud_suffix != lex::no_name) {
        s_->diag(begin, Diag::not_supported, Tk::int_lit);
        return {NodeId::none, begin, pos()};
      }
      if (l.bad) return {NodeId::none, begin, pos()};
      return s_->integer_literal(l, begin, pos());
    }
    case Tk::char_lit: {
      const lex::Literal& l = literal();
      bump();
      if (l.bad || l.ud_suffix != lex::no_name) {
        if (!l.bad) s_->diag(begin, Diag::not_supported, Tk::char_lit);
        return {NodeId::none, begin, pos()};
      }
      return s_->char_literal(l, begin, pos());
    }
    case Tk::string_lit: {
      const lex::Literal& l = literal();
      bump();
      while (is(Tk::string_lit) && tok().has(lex::tok_flag::continued)) bump();  // phase 5 joined them
      if (l.bad) return {NodeId::none, begin, pos()};
      if (l.ud_suffix != lex::no_name) {
        s_->diag(begin, Diag::not_supported, Tk::string_lit);
        return {NodeId::none, begin, pos()};
      }
      return s_->string_literal(l, begin, pos());
    }
    case Tk::kw_true:
    case Tk::kw_false: {
      bool v = is(Tk::kw_true);
      bump();
      return s_->bool_literal(v, begin, pos());
    }
    case Tk::kw_nullptr: bump(); return s_->nullptr_literal(begin, pos());
    case Tk::l_paren: {
      if (fold_ahead()) return fold_expression(begin);
      bump();
      int saved = std::exchange(angle_, 0);
      Ex e = expression();
      angle_ = saved;
      if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
      return s_->paren(e, begin, pos());
    }
    case Tk::identifier: {
      NameId n = name_of(tok());
      // A type-testing built-in, `__is_same(T, U)` (docs/EXTENSIONS.md).
      if (kind(1) == Tk::l_paren)
        if (const lex::TypeTraitInfo* tr = s_->type_trait(n); tr && !tr->transforms) {
          bump();
          bump();
          std::vector<QualType> args;
          bool ok = true;
          int saved = std::exchange(angle_, 0);
          for (int i = 0; i < tr->arity; ++i) {
            if (i > 0 && !expect(Tk::comma)) ok = false;
            auto t = ok ? type_id() : std::nullopt;
            if (!t) ok = false;
            else args.push_back(*t);
          }
          angle_ = saved;
          if (!expect(Tk::r_paren) || !ok) return {NodeId::none, begin, pos()};
          return s_->trait_expr(tr->trait, args, begin, pos());
        }
      if (qualified_ahead()) return qualified_id(begin);
      // `X<args>(…)` and `X<args>{…}` for a class template X ([expr.type.conv]).
      if (kind(1) == Tk::less && s_->type_template(n)) {
        auto t = class_template_type();
        if (!t) return {NodeId::none, begin, pos()};
        if (is(Tk::l_brace)) {
          auto b = braced_init_list();
          Ex r = s_->list_init(*t, *b);
          return {r.id, begin, pos(), r.info};
        }
        if (!expect(Tk::l_paren)) return {NodeId::none, begin, pos()};
        std::vector<Ex> args;
        if (!is(Tk::r_paren))
          do args.push_back(assignment_expression());
          while (accept(Tk::comma));
        if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
        Ex r = s_->functional_cast(*t, std::move(args), begin, pos());
        return {r.id, begin, pos(), r.info};
      }
      if ((kind(1) == Tk::l_paren || kind(1) == Tk::l_brace))
        if (auto t = s_->class_name(n)) {
          // `T(args)` or `T{…}` ([expr.type.conv]), T a type name: a class, an enumeration, a
          // typedef-name or a template parameter.
          bump();
          if (is(Tk::l_brace)) {
            auto b = braced_init_list();
            Ex r = s_->list_init(*t, *b);
            return {r.id, begin, pos(), r.info};
          }
          bump();
          std::vector<Ex> args;
          if (!is(Tk::r_paren))
            do args.push_back(assignment_expression());
            while (accept(Tk::comma));
          if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
          Ex r = s_->functional_cast(*t, std::move(args), begin, pos());
          return {r.id, begin, pos(), r.info};
        }
      if (kind(1) == Tk::coloncolon) return qualified_id(begin);
      bump();
      if (is(Tk::less) && s_->is_template_name(n)) {
        // A template-id ([temp.names]): `f<args>`.
        auto args = template_argument_list();
        if (!args) return {NodeId::none, begin, pos()};
        // [temp.dep.expr]/3: a template-id with a dependent argument is type-dependent.
        if (std::ranges::any_of(*args, [&](const ast::TemplateArg& a) { return s_->dependent(a); })) {
          // A call of it is a dependent call ([temp.dep.candidate]): its instantiation adds
          // argument-dependent lookup.
          if (is(Tk::l_paren)) s_->dependent_calls.push_back(begin);
          return s_->dependent_node({}, begin, pos());
        }
        return s_->template_id(n, std::move(*args), begin, pos());
      }
      return s_->name_expr(n, begin, pos(), is(Tk::l_paren));
    }
    case Tk::coloncolon: return qualified_id(begin);
    case Tk::kw_this: bump(); return s_->this_expr(begin, pos());
    case Tk::float_lit:
    case Tk::l_square:
    case Tk::kw_requires:
    case Tk::kw_operator:
    case Tk::tilde:
    case Tk::kw_static_cast: {
      // `static_cast<T>(e)` ([expr.static.cast]).
      bump();
      if (!expect(Tk::less)) return {NodeId::none, begin, pos()};
      auto t = type_id();
      if (!t || !expect(Tk::greater) || !expect(Tk::l_paren)) return {NodeId::none, begin, pos()};
      int saved = std::exchange(angle_, 0);
      Ex e = expression();
      angle_ = saved;
      if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
      Ex r = s_->static_cast_to(e, *t, begin);
      return {r.id, begin, pos(), r.info};
    }
    case Tk::kw_typeid:
    case Tk::kw_dynamic_cast:
    case Tk::kw_const_cast:
    case Tk::kw_reinterpret_cast:
    case Tk::kw_throw:
      s_->diag(begin, Diag::not_supported, kind());
      bump();
      return {NodeId::none, begin, pos()};
    default:
      if (simple_type_keyword(kind()) && (kind(1) == Tk::l_paren || kind(1) == Tk::l_brace)) {
        // `int(x)`, `unsigned{}` ([expr.type.conv]): one simple-type-specifier keyword.
        QualType t = decl_specifiers().type;
        return type_conversion(t, begin);
      }
      s_->diag(begin, Diag::expected_expression);
      return {NodeId::none, begin, begin};
    }
  }
  static bool simple_type_keyword(Tk k) {
    switch (k) {
    case Tk::kw_void: case Tk::kw_bool: case Tk::kw_char: case Tk::kw_char8_t: case Tk::kw_char16_t:
    case Tk::kw_char32_t: case Tk::kw_wchar_t: case Tk::kw_short: case Tk::kw_int: case Tk::kw_long:
    case Tk::kw_signed: case Tk::kw_unsigned: case Tk::kw_float: case Tk::kw_double:
      return true;
    default: return false;
    }
  }
  // `T(args)` or `T{…}` at the `(` or `{` ([expr.type.conv]).
  Ex type_conversion(QualType t, u32 begin) {
    if (is(Tk::l_brace)) {
      auto b = braced_init_list();
      Ex r = s_->list_init(t, *b);
      return {r.id, begin, pos(), r.info};
    }
    bump();
    std::vector<Ex> args;
    if (!is(Tk::r_paren))
      do args.push_back(assignment_expression());
      while (accept(Tk::comma));
    if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
    Ex r = s_->functional_cast(t, std::move(args), begin, pos());
    return {r.id, begin, pos(), r.info};
  }

  // A namespace named by `N`, `N::M` or `::N` ([namespace.alias], [namespace.udir]), consumed.
  std::optional<ast::EntityId> namespace_spec() {
    u32 at = pos();
    std::optional<sema::Sema::Scope> s;
    u32 next = 0;
    if (is(Tk::identifier) && kind(1) != Tk::coloncolon) {
      s = s_->scope_named(name_of(tok()));
      next = 0;
      if (s) {
        bump();
        if (s->ns) return s->ns;
      }
    } else if ((s = nested_name(0, next)) && kind(next) == Tk::identifier) {
      for (u32 j = 0; j < next; ++j) bump();
      auto m = s_->scope_member(*s, name_of(tok()));
      bump();
      if (m && m->ns) return m->ns;
    }
    s_->diag(at, Diag::not_a_namespace, s_->no_name());
    skip_to_semi();
    return std::nullopt;
  }

  // braced-init-list ([dcl.init.general]): `{ initializer-clause, … }`, or with designators
  // `{ .m = e, .n{…} }` ([dcl.init.aggr]).
  std::unique_ptr<sema::Braced> braced_init_list() {
    auto b = std::make_unique<sema::Braced>();
    b->begin = pos();
    expect(Tk::l_brace);
    while (!is(Tk::r_brace) && !at_end()) {
      sema::Braced::Clause c;
      c.begin = pos();
      if (is(Tk::period) && kind(1) == Tk::identifier) {
        bump();
        c.designated = true;
        c.designator = name_of(tok());
        bump();
        if (!is(Tk::l_brace) && !expect(Tk::equal)) {
          skip_to_brace_end();
          break;
        }
      }
      if (is(Tk::l_brace)) {
        c.list = braced_init_list();
      } else if (!c.designated) {
        // An initializer-clause may be a pack expansion ([dcl.init.general]/1).
        std::vector<Ex> xs = expandable([&] { return assignment_expression(); });
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
          sema::Braced::Clause e;
          e.begin = c.begin;
          e.expr = xs[i];
          b->clauses.push_back(std::move(e));
        }
        if (xs.empty()) {
          if (!accept(Tk::comma)) break;
          continue;
        }
        c.expr = xs.back();
      } else {
        c.expr = assignment_expression();
      }
      b->clauses.push_back(std::move(c));
      if (!accept(Tk::comma)) break;
    }
    if (!expect(Tk::r_brace)) skip_to_brace_end();
    b->end = pos();
    return b;
  }
  // After an error in a braced list: past its closing `}`.
  void skip_to_brace_end() {
    int depth = 0;
    while (!at_end()) {
      Tk k = kind();
      if (k == Tk::semi && depth == 0) return;
      bump();
      if (k == Tk::l_brace || k == Tk::l_paren || k == Tk::l_square) ++depth;
      if (k == Tk::r_brace || k == Tk::r_paren || k == Tk::r_square) {
        if (depth == 0) return;
        --depth;
      }
    }
  }

  // A qualified-id ([expr.prim.id.qual]): `N::x`, `::x`, `C::m`, `E::e`, `N::C::f`.
  Ex qualified_id(u32 begin) {
    u32 next = 0;
    auto s = nested_name(0, next);
    if (!s || kind(next) != Tk::identifier) {
      s_->diag(begin, Diag::not_a_namespace, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
      while (is(Tk::identifier) || is(Tk::coloncolon)) bump();
      return {NodeId::none, begin, pos()};
    }
    for (u32 j = 0; j < next; ++j) bump();
    u32 name_at = pos();
    NameId n = name_of(tok());
    bump();
    // `N::T(args)` or `N::T{…}` ([expr.type.conv]): a qualified type name, unless a function or
    // variable of the name hides it.
    if ((is(Tk::l_paren) || is(Tk::l_brace)) && !s_->scope_names_object(*s, n))
      if (auto t = s_->scope_type(*s, n)) return type_conversion(*t, begin);
    if (is(Tk::less)) {
      // `N::v<args>`: a variable template of the namespace ([temp.names]/3).
      if (s->ns)
        if (auto v = s_->namespace_template(*s->ns, n, ast::DeclKind::variable)) {
          auto args = template_argument_list();
          if (!args) return {NodeId::none, begin, pos()};
          if (std::ranges::any_of(*args, [&](const ast::TemplateArg& a) { return s_->dependent(a); }))
            return s_->dependent_node({}, begin, pos());
          return s_->variable_template_id(*v, *args, begin, pos());
        }
      Ex r = s_->scope_expr(*s, n, begin, name_at, pos());
      if (r.ok() && s_->types().is(r.type(), Builtin::overload_set)) {
        s_->diag(pos(), Diag::not_supported, Tk::less);  // qualified template-ids (#88)
        return {NodeId::none, begin, pos()};
      }
      return r;
    }
    return s_->scope_expr(*s, n, begin, name_at, pos());
  }

  // ------------------------------------------------------------ [stmt]

  Ex compound_statement() {
    u32 begin = pos();
    if (!expect(Tk::l_brace)) return {NodeId::none, begin, begin};
    s_->push_scope();
    std::vector<Ex> stmts;
    while (!is(Tk::r_brace) && !at_end()) {
      u32 before = i_;
      stmts.push_back(statement());
      if (i_ == before) bump();  // no progress: drop the token
    }
    expect(Tk::r_brace);
    s_->pop_scope();
    std::vector<ast::Child> cs;
    for (const Ex& st : stmts)
      if (st.ok()) cs.push_back(sema::Sema::at(st, begin));
    return s_->node(ast::Compound{s_->builder().list(cs)}, begin, pos());
  }


  // A substatement that is not a compound-statement has its own block scope ([stmt.pre]).
  Ex substatement() {
    if (is(Tk::l_brace)) return compound_statement();
    s_->push_scope();
    Ex s = statement();
    s_->pop_scope();
    return s;
  }

  Ex statement() {
    u32 begin = pos();
    switch (kind()) {
    case Tk::l_brace: return compound_statement();
    case Tk::semi: bump(); return s_->node(ast::ExprStmt{}, begin, pos());
    case Tk::kw_if: return if_statement();
    case Tk::kw_while: return while_statement();
    case Tk::kw_do: return do_statement();
    case Tk::kw_for: return for_statement();
    case Tk::kw_break:
    case Tk::kw_continue: {
      bool brk = is(Tk::kw_break);
      bump();
      expect(Tk::semi);
      if (s_->loop_depth == 0) {
        s_->diag(begin, brk ? Diag::break_outside_loop : Diag::continue_outside_loop);
        return {NodeId::none, begin, pos()};
      }
      if (brk) return s_->node(ast::Break{}, begin, pos());
      return s_->node(ast::Continue{}, begin, pos());
    }
    case Tk::kw_return: return return_statement();
    case Tk::kw_switch:
    case Tk::kw_case:
    case Tk::kw_default:
    case Tk::kw_goto:
    case Tk::kw_try:
    case Tk::kw_static_assert: return static_assert_declaration();
    case Tk::kw_namespace:
      if (kind(1) == Tk::identifier && kind(2) == Tk::equal) {
        // A namespace alias in a block ([namespace.alias]).
        NameId alias = name_of(tok(1));
        bump();
        bump();
        bump();
        if (auto ns = namespace_spec()) s_->namespace_alias(alias, *ns);
        expect(Tk::semi);
        return s_->node(ast::ExprStmt{}, begin, pos());
      }
      s_->diag(begin, Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    case Tk::kw_using:
      if (kind(1) == Tk::kw_namespace) {
        // A using-directive in a block ([namespace.udir]).
        bump();
        bump();
        if (auto ns = namespace_spec()) s_->using_directive(*ns);
        expect(Tk::semi);
        return s_->node(ast::ExprStmt{}, begin, pos());
      }
      if (kind(1) == Tk::identifier && kind(past_attributes(2)) == Tk::equal) {
        // An alias-declaration in a block ([dcl.typedef]/2): `using T [[…]] = type-id;`.
        bump();
        NameId n = name_of(tok());
        u32 at = pos();
        bump();
        skip_attributes();
        bump();
        if (auto t = type_id()) s_->local_type_alias(n, *t, at);
        else {
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        expect(Tk::semi);
        return s_->node(ast::ExprStmt{}, begin, pos());
      }
      if (kind(1) == Tk::identifier || kind(1) == Tk::coloncolon) {
        // A using-declaration in a block ([namespace.udecl]): `using N::x;`.
        bump();
        u32 next = 0;
        auto s = nested_name(0, next);
        if (!s || kind(next) != Tk::identifier) {
          s_->diag(pos(), Diag::not_a_namespace, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        for (u32 j = 0; j < next; ++j) bump();
        s_->using_declaration(*s, name_of(tok()), pos());
        bump();
        expect(Tk::semi);
        return s_->node(ast::ExprStmt{}, begin, pos());
      }
      if (kind(1) == Tk::kw_enum && kind(2) == Tk::identifier) {
        // A using-enum-declaration ([enum.udecl]).
        bump();
        bump();
        u32 at = pos();
        NameId n = name_of(tok());
        bump();
        auto t = s_->class_name(n, true);
        if (!t) s_->diag(at, Diag::not_a_type, n);
        else s_->using_enum(*t, at);
        expect(Tk::semi);
        return s_->node(ast::ExprStmt{}, begin, pos());
      }
      s_->diag(begin, Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    case Tk::kw_co_return:
    case Tk::kw_asm:
    case Tk::kw_contract_assert:
    case Tk::kw_template:
      s_->diag(begin, Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    default: break;
    }
    if (past_attributes(0) != 0) {
      // An attribute-specifier-seq appertaining to the statement ([stmt.pre]/1), or GNU
      // attributes (docs/EXTENSIONS.md).
      skip_attributes();
      return statement();
    }
    if (is(Tk::identifier) && kind(1) == Tk::colon) {
      s_->diag(begin, Diag::not_supported, Tk::colon);  // labels
      skip_statement();
      return {NodeId::none, begin, pos()};
    }
    if (starts_declaration()) return declaration_statement();
    Ex e = expression();
    if (!expect(Tk::semi)) skip_to_semi();
    e = s_->discarded(e);
    return s_->node(ast::ExprStmt{sema::Sema::at(e, begin)}, begin, pos());
  }

  void skip_statement() {
    if (is(Tk::l_brace)) {
      int depth = 0;
      do {
        if (is(Tk::l_brace)) ++depth;
        if (is(Tk::r_brace)) --depth;
        bump();
      } while (depth > 0 && !at_end());
      return;
    }
    skip_to_semi();
  }

  // [stmt.if]
  Ex if_statement() {
    u32 begin = pos();
    bump();
    if (is(Tk::kw_consteval) || (is(Tk::exclaim) && kind(1) == Tk::kw_consteval)) return if_consteval(begin);
    if (is(Tk::kw_constexpr) || is(Tk::exclaim)) {
      s_->diag(pos(), Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    }
    s_->push_scope();  // [stmt.pre]: the condition's scope covers both substatements
    Ex cond = condition();
    Ex then_ = substatement();
    Ex else_;
    if (accept(Tk::kw_else)) else_ = substatement();
    s_->pop_scope();
    return s_->node(ast::If{{}, sema::Sema::at(cond, begin), sema::Sema::at(then_, begin), sema::Sema::at(else_, begin)},
                    begin, pos());
  }

  // `if !opt consteval compound-statement else statement` ([stmt.if]): the statement after `else`
  // must be a compound statement too ([stmt.if]/4). The branch taken when manifestly constant-evaluated is an immediate
  // function context ([expr.const]).
  Ex if_consteval(u32 begin) {
    bool negated = accept(Tk::exclaim);
    bump();  // consteval
    auto branch = [&](bool immediate) -> Ex {
      if (!is(Tk::l_brace)) {
        s_->diag(pos(), Diag::expected_token, Tk::l_brace);
        skip_statement();
        return {NodeId::none, pos(), pos()};
      }
      s_->immediate_context += immediate;
      Ex b = compound_statement();
      s_->immediate_context -= immediate;
      return b;
    };
    Ex then_ = branch(!negated);
    Ex else_;
    if (accept(Tk::kw_else)) else_ = branch(negated);
    return s_->node(ast::IfConsteval{negated, sema::Sema::at(then_, begin), sema::Sema::at(else_, begin)}, begin, pos());
  }

  // After an error inside parentheses: past the `)` and the `;`.
  void skip_rest() {
    skip_to_semi();
    if (accept(Tk::r_paren)) skip_to_semi();
  }

  // `static_assert ( constant-expression , string-literal opt ) ;` ([dcl.pre]): the condition,
  // contextually converted to bool, must be a constant expression whose value is true.
  Ex static_assert_declaration() {
    u32 begin = pos();
    bump();
    if (!expect(Tk::l_paren)) {
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    Ex e = constant_expression();
    Ex cond = s_->to_bool(e, e.begin);
    sema::ConstId message = sema::ConstId::none;
    if (accept(Tk::comma)) {
      // An unevaluated-string ([lex.string.uneval]): no encoding prefix and no suffix. A message
      // that is a constant expression needs classes (#102).
      if (!is(Tk::string_lit)) {
        s_->diag(pos(), Diag::expected_string_literal);
        skip_rest();
        return {NodeId::none, begin, pos()};
      }
      if (literal().encoding != lex::Encoding::ordinary || literal().ud_suffix != lex::no_name) {
        s_->diag(pos(), Diag::unevaluated_string);
        skip_rest();
        return {NodeId::none, begin, pos()};
      }
      Ex m = primary_expression();
      auto lit = s_->as<ast::Literal>(m);
      if (!lit) {
        skip_rest();
        return {NodeId::none, begin, pos()};
      }
      message = lit->value;
    }
    bool ok = expect(Tk::r_paren);
    if (!expect(Tk::semi)) skip_to_semi();
    if (!cond.ok() || !ok) return {NodeId::none, begin, pos()};
    sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(cond);
    if (v.dependent) {
      // Checked when the template is instantiated ([dcl.pre]/10).
    } else if (!v.value) {
      s_->diag(e.begin, Diag::not_constant, v.reason);
    } else if (s_->world().values.rec(*v.value).bits == 0) {
      if (message == sema::ConstId::none) s_->diag(begin, Diag::static_assert_failed);
      else s_->diag(begin, Diag::static_assert_failed_message, message);
    }
    return s_->node(ast::StaticAssert{sema::Sema::at(cond, begin), message}, begin, pos());
  }

  // A call to a consteval function outside an immediate function context is an immediate
  // invocation: it must be a constant expression, and it is replaced by its value ([expr.const]).
  Ex immediate(Ex call) {
    auto c = s_->as<ast::Call>(call);
    if (!c || c->function == ast::EntityId::none || !s_->is_consteval(c->function) || s_->immediate_context) return call;
    sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(call);
    if (!v.constant) {
      s_->diag(call.begin, Diag::not_constant, v.reason);
      return {NodeId::none, call.begin, call.end};
    }
    if (!v.value) {
      // A void function: the invocation is replaced by its (empty) result, `(void)0`, so the
      // consteval function's body never runs at run time.
      sema::QualType i = s_->builtin(sema::Builtin::int_);
      Ex zero = s_->literal(i, s_->world().values.integer(i, 0), ast::ValueCategory::prvalue, call.begin, call.end);
      return s_->convert_node(zero, ast::ConversionKind::to_void, s_->builtin(sema::Builtin::void_));
    }
    return s_->literal(call.type(), *v.value, ast::ValueCategory::prvalue, call.begin, call.end);
  }

  // `( condition )` ([stmt.pre]), converted to bool. A condition that declares a variable is
  // beyond the α subset.
  Ex condition() {
    if (!expect(Tk::l_paren)) return {};
    if (starts_declaration()) {
      s_->diag(pos(), Diag::not_supported, kind());
      skip_param();
      expect(Tk::r_paren);
      return {};
    }
    Ex e = expression();
    Ex c = s_->to_bool(e, e.begin);
    expect(Tk::r_paren);
    return c;
  }

  // [stmt.while]
  Ex while_statement() {
    u32 begin = pos();
    bump();
    s_->push_scope();
    Ex cond = condition();
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    s_->pop_scope();
    return s_->node(ast::While{sema::Sema::at(cond, begin), sema::Sema::at(body, begin)}, begin, pos());
  }

  // [stmt.do]
  Ex do_statement() {
    u32 begin = pos();
    bump();
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    expect(Tk::kw_while);
    Ex cond = condition();
    expect(Tk::semi);
    return s_->node(ast::Do{sema::Sema::at(body, begin), sema::Sema::at(cond, begin)}, begin, pos());
  }

  // [stmt.for]: `for ( init-statement condition_opt ; expression_opt ) statement`.
  Ex for_statement() {
    u32 begin = pos();
    bump();
    expect(Tk::l_paren);
    s_->push_scope();
    s_->guard_scope();  // [basic.scope.block]/2: the for-init's names are not redeclared in the body
    Ex init;
    if (starts_declaration()) {
      init = declaration_statement();
    } else {
      u32 b = pos();
      Ex e = is(Tk::semi) ? Ex{} : expression();
      if (is(Tk::colon)) {
        s_->diag(pos(), Diag::not_supported, Tk::colon);  // range-based for
        skip_to_semi();
        s_->pop_scope();
        return {NodeId::none, begin, pos()};
      }
      expect(Tk::semi);
      init = s_->node(ast::ExprStmt{sema::Sema::at(e, b)}, b, pos());
    }
    Ex cond;
    if (!is(Tk::semi)) {
      Ex e = expression();
      cond = s_->to_bool(e, e.begin);
    }
    expect(Tk::semi);
    Ex inc;
    if (!is(Tk::r_paren)) inc = expression();
    expect(Tk::r_paren);
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    s_->pop_scope();
    return s_->node(ast::For{sema::Sema::at(init, begin), sema::Sema::at(cond, begin), sema::Sema::at(inc, begin),
                             sema::Sema::at(body, begin)},
                    begin, pos());
  }

  // [stmt.return]: the operand copy-initializes the result; a void function may return a void
  // expression.
  Ex return_statement() {
    u32 begin = pos();
    bump();
    QualType rt = s_->return_type();
    Ex value;
    if (!is(Tk::semi)) {
      if (is(Tk::l_brace)) {
        // `return {…};` copy-list-initializes the result ([stmt.return]/2).
        auto b = braced_init_list();
        if (s_->types().is_void(rt)) s_->diag(begin, Diag::return_value_in_void);
        else value = s_->list_init(rt, *b);
        if (!expect(Tk::semi)) skip_to_semi();
        return s_->node(ast::Return{sema::Sema::at(value, begin)}, begin, pos());
      }
      Ex e = expression();
      if (s_->types().is_void(rt)) {
        if (e.ok() && !s_->types().is_void(e.type()) && !s_->dependent(e)) s_->diag(begin, Diag::return_value_in_void);
        value = e;
      } else {
        value = s_->convert(e, rt, e.begin);
      }
    } else if (!s_->types().is_void(rt) && !s_->types().is_dependent(rt)) {
      s_->diag(begin, Diag::return_missing_value);
    }
    if (!expect(Tk::semi)) skip_to_semi();
    return s_->node(ast::Return{sema::Sema::at(value, begin)}, begin, pos());
  }

  // A block-scope simple-declaration ([stmt.dcl], [dcl.pre]): each declarator is a local
  // variable, in scope from the end of its declarator ([basic.scope.pdecl]).
  Ex declaration_statement() {
    u32 begin = pos();
    DeclSpec spec = decl_specifiers();
    if (spec.is_static || spec.is_extern || spec.is_inline) {
      s_->diag(begin, Diag::not_supported,
               spec.is_static ? Tk::kw_static : spec.is_extern ? Tk::kw_extern : Tk::kw_inline);
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    if (spec.is_constinit) {
      s_->diag(begin, Diag::constinit_automatic);  // [dcl.constinit]/1
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    if (spec.is_consteval) {
      s_->diag(begin, Diag::consteval_not_function);
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    if (spec.is_typedef) {
      // Block-scope typedef-names ([dcl.typedef]).
      do {
        Declarator d = declarator(spec.type, false);
        if (!d.ok || !spec.ok || !d.named || d.unknown_bound) {
          if (d.ok && d.unknown_bound) s_->diag(d.name_at, Diag::not_supported, Tk::l_square);
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        s_->local_type_alias(d.name, d.type, d.name_at);
      } while (accept(Tk::comma));
      expect(Tk::semi);
      return s_->node(ast::ExprStmt{}, begin, pos());
    }
    std::vector<Ex> vars;
    do {
      u32 dbegin = pos();
      Declarator d = declarator(spec.type, false);
      if (!d.ok || !spec.ok) {
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (s_->types().is_function(d.type)) {
        // [dcl.fct.def.general]/2: a function is defined only at namespace or class scope; a
        // block-scope declaration of one is not supported yet.
        if (is(Tk::l_brace)) s_->diag(d.name_at, Diag::function_definition_in_block);
        else s_->diag(d.name_at, Diag::not_supported, Tk::l_paren);
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (s_->types().is_void(d.type)) {
        s_->diag(d.name_at, Diag::void_variable, d.name);
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      // [dcl.constexpr]: a constexpr variable is const.
      if (spec.is_constexpr) d.type = s_->types().add_cv(d.type, sema::Cv::const_);
      // `char s[] = "…"`: the bound from the literal, null included ([dcl.init.string]/1).
      if (d.unknown_bound && is(Tk::equal) && kind(1) == Tk::string_lit) {
        d.type = s_->types().array(d.type, literal_at(1).units.size() + 1);
        d.unknown_bound = false;
      }
      u32 local = s_->declare_local(d.name, d.type, 0, d.name_at);
      Ex init;
      bool list = is(Tk::l_brace) || (is(Tk::equal) && kind(1) == Tk::l_brace);
      if (d.unknown_bound && !list) {
        s_->diag(d.name_at, Diag::not_supported, Tk::l_square);  // an array of unknown bound
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (list) {
        // List-initialization ([dcl.init.list]): `T x{…}` and `T x = {…}`.
        accept(Tk::equal);
        auto b = braced_init_list();
        if (d.unknown_bound) {
          u64 n = s_->deduce_bound(d.type, *b);
          if (n == 0) {  // [dcl.init.aggr]/12: an empty list does not give an array a bound
            s_->diag(b->begin, Diag::bound_not_constant);
            return {NodeId::none, begin, pos()};
          }
          d.type = s_->types().array(d.type, n);
          s_->set_local_type(local, d.type);
        }
        // A reference from one element binds as `= e` does, extending a temporary ([class.temporary]/6).
        if (s_->types().is_reference(d.type) && b->clauses.size() == 1 && !b->clauses[0].list && !b->clauses[0].designated)
          init = s_->bind_reference(b->clauses[0].expr, d.type, b->clauses[0].begin, true);
        else
          init = s_->list_init(d.type, *b);
        // As for `= e` below: a constexpr local, or a const integral one with a constant
        // initializer, is usable in constant expressions ([expr.const]).
        bool usable_kind = sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                           s_->types().is_integral(d.type);
        if (init.ok() && (spec.is_constexpr || usable_kind)) {
          sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(init);
          if (v.value) s_->set_local_value(local, *v.value);
          else if (spec.is_constexpr && !v.dependent) s_->diag(b->begin, Diag::not_constant, v.reason);
        }
      } else if (accept(Tk::equal)) {
        Ex e = assignment_expression();
        if (s_->types().is_array(d.type)) {
          init = s_->convert(e, d.type, e.begin);  // from a string literal ([dcl.init.string])
        } else if (s_->types().is_reference(d.type)) {
          // A temporary bound to the reference lives as long as it ([class.temporary]/6).
          init = s_->bind_reference(e, d.type, e.begin, true);
        } else {
          init = s_->convert(e, d.type, e.begin);
          // A constexpr variable, or a const integral one with a constant initializer, is usable
          // in constant expressions ([expr.const]); a constexpr one must have one ([dcl.constexpr]).
          bool usable_kind = sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                             s_->types().is_integral(d.type);
          if (init.ok() && (spec.is_constexpr || usable_kind)) {
            sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(init);
            if (v.value) s_->set_local_value(local, *v.value);
            else if (spec.is_constexpr && !v.dependent) s_->diag(e.begin, Diag::not_constant, v.reason);
          }
        }
      } else if (is(Tk::l_paren)) {
        // Direct-initialization ([dcl.init.general]): `T x(args)`.
        u32 open = pos();
        bump();
        std::vector<Ex> args;
        if (!is(Tk::r_paren))
          do args.push_back(assignment_expression());
          while (accept(Tk::comma));
        if (!expect(Tk::r_paren)) {
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        if (s_->types().is_dependent(d.type) || s_->any_dependent(args)) {
          init = s_->construct(d.type, std::move(args), open, pos());
        } else if (s_->types().is_class(d.type)) {
          init = s_->has_user_ctors(d.type) || args.size() != 1 ? s_->construct(d.type, std::move(args), open, pos())
                                                                 : s_->convert(args[0], d.type, open);
        } else if (args.size() != 1 || s_->types().is_array(d.type)) {
          s_->diag(open, Diag::not_supported, Tk::l_paren);  // a parenthesized list for a scalar or array
        } else if (s_->types().is_reference(d.type)) {
          init = s_->bind_reference(args[0], d.type, open, true);
        } else {
          init = s_->convert(args[0], d.type, open);
        }
      } else if (s_->types().is_reference(d.type)) {
        s_->diag(d.name_at, Diag::reference_uninitialized);  // [dcl.ref]/5
      } else if (s_->types().is_dependent(d.type)) {
        init = s_->default_init(d.type, d.name_at, pos());  // checked when the template is instantiated
      } else if (!s_->require_complete(d.type, d.name_at)) {
        // Default-initialization needs a complete object type ([dcl.init.general]).
      } else if (s_->has_member_inits(d.type)) {
        init = s_->default_init(d.type, d.name_at, pos());
      } else if (spec.is_constexpr) {
        s_->diag(d.name_at, Diag::constexpr_uninitialized, d.name);
      } else if (sema::Types::is_const(d.type)) {
        s_->diag(d.name_at, Diag::const_uninitialized, d.name);
      }
      // The destructor that ends the variable at scope exit ([stmt.dcl]); a reference has none.
      ast::EntityId dtor = s_->types().is_reference(d.type) ? ast::EntityId::none : s_->destructor_of(d.type);
      Ex var = s_->node(ast::LocalVar{local, sema::Sema::at(init, dbegin), dtor}, dbegin, pos());
      s_->set_local_decl(local, var.id);
      vars.push_back(var);
    } while (accept(Tk::comma));
    if (!expect(Tk::semi)) skip_to_semi();
    std::vector<ast::Child> cs;
    for (const Ex& v : vars) cs.push_back(sema::Sema::at(v, begin));
    return s_->node(ast::DeclStmt{s_->builder().list(cs)}, begin, pos());
  }

  // The questions constant evaluation asks (array bounds, constexpr, static_assert); set by the
  // owner.
  void set_questions(sema::Questions& q) {
    q_ = &q;
    s_->constant_of = [this](Ex e) { return sema::Evaluator(*s_, questions()).evaluate(e).value; };
  }
  sema::Questions& questions() { return *q_; }

private:
  const pp::Region* r_;
  u32 begin_;
  u32 end_;
  u32 i_;
  sema::Sema* s_;
  sema::Questions* q_ = nullptr;
  LangLinkage linkage_ = LangLinkage::cxx;
  int in_params_ = 0;
  int angle_ = 0;               // inside a template argument or parameter list, outside parentheses
  bool half_ = false;           // the current `>>` has had its first `>` taken by a template-id
public:
  bool body = false;  // the part is a function body, not a declaration part
private:
  const Level* top_ = nullptr;  // the declarator being built
};

}  // namespace ycxx::parse
