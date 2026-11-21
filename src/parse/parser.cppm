// The parser: recursive descent over one part of one item (docs/design/ast.md §3.2, §3.4), with
// a function per grammar production of [gram], calling sema's actions as it goes. It reads the
// region's tokens in [begin, end) and never past them, so a part is analysed alone, as the
// per-item queries require.
//
// The α subset (#39) is parsed; what lies beyond it is diagnosed as not supported where the
// grammar meets it, so M2 widens these functions rather than replacing them.
export module ycxx.parse:parser;

import std;
import ycxx.stack;
import ycxx.ast;
import ycxx.engine;
import ycxx.items;
import ycxx.pp;
import ycxx.sema;

export namespace ycxx::parse {

// Thrown when the input nests deeper than half of the thread's stack allows (#187), after the
// diagnostic; caught where an item's or a body's analysis begins, which then fails.
struct TooDeep {};

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
  std::optional<u32> virtual_at;
  std::optional<u32> mutable_at;
  // An elaborated-type-specifier ([dcl.type.elab]): the class it names, and where its name is.
  bool elaborated = false;
  Tk class_key = Tk::eof;
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
  // Its default argument ([dcl.fct.default]): the initializer-clause converted to the parameter's
  // type, analysed here; none if it has none.
  NodeId default_arg = NodeId::none;
  std::optional<std::pair<u32, u32>> default_tokens{};
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
  u32 ref_at = 0;
  bool has_noexcept = false;
  std::optional<Ex> noexcept_condition;
  // `T x[] = {…}`: the outermost array bound is left to the initializer ([dcl.array]/8); `type`
  // is then the element type.
  bool unknown_bound = false;
  bool function = false;  // it has a parameter list of its own, not a function type from a name
  // `f<args>`: a declarator-id that is a template-id, which an explicit specialization declares
  // ([temp.expl.spec]/1); `f<>` has none.
  std::optional<std::vector<ast::TemplateArg>> template_args;
  u32 template_args_at = 0;
  std::optional<u32> replay_template_args_at;  // written arguments with deferred substitution checks
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
  u32 deferred_argument_checks() const { return deferred_argument_checks_; }
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
  bool literal_operator_ahead(u32 ahead = 0) const {
    return kind(ahead) == Tk::kw_operator && kind(ahead + 1) == Tk::string_lit;
  }
  bool lookup_id_ahead(u32 ahead = 0) const { return kind(ahead) == Tk::identifier || literal_operator_ahead(ahead); }
  std::optional<NameId> lookup_id() {
    if (is(Tk::identifier)) {
      NameId n = name_of(tok());
      bump();
      return n;
    }
    if (auto id = items::literal_operator_id(*r_, i_, end_)) {
      if (literal_at(1).ud_suffix == lex::no_name) s_->diag(pos(), Diag::deprecated_literal_operator_id);
      seek(id->end);
      return s_->ins().names.intern(lex::literal_operator_name(id->suffix));
    }
    s_->diag(pos(), Diag::invalid_literal_operator_id);
    bump();
    return std::nullopt;
  }
  // Moves to the region's token `i`, inside the part (past a class's member-specification).
  void seek(u32 i) { i_ = std::min(i, end_); }
  u32 index() const { return i_; }  // the current token's index in the region, for `seek`
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

  static bool is_class_key(Tk k) { return k == Tk::kw_class || k == Tk::kw_struct || k == Tk::kw_union; }

  static bool starts_decl_specifier(Tk k) {
    switch (k) {
    case Tk::kw_void: case Tk::kw_bool: case Tk::kw_char: case Tk::kw_short: case Tk::kw_int: case Tk::kw_long:
    case Tk::kw_signed: case Tk::kw_unsigned: case Tk::kw_const: case Tk::kw_volatile: case Tk::kw_static:
    case Tk::kw_extern: case Tk::kw_inline: case Tk::kw_constexpr: case Tk::kw_char8_t: case Tk::kw_char16_t:
    case Tk::kw_char32_t: case Tk::kw_wchar_t: case Tk::kw_float: case Tk::kw_double: case Tk::kw_auto:
    case Tk::kw_typedef: case Tk::kw_struct: case Tk::kw_class: case Tk::kw_union: case Tk::kw_enum:
    case Tk::kw_thread_local: case Tk::kw_constinit: case Tk::kw_consteval: case Tk::kw_register:
    case Tk::kw_mutable: case Tk::kw_typename: case Tk::kw_decltype: case Tk::kw_virtual:
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
  // ([stmt.ambig]: what can be a declaration is one). In a `statement`, a member of a dependent
  // class names a type only after `typename` ([temp.res.general]/4-5); a namespace-scope or member
  // declaration may imply it.
  bool starts_declaration(bool statement = false) {
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
      auto context = statement ? PointerLookahead::statement : PointerLookahead::declaration;
      return q && !(statement && s_->types().kind(q->first) == sema::TypeKind::dep_name) &&
             (kind(q->second) != Tk::l_paren || member_pointer_star(q->second + 1) ||
              function_pointer_declarator_ahead(q->second, context)) &&
             kind(q->second) != Tk::period && kind(q->second) != Tk::l_brace;
    }
    u32 next = 1;  // the token after the type's name: after `X<…>` for a class template
    if (is(Tk::identifier) && kind(1) == Tk::less && s_->names_type_template(name_of(tok()))) next = past_template_args(1);
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
    if (kind(next + 1) == Tk::star || kind(next + 1) == Tk::amp || kind(next + 1) == Tk::ampamp ||
        member_pointer_star(next + 1))
      return after != Tk::period && after != Tk::arrow;
    return after != Tk::period && after != Tk::arrow && after != Tk::l_paren && after != Tk::l_square;
  }

  // Return the token after a parenthesized group, or the end of the token range.
  u32 past_parentheses(u32 open) const {
    u32 depth = 1;
    for (u32 j = open + 1; i_ + j < end_; ++j) {
      if (kind(j) == Tk::l_paren) ++depth;
      if (kind(j) == Tk::r_paren && --depth == 0) return j + 1;
    }
    return end_ - i_;
  }

  enum class PointerLookahead { declaration, statement, abstract };

  // A qualified type followed by `(*name)(parameters)` can start a declaration
  // ([stmt.ambig]). Keep a functional conversion followed by a call and an operator
  // on the expression path.
  bool function_pointer_declarator_ahead(u32 open, PointerLookahead context = PointerLookahead::declaration) {
    if (kind(open) != Tk::l_paren) return false;
    bool statement = context == PointerLookahead::statement;
    u32 next = open + 1;
    u32 groups = 1;
    bool pointer = false;
    bool member_pointer = false;
    for (;;) {
      if (kind(next) == Tk::l_paren) {
        ++groups;
        ++next;
        continue;
      }
      if (auto star = member_pointer_star(next)) {
        next = *star;
        member_pointer = true;
      }
      if (kind(next) != Tk::star) break;
      pointer = true;
      next = past_attributes(next + 1);
      while (kind(next) == Tk::kw_const || kind(next) == Tk::kw_volatile ||
             kind(next) == Tk::kw___restrict || kind(next) == Tk::kw___restrict__)
        ++next;
    }
    if (!pointer) return false;
    if (context != PointerLookahead::abstract) {
      if (qualified_ahead(next)) {
        u32 name = next;
        // A template argument in the scope can itself read a qualified type name.
        bool saved_half = qualified_half_;
        auto scope = nested_name(next, name);
        qualified_half_ = saved_half;
        if (!scope) return false;
        next = name;
      }
      if (kind(next) != Tk::identifier) return false;
      next = past_attributes(next + 1);
    }
    for (; groups != 0; --groups, ++next)
      if (kind(next) != Tk::r_paren) return false;

    if (kind(next) != Tk::l_paren || (statement && !params_ahead(next, /*statement=*/true))) return false;
    u32 after = past_parentheses(next);
    if (member_pointer && (kind(after) == Tk::amp || kind(after) == Tk::ampamp)) ++after;
    switch (kind(after)) {
    case Tk::semi: case Tk::comma: case Tk::equal: case Tk::l_brace:
    case Tk::kw_noexcept: case Tk::kw_const: case Tk::kw_volatile:
      return true;
    case Tk::r_paren:
      return !statement;  // the end of an enclosing parameter list
    default:
      return false;
    }
  }

  // Whether a qualified name starts here: `::`, `N::`, or `X<…>::` ([basic.lookup.qual]).
  bool qualified_ahead(u32 start = 0) {
    if (kind(start) == Tk::coloncolon) return true;
    if (kind(start) != Tk::identifier) return false;
    if (kind(start + 1) == Tk::coloncolon) return true;
    return kind(start + 1) == Tk::less && kind(past_template_args(start + 1)) == Tk::coloncolon;
  }

  // A ptr-operator can start with a nested-name-specifier. Find its `*` before doing lookup, so
  // an invalid owner still consumes the operator rather than looking like a declarator-id.
  std::optional<u32> member_pointer_star(u32 j) {
    u32 start = j;
    if (kind(j) == Tk::kw_decltype && kind(j + 1) == Tk::l_paren) {
      u32 depth = 1;
      j += 2;
      for (; depth > 0 && i_ + j < end_; ++j) {
        if (kind(j) == Tk::l_paren) ++depth;
        else if (kind(j) == Tk::r_paren) --depth;
      }
      if (kind(j) != Tk::coloncolon) return std::nullopt;
      ++j;
    } else if (kind(j) == Tk::coloncolon) {
      ++j;
    }
    while (kind(j) == Tk::identifier || kind(j) == Tk::kw_template) {
      if (kind(j) == Tk::kw_template) ++j;
      if (kind(j) != Tk::identifier) break;
      u32 after = kind(j + 1) == Tk::less ? past_template_args(j + 1) : j + 1;
      if (kind(after) != Tk::coloncolon) break;
      j = after + 1;
    }
    if (j != start && kind(j) == Tk::star) return j;
    return std::nullopt;
  }

  // `decltype(e)` [dcl.type.decltype], with `decltype` and `(` at the cursor.
  std::optional<QualType> decltype_expression_type() {
    bump();
    bump();
    int saved = std::exchange(angle_, 0);
    ++s_->unevaluated;
    Ex e = expression();
    --s_->unevaluated;
    angle_ = saved;
    bool closed = expect(Tk::r_paren);
    if (!e.ok() || !closed) return std::nullopt;
    if (s_->dependent(e)) ++deferred_argument_checks_;
    return s_->decltype_of(e, s_->as<ast::Paren>(e).has_value());
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
    NameId name = name_of(tok());
    const ast::TemplateParamSema* tt = s_->template_template_param(name);
    auto tmpl = tt ? std::nullopt : s_->type_template(name);
    bump();
    auto args = template_argument_list();
    if (!args) return std::nullopt;
    if (tt) return s_->template_param_id(*tt, *args, at);  // `TT<args>` ([temp.param])
    if (!tmpl) return s_->builtin_template_id(name, *args, at);  // `__make_integer_seq<…>`
    return type_template_id(*tmpl, *args, at);
  }

  std::optional<QualType> type_template_id(const ast::DeclSema& tmpl, std::span<const ast::TemplateArg> args, u32 at) {
    // An alias can erase a dependent argument from the resulting type, but the written argument
    // still has to be valid when substituted ([temp.alias]).
    if (tmpl.kind == ast::DeclKind::type_alias && std::ranges::any_of(args, [&](const auto& a) { return s_->dependent(a); }))
      ++deferred_argument_checks_;
    return s_->class_template_id(tmpl, args, at);
  }

  // What an elaborated-type-specifier naming no declared class does: at namespace scope it
  // declares the class ([basic.scope.pdecl]); set by the owner where that applies.
  std::function<std::optional<QualType>(NameId, Tk, u32)> declare_class;

  // decl-specifier-seq ([dcl.spec.general]) with the simple-type-specifiers of [dcl.type.simple],
  // class names and elaborated-type-specifiers ([dcl.type.elab]), and the cv-qualifiers.
  DeclSpec decl_specifiers(bool in_type_id = false, std::optional<u32>* friend_at = nullptr,
                           bool allow_conversion_id = false, bool class_member = false) {
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
      case Tk::kw_mutable:
        if (!class_member) {
          s_->diag(pos(), Diag::mutable_placement);
          d.ok = false;
        } else if (d.mutable_at) {
          s_->diag(pos(), Diag::duplicate_specifier, k);
          d.ok = false;
        } else {
          d.mutable_at = pos();
        }
        break;
      case Tk::kw_virtual:
        if (!class_member) {
          s_->diag(pos(), Diag::virtual_placement);
          d.ok = false;
        } else if (d.virtual_at) {
          s_->diag(pos(), Diag::duplicate_specifier, k);
          d.ok = false;
        } else {
          d.virtual_at = pos();
        }
        break;
      case Tk::kw_friend:
        if (!friend_at) goto done;
        if (*friend_at) s_->diag(pos(), Diag::duplicate_specifier, k);
        else *friend_at = pos();
        break;
      case Tk::kw_auto:
        // A placeholder type ([dcl.spec.auto]), deduced from an initializer or a return statement.
        if (any_type()) {
          s_->diag(pos(), Diag::conflicting_specifiers, k);
          d.ok = false;
          break;
        }
        cls = s_->types().placeholder(false);
        break;
      case Tk::kw_decltype: {
        if (any_type() && member_pointer_star(0)) goto done;
        // `decltype(auto)` ([dcl.spec.auto]) and `decltype(e)` ([dcl.type.decltype]).
        if (!any_type() && kind(1) == Tk::l_paren && kind(2) == Tk::kw_auto && kind(3) == Tk::r_paren) {
          cls = s_->types().placeholder(true);
          bump();
          bump();
          bump();
          break;
        }
        if (!any_type() && kind(1) == Tk::l_paren && kind(2) == Tk::kw_auto) {  // `decltype(auto` without `)`
          s_->diag(pos() + 3, Diag::expected_token, Tk::r_paren);
          d.ok = false;
          goto done;
        }
        if (any_type() || kind(1) != Tk::l_paren) {
          s_->diag(pos(), Diag::not_supported, k);
          d.ok = false;
          goto done;
        }
        cls = decltype_expression_type();
        if (!cls) {
          d.ok = false;
          goto done;
        }
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
        auto q = qualified_ahead() ? qualified_type(0, true) : std::nullopt;
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
        if (qualified_ahead(1)) {
          bump();
          auto q = qualified_type(0, /*checked=*/true, /*type_only=*/true);
          if (!q || !s_->types().is_class(q->first) || s_->is_union_type(q->first) != (k == Tk::kw_union) ||
              any_type()) {
            s_->diag(at, Diag::expected_type_specifier);
            d.ok = false;
            goto done;
          }
          cls = q->first;
          take_qualified(q->second);
          continue;
        }
        // A class-specifier where a type is named (a local or unnamed class) is not supported yet.
        if (kind(1) != Tk::identifier || kind(2) == Tk::l_brace || kind(2) == Tk::colon || kind(2) == Tk::coloncolon) {
          s_->diag(at, Diag::not_supported, k);
          d.ok = false;
          goto done;
        }
        bump();
        NameId n = name_of(tok());
        u32 name_at = pos();
        bump();
        auto t = s_->class_name(n, true, false);
        if (!t && s_->class_name(n, true)) {  // [dcl.type.elab]/2
          s_->diag(at + 1, Diag::elaborated_typedef, n);
          d.ok = false;
          continue;
        }
        if (!t && declare_class) t = declare_class(n, k, name_at);
        if (!t) {
          s_->diag(at + 1, Diag::not_a_type, n);
          d.ok = false;
        } else if (any_type()) {
          s_->diag(at, Diag::conflicting_specifiers, k);
          d.ok = false;
        } else {
          cls = t;
          d.elaborated = true;
          d.class_key = k;
          d.class_name = n;
          d.class_at = name_at;
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
            if (auto q = qualified_type(0, true);
                q && (in_type_id || kind(q->second) != Tk::l_paren ||
                      function_pointer_declarator_ahead(q->second) ||
                      (in_params_ > 0 && function_pointer_declarator_ahead(q->second, PointerLookahead::abstract)))) {
              cls = q->first;
              take_qualified(q->second);
              continue;
            }
            goto done;
          }
          if (kind(1) == Tk::less && s_->names_type_template(name_of(tok()))) {
            auto t = class_template_type();
            if (!t) {
              d.ok = false;
              goto done;
            }
            cls = t;
            continue;  // the arguments are consumed
          }
          if (kind(1) != Tk::coloncolon)
            if (auto t = s_->class_name(name_of(tok()), false, true, pos())) {
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
    if (d.is_typedef && (d.is_static || d.is_extern || d.is_inline || d.is_constexpr || d.is_consteval || d.is_constinit ||
                         d.virtual_at)) {
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
      if (allow_conversion_id && !c && !v && conversion_id_ahead()) return d;
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
    ast::RefQual ref = ast::RefQual::none;
    u32 ref_at = 0;
    bool has_noexcept = false;
    std::optional<Ex> noexcept_condition;
    u64 bound = 0;
    bool unknown_bound = false;  // an array suffix with an omitted bound ([dcl.array])
    // A value-dependent bound ([temp.dep.constexpr]): a template parameter's `VDepParam`.
    std::optional<sema::ConstId> dependent_bound;
    std::optional<QualType> trailing;  // `-> T`: the trailing return type ([dcl.fct]/2)
  };
  struct Level {
    // ptr-operators, outermost first ([dcl.ptr], [dcl.mptr], [dcl.ref]).
    struct PtrOp {
      Tk op;
      sema::Cv cv;
      u32 at;
      std::optional<QualType> owner;
      std::optional<u32> restrict_at;
    };
    std::vector<PtrOp> pointers;
    std::unique_ptr<Level> inner;
    std::optional<QualType> conversion_target;
    NameId name{};
    bool named = false;
    u32 name_at = 0;
    bool qualified = false;
    NameId qualifier{};
    u32 qualifier_at = 0;
    sema::Sema::Scope scope{};
    std::optional<std::vector<ast::TemplateArg>> template_args;
    u32 template_args_at = 0;
    std::optional<u32> replay_template_args_at;
    bool pack = false;
    std::vector<Suffix> suffixes;
  };

  enum class NamedExceptionSpec { ordinary, defaulted_assignment };

  // declarator, or abstract-declarator when `abstract` ([dcl.decl.general], [dcl.name]).
  Declarator declarator(QualType base, bool abstract, bool typedef_ = false, bool conversion = false,
                        NamedExceptionSpec named_exception = NamedExceptionSpec::ordinary, bool defer_ref_check = false,
                        bool reject_parameter_defaults = false) {
    Declarator d;
    d.begin = pos();
    const std::size_t first_local = s_->locals().size();
    const bool in_body = s_->current_function != ast::EntityId::none;
    Level top = level(abstract, d.ok, conversion);
    if (conversion && !top.conversion_target) {
      if (in_body) s_->discard_local_declarations(first_local);
      return d;
    }
    const Suffix* fn = nullptr;
    d.type = build(top, top.conversion_target.value_or(base), fn, d, abstract || typedef_, named_exception);
    const bool spelled_unknown_bound = std::exchange(d.unknown_bound, false);
    if (d.ok && !abstract && spelled_unknown_bound && s_->types().is_array(d.type)) {
      const sema::TypeRec array = s_->types().rec(d.type);
      if (array.bound_value == sema::ConstId::none) {
        // An outer object-array bound can be supplied by its initializer ([dcl.array]).
        d.unknown_bound = true;
        d.type = array.inner;
      }
    }
    const bool function_type = s_->types().is_function(d.type);
    if (member_template_name || reject_parameter_defaults) {
      for (const Level* level = &top; level; level = level->inner.get())
        for (const Suffix& suffix : level->suffixes) {
          if (&suffix == fn && !abstract && !typedef_ && function_type) continue;
          for (const Param& param : suffix.params)
            if (param.default_tokens || (reject_parameter_defaults && param.default_arg != NodeId::none)) {
              s_->diag(param.at, Diag::default_arg_not_allowed);
              d.ok = false;
            }
        }
    }
    if (fn) {
      d.function = function_type;
      // The declaring context also checks defaults in a nested function type.
      d.params = fn->params;
      d.variadic = fn->variadic;
      d.ref_at = fn->ref_at;
      d.has_noexcept = fn->has_noexcept;
      d.noexcept_condition = fn->noexcept_condition;
    } else {
      d.ref_at = d.name_at;
    }
    if (!abstract && !typedef_ && function_type && !conversion && !defer_ref_check)
      d.ok = check_named_ref_qualifier(d) && d.ok;
    if (d.ok) {
      skip_attributes();
      if (!abstract) {
        d.asm_label = asm_label(d.ok);
        skip_attributes();
      }
    }
    // Prototype names support type and constant-expression checking inside this
    // declarator. They have no storage in the enclosing function's body.
    if (in_body) {
      s_->discard_local_declarations(first_local);
      d.noexcept_condition.reset();
    }
    return d;
  }

  bool check_named_ref_qualifier(const Declarator& d) {
    const ast::RefQual ref = s_->types().rec(d.type).fn_ref;
    if (ref == ast::RefQual::none) return true;
    s_->diag(d.ref_at, Diag::not_supported, ref == ast::RefQual::lvalue ? Tk::amp : Tk::ampamp);
    return false;
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
  // [dcl.attr.grammar]: each attribute argument must have balanced tokens.
  bool skip_balanced_attributes() {
    const u32 count = past_attributes(0);
    std::vector<Tk> closing;
    for (u32 offset = 0; offset < count; ++offset) {
      switch (Tk token = kind(offset)) {
      case Tk::l_square: closing.push_back(Tk::r_square); break;
      case Tk::l_paren: closing.push_back(Tk::r_paren); break;
      case Tk::l_brace: closing.push_back(Tk::r_brace); break;
      case Tk::l_splice: closing.push_back(Tk::r_splice); break;
      case Tk::r_square:
      case Tk::r_paren:
      case Tk::r_brace:
      case Tk::r_splice:
        if (closing.empty()) {
          s_->diag(pos() + offset, Diag::unexpected_token, token);
          return false;
        }
        if (token != closing.back()) {
          s_->diag(pos() + offset, Diag::expected_token, closing.back());
          return false;
        }
        closing.pop_back();
        break;
      default: break;
      }
    }
    if (!closing.empty()) {
      s_->diag(pos() + count - 1, Diag::expected_token, closing.back());
      return false;
    }
    seek(index() + count);
    return true;
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
  std::string_view paired_operator_suffix(u32 ahead = 0) const {
    if (kind(ahead) == Tk::l_paren && kind(ahead + 1) == Tk::r_paren) return "()";
    if (kind(ahead) == Tk::l_square && kind(ahead + 1) == Tk::r_square) return "[]";
    return {};
  }
  std::string_view punctuator_operator_suffix(u32 ahead) const {
    if (auto pair = paired_operator_suffix(ahead); !pair.empty()) return pair;
    Tk k = kind(ahead);
    return lex::is_punctuator(k) && overloadable(k) ? lex::spelling(k) : std::string_view{};
  }
  // A name in an id-expression, including the punctuator part of an ordinary operator-function-id.
  std::optional<NameId> expression_id(std::string_view operator_suffix) {
    if (operator_suffix.empty()) return lookup_id();
    bump();  // operator
    bump();
    if (operator_suffix == "()" || operator_suffix == "[]") bump();
    std::string name = "operator";
    name += operator_suffix;
    return s_->ins().names.intern(name);
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

  // Shared by a declarator and the ptr-operators of an unparenthesized new-type-id.
  void pointer_operators(Level& l, bool& ok) {
    for (;;) {
      u32 at = pos();
      Tk op = kind();
      std::optional<QualType> owner;
      if (auto star = member_pointer_star(0)) {
        u32 next = 0;
        auto scope = nested_name(0, next, /*member_pointer=*/true, substituting_partial_arguments);
        if (scope && next == *star && scope->type && s_->types().valid_member_owner(*scope->type)) {
          owner = scope->type;
        } else {
          s_->diag(at, Diag::member_pointer_owner);
          ok = false;
        }
        for (u32 j = 0; j < *star; ++j) bump();
        op = Tk::star;
      } else if (op != Tk::star && op != Tk::amp && op != Tk::ampamp) {
        break;
      }
      bump();
      if (op == Tk::star) skip_attributes();  // appertains to the pointer ([dcl.ptr], [dcl.mptr])
      int cv = 0;
      std::optional<u32> restrict_at;
      while (op == Tk::star) {
        if (is(Tk::kw_const) || is(Tk::kw_volatile)) {
          int bit = is(Tk::kw_const) ? 1 : 2;
          if (cv & bit) {
            s_->diag(pos(), Diag::duplicate_specifier, kind());
            ok = false;
          }
          cv |= bit;
        } else if (is(Tk::kw___restrict) || is(Tk::kw___restrict__)) {
          if (restrict_at) {
            s_->diag(pos(), Diag::duplicate_specifier, kind());
            ok = false;
          } else {
            restrict_at = pos();
          }
        } else break;
        bump();
      }
      l.pointers.push_back({op, sema::Cv(cv), at, owner, restrict_at});
    }
  }

  bool allocation_operator_ahead(u32 ahead = 0) const {
    return kind(ahead) == Tk::kw_operator && (kind(ahead + 1) == Tk::kw_new || kind(ahead + 1) == Tk::kw_delete);
  }

  bool conversion_id_ahead() const {
    Tk next = kind(1);
    return is(Tk::kw_operator) && (starts_decl_specifier(next) || next == Tk::identifier || next == Tk::coloncolon);
  }

  // [class.conv.fct]: a type-specifier-seq followed by its ptr-operators. The
  // conversion function's own parameter list is outside the conversion-type-id.
  std::optional<QualType> conversion_type_id() {
    DeclSpec spec = decl_specifiers(/*in_type_id=*/true);
    if (!spec.ok) return std::nullopt;
    if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_constexpr || spec.is_consteval ||
        spec.is_constinit || spec.is_typedef) {
      s_->diag(spec.begin, Diag::expected_type_specifier);
      return std::nullopt;
    }
    Level target;
    Declarator d;
    d.begin = spec.begin;
    pointer_operators(target, d.ok);
    const Suffix* fn = nullptr;
    QualType type = build(target, spec.type, fn, d, true);
    if (!d.ok) return std::nullopt;
    if (s_->types().has_placeholder(type)) {
      s_->diag(spec.begin, Diag::not_supported, Tk::kw_auto);
      return std::nullopt;
    }
    return type;
  }

  static sema::AllocationFunction allocation_operator(Tk k) {
    return k == Tk::kw_new ? sema::AllocationFunction::allocate : sema::AllocationFunction::deallocate;
  }

  Level level(bool abstract, bool& ok, bool conversion = false) {
    Level l;
    pointer_operators(l, ok);
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
    if (conversion) {
      l.name_at = pos();
      bump();  // operator
      l.conversion_target = conversion_type_id();
      ok = l.conversion_target.has_value() && ok;
      if (!ok) return l;
      l.name = s_->ins().names.intern(lex::conversion_function_name);
      l.named = true;
      skip_attributes();
    } else if (!abstract && literal_operator_ahead()) {
      l.name_at = pos();
      auto n = lookup_id();
      if (!n) {
        ok = false;
        return l;
      }
      l.name = *n;
      l.named = true;
      skip_attributes();
    } else if (!abstract && is(Tk::kw_operator)) {
      // An operator-function-id ([over.oper.general]): `operator@`, named as the splitter spells
      // it. Only the ordinary global single-object forms of the allocation operators are supported.
      l.name_at = pos();
      bump();
      std::string spelled = "operator";
      if (auto pair = paired_operator_suffix(); !pair.empty()) {
        spelled += pair;
        bump();
        bump();
      } else if (is(Tk::kw_new) || is(Tk::kw_delete)) {
        spelled = sema::allocation_name(allocation_operator(kind()));
        if (kind(1) == Tk::l_square && kind(2) == Tk::r_square) {
          s_->diag(pos(), Diag::not_supported, kind());  // operator new[] and operator delete[]
          ok = false;
          return l;
        }
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
        l.template_args = template_argument_list(&l.replay_template_args_at);
        if (!l.template_args) ok = false;
      }
      skip_attributes();  // appertaining to the declarator-id ([dcl.meaning.general]/1)
    } else if (is(Tk::l_paren) &&
               (kind(1) == Tk::star || kind(1) == Tk::amp || kind(1) == Tk::ampamp || kind(1) == Tk::l_paren ||
                (kind(1) == Tk::identifier && (!abstract || !params_ahead())) || member_pointer_star(1))) {
      // In an abstract declarator, a type-name after `(` begins a parameter declaration
      // instead of naming the entity declared ([dcl.ambig.res]).
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
        Suffix& suffix = l.suffixes.back();
        suffix.cv = sema::Cv(cv);
        suffix.ref_at = pos();
        if (accept(Tk::amp)) suffix.ref = ast::RefQual::lvalue;
        else if (accept(Tk::ampamp)) suffix.ref = ast::RefQual::rvalue;
        // Parameter scope includes the exception specification and trailing return type
        // ([basic.scope.param]). Bind the adjusted types from the parameter list.
        if (is(Tk::kw_noexcept) || is(Tk::arrow)) {
          s_->push_scope();
          for (const Param& p : suffix.params)
            if (p.name != s_->no_name()) s_->declare_local(p.name, p.type, sema::local_flag::parameter, p.at, false);
          exception_specifier(suffix, ok);
          const bool has_trailing = accept(Tk::arrow);
          if (has_trailing)
            suffix.trailing = type_id(true);
          s_->pop_scope();
          if (has_trailing && !suffix.trailing) {
            ok = false;
            return l;
          }
        }
        // Dynamic exception specifications and repeated qualifiers are unsupported.
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
          if (s.dependent_bound && s_->ins().values.is<ast::VDepOp>(*s.dependent_bound)) {
            s_->diag(e.begin, Diag::not_supported, Tk::l_square);
            s.dependent_bound.reset();
          }
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
  std::optional<sema::Sema::Scope> nested_name(u32 start, u32& next, bool member_pointer = false,
                                               bool checked = false, bool type_components = false,
                                               bool check_injected_access = false) {
    u32 j = start;
    std::optional<sema::Sema::Scope> s;
    if (kind(j) == Tk::kw_decltype && kind(j + 1) == Tk::l_paren) {
      u32 saved = i_;
      const bool saved_half = half_;
      i_ += j;
      auto t = decltype_expression_type();
      bool closed = is(Tk::coloncolon);
      j = i_ - saved + 1;
      i_ = saved;
      half_ = saved_half;
      if (!t || !closed) return std::nullopt;
      s = sema::Sema::Scope{std::nullopt, *t};
    } else if (kind(j) == Tk::coloncolon) {
      s = sema::Sema::global_scope();
      ++j;
    } else if (kind(j) == Tk::identifier && kind(j + 1) == Tk::coloncolon) {
      s = s_->scope_named(name_of(tok(j)));
      if (!s) return std::nullopt;
      j += 2;
    } else if (auto t = template_id_scope(std::nullopt, j, /*scope=*/true, checked)) {
      s = t;
    } else {
      return std::nullopt;
    }
    for (;;) {
      if (kind(j) == Tk::identifier && kind(j + 1) == Tk::coloncolon) {
        NameId n = name_of(tok(j));
        if (type_components) {
          std::optional<u32> at = check_injected_access ? std::optional{pos() + j} : std::nullopt;
          auto injected = s_->qualified_injected_type(*s, n, at, /*type_only=*/true);
          if (injected.type) {
            s = sema::Sema::Scope{std::nullopt, *injected.type};
            j += 2;
            continue;
          }
        }
        auto m = s_->scope_member(*s, n);
        if (!m) return std::nullopt;
        if (member_pointer || checked) check_type_access(*s, n, pos() + j);
        s = m;
        j += 2;
      } else if (auto t = template_id_scope(s, j, /*scope=*/true, checked)) {
        s = t;
      } else {
        break;
      }
    }
    if (member_pointer && kind(j) == Tk::star) {
      next = j;
      return s;
    }
    // `S::template f`: the name after the keyword ([temp.names]/3); `next` stays at the keyword.
    if (kind(j) == Tk::kw_template && kind(j + 1) == Tk::identifier) {
      next = j;
      return s;
    }
    if (kind(j) != Tk::identifier && kind(j) != Tk::tilde && kind(j) != Tk::kw_operator) return std::nullopt;
    next = j;
    return s;
  }
  // A component `X<args>::` at `j` ahead naming a class template specialization, found by
  // unqualified lookup or in namespace `in` ([temp.names]); `j` moves past the `::`. The arguments
  // are parsed ahead of the tokens being taken. A speculative lookup discards its diagnostics.
  std::optional<sema::Sema::Scope> template_id_scope(std::optional<sema::Sema::Scope> in, u32& j, bool scope = true,
                                                     bool checked = false) {
    if (kind(j) != Tk::identifier || kind(j + 1) != Tk::less) return std::nullopt;
    if (in && !in->ns) return std::nullopt;  // member templates of classes come later (#88)
    NameId n = name_of(tok(j));
    const ast::TemplateParamSema* tt = in ? nullptr : s_->template_template_param(n);
    auto tmpl = in || !tt ? (in ? s_->namespace_type_template(*in->ns, n) : s_->type_template(n)) : std::nullopt;
    bool builtin = !in && !tt && !tmpl && s_->builtin_template(n);  // `__make_integer_seq<…>::`
    if (!tmpl && !tt && !builtin) return std::nullopt;
    u32 saved = i_;
    bool saved_half = half_;
    std::size_t diags = s_->diag_count();
    i_ += j + 1;
    auto args = template_argument_list();
    u32 after = i_ - saved;
    bool closed = scope ? !half_ && kind() == Tk::coloncolon : true;
    bool ended_half = half_;  // the list ended in the first half of a `>>`
    std::optional<QualType> t;
    if (args && closed)
      t = tt ? s_->template_param_id(*tt, *args, 0) : builtin ? s_->builtin_template_id(n, *args, 0) : type_template_id(*tmpl, *args, 0);
    i_ = saved;
    half_ = saved_half;
    if (!scope) qualified_half_ = ended_half;
    if (!checked) s_->truncate_diags(diags);
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
  // `checked`: the name is taken here, so its access is checked ([class.access]/4).
  std::optional<std::pair<QualType, u32>> qualified_type(u32 start, bool checked = false, bool type_only = false) {
    u32 next = 0;
    bool check_qualifier = checked && substituting_partial_arguments;
    auto s = nested_name(start, next, /*member_pointer=*/false, check_qualifier, /*type_components=*/true, checked);
    if (!s || kind(next) != Tk::identifier || kind(next + 1) == Tk::coloncolon) return std::nullopt;
    // `N::X<args>`: a class template specialization named in a namespace ([temp.names]).
    qualified_half_ = false;
    if (kind(next + 1) == Tk::less && s->ns) {
      u32 j = next;
      if (auto t = template_id_scope(s, j, /*scope=*/false, check_qualifier)) return std::pair{*t->type, j - start};
    }
    NameId name = name_of(tok(next));
    std::optional<u32> at = checked ? std::optional{pos() + next} : std::nullopt;
    auto injected = s_->qualified_injected_type(*s, name, at, type_only);
    if (injected.found)
      return injected.type ? std::optional{std::pair{*injected.type, next + 1 - start}} : std::nullopt;
    if (type_only) return std::nullopt;
    auto t = s_->scope_type(*s, name);
    if (!t) return std::nullopt;
    if (checked) check_type_access(*s, name, *at);
    return std::pair{*t, next + 1 - start};
  }
  void check_type_access(const sema::Sema::Scope& scope, NameId name, u32 at) {
    if (scope.type && s_->types().is_dependent(*scope.type)) ++deferred_argument_checks_;
    s_->type_member_access(scope, name, at);
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
  bool params_ahead(u32 open = 0, bool statement = false) {
    u32 first = open + 1;
    if (past_attributes(first) != first) return true;  // a parameter's attributes ([dcl.fct]/3)
    auto can_start_parameter = [&](u32 after_type) {
      Tk following = kind(after_type);
      if (following == Tk::l_brace) return false;  // `T{args}` is a functional conversion.
      if (following != Tk::l_paren) return true;
      if (statement)
        // An unnamed grouped pointer such as `int (*)(int)` cannot be a cast operand.
        return function_pointer_declarator_ahead(after_type, PointerLookahead::abstract);

      // `T(1)` and `T(-1)` cannot be parameter declarations. Keep the declaration reading for
      // `T()` and `T(name)`, where the parentheses can form part of a declarator.
      u32 operand = after_type + 1;
      while (kind(operand) == Tk::l_paren) ++operand;
      Tk first_operand = kind(operand);
      return !lex::is_literal(first_operand) && first_operand != Tk::kw_true &&
             first_operand != Tk::kw_false && first_operand != Tk::kw_nullptr &&
             first_operand != Tk::plus && first_operand != Tk::minus;
    };
    Tk k = kind(first);
    Tk next = kind(first + 1);
    if (simple_type_keyword(k)) return can_start_parameter(first + 1);
    if (k == Tk::r_paren || k == Tk::ellipsis || starts_decl_specifier(k)) return true;
    if ((k == Tk::identifier && next == Tk::coloncolon) || k == Tk::coloncolon) {
      auto q = qualified_type(first);  // `N::S`, `C::E`, `::S`
      return q && can_start_parameter(first + q->second);
    }
    if (k == Tk::identifier && next == Tk::less && s_->names_type_template(name_of(tok(first))))
      return can_start_parameter(past_template_args(first + 1));
    if (transform_trait_ahead(first)) return true;
    return k == Tk::identifier && next != Tk::coloncolon && s_->class_name(name_of(tok(first))).has_value() &&
           can_start_parameter(first + 1);
  }

  std::int64_t i64_of(sema::ConstId c) { return s_->world().values.as_signed(c); }

  static bool member_pointer_to_suffix(const Level& l) {
    const Level* inner = l.inner.get();
    while (inner && inner->pointers.empty() && inner->suffixes.empty()) inner = inner->inner.get();
    return inner && !inner->pointers.empty() && inner->pointers.front().owner.has_value();
  }

  // The type a declarator gives its name, built from the base type outwards ([dcl.meaning]).
  QualType build(const Level& l, QualType t, const Suffix*& fn, Declarator& d, bool spells_type,
                 NamedExceptionSpec named_exception = NamedExceptionSpec::ordinary) {
    for (const Level::PtrOp& p : l.pointers) {
      if (p.owner) {
        if (p.restrict_at) {
          s_->diag(*p.restrict_at, Diag::restrict_member_pointer);
          d.ok = false;
        }
        if (!s_->types().valid_member_type(t)) {
          s_->diag(p.at, Diag::member_pointer_type, t);
          d.ok = false;
          return t;
        }
        t = s_->types().add_cv(s_->types().member_pointer(*p.owner, t), p.cv);
        continue;
      }
      if (s_->types().is_cvref_function(t)) {
        s_->diag(p.at, Diag::indirection_to_qualified_function, t);
        d.ok = false;
        return t;
      }
      // [dcl.ptr], [dcl.ref]: no pointer to a reference, no reference to void.
      if (s_->types().is_reference(t) && p.op == Tk::star) {
        s_->diag(p.at, Diag::pointer_to_reference, t);
        d.ok = false;
        return t;
      }
      if (s_->types().is_void(t) && p.op != Tk::star) {
        s_->diag(d.begin, Diag::void_variable, s_->no_name());
        d.ok = false;
        return t;
      }
      // GNU restrict is accepted on pointer declarators without changing the built type.
      if (p.restrict_at && s_->types().is_function(t)) {
        s_->diag(*p.restrict_at, Diag::restrict_function_pointer);
        d.ok = false;
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
        // `auto f() -> T`: the return type is T, and the declared type `auto` alone ([dcl.fct]/2).
        if (s->trailing) {
          if (s_->types().kind(t) != sema::TypeKind::placeholder || s_->types().rec(t).decltype_auto ||
              sema::Types::cv(t) != sema::Cv::none) {
            s_->diag(d.begin, Diag::trailing_return_needs_auto);
            d.ok = false;
          }
          t = *s->trailing;
        }
        std::vector<QualType> ps;
        for (const Param& p : s->params) ps.push_back(p.type);
        std::string_view name = l.named ? s_->ins().names.spelling(l.name) : std::string_view{};
        bool allocation = sema::is_allocation_name(name);
        // [basic.stc.dynamic.deallocation]: a deallocation function without an explicit
        // exception specification is non-throwing.
        bool nonthrowing = name == sema::allocation_name(sema::AllocationFunction::deallocate) && !s->has_noexcept;
        const bool keep_named_exception = named_exception == NamedExceptionSpec::defaulted_assignment && name == "operator=";
        if (s->has_noexcept && (allocation || keep_named_exception || member_pointer_to_suffix(l) ||
                                (spells_type && !l.inner)))
          nonthrowing = noexcept_value(s->noexcept_condition, d.ok);
        else if (d.ok && s->has_noexcept && !s_->templated())
          nonthrowing = noexcept_value(s->noexcept_condition, d.ok);
        t = s_->types().function(t, ps, s->variadic, linkage_, s->cv, s->ref, nonthrowing);
        fn = &*s;
      } else if (!s->unknown_bound && !s->dependent_bound && s->bound == 0) {
        d.ok = false;  // the bound was diagnosed when parsed
      } else if (!s_->types().is_object(t) ||
                 (s_->types().is_array(t) && s_->types().rec(t).bound_value == sema::ConstId::none)) {
        // [dcl.array]: an element cannot be a reference, a function, void or an unknown-bound array.
        s_->diag(d.begin, Diag::invalid_array_element, t);
        d.ok = false;
      } else if (s->unknown_bound) {
        d.unknown_bound = true;
        t = s_->types().array_unknown(t);
      } else if (s->dependent_bound) {
        t = s_->types().array_dependent(t, *s->dependent_bound);
      } else {
        t = s_->types().array(t, s->bound);
      }
    }
    if (l.inner) return build(*l.inner, t, fn, d, spells_type, named_exception);
    d.name = l.name;
    d.named = l.named;
    d.name_at = l.name_at;
    d.qualified = l.qualified;
    d.qualifier = l.qualifier;
    d.qualifier_at = l.qualifier_at;
    d.scope = l.scope;
    d.template_args = l.template_args;
    d.template_args_at = l.template_args_at;
    d.replay_template_args_at = l.replay_template_args_at;
    d.pack = l.pack;
    return t;
  }

  // An exception specification on a function declarator ([except.spec]).
  void exception_specifier(Suffix& suffix, bool& ok) {
    suffix.has_noexcept = accept(Tk::kw_noexcept);
    if (!suffix.has_noexcept || !accept(Tk::l_paren)) return;
    const int saved = std::exchange(angle_, 0);
    Ex e = constant_expression();
    angle_ = saved;
    ok = e.ok() && expect(Tk::r_paren) && ok;
    suffix.noexcept_condition = e;
  }

  bool noexcept_value(const std::optional<Ex>& condition, bool& ok) {
    if (!condition) return true;
    Ex operand = s_->rvalue(*condition);
    Ex e = s_->to_bool(operand, operand.begin);
    sema::Evaluated result = sema::Evaluator(*s_, questions()).evaluate(e);
    if (result.value) {
      if (s_->narrows(operand, e.type())) {
        s_->diag(operand.begin, Diag::noexcept_narrowing, operand.type());
        ok = false;
      }
      return s_->world().values.as_signed(*result.value) != 0;
    }
    if (result.dependent) s_->diag(e.begin, Diag::not_supported, Tk::kw_noexcept);
    else if (e.ok()) s_->diag(e.begin, Diag::not_constant, result.reason);
    ok = false;
    return false;
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
      else if (s_->types().is_function(t)) {
        if (s_->types().is_cvref_function(t)) {
          s_->diag(at, Diag::indirection_to_qualified_function, t);
          ok = false;
        } else {
          t = s_->types().pointer(t);
        }
      }
      if (s_->types().is_void(t)) {
        s_->diag(at, Diag::void_parameter);
        ok = false;
      }
      // `T...` with no pack in T is `T, ...` ([dcl.fct]/3).
      // A generic lambda's `auto... xs` names the pack its placeholder invents (lambda_expression).
      if (d.pack && !s_->names_pack(t) && !(placeholder_params && s_->types().has_placeholder(t))) {
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
      if (accept(Tk::equal)) {
        if (member_template_name && !placeholder_params && in_params_ == 1) {
          u32 begin = index();
          std::map<u32, bool> template_probes;
          scan_member_default(s.params, template_probes);
          s.params.back().default_tokens = {begin, index()};
        } else {
          s.params.back().default_arg = default_argument(t);
          if (s.params.back().default_arg == NodeId::none) ok = false;  // diagnosed
        }
      }
      if (!accept(Tk::comma)) break;
      if (is(Tk::ellipsis)) {
        bump();
        s.variadic = true;
        break;
      }
    }
    ok = expect(Tk::r_paren) && ok;
    // A placeholder parameter makes an abbreviated function template ([dcl.fct]/22, #286), or a
    // generic lambda (#281); in a function type it is not allowed ([dcl.spec.auto.general]/2).
    if (!placeholder_params)
      for (const Param& p : s.params)
        if (s_->types().has_placeholder(p.type)) {
          s_->diag(p.at, Diag::not_supported, Tk::kw_auto);
          ok = false;
          break;
        }
    return s;
  }
  // The splitter's classification of a name declared anywhere in the enclosing class.
  std::function<std::optional<bool>(NameId)> member_template_name;

  // Capture the initializer-clause before its complete-class lookup context is available.
  bool scan_member_default(std::span<const Param> params, std::map<u32, bool>& template_probes,
                           bool template_probe = false) {
    deep_enough();
    std::vector<Tk> closes;
    unsigned angles = template_probe ? 1 : 0;
    unsigned conditionals = 0;
    while (!at_end()) {
      Tk k = kind();
      if (closes.empty()) {
        if (k == Tk::r_paren || k == Tk::r_brace || k == Tk::semi) break;
        if (k == Tk::question) ++conditionals;
        if (k == Tk::colon && conditionals != 0) --conditionals;
        if (conditionals == 0 && ((k == Tk::comma && angles == 0) || (template_probe && k == Tk::equal))) break;
        if (angles != 0 && (k == Tk::greater || k == Tk::greatergreater)) {
          unsigned closed = k == Tk::greater ? 1 : 2;
          angles -= std::min(angles, closed);
          if (template_probe && angles == 0) return true;
        }
        if (auto args = member_default_template_args(params, template_probes)) {
          seek(index() + *args + 1);
          ++angles;
          continue;
        }
      }
      if (k == Tk::l_paren) closes.push_back(Tk::r_paren);
      if (k == Tk::l_square) closes.push_back(Tk::r_square);
      if (k == Tk::l_brace) closes.push_back(Tk::r_brace);
      if (!closes.empty() && k == closes.back()) closes.pop_back();
      bump();
    }
    return false;
  }

  std::optional<u32> member_default_template_args(std::span<const Param> params, std::map<u32, bool>& template_probes) {
    if ((is(Tk::kw_static_cast) || is(Tk::kw_reinterpret_cast)) && kind(1) == Tk::less) return 1;
    if (is(Tk::kw_template) && kind(1) == Tk::identifier && kind(2) == Tk::less) return 2;
    if ((is(Tk::period) || is(Tk::arrow)) && kind(1) == Tk::identifier && kind(2) == Tk::less) {
      // Lookup of a template through an object needs the type of the expression. A balanced
      // template-id cannot cross the '=' of a following defaulted parameter.
      u32 start = index();
      auto [probe, fresh] = template_probes.try_emplace(start, false);
      if (fresh) {
        seek(start + 3);
        probe->second = scan_member_default(params, template_probes, true);
        seek(start);
      }
      if (probe->second) return 2;
    }
    if ((is(Tk::identifier) && kind(1) == Tk::coloncolon) || is(Tk::coloncolon)) {
      u32 name = 0;
      // Resolve the non-template prefix only. Forming a specialization here could try to
      // evaluate arguments before the enclosing class is complete.
      std::optional<sema::Sema::Scope> scope;
      if (is(Tk::coloncolon)) {
        scope = sema::Sema::global_scope();
        name = 1;
      }
      while (kind(name) == Tk::identifier && kind(name + 1) == Tk::coloncolon) {
        NameId qualifier = name_of(tok(name));
        scope = scope ? s_->scope_member(*scope, qualifier) : s_->scope_named(qualifier);
        if (!scope) return std::nullopt;
        name += 2;
      }
      bool keyword = kind(name) == Tk::kw_template;
      if (keyword) ++name;
      if (kind(name) != Tk::identifier || kind(name + 1) != Tk::less) return std::nullopt;
      NameId n = name_of(tok(name));
      if (keyword) return name + 1;
      if (scope && (s_->scope_names_template(*scope, n) ||
                    (scope->type && s_->types().is_class(*scope->type) &&
                     s_->types().class_of(*scope->type) == s_->current_class && member_template_name(n).value_or(false)) ||
                    (scope->ns && (s_->namespace_template(*scope->ns, n, ast::DeclKind::variable) ||
                                   s_->namespace_type_template(*scope->ns, n)))))
        return name + 1;
      return std::nullopt;
    }
    if (!is(Tk::identifier) || kind(1) != Tk::less) return std::nullopt;
    NameId n = name_of(tok());
    if (std::ranges::contains(params, n, &Param::name)) return std::nullopt;
    auto member = member_template_name(n);
    if (member ? *member : (s_->is_template_name(n) || s_->names_type_template(n))) return 1;
    return std::nullopt;
  }

  // A default argument ([dcl.fct.default]/1, /5): its names are bound and its copy-initialization
  // of the parameter is checked here. A template's that depends on its parameters is kept as it
  // is, and rejected where a call would use it.
  NodeId default_argument(QualType t) {
    // Not in the parameters of a parameter's own function type ([dcl.fct.default]/3).
    if (in_params_ > 1) s_->diag(pos(), Diag::default_arg_not_allowed);
    Ex e;
    bool ref = s_->types().is_reference(t);
    if (is(Tk::l_brace)) {
      auto b = braced_init_list();
      if (!b) return NodeId::none;
      if (ref && !s_->types().is_class(s_->types().referee(t)) &&
          b->clauses.size() == 1 && !b->clauses[0].list && !b->clauses[0].designated)
        e = s_->bind_reference(b->clauses[0].expr, t, b->clauses[0].begin, false);
      else
        e = s_->list_init(sema::Types::unqualified(t), *b, sema::Construction::copy_list);
    } else {
      Ex a = assignment_expression();
      if (!a.ok()) return NodeId::none;
      e = s_->types().is_dependent(t) || s_->dependent(a) ? a
          : ref                                             ? s_->bind_reference(a, t, a.begin, false)
                                                            : s_->convert(a, t, a.begin);
    }
    if (!e.ok()) return NodeId::none;
    if (s_->types().is_dependent(t) || s_->dependent(e)) e = s_->dependent_node({e}, e.begin, e.end);
    return s_->node(ast::DefaultArg{e.info, sema::Sema::at(e, e.begin)}, e.begin, e.end, e.info).id;
  }

  // Set while a lambda's parameters are read, which check their placeholders themselves.
  bool placeholder_params = false;
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
  // `keyword` false: a lambda's template-parameter-list, without `template` ([expr.prim.lambda.general]).
  bool template_head(u16 depth, bool keyword = true) {
    s_->template_scopes.emplace_back();
    s_->head_constraints.clear();
    if ((keyword && !expect(Tk::kw_template)) || !expect(Tk::less)) return false;
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
    if (is(Tk::kw_template)) return template_template_parameter(p, ok);
    // A type-constraint ([temp.param]/2): `C T`, `C<args> T`, `N::C T`: a type parameter whose
    // constraint is `C<T, args…>` ([temp.constr.decl]/3).
    u32 concept_name = 0;
    if (std::optional<ast::DeclSema> c = type_constraint_ahead(concept_name)) {
      u32 at = pos();
      for (u32 j = 0; j <= concept_name; ++j) bump();  // its nested-name-specifier and name
      std::vector<ast::TemplateArg> cargs;
      if (is(Tk::less)) {
        auto a = template_argument_list();
        if (!a) {
          ok = false;
          return std::nullopt;
        }
        cargs = *a;
      }
      if (is(Tk::ellipsis)) {
        s_->diag(pos(), Diag::not_supported, Tk::ellipsis);  // a constrained pack: a fold-expanded constraint
        ok = false;
        return std::nullopt;
      }
      p.kind = ast::TemplateArgKind::type;
      p.type = s_->types().template_param(depth, index);
      if (is(Tk::identifier)) {
        p.name = name_of(tok());
        bump();
      }
      cargs.insert(cargs.begin(), s_->templates().type_arg(p.type));
      if (auto nf = s_->concept_constraint(*c, cargs, at)) s_->head_constraints.push_back(*nf);
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
      bool later = s_->types().is_class(t) || s_->types().kind(t) == sema::TypeKind::enum_ ||
                   s_->types().is_floating(t);  // structural, not supported yet (floating: #317)
      s_->diag(at, later ? Diag::not_supported : Diag::invalid_template_parameter_type,
               s_->types().is_class(t)      ? Tk::kw_class
               : s_->types().is_floating(t) ? (s_->types().is(t, Builtin::float_) ? Tk::kw_float : Tk::kw_double)
                                            : Tk::kw_enum);
      ok = false;
      return std::nullopt;
    }
    if (d.named) p.name = d.name;
    p.kind = ast::TemplateArgKind::value;
    p.type = t;
    if (!p.pack && accept(Tk::equal)) {
      Ex e = conditional_expression();
      auto v = s_->types().is(t, Builtin::bool_) ? negated_template_variable(e) : std::nullopt;
      if (!v) v = template_value(e, t);
      if (!v) {
        ok = false;
        return std::nullopt;
      }
      p.has_default = 1;
      p.default_ = {ast::TemplateArgKind::value, s_->types().canonical(t), *v};
    }
    return p;
  }

  // A template template parameter ([temp.param]/1): `template<…> class TT = X`, `template<…>
  // class... TTs`. Its own head is at the next depth, in scope only in that head.
  std::optional<ast::TemplateParamSema> template_template_parameter(ast::TemplateParamSema p, bool& ok) {
    std::vector<sema::ConstId> constraints = s_->head_constraints;
    bool head = template_head(u16(p.depth + 1));
    std::vector<ast::TemplateParamSema> inner = std::move(s_->template_scopes.back());
    s_->template_scopes.pop_back();
    if (!head) {
      ok = false;
      return std::nullopt;
    }
    // A constrained head needs [temp.arg.template]/3's constraint check (#271).
    if (!s_->head_constraints.empty() || is(Tk::kw_requires)) {
      s_->diag(pos(), Diag::not_supported, Tk::kw_requires);
      ok = false;
      return std::nullopt;
    }
    s_->head_constraints = std::move(constraints);
    if (!accept(Tk::kw_class) && !accept(Tk::kw_typename)) {
      expect(Tk::kw_class);
      ok = false;
      return std::nullopt;
    }
    p.kind = ast::TemplateArgKind::template_;
    p.params = s_->world().ins.intern_seq(std::span<const ast::TemplateParamSema>(inner));
    if (accept(Tk::ellipsis)) p.pack = 1;
    p.type = s_->types().template_param(p.depth, p.index, p.pack != 0);
    if (is(Tk::identifier)) {
      p.name = name_of(tok());
      bump();
    }
    if (!p.pack && accept(Tk::equal)) {
      auto a = template_name_argument();
      if (!a) {
        ok = false;
        return std::nullopt;
      }
      p.has_default = 1;
      p.default_ = *a;
    }
    return p;
  }
  // Whether a template argument here is a template-name alone ([temp.arg.template]/1): one naming
  // a class or alias template, or a template template parameter, followed by the argument's end.
  bool template_name_ahead() {
    u32 at = 0;
    std::optional<ast::TemplateArg> a;
    if (qualified_ahead()) {
      auto sc = nested_name(0, at);
      if (!sc || !sc->ns || kind(at) != Tk::identifier) return false;
      a = s_->template_name_arg(sc->ns, name_of(tok(at)));
    } else if (is(Tk::identifier)) {
      a = s_->template_name_arg(std::nullopt, name_of(tok()));
    }
    Tk k = kind(at + 1);
    return a && (k == Tk::comma || k == Tk::greater || k == Tk::greatergreater || k == Tk::ellipsis);
  }
  // A template argument that is a template-name, `X` or `N::X` ([temp.arg.template]/1): none, with
  // nothing read, otherwise.
  std::optional<ast::TemplateArg> template_name_argument() {
    std::optional<ast::TemplateArg> a;
    u32 at = 0;
    if (qualified_ahead()) {
      auto sc = nested_name(0, at);
      if (!sc || !sc->ns || kind(at) != Tk::identifier) return std::nullopt;
      a = s_->template_name_arg(sc->ns, name_of(tok(at)));
    } else if (is(Tk::identifier)) {
      a = s_->template_name_arg(std::nullopt, name_of(tok()));
    }
    if (!a) return std::nullopt;
    for (u32 j = 0; j <= at; ++j) bump();
    return a;
  }

  // A type-constraint's concept here, before a parameter's name: `C T`, `C<…> T`, `N::C T`.
  // `name` is set to the offset of the concept's name.
  std::optional<ast::DeclSema> type_constraint_ahead(u32& name) {
    if (qualified_ahead()) {
      u32 next = 0;
      auto sc = nested_name(0, next);
      if (!sc || !sc->ns || kind(next) != Tk::identifier) return std::nullopt;
      name = next;
      return s_->namespace_template(*sc->ns, name_of(tok(next)), ast::DeclKind::concept_);
    }
    name = 0;
    if (!is(Tk::identifier)) return std::nullopt;
    Tk k = kind(1);
    if (k != Tk::identifier && k != Tk::less && k != Tk::ellipsis && k != Tk::comma && k != Tk::greater && k != Tk::equal)
      return std::nullopt;
    return s_->concept_named(name_of(tok()));
  }

  // A type-id ([dcl.name]): a type-specifier-seq and an optional abstract declarator.
  // A placeholder is allowed only where `placeholder_ok` says (a trailing return type,
  // [dcl.spec.auto.general]/3): not in a template argument, a cast or an alias ([dcl.spec.auto.general]/6).
  std::optional<QualType> type_id(bool placeholder_ok = false) {
    DeclSpec spec = decl_specifiers(/*in_type_id=*/true);
    if (!spec.ok) return std::nullopt;
    Declarator d = declarator(spec.type, true);
    if (!d.ok) return std::nullopt;
    if (d.named) {
      s_->diag(d.name_at, Diag::unexpected_identifier, d.name);
      return std::nullopt;
    }
    if (!placeholder_ok && s_->types().has_placeholder(d.type)) {
      s_->diag(spec.begin, Diag::auto_not_allowed);
      return std::nullopt;
    }
    return d.type;
  }

  // In a declaration's bool template argument or default, keep a chain of `!` over a dependent
  // bool variable template-id. Function bodies reparse the expression once its type and destination
  // are known, including when the destination is an integer parameter.
  std::optional<sema::ConstId> negated_template_variable(Ex e) {
    if (body || !s_->dependent(e)) return std::nullopt;
    std::size_t negations = 0;
    for (;;) {
      while (auto paren = s_->as<ast::Paren>(e)) e.id = paren->operand.id;
      auto u = s_->as<ast::Unary>(e);
      if (!u || u->op != ast::UnaryOp::not_) break;
      ++negations;
      e.id = u->operand.id;
    }
    if (negations == 0) return std::nullopt;
    auto literal = s_->as<ast::Literal>(e);
    if (!literal || !s_->types().is(literal->info.type, Builtin::bool_)) return std::nullopt;
    return s_->templates().negated_variable_value(literal->value, negations);
  }

  // The value of a template argument for a non-type parameter of type `t`: a converted constant
  // expression ([temp.arg.nontype]/1). One that names a template parameter is value-dependent and
  // stays a `VDep*` value ([temp.dep.constexpr]).
  std::optional<sema::ConstId> template_value(Ex e, QualType t) {
    if (!e.ok()) return std::nullopt;
    Ex operand = e;
    while (auto p = s_->as<ast::Paren>(operand)) operand.id = p->operand.id;
    if (auto lit = s_->as<ast::Literal>(operand); lit && s_->ins().values.is_dependent(lit->value)) {
      if (s_->ins().values.is<ast::VDepOp>(lit->value) && !s_->types().is(t, Builtin::bool_)) {
        s_->diag(e.begin, Diag::not_supported, Tk::kw_template);
        return std::nullopt;
      }
      return lit->value;
    }
    // Value-dependent: in a body its value is known when the body is instantiated; in a declaration
    // it is part of the template's signature, which needs the expression's `VDep*` form (#88).
    if (s_->dependent(e)) {
      if (body && !(e.info.flags & sema::expr_flag::unrepresented_value))
        return s_->world().ins.intern_value(ast::VDepParam{sema::dependent_depth, 0});
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
  std::optional<std::vector<ast::TemplateArg>> template_argument_list(std::optional<u32>* replay_at = nullptr) {
    u32 begin = pos();
    u32 deferred = deferred_argument_checks_;
    if (!expect(Tk::less)) return std::nullopt;
    std::vector<ast::TemplateArg> args;
    bool ok = true;
    ++angle_;
    if (!is(Tk::greater) && !is(Tk::greatergreater)) {
      do {
        u32 arg_at = pos();
        std::size_t diags = s_->diag_count();
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
        if (template_name_ahead()) {
          args.push_back(*template_name_argument());
        } else if (starts_type_id()) {
          auto t = type_id();
          if (!t) ok = false;
          else args.push_back({ast::TemplateArgKind::type, s_->types().canonical(*t), sema::ConstId::none});
        } else {
          Ex e = conditional_expression();
          if (e.ok()) {
            // Converted to its parameter's type when the template is known ([temp.arg.nontype]).
            auto v = negated_template_variable(e);
            QualType t = v ? s_->builtin(Builtin::bool_) : sema::Types::unqualified(s_->rvalue(e).type());
            if (!v) v = template_value(e, t);
            if (!v) ok = false;
            else args.push_back({ast::TemplateArgKind::value, s_->types().canonical(t), *v});
          } else {
            ok = false;
          }
        }
        // `pattern...`: a pack expansion ([temp.variadic]/5), expanded where its packs are bound.
        if (ok && is(Tk::ellipsis)) {
          u32 at = pos();
          bump();
          if (!s_->names_pack(args.back())) {
            s_->diag(at, Diag::expansion_without_pack);
            ok = false;
          } else {
            args.back() = s_->templates().expansion_of(args.back());
            end_pattern.expanded = true;
          }
        }
        // Substitution stops at the first failure ([temp.deduct.general]). A later argument may
        // instantiate another template and produce a hard error even during candidate matching.
        if (substituting_partial_arguments && (!ok || s_->has_error_since(diags))) {
          --angle_;
          return std::nullopt;
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
    if (replay_at && deferred != deferred_argument_checks_) *replay_at = begin;
    return args;
  }
  bool starts_type_id() {
    if (transform_trait_ahead()) return true;
    if (starts_decl_specifier(kind())) return !is(Tk::kw_static) && !is(Tk::kw_extern);
    // `X<args>::m` is a type only when m is: the nested name first. A member of a dependent class
    // is a type only after `typename` ([temp.res.general]/4).
    if (qualified_ahead()) {
      auto q = qualified_type(0);
      return q && s_->types().kind(q->first) != sema::TypeKind::dep_name;
    }
    if (is(Tk::identifier) && kind(1) == Tk::less && s_->names_type_template(name_of(tok()))) return true;
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

  // ------------------------------------------------------------ constraints ([temp.constr])

  // A constraint-expression read into its normal form ([temp.constr.normal]): `||`, `&&`,
  // parentheses and concept-ids decompose; any other operand is an atomic constraint. With
  // `primaries` (a requires-clause, [temp.pre]/9) operands are primary-expressions; otherwise (a
  // concept's definition) they are what binds tighter than `&&`.
  sema::ConstId constraint_expression(bool primaries) {
    sema::ConstId l = constraint_conjunction(primaries);
    while (is(Tk::pipepipe)) {
      bump();
      l = s_->templates().constraint_op(1, l, constraint_conjunction(primaries));
    }
    return l;
  }
  sema::ConstId constraint_conjunction(bool primaries) {
    sema::ConstId l = constraint_operand(primaries);
    while (is(Tk::ampamp)) {
      bump();
      l = s_->templates().constraint_op(0, l, constraint_operand(primaries));
    }
    return l;
  }
  // Whether an operand of `&&` or `||` ends here.
  bool operand_ends(bool primaries) {
    return primaries || is(Tk::ampamp) || is(Tk::pipepipe) || is(Tk::r_paren) || is(Tk::semi) || at_end();
  }
  sema::ConstId constraint_operand(bool primaries) {
    u32 back = i_;
    std::size_t n = s_->diag_count();
    // `( constraint-expression )`, when it is the whole operand.
    if (is(Tk::l_paren)) {
      bump();
      int saved = std::exchange(angle_, 0);
      sema::ConstId c = constraint_expression(false);
      angle_ = saved;
      if (accept(Tk::r_paren) && operand_ends(primaries)) return c;
      i_ = back;
      s_->truncate_diags(n);
    }
    // A concept-id, when it is the whole operand: its concept's normal form with the arguments
    // substituted ([temp.constr.normal]/1.4).
    if (auto c = concept_id_constraint()) {
      if (operand_ends(primaries)) return *c;
      i_ = back;
      s_->truncate_diags(n);
    }
    u32 begin = pos();
    Ex e = primaries ? primary_expression() : binary_expression(3);
    (void)e;
    // The template parameters the expression names: its parameter mapping's ([temp.constr.atomic]/1).
    std::vector<NameId> names;
    for (u32 j = begin_ + begin; j < i_; ++j)
      if (r_->tokens[j].kind == Tk::identifier) names.push_back(name_of(r_->tokens[j]));
    return s_->atom(begin, pos(), names);
  }
  // `C<args>` or `N::C<args>` naming a concept here: its normal form; none (nothing read) if it
  // is no concept-id.
  std::optional<sema::ConstId> concept_id_constraint() {
    u32 back = i_;
    std::size_t n = s_->diag_count();
    std::optional<ast::DeclSema> c;
    u32 at = pos();
    if (qualified_ahead()) {
      u32 next = 0;
      auto sc = nested_name(0, next);
      if (!sc || !sc->ns || kind(next) != Tk::identifier || kind(next + 1) != Tk::less) return std::nullopt;
      c = s_->namespace_template(*sc->ns, name_of(tok(next)), ast::DeclKind::concept_);
      if (!c) return std::nullopt;
      for (u32 j = 0; j <= next; ++j) bump();
    } else {
      if (!is(Tk::identifier) || kind(1) != Tk::less) return std::nullopt;
      c = s_->concept_named(name_of(tok()));
      if (!c) return std::nullopt;
      bump();
    }
    auto args = template_argument_list();
    if (!args) {  // read again as an expression, which diagnoses it
      i_ = back;
      s_->truncate_diags(n);
      return std::nullopt;
    }
    return s_->concept_constraint(*c, *args, at).value_or(sema::ConstId::none);
  }

  // `C` or `N::C` naming a concept here, read; none, with nothing read, otherwise.
  std::optional<ast::DeclSema> concept_name() {
    std::optional<ast::DeclSema> c;
    u32 at = 0;
    if (qualified_ahead()) {
      auto sc = nested_name(0, at);
      if (!sc || !sc->ns || kind(at) != Tk::identifier) return std::nullopt;
      c = s_->namespace_template(*sc->ns, name_of(tok(at)), ast::DeclKind::concept_);
    } else if (is(Tk::identifier)) {
      c = s_->concept_named(name_of(tok()));
    }
    if (!c) return std::nullopt;
    for (u32 j = 0; j <= at; ++j) bump();
    return c;
  }

  // A nested-requirement's constraint-expression, checked where it is written, with the
  // requires-expression's parameters in scope ([expr.prim.req.nested]): its `||` and `&&`
  // short-circuit as satisfaction does ([temp.constr.op]), so an operand that is not checked may be
  // invalid. Its normal form is not needed: subsumption does not look into a requires-expression.
  enum class Truth : u8 { no, yes, dependent, invalid };
  Truth nested_constraint(bool templated) {
    Truth l = nested_conjunction(templated);
    while (accept(Tk::pipepipe)) {
      std::size_t n = s_->diag_count();
      Truth r = nested_conjunction(templated);
      if (l == Truth::yes) {
        s_->drop_failures(n);
        continue;
      }
      if (r == Truth::yes) l = r;
      else if (l == Truth::dependent || r == Truth::dependent) l = Truth::dependent;
      else l = Truth::no;
    }
    return l;
  }
  Truth nested_conjunction(bool templated) {
    Truth l = nested_operand(templated);
    while (accept(Tk::ampamp)) {
      std::size_t n = s_->diag_count();
      Truth r = nested_operand(templated);
      if (l == Truth::no || l == Truth::invalid) {
        s_->drop_failures(n);
        continue;
      }
      if (r == Truth::no || r == Truth::invalid) l = r;
      else if (l == Truth::dependent || r == Truth::dependent) l = Truth::dependent;
    }
    return l;
  }
  Truth nested_operand(bool templated) {
    std::size_t n = s_->diag_count();
    u32 at = pos();
    Ex e = binary_expression(3);
    if (s_->diag_count() > n || !e.ok()) {
      if (templated) s_->drop_failures(n);
      return Truth::invalid;
    }
    if (s_->dependent(e)) return Truth::dependent;
    Ex v = s_->rvalue(e);
    if (!s_->types().is(sema::Types::unqualified(s_->types().canonical(v.type())), sema::Builtin::bool_)) {
      s_->diag(at, Diag::constraint_not_bool, v.type());
      return Truth::invalid;
    }
    sema::Evaluated r = sema::Evaluator(*s_, questions()).evaluate(v);
    if (r.dependent) return Truth::dependent;
    if (!r.value) {
      s_->diag(at, Diag::constraint_not_constant, r.reason);
      return Truth::invalid;
    }
    return s_->world().values.as_signed(*r.value) != 0 ? Truth::yes : Truth::no;
  }

  // A requires-expression ([expr.prim.req]): a `bool` prvalue, true when every requirement is
  // satisfied, value-dependent while one depends on a template parameter. In a templated entity
  // an invalid requirement makes it false; elsewhere it is ill-formed (/5).
  Ex requires_expression() {
    u32 begin = pos();
    bump();
    s_->push_scope();
    const bool templated = s_->templated();
    const std::size_t param_diags = s_->diag_count();
    const std::size_t first_local = s_->locals().size();
    auto close_requires_scope = [&] {
      s_->pop_scope();
      s_->discard_local_declarations(first_local);
    };
    bool ok = true;
    if (accept(Tk::l_paren)) {
      Suffix params = parameters(ok);
      ok = ok && s_->diag_count() == param_diags;
      for (const Param& p : params.params)
        if (p.name != s_->no_name())
          s_->declare_local(p.name, p.type, sema::local_flag::parameter | ast::local_flag::requires_parameter, p.at, false);
    }
    if (!expect(Tk::l_brace)) {
      close_requires_scope();
      return {NodeId::none, begin, pos()};
    }
    // [expr.prim.req.general]: substitution into the local parameter declarations can fail
    // before a requirement is checked. Consume the body without analysing it in that case.
    if (!ok) {
      int depth = 1;
      while (depth > 0 && !at_end()) {
        if (is(Tk::l_brace)) ++depth;
        else if (is(Tk::r_brace)) --depth;
        bump();
      }
      close_requires_scope();
      if (!templated) return {NodeId::none, begin, pos()};
      s_->drop_failures(param_diags);
      return s_->bool_literal(false, begin, pos());
    }
    bool satisfied = true;
    bool dependent = false;
    while (!is(Tk::r_brace) && !at_end()) {
      std::size_t n = s_->diag_count();
      u32 before = i_;
      bool failed = false;
      if (is(Tk::kw_typename)) {  // a type-requirement
        auto t = type_id();
        if (t && s_->types().is_dependent(*t)) dependent = true;
      } else if (accept(Tk::l_brace)) {  // a compound-requirement
        ++s_->unevaluated;
        Ex e = expression();
        --s_->unevaluated;
        expect(Tk::r_brace);
        if (is(Tk::kw_noexcept)) {
          s_->diag(pos(), Diag::not_supported, Tk::kw_noexcept);
          bump();
        }
        bool dep = s_->dependent(e);
        if (accept(Tk::arrow)) {  // `-> C<args>`: C<decltype((e)), args…> ([expr.prim.req.compound])
          u32 at = pos();
          if (std::optional<ast::DeclSema> c = concept_name()) {
            std::vector<ast::TemplateArg> args;
            if (is(Tk::less)) args = template_argument_list().value_or(std::vector<ast::TemplateArg>{});
            if (!dep && e.ok()) {
              args.insert(args.begin(), s_->templates().type_arg(s_->decltype_of(e, true)));
              auto nf = s_->concept_constraint(*c, args, at);
              auto sat = nf ? s_->questions().satisfied(*nf) : std::optional<bool>(false);
              if (!sat) dependent = true;
              else if (!*sat) failed = true;
            }
          } else {
            s_->diag(at, Diag::not_a_concept, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
            failed = true;
            skip_to_semi();
          }
        }
        if (dep) dependent = true;
      } else if (accept(Tk::kw_requires)) {  // a nested-requirement ([expr.prim.req.nested])
        Truth t = nested_constraint(templated);
        if (t == Truth::dependent) dependent = true;
        else if (t != Truth::yes) failed = true;
      } else {  // a simple-requirement
        ++s_->unevaluated;
        Ex e = expression();
        --s_->unevaluated;
        if (s_->dependent(e)) dependent = true;
      }
      if (!expect(Tk::semi)) skip_to_semi();
      if (i_ == before) bump();  // no progress: drop the token
      if (s_->diag_count() > n || failed) {
        satisfied = false;
        if (templated) s_->drop_failures(n);
      }
    }
    expect(Tk::r_brace);
    close_requires_scope();
    if (satisfied && dependent) return s_->dependent_node({}, begin, pos(), s_->builtin(Builtin::bool_));
    return s_->bool_literal(satisfied, begin, pos());
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
    if (is(Tk::kw_throw)) return throw_expression();
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

  // [expr.throw]: the optional operand is itself an assignment-expression.
  Ex throw_expression() {
    u32 begin = pos();
    bump();
    std::optional<Ex> operand;
    if (!at_end() && !is(Tk::semi) && !is(Tk::comma) && !is(Tk::colon) &&
        !is(Tk::r_paren) && !is(Tk::r_square) && !is(Tk::r_brace) &&
        !(angle_ && (is(Tk::greater) || is(Tk::greatergreater)))) {
      operand = assignment_expression();
    }
    return s_->throw_expression(operand, begin, pos());
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

  // The binary operators from `||` (precedence 1) to pointer-to-member access (11).
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
    case Tk::periodstar: return BinInfo{11, B::dot_star};
    case Tk::arrowstar: return BinInfo{11, B::arrow_star};
    default: return std::nullopt;
    }
  }

  // logical-or-expression down to pm-expression ([expr.mptr.oper]–[expr.log.or]), all
  // left-associative, by precedence climbing.
  Ex binary_expression(int min_prec) {
    Ex l = cast_expression();
    for (;;) {
      if (is(Tk::spaceship)) {
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
  // Diagnoses and abandons the item when the nesting is too deep for the stack (#187).
  void deep_enough() {
    if (!stack::deep()) return;
    s_->diag(pos(), Diag::nested_too_deeply);
    throw TooDeep{};
  }

  Ex cast_expression() {
    deep_enough();  // parentheses and casts nest through here
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

  // Ordinary new-expressions ([expr.new]). A parenthesized type-id uses the abstract declarator;
  // a bare new-type-id greedily takes ptr-operators and cannot have function suffixes.
  Ex new_expression(u32 begin) {
    bump();  // new, after an optional ::
    std::optional<QualType> type;
    std::optional<Ex> bound;
    bool excluded = false;
    if (is(Tk::l_paren)) {
      u32 past = i_ + past_parentheses(0);
      bump();
      if (starts_type_id()) {
        type = type_id(/*placeholder_ok=*/true);
        if (!expect(Tk::r_paren) || !type) return {NodeId::none, begin, pos()};
      } else {
        excluded = true;  // placement arguments
        i_ = past;
        if (accept(Tk::l_paren)) {
          type = type_id(/*placeholder_ok=*/true);
          if (!expect(Tk::r_paren) || !type) return {NodeId::none, begin, pos()};
        }
      }
    }
    if (!type) {
      DeclSpec spec = decl_specifiers(/*in_type_id=*/true);
      if (!spec.ok) return {NodeId::none, begin, pos()};
      if (spec.is_static || spec.is_extern || spec.is_constexpr || spec.is_consteval || spec.is_constinit ||
          spec.is_inline || spec.is_typedef) {
        s_->diag(spec.begin, Diag::expected_type_specifier);
        return {NodeId::none, begin, pos()};
      }
      Level l;
      Declarator d;
      d.begin = pos();
      pointer_operators(l, d.ok);
      const Suffix* fn = nullptr;
      type = build(l, spec.type, fn, d, true);
      if (!d.ok) return {NodeId::none, begin, pos()};
      while (is(Tk::l_square) && kind(1) != Tk::l_square) {
        bump();
        int saved = std::exchange(angle_, 0);
        if (is(Tk::r_square)) {
          excluded = true;  // omitted bound
        } else if (bound) {
          excluded = true;  // multiple dimensions
          constant_expression();
        } else {
          bound = expression();
        }
        angle_ = saved;
        if (!expect(Tk::r_square)) return {NodeId::none, begin, pos()};
        skip_attributes();
      }
    }

    std::unique_ptr<sema::Braced> braces;
    std::optional<std::vector<Ex>> args;
    if (is(Tk::l_brace)) {
      braces = braced_init_list();
    } else if (accept(Tk::l_paren)) {
      args.emplace();
      int saved = std::exchange(angle_, 0);
      if (!is(Tk::r_paren))
        do {
          args->push_back(is(Tk::l_brace) ? s_->braced_argument(braced_init_list()) : assignment_expression());
        } while (accept(Tk::comma));
      angle_ = saved;
      if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
    }
    if (excluded) {
      s_->diag(begin, Diag::not_supported, Tk::kw_new);
      return {NodeId::none, begin, pos()};
    }
    if (bound) return s_->new_array(*type, *bound, args, braces.get(), begin, pos());
    return s_->new_object(*type, args, braces.get(), begin, pos());
  }

  Ex delete_expression(u32 begin) {
    bump();  // delete, after an optional ::
    bool array = is(Tk::l_square) && kind(1) == Tk::r_square;
    if (array) {
      bump();
      bump();
    }
    Ex operand = cast_expression();
    return array ? s_->delete_array(operand, begin, pos()) : s_->delete_object(operand, begin, pos());
  }

  // unary-expression ([expr.unary]).
  Ex unary_expression() {
    deep_enough();  // `++`, `--` and `sizeof` take their operands through here
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
      ++s_->unevaluated;
      Ex e = unary_expression();
      --s_->unevaluated;
      if (!e.ok()) return {NodeId::none, begin, pos()};
      if (e.glvalue() && e.info.bit_width) {
        s_->diag(begin, Diag::bit_field_size);
        return {NodeId::none, begin, pos()};
      }
      Ex r = s_->size_of(e.type(), false, begin, pos());
      return {r.id, begin, pos(), r.info};
    }
    case Tk::coloncolon:
      if (kind(1) != Tk::kw_new && kind(1) != Tk::kw_delete) return postfix_expression();
      bump();
      return is(Tk::kw_new) ? new_expression(begin) : delete_expression(begin);
    case Tk::kw_new: return new_expression(begin);
    case Tk::kw_delete: return delete_expression(begin);
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
    auto saved_address = address_op_;
    if (*op == ast::UnaryOp::address_of) address_op_ = begin;
    Ex e = is_incdec ? unary_expression() : cast_expression();
    address_op_ = saved_address;
    if (*op == ast::UnaryOp::address_of && e.begin == begin) return e;  // qualified member address
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
        // `E.template f<args>`: the name is a member template's ([temp.names]/3).
        bool keyword = accept(Tk::kw_template);
        u32 qualifier = 0;
        while (kind(qualifier) == Tk::identifier && kind(qualifier + 1) == Tk::coloncolon) qualifier += 2;
        if (kind(qualifier) == Tk::tilde) {
          s_->diag(pos() + qualifier, Diag::not_supported, Tk::tilde);
          seek(index() + qualifier + 1);
          if (is(Tk::identifier)) bump();
          if (is(Tk::l_paren) && kind(1) == Tk::r_paren) seek(index() + 2);
          return {NodeId::none, e.begin, pos()};
        }
        std::optional<NameId> member_class;
        // This stage recognizes only a direct qualified invocation on the Linux consumer path.
        if (qualifier == 2 && !keyword && kind(2) == Tk::identifier && kind(3) == Tk::l_paren &&
            s_->types().target().triple == ast::x86_64_linux.triple) {
          member_class = name_of(tok());
          bump();
          bump();
        }
        if (!is(Tk::identifier)) {
          if (is(Tk::tilde) || is(Tk::coloncolon) || is(Tk::kw_operator))
            s_->diag(pos(), Diag::not_supported, kind());
          else
            s_->diag(pos(), Diag::expected_unqualified_id);
          if (!at_end()) bump();
          return {NodeId::none, e.begin, pos()};
        }
        u32 name_at = pos();
        NameId n = name_of(tok());
        bump();
        // A member template's explicit template arguments ([temp.arg.explicit]): after `template`,
        // or where the member found is a template ([temp.names]/3).
        std::optional<std::vector<ast::TemplateArg>> targs;
        if (is(Tk::less) && (keyword || s_->names_member_template(e, arrow, n))) {
          targs = template_argument_list();
          if (!targs) return {NodeId::none, e.begin, pos()};
        }
        e = s_->member(e, arrow, n, pos(), at, name_at, member_class);
        if (targs) s_->explicit_template_args(e, std::move(*targs));
        break;
      }
      default: return e;
      }
    }
  }

  Ex va_expression(lex::VaBuiltin op, u32 begin) {
    bump();  // builtin name
    bump();  // (
    int saved = std::exchange(angle_, 0);
    Ex cursor = assignment_expression();
    std::optional<Ex> source;
    std::optional<QualType> fetch;
    bool ok = true;
    using enum lex::VaBuiltin;
    if (op == arg || op == copy || op == legacy_start) {
      ok = expect(Tk::comma);
      if (ok && op == arg) {
        fetch = type_id();
        ok = fetch.has_value();
      } else if (ok) {
        Ex second = assignment_expression();
        ok = second.ok();
        if (op == copy) source = second;
      }
    }
    angle_ = saved;
    if (!expect(Tk::r_paren) || !ok) return {NodeId::none, begin, pos()};
    return s_->va_call(op, cursor, source, fetch, begin, pos());
  }

  // primary-expression ([expr.prim]).
  Ex primary_expression() {
    u32 begin = pos();
    switch (kind()) {
    case Tk::int_lit: {
      const lex::Literal& l = literal();
      bump();
      if (l.bad) return {NodeId::none, begin, pos()};
      if (l.ud_suffix != lex::no_name) {
        auto n = s_->ins().names.intern(lex::literal_operator_name(r_->names[l.ud_suffix]));
        return immediate(s_->integer_user_literal(n, l, begin, pos()));
      }
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
      // A GNU built-in function ycxx does not have yet: its arguments, which may be types, are
      // skipped (docs/EXTENSIONS.md).
      if (kind(1) == Tk::l_paren && s_->unsupported_builtin(n)) {
        s_->diag(begin, Diag::builtin_unsupported, n);
        bump();
        bump();
        do skip_param();
        while (accept(Tk::comma));
        expect(Tk::r_paren);
        return {NodeId::none, begin, pos()};
      }
      if (kind(1) == Tk::l_paren)
        if (auto op = s_->va_builtin(n)) return va_expression(*op, begin);
      // A builtin with ordinary expression arguments (docs/EXTENSIONS.md).
      if (kind(1) == Tk::l_paren)
        if (const lex::BuiltinFunctionInfo* bf = s_->builtin_function(n)) {
          bump();
          bump();
          std::vector<Ex> args;
          int saved = std::exchange(angle_, 0);
          if (!is(Tk::r_paren))
            do args.push_back(assignment_expression());
            while (accept(Tk::comma));
          angle_ = saved;
          if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
          Ex r = s_->builtin_call(*bf, std::move(args), begin, pos());
          return {r.id, begin, pos(), r.info};
        }
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
      if (kind(1) == Tk::less && s_->names_type_template(n)) {
        auto t = class_template_type();
        if (!t) return {NodeId::none, begin, pos()};
        return type_conversion(*t, begin);
      }
      if ((kind(1) == Tk::l_paren || kind(1) == Tk::l_brace))
        if (auto t = s_->class_name(n, false, true, pos())) {
          // `T(args)` or `T{…}` ([expr.type.conv]), T a type name: a class, an enumeration, a
          // typedef-name or a template parameter.
          bump();
          return type_conversion(*t, begin);
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
    case Tk::coloncolon:
      if (!allocation_operator_ahead(1)) return qualified_id(begin);
      bump();
      return allocation_function_id(begin);
    case Tk::kw_decltype:
      if (kind(1) == Tk::l_paren && kind(past_parentheses(1)) == Tk::coloncolon) return qualified_id(begin);
      s_->diag(begin, Diag::expected_expression);
      return {NodeId::none, begin, begin};
    case Tk::kw_this: bump(); return s_->this_expr(begin, pos());
    case Tk::kw_requires: return requires_expression();
    case Tk::kw_static_cast:
    case Tk::kw_reinterpret_cast: {
      const bool reinterpret = is(Tk::kw_reinterpret_cast);
      bump();
      if (!expect(Tk::less)) return {NodeId::none, begin, pos()};
      auto t = type_id();
      if (!t || !expect(Tk::greater) || !expect(Tk::l_paren)) return {NodeId::none, begin, pos()};
      int saved = std::exchange(angle_, 0);
      Ex e = expression();
      angle_ = saved;
      if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
      Ex r = reinterpret ? s_->reinterpret_cast_to(e, *t, begin)
                         : s_->static_cast_to(e, *t, begin, /*named_static_cast=*/true);
      return {r.id, begin, pos(), r.info};
    }
    case Tk::float_lit: {
      const lex::Literal& l = literal();
      bump();
      if (l.ud_suffix != lex::no_name) {
        s_->diag(begin, Diag::not_supported, Tk::float_lit);
        return {NodeId::none, begin, pos()};
      }
      if (l.bad) return {NodeId::none, begin, pos()};
      return s_->float_literal(l, begin, pos());
    }
    case Tk::l_square:
      // [dcl.attr.grammar]/7: `[[` begins an attribute-specifier only, never a lambda.
      if (kind(1) == Tk::l_square) {
        s_->diag(begin, Diag::not_supported, Tk::l_square);
        bump();
        return {NodeId::none, begin, pos()};
      }
      return lambda_expression();
    case Tk::kw_operator:
      if (auto suffix = punctuator_operator_suffix(1); !suffix.empty() || literal_operator_ahead()) {
        auto n = expression_id(suffix);
        if (!n) return {NodeId::none, begin, pos()};
        if (is(Tk::less) && s_->is_template_name(*n)) {
          s_->diag(begin, Diag::not_supported, Tk::kw_operator);
          return {NodeId::none, begin, pos()};
        }
        return s_->name_expr(*n, begin, pos(), is(Tk::l_paren));
      }
      if (allocation_operator_ahead()) return allocation_function_id(begin);
      [[fallthrough]];
    case Tk::tilde:
    case Tk::kw_typeid:
    case Tk::kw_dynamic_cast:
    case Tk::kw_const_cast:
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
  Ex allocation_function_id(u32 begin) {
    bump();  // operator
    Tk k = kind();
    bump();
    if (is(Tk::l_square) && kind(1) == Tk::r_square) {
      bump();
      bump();
      s_->diag(begin, Diag::not_supported, k);
      return {NodeId::none, begin, pos()};
    }
    return s_->allocation_name_expr(allocation_operator(k), begin, pos());
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
    if (!expect(Tk::l_paren)) return {NodeId::none, begin, pos()};
    std::vector<Ex> args;
    if (!is(Tk::r_paren))
      do args.push_back(assignment_expression());
      while (accept(Tk::comma));
    if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
    const bool lambda_result = lambda_result_operand_ == begin && is(Tk::semi);
    Ex r = lambda_result ? s_->lambda_result_cast(t, std::move(args), begin, pos())
                        : s_->functional_cast(t, std::move(args), begin, pos());
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
    deep_enough();  // nested braced lists
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
    auto address = address_op_ && *address_op_ + 1 == begin ? address_op_ : std::nullopt;
    auto s = nested_name(0, next, /*member_pointer=*/false, substituting_partial_arguments || address.has_value());
    // `S::template f<args>`: f names a template ([temp.names]/3).
    bool keyword = s && kind(next) == Tk::kw_template && kind(next + 1) == Tk::identifier;
    if (keyword) ++next;
    std::string_view suffix;
    if (s && s->ns && kind(next) == Tk::kw_operator) suffix = punctuator_operator_suffix(next + 1);
    if (!s || (suffix.empty() && !lookup_id_ahead(next))) {
      s_->diag(begin, Diag::not_a_namespace, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
      while (is(Tk::identifier) || is(Tk::coloncolon)) bump();
      return {NodeId::none, begin, pos()};
    }
    for (u32 j = 0; j < next; ++j) bump();
    u32 name_at = pos();
    bool operator_name = !suffix.empty() || literal_operator_ahead();
    auto name = expression_id(suffix);
    if (!name) return {NodeId::none, begin, pos()};
    NameId n = *name;
    auto scope_expr = [&] {
      // [expr.unary.op]: `&C::m` can form a pointer to member. Parentheses or postfix
      // operations make the operand something other than the qualified-id itself.
      bool postfix = is(Tk::l_paren) || is(Tk::l_square) || is(Tk::period) || is(Tk::arrow) ||
                     is(Tk::plusplus) || is(Tk::minusminus);
      return s_->scope_expr(*s, n, begin, name_at, pos(), postfix ? std::nullopt : address);
    };
    if (operator_name) {
      if (is(Tk::less) && s_->scope_names_template(*s, n)) {
        s_->diag(name_at, Diag::not_supported, Tk::kw_operator);
        return {NodeId::none, begin, pos()};
      }
      return scope_expr();
    }
    // `N::T(args)` or `N::T{…}` ([expr.type.conv]): a qualified type name, unless a function or
    // variable of the name hides it.
    if ((is(Tk::l_paren) || is(Tk::l_brace)) && !s_->scope_names_object(*s, n))
      if (auto t = s_->scope_type(*s, n)) {
        if (substituting_partial_arguments) s_->type_member_access(*s, n, name_at);
        return type_conversion(*t, begin);
      }
    if (is(Tk::less)) {
      // `N::v<args>`: a variable template of the namespace ([temp.names]/3).
      if (s->ns)
        if (auto v = s_->namespace_template(*s->ns, n, ast::DeclKind::variable)) {
          auto args = template_argument_list();
          if (!args) return {NodeId::none, begin, pos()};
          if (std::ranges::any_of(*args, [&](const ast::TemplateArg& a) { return s_->dependent(a); }))
            return s_->dependent_variable_template_id(*v, *args, begin, pos());
          return s_->variable_template_id(*v, *args, begin, pos());
        }
      // `S::f<args>`: a member template of class S or a function template of namespace S, with
      // explicit arguments ([temp.arg.explicit]).
      if (keyword || s_->scope_names_template(*s, n)) {
        auto args = template_argument_list();
        if (!args) return {NodeId::none, begin, pos()};
        Ex r = scope_expr();
        if (r.ok() && !s_->dependent(r) && std::ranges::any_of(*args, [&](const ast::TemplateArg& a) { return s_->dependent(a); }))
          return s_->dependent_node({}, begin, pos());
        s_->explicit_template_args(r, std::move(*args));
        return r;
      }
    }
    return scope_expr();
  }

  // ------------------------------------------------------------ [stmt]

  Ex compound_statement(bool barrier = false) {
    s_->push_scope();
    if (barrier) s_->jump_barrier();
    Ex body = compound_in_scope();
    s_->pop_scope();
    return body;
  }

  Ex compound_in_scope() {
    u32 begin = pos();
    if (!expect(Tk::l_brace)) return {NodeId::none, begin, begin};
    std::vector<Ex> stmts;
    while (!is(Tk::r_brace) && !at_end()) {
      u32 before = i_;
      stmts.push_back(statement());
      if (i_ == before) bump();  // no progress: drop the token
    }
    expect(Tk::r_brace);
    std::vector<ast::Child> cs;
    for (const Ex& st : stmts)
      if (st.ok()) cs.push_back(sema::Sema::at(st, begin));
    return s_->node(ast::Compound{s_->builder().list(cs)}, begin, pos());
  }


  // [except.pre], [except.handle]: each handler declaration shares its compound's scope.
  Ex try_statement() {
    u32 begin = pos();
    bump();
    Ex body = compound_statement(/*barrier=*/true);
    if (!expect(Tk::kw_catch)) return {NodeId::none, begin, pos()};
    std::vector<ast::Child> handlers;
    std::optional<u32> catch_all_at;
    do {
      u32 at = pos() - 1;
      if (catch_all_at) {
        s_->diag(*catch_all_at, Diag::catch_all_not_last);
        catch_all_at.reset();
      }
      if (!expect(Tk::l_paren)) break;
      s_->push_scope();
      s_->jump_barrier();
      const QualType placeholder = s_->builtin(Builtin::void_);
      QualType type = placeholder;
      auto kind = ast::HandlerKind::invalid;
      u32 local = ~0u;
      if (accept(Tk::ellipsis)) {
        kind = ast::HandlerKind::catch_all;
        catch_all_at = at;
      } else {
        u32 declaration_at = pos();
        DeclSpec spec = decl_specifiers(/*in_type_id=*/true);
        Declarator d = declarator(spec.type, /*abstract=*/true, /*typedef_=*/false, /*conversion=*/false,
                                  NamedExceptionSpec::ordinary, /*defer_ref_check=*/false,
                                  /*reject_parameter_defaults=*/true);
        if (spec.ok && d.ok) {
          type = d.type;
          if (s_->types().is_array(type)) type = s_->types().pointer(s_->types().rec(type).inner);
          if (s_->types().is_function(type) && !s_->types().is_cvref_function(type))
            type = s_->types().pointer(type);
          if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_typedef ||
              spec.is_constexpr || spec.is_consteval || spec.is_constinit) {
            s_->diag(declaration_at, Diag::invalid_exception_declaration);
          } else if (s_->types().is_cvref_function(type)) {
            s_->diag(declaration_at, Diag::indirection_to_qualified_function, type);
          } else if (s_->check_handler_type(type, declaration_at)) {
            kind = ast::HandlerKind::typed;
          }
          if (d.named) {
            local = s_->declare_local(d.name, type, 0, d.name_at);
            s_->set_initialized(local);
          }
        }
      }
      if (!expect(Tk::r_paren)) kind = ast::HandlerKind::invalid;
      Ex handler_body = compound_in_scope();
      s_->pop_scope();
      if (kind == ast::HandlerKind::invalid) type = placeholder;
      Ex handler = s_->node(ast::Handler{kind, type, local, sema::Sema::at(handler_body, at)}, at, pos());
      handlers.push_back(sema::Sema::at(handler, begin));
    } while (accept(Tk::kw_catch));
    return s_->node(ast::TryBlock{sema::Sema::at(body, begin), s_->builder().list(handlers)}, begin, pos());
  }

  // lambda-expression ([expr.prim.lambda]): its introducer and declarator, read in the enclosing
  // context, then its compound-statement as the body of the closure's function call operator.
  Ex lambda_expression() {
    using K = sema::Sema::CaptureSpec::Kind;
    sema::Sema::LambdaSpec spec;
    spec.begin = pos();
    bump();
    bool intro = true;  // still in the lambda-introducer, whose `]` recovery skips first
    bool head = false;  // a generic lambda's template-head was pushed onto the template scopes
    auto fail = [&] {
      if (head) s_->template_scopes.pop_back();
      head = false;
      if (intro) {
        for (int depth = 0; !at_end(); bump()) {
          if (is(Tk::l_square)) ++depth;
          if (is(Tk::r_square) && depth-- == 0) {
            bump();
            break;
          }
          if (is(Tk::semi) || is(Tk::l_brace)) break;
        }
      }
      skip_to_brace_end_of_lambda();
      return Ex{NodeId::none, spec.begin, pos()};
    };
    // lambda-capture ([expr.prim.lambda.capture]): a capture-default, then captures.
    if (!is(Tk::r_square)) spec.has_capture = true;
    if ((is(Tk::equal) || is(Tk::amp)) && (kind(1) == Tk::comma || kind(1) == Tk::r_square)) {
      spec.default_capture = is(Tk::equal) ? 1 : 2;
      bump();
      if (is(Tk::comma)) bump();
    }
    while (!is(Tk::r_square) && !at_end()) {
      sema::Sema::CaptureSpec c;
      c.at = pos();
      if (is(Tk::kw_this)) {
        c.kind = K::this_;
        c.name = s_->world().ins.names.intern("this");
        bump();
      } else if (is(Tk::star) && kind(1) == Tk::kw_this) {
        c.kind = K::star_this;
        c.name = s_->world().ins.names.intern("this");
        bump();
        bump();
      } else {
        bool ref = accept(Tk::amp);
        if (!is(Tk::identifier)) {
          s_->diag(pos(), is(Tk::ellipsis) ? Diag::not_supported : Diag::expected_unqualified_id, kind());
          return fail();
        }
        c.at = pos();
        c.name = name_of(tok());
        bump();
        if (is(Tk::ellipsis) && (kind(1) == Tk::comma || kind(1) == Tk::r_square)) {
          // A pack capture `xs...` ([expr.prim.lambda.capture]/17): each element of the pack.
          bump();
          c.kind = ref ? K::ref : K::copy;
          c.pack = true;
          spec.captures.push_back(std::move(c));
          if (!accept(Tk::comma)) break;
          continue;
        }
        if (is(Tk::ellipsis)) {  // an init-capture pack `...xs = e` (#350)
          s_->diag(pos(), Diag::not_supported, Tk::ellipsis);
          return fail();
        }
        if (is(Tk::equal) || is(Tk::l_brace) || is(Tk::l_paren)) {
          // An init-capture: `x = e`, `x{e}`, `x(e)` ([expr.prim.lambda.capture]/6).
          c.kind = ref ? K::init_ref : K::init_copy;
          if (accept(Tk::equal)) {
            c.init = assignment_expression();
          } else {
            bool brace = is(Tk::l_brace);
            bump();
            c.init = assignment_expression();
            if (!expect(brace ? Tk::r_brace : Tk::r_paren)) return fail();
          }
        } else {
          c.kind = ref ? K::ref : K::copy;
          // [expr.prim.lambda.capture]/2: with `&` a simple capture is not by reference, with `=`
          // not by copy.
          if ((spec.default_capture == 2 && ref) || (spec.default_capture == 1 && !ref)) {
            s_->diag(c.at, Diag::capture_redundant, c.name);
            return fail();
          }
          c.source = s_->capture_source(c.name);
          if (!c.source) {
            s_->diag(c.at, Diag::cannot_capture, c.name);
            return fail();
          }
          c.init = s_->captured(c.source->first, c.source->second, c.name, c.at, c.at + 1);
        }
      }
      spec.captures.push_back(std::move(c));
      if (!accept(Tk::comma)) break;
      if (is(Tk::r_square)) {  // a capture follows a comma ([expr.prim.lambda.capture]/1)
        s_->diag(pos(), Diag::expected_unqualified_id);
        return fail();
      }
    }
    if (!expect(Tk::r_square)) return fail();
    intro = false;
    // A template-parameter-list: a generic lambda ([expr.prim.lambda.general]/2).
    const u16 depth = u16(s_->template_scopes.size());
    if (is(Tk::less)) {
      head = true;
      if (!template_head(depth, false)) return fail();
      if (is(Tk::kw_requires)) {  // a requires-clause on a lambda (#281)
        s_->diag(pos(), Diag::not_supported, Tk::kw_requires);
        return fail();
      }
    }
    // lambda-declarator ([expr.prim.lambda.general]): parameters, specifiers, trailing return type;
    // without parameters, it may still have specifiers (C++23).
    if (is(Tk::l_paren) || is(Tk::kw_mutable) || is(Tk::kw_constexpr) || is(Tk::kw_noexcept) || is(Tk::arrow) ||
        is(Tk::kw_static) || is(Tk::kw_consteval)) {
      Suffix ps;
      if (accept(Tk::l_paren)) {
        bool ok = true;
        placeholder_params = true;
        ps = parameters(ok);
        placeholder_params = false;
        if (!ok) return fail();
      }
      for (const Param& p : ps.params) {
        QualType t = p.type;
        // A function parameter pack `auto... xs` or `Ts... xs` ([dcl.fct]/23): its pattern.
        const bool pack = s_->types().kind(t) == sema::TypeKind::pack_expansion;
        if (pack) t = s_->types().rec(t).inner;
        // A placeholder parameter declares an invented type parameter of the operator's
        // template-head, appended to it ([expr.prim.lambda.closure]/7, [dcl.fct]/22); a pack's
        // is a template parameter pack.
        if (s_->types().has_placeholder(t)) {
          if (!head) {
            s_->template_scopes.emplace_back();
            head = true;
          }
          ast::TemplateParamSema inv{};
          inv.name = s_->invented_name();
          inv.kind = ast::TemplateArgKind::type;
          inv.depth = depth;
          inv.index = u16(s_->template_scopes.back().size());
          inv.pack = u8(pack);
          inv.type = s_->types().template_param(depth, inv.index, pack);
          inv.default_ = {ast::TemplateArgKind::type, s_->builtin(Builtin::void_), sema::ConstId::none};
          inv.params = s_->world().ins.intern_seq(std::span<const ast::TemplateParamSema>{});
          s_->template_scopes.back().push_back(inv);
          sema::Bindings invent;
          invent.placeholder = inv.type;
          auto r = s_->templates().substitute(t, invent);
          if (!r || s_->types().rec(t).decltype_auto) {
            s_->diag(p.at, Diag::auto_not_allowed);
            return fail();
          }
          t = *r;
        }
        if (pack) t = s_->types().pack_expansion(t);
        spec.params.push_back({p.name, t, p.at});
      }
      spec.variadic = ps.variadic;
      bool constexpr_ = false;
      for (;;) {
        // [expr.prim.lambda.general]/3: each lambda-specifier at most once.
        if ((is(Tk::kw_mutable) && spec.mutable_) || (is(Tk::kw_constexpr) && constexpr_)) {
          s_->diag(pos(), Diag::duplicate_specifier, kind());
          return fail();
        }
        if (accept(Tk::kw_mutable)) spec.mutable_ = true;
        else if (accept(Tk::kw_constexpr)) {
          constexpr_ = true;
        } else if (is(Tk::kw_consteval) || is(Tk::kw_static)) {
          s_->diag(pos(), Diag::not_supported, kind());
          return fail();
        } else break;
      }
      if (is(Tk::kw_noexcept)) {  // accepted, not part of the type yet (#219)
        bump();
        if (is(Tk::l_paren)) {
          bump();
          Ex e = constant_expression();
          if (!e.ok() || !expect(Tk::r_paren)) return fail();
        }
      }
      skip_attributes();
      if (accept(Tk::arrow)) {
        s_->push_scope();
        for (const sema::Sema::LambdaParam& p : spec.params)
          if (p.name != s_->no_name()) s_->declare_local(p.name, p.type, sema::local_flag::parameter, p.at, false);
        auto t = type_id(true);  // `-> auto&`: a placeholder the body deduces ([dcl.spec.auto.general]/3)
        s_->pop_scope();
        if (!t) return fail();
        spec.trailing = *t;
      }
    }
    if (!is(Tk::l_brace)) {
      s_->diag(pos(), Diag::expected_token, Tk::l_brace);
      return fail();
    }
    if (head) spec.head = s_->template_scopes.back();
    u32 begin = spec.begin;
    if (!s_->open_lambda(std::move(spec))) {
      if (head) s_->template_scopes.pop_back();
      skip_to_brace_end();
      return Ex{NodeId::none, begin, pos()};
    }
    u32 body_begin = pos();
    int saved = std::exchange(angle_, 0);
    Ex body;
    {
      sema::Sema::ImmediateReferenceScope references(*s_, /*allowed=*/false);
      // A non-generic lambda's body is a separate evaluation context from its captures.
      const u32 enclosing_unevaluated = s_->unevaluated;
      if (!head) s_->unevaluated = 0;
      body = compound_statement();
      s_->unevaluated = enclosing_unevaluated;
    }
    angle_ = saved;
    Ex r = s_->close_lambda(body, body_begin, pos());
    if (head) s_->template_scopes.pop_back();
    return r;
  }
  // After an error in a lambda's introducer or declarator: past its body, if it has one.
  void skip_to_brace_end_of_lambda() {
    int depth = 0;
    while (!at_end()) {
      Tk k = kind();
      if (k == Tk::l_paren || k == Tk::l_square) ++depth;
      if ((k == Tk::r_paren || k == Tk::r_square) && --depth < 0) return;
      if (depth == 0 && (k == Tk::semi || k == Tk::comma)) return;
      if (k == Tk::l_brace) {
        skip_to_brace_end();
        return;
      }
      bump();
    }
  }

  // A substatement that is not a compound-statement has its own block scope ([stmt.pre]).
  Ex substatement(bool barrier = false) {
    if (is(Tk::l_brace)) return compound_statement(barrier);
    s_->push_scope();
    if (barrier) s_->jump_barrier();
    Ex s = statement();
    s_->pop_scope();
    return s;
  }

  Ex statement() {
    deep_enough();  // blocks and the statements they hold nest through here
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
      if (s_->loop_depth == 0 && (!brk || s_->switches.empty())) {
        s_->diag(begin, brk ? Diag::break_outside_loop : Diag::continue_outside_loop);
        return {NodeId::none, begin, pos()};
      }
      if (brk) return s_->node(ast::Break{}, begin, pos());
      return s_->node(ast::Continue{}, begin, pos());
    }
    case Tk::kw_return: return return_statement();
    case Tk::kw_switch: return switch_statement();
    case Tk::kw_case:
    case Tk::kw_default: return case_label();
    case Tk::kw_goto: return goto_statement();
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
        if (!s || !lookup_id_ahead(next)) {
          s_->diag(pos(), Diag::not_a_namespace, is(Tk::identifier) ? name_of(tok()) : s_->no_name());
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        for (u32 j = 0; j < next; ++j) bump();
        u32 at = pos();
        if (auto n = lookup_id()) s_->using_declaration(*s, *n, at);
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
    case Tk::kw_try: return try_statement();
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
      // [stmt.label]: an identifier label.
      NameId n = name_of(tok());
      bump();
      bump();
      s_->define_label(n, begin);
      Ex st = labeled_statement();
      return s_->node(ast::Label{n, sema::Sema::at(st, begin)}, begin, pos());
    }
    if (starts_declaration(true)) return declaration_statement();
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

  // The statement a label labels ([stmt.label]); none when the label ends a compound statement
  // (C++23).
  Ex labeled_statement() { return is(Tk::r_brace) ? Ex{} : statement(); }

  // `case constant-expression :` and `default :` ([stmt.label], [stmt.switch]). The value is a
  // converted constant expression of the condition's promoted type ([stmt.switch]/2).
  Ex case_label() {
    u32 begin = pos();
    bool dflt = is(Tk::kw_default);
    bump();
    sema::Sema::SwitchScope* sw = s_->switches.empty() ? nullptr : &s_->switches.back();
    if (!sw) s_->diag(begin, Diag::case_outside_switch, dflt ? Tk::kw_default : Tk::kw_case);
    ast::ConstId value = ast::ConstId::none;
    if (!dflt) {
      Ex e = conditional_expression();
      if (sw && !sw->dependent && e.ok() && !s_->dependent(e)) {
        Ex from = s_->rvalue(e);
        Ex c = s_->convert(e, sw->type, e.begin);
        if (c.ok() && s_->narrows(from, sw->type)) {
          s_->diag(e.begin, Diag::narrowing, from.type(), sw->type);
        } else if (c.ok()) {
          // The case immediate uses the enumeration's underlying integer constant.
          if (s_->types().is_enum(sw->type))
            c = s_->convert_node(s_->rvalue(c), ast::ConversionKind::integral, s_->underlying_of(sw->type));
          sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(c);
          if (v.value) value = *v.value;
          else if (!v.dependent) s_->diag(e.begin, Diag::not_constant, v.reason);
        }
      }
      if (value != ast::ConstId::none) {
        if (std::ranges::contains(sw->values, value)) s_->diag(begin, Diag::duplicate_case);
        sw->values.push_back(value);
      }
    } else if (sw) {
      if (sw->has_default) s_->diag(begin, Diag::duplicate_default);
      sw->has_default = true;
    }
    if (!expect(Tk::colon)) {
      skip_statement();
      return {NodeId::none, begin, pos()};
    }
    // [stmt.dcl]/2: the switch jumps here, so no initialization may be bypassed.
    if (sw && sema::Sema::bypasses(sw->initialized, s_->initialized_in_scope())) s_->diag(begin, Diag::jump_past_init);
    Ex st = labeled_statement();
    if (!sw) return st;
    if (dflt) return s_->node(ast::Default{sema::Sema::at(st, begin)}, begin, pos());
    return s_->node(ast::Case{value, sema::Sema::at(st, begin)}, begin, pos());
  }

  // A substatement of `if constexpr` or `if consteval`: a goto, case or default label outside it
  // may not jump into it ([stmt.if]/2, /4).
  Ex guarded_substatement() {
    return substatement(/*barrier=*/true);
  }

  // [stmt.goto]
  Ex goto_statement() {
    u32 begin = pos();
    bump();
    if (!is(Tk::identifier)) {
      s_->diag(pos(), Diag::expected_unqualified_id);
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    NameId n = name_of(tok());
    bump();
    if (!expect(Tk::semi)) skip_to_semi();
    s_->use_label(n, begin);
    return s_->node(ast::Goto{n}, begin, pos());
  }

  // [stmt.switch]: `switch ( init-statement_opt condition ) statement`. The init-statement's and
  // the condition's names are in scope in the statement, and not redeclared in its outermost
  // block ([basic.scope.block]/2).
  Ex switch_statement() {
    u32 begin = pos();
    bump();
    s_->push_scope();
    s_->guard_scope();
    Condition c = condition(true, CondKind::switch_);
    sema::Sema::SwitchScope sw;
    sw.dependent = !c.value.ok() || s_->dependent(c.value);
    if (!sw.dependent) sw.type = c.value.type();
    sw.initialized = s_->initialized_in_scope();
    s_->switches.push_back(std::move(sw));
    Ex body = substatement();
    s_->switches.pop_back();
    s_->pop_scope();
    return s_->node(ast::Switch{sema::Sema::at(c.init, begin), sema::Sema::at(c.node, begin), sema::Sema::at(body, begin)},
                    begin, pos());
  }

  // [stmt.if]
  Ex if_statement() {
    u32 begin = pos();
    bump();
    if (is(Tk::kw_consteval) || (is(Tk::exclaim) && kind(1) == Tk::kw_consteval)) return if_consteval(begin);
    if (is(Tk::kw_constexpr)) return if_constexpr(begin);
    if (is(Tk::exclaim)) {
      s_->diag(pos(), Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    }
    s_->push_scope();  // [stmt.pre]: the condition's scope covers both substatements
    s_->guard_scope();
    Condition c = condition(true);
    Ex then_ = substatement();
    Ex else_;
    if (accept(Tk::kw_else)) else_ = substatement();
    s_->pop_scope();
    return s_->node(ast::If{sema::Sema::at(c.init, begin), sema::Sema::at(c.node, begin), sema::Sema::at(then_, begin),
                            sema::Sema::at(else_, begin)},
                    begin, pos());
  }

  // `if constexpr ( condition ) statement else statement` ([stmt.if]/2): the condition is a
  // constant expression contextually converted to bool. While it is value-dependent (a template's
  // definition) both branches are analysed; otherwise only the one it selects is the statement.
  // In an instantiation the other, discarded, branch is not instantiated: its tokens are skipped.
  // Outside templates it is checked, but neither emits code nor demands anything.
  Ex if_constexpr(u32 begin) {
    bump();  // constexpr
    s_->push_scope();
    s_->guard_scope();
    if (!expect(Tk::l_paren)) {
      s_->pop_scope();
      skip_statement();
      return {NodeId::none, begin, pos()};
    }
    Condition c;
    if (init_statement_ahead()) c.init = init_statement();
    std::size_t diags = s_->diag_count();
    c.node = condition_operand(CondKind::boolean, c.value);
    expect(Tk::r_paren);
    std::optional<bool> take;
    if (c.node.ok() && c.value.ok() && !s_->dependent(c.value) && !s_->has_error_since(diags)) {
      sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(c.value);
      if (v.value) take = s_->world().values.as_signed(*v.value) != 0;
      else if (!v.dependent) s_->diag(c.value.begin, Diag::not_constant, v.reason);
    }
    if (!take) {  // dependent, or in error: both branches, as an ordinary `if`
      // Which branch returns is known only to an instantiation: neither deduces a placeholder
      // return type here ([dcl.spec.auto.general]/12).
      ++s_->no_return_deduction;
      Ex then_ = guarded_substatement();
      Ex else_;
      if (accept(Tk::kw_else)) else_ = guarded_substatement();
      --s_->no_return_deduction;
      s_->pop_scope();
      return s_->node(ast::If{sema::Sema::at(c.init, begin), sema::Sema::at(c.node, begin), sema::Sema::at(then_, begin),
                              sema::Sema::at(else_, begin)},
                      begin, pos());
    }
    auto discarded = [&] {
      if (s_->instantiating()) return skip_substatement();
      std::size_t n = s_->demands.size();
      // Its return statements deduce nothing: a discarded statement's are not used ([stmt.if]/2).
      ++s_->no_return_deduction;
      guarded_substatement();
      --s_->no_return_deduction;
      s_->demands.resize(n);
    };
    Ex kept;
    if (*take) kept = guarded_substatement();
    else discarded();
    if (accept(Tk::kw_else)) {
      if (*take) discarded();
      else kept = guarded_substatement();
    }
    s_->pop_scope();
    // Initialization precedes the selected arm, and its objects remain in scope through the arm
    // ([stmt.if], [stmt.pre]). This also retains them when the conditional has no selected arm.
    std::vector<ast::Child> stmts;
    if (c.init.ok()) stmts.push_back(sema::Sema::at(c.init, begin));
    if (auto declaration = s_->as<ast::CondDecl>(c.node)) {
      Ex decl = s_->node(ast::DeclStmt{s_->builder().list(std::span(&declaration->var, 1))}, c.node.begin, c.node.end);
      stmts.push_back(sema::Sema::at(decl, begin));
    }
    if (stmts.empty() && kept.ok()) return kept;
    if (kept.ok()) stmts.push_back(sema::Sema::at(kept, begin));
    return s_->node(ast::Compound{s_->builder().list(stmts)}, begin, pos());
  }
  // Past a statement, unanalysed: a discarded statement of an instantiation ([stmt.if]/2).
  void skip_substatement() {
    auto parens = [&] {
      if (!is(Tk::l_paren)) return;
      int depth = 0;
      do {
        if (is(Tk::l_paren)) ++depth;
        if (is(Tk::r_paren)) --depth;
        bump();
      } while (depth > 0 && !at_end());
    };
    if (u32 n = past_attributes(0); n != 0)
      for (u32 j = 0; j < n; ++j) bump();
    // A labeled statement: past its labels ([stmt.label]).
    if ((is(Tk::identifier) || is(Tk::kw_default)) && kind(1) == Tk::colon) {
      bump();
      bump();
      if (!is(Tk::r_brace)) skip_substatement();
      return;
    }
    if (is(Tk::kw_case)) {
      int depth = 0;
      while (!at_end() && !(depth == 0 && is(Tk::colon))) {
        if (is(Tk::l_paren) || is(Tk::l_square)) ++depth;
        if (is(Tk::r_paren) || is(Tk::r_square)) --depth;
        bump();
      }
      accept(Tk::colon);
      if (!is(Tk::r_brace)) skip_substatement();
      return;
    }
    switch (kind()) {
    case Tk::l_brace: skip_statement(); return;
    case Tk::kw_try:
      bump();
      if (is(Tk::l_brace)) skip_statement();
      while (accept(Tk::kw_catch)) {
        parens();
        if (is(Tk::l_brace)) skip_statement();
      }
      return;
    case Tk::kw_if:
      bump();
      while (is(Tk::kw_constexpr) || is(Tk::exclaim) || is(Tk::kw_consteval)) bump();
      parens();
      skip_substatement();
      if (accept(Tk::kw_else)) skip_substatement();
      return;
    case Tk::kw_for:
    case Tk::kw_while:
    case Tk::kw_switch:
      bump();
      parens();
      skip_substatement();
      return;
    case Tk::kw_do:
      bump();
      skip_substatement();
      accept(Tk::kw_while);
      parens();
      accept(Tk::semi);
      return;
    default: skip_to_semi(); return;
    }
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
      Ex b = guarded_substatement();
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

  // `static_assert ( constant-expression , string-literal opt ) ;` ([dcl.pre]). The condition,
  // contextually converted to bool, must be a constant expression. A false value has no effect
  // in a template definition (P2593).
  Ex static_assert_declaration() {
    u32 begin = pos();
    bump();
    if (!expect(Tk::l_paren)) {
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    sema::Sema::ImmediateReferenceScope references(*s_, /*allowed=*/true);
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
    if (v.dependent && (s_->templated() || questions().instantiation_gave_up())) {
      // The instantiation checks a dependent condition ([dcl.pre]). Exceeding the nesting limit
      // has already produced a diagnostic.
    } else if (v.dependent) {
      s_->diag(e.begin, Diag::not_constant, sema::reason_unsupported);  // nothing outside a template depends
    } else if (!v.value) {
      s_->diag(e.begin, Diag::not_constant, v.reason);
    } else if (!s_->in_template_definition() && s_->world().values.rec(*v.value).bits == 0) {
      if (message == sema::ConstId::none) s_->diag(begin, Diag::static_assert_failed);
      else s_->diag(begin, Diag::static_assert_failed_message, message);
    }
    return s_->node(ast::StaticAssert{sema::Sema::at(cond, begin), message}, begin, pos());
  }

  // A call to a consteval function outside an immediate function context is an immediate
  // invocation: it must be a constant expression, and it is replaced by its value ([expr.const]).
  Ex immediate(Ex call) {
    auto c = s_->as<ast::Call>(call);
    if (!c || c->function == ast::EntityId::none || !s_->is_consteval(c->function) || s_->immediate_context || s_->unevaluated)
      return call;
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

  // A condition ([stmt.pre]) and the init-statement before it, if any: `node` is what the
  // statement holds (the converted value, or a `CondDecl`), `value` the converted value.
  enum class CondKind { boolean, switch_ };
  struct Condition {
    Ex init;
    Ex node;
    Ex value;
  };

  // `( init-statement_opt condition )`, the init-statement only where `with_init` allows one (`if`,
  // `switch`). The value is contextually converted to bool, or for a switch promoted
  // ([stmt.switch]/5).
  Condition condition(bool with_init, CondKind k = CondKind::boolean) {
    Condition c;
    if (!expect(Tk::l_paren)) return c;
    if (with_init && init_statement_ahead()) c.init = init_statement();
    c.node = condition_operand(k, c.value);
    expect(Tk::r_paren);
    return c;
  }

  // The condition itself: an expression, or a declaration of a variable with an initializer
  // ([stmt.pre]/4–/6), whose value is the variable's.
  Ex condition_operand(CondKind k, Ex& value) {
    auto converted = [&](Ex e, u32 at) { return k == CondKind::boolean ? s_->to_bool(e, at) : s_->switch_condition(e, at); };
    skip_attributes();  // of the declaration ([stmt.pre]/1)
    if (!condition_declaration_ahead()) {
      Ex e = expression();
      return value = converted(e, e.begin);
    }
    u32 begin = pos();
    DeclSpec spec = decl_specifiers();
    // [stmt.pre]/5: only type-specifiers and `constexpr`.
    if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_typedef || spec.is_consteval || spec.is_constinit) {
      s_->diag(spec.begin, Diag::condition_specifier,
               spec.is_static ? Tk::kw_static : spec.is_extern ? Tk::kw_extern : spec.is_inline ? Tk::kw_inline
               : spec.is_typedef ? Tk::kw_typedef : spec.is_consteval ? Tk::kw_consteval : Tk::kw_constinit);
      skip_param();
      return {NodeId::none, begin, pos()};
    }
    Declarator d = declarator(spec.type, false);
    if (!d.ok || !spec.ok) {
      skip_param();
      return {NodeId::none, begin, pos()};
    }
    if (s_->types().is_function(d.type) || s_->types().is_array(d.type) || d.unknown_bound) {
      s_->diag(d.name_at, Diag::condition_declarator);
      skip_param();
      return {NodeId::none, begin, pos()};
    }
    sema::Sema::ImmediateReferenceScope references(*s_, s_->constant_reference_context || spec.is_constexpr);
    if (spec.is_constexpr) d.type = s_->types().add_cv(d.type, sema::Cv::const_);
    u32 local = s_->declare_local(d.name, d.type, 0, d.name_at);
    Ex init;
    bool placeholder = s_->types().has_placeholder(d.type);
    if (placeholder && (is(Tk::equal) || is(Tk::l_brace))) {
      std::optional<QualType> replaced;
      init = placeholder_variable(d, local, spec.is_constexpr, replaced);
      if (!init.ok() && s_->types().has_placeholder(d.type)) s_->set_local_type(local, s_->builtin(Builtin::dependent));
    } else if (is(Tk::l_brace) || (is(Tk::equal) && kind(1) == Tk::l_brace)) {
      const auto form = accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
      auto b = braced_init_list();
      if (s_->types().is_reference(d.type) && !s_->types().is_class(s_->types().referee(d.type)) &&
          b->clauses.size() == 1 && !b->clauses[0].list && !b->clauses[0].designated)
        init = s_->bind_reference(b->clauses[0].expr, d.type, b->clauses[0].begin, true);
      else
        init = s_->list_init(d.type, *b, form, /*extend_reference=*/true);
    } else if (accept(Tk::equal)) {
      Ex e = assignment_expression();
      init = s_->types().is_reference(d.type) ? s_->bind_reference(e, d.type, e.begin, true) : s_->convert(e, d.type, e.begin);
    } else {
      expect(Tk::equal);  // [stmt.pre]: a declaration in a condition has an initializer
      skip_param();
      return {NodeId::none, begin, pos()};
    }
    if (!placeholder && !s_->types().is_reference(d.type)) constant_local(local, d.type, spec.is_constexpr, init);
    ast::EntityId dtor = s_->types().is_reference(d.type) ? ast::EntityId::none : s_->destructor_of(d.type);
    record_local_constant_initializer(local, begin, init);
    Ex var = s_->node(ast::LocalVar{local, sema::Sema::at(init, begin), dtor}, begin, pos());
    s_->set_local_decl(local, var.id);
    if (init.ok() || dtor != ast::EntityId::none) s_->set_initialized(local);
    value = converted(s_->local_ref(local, d.name_at, pos()), d.name_at);
    if (!init.ok() || !value.ok()) return {NodeId::none, begin, pos()};
    return s_->node(ast::CondDecl{sema::Sema::at(var, begin), sema::Sema::at(value, begin)}, begin, pos());
  }

  // Whether the condition declares a variable: what can be a declaration ([stmt.ambig]) and has
  // the initializer one needs, `=` or `{` outside brackets, so `T(1)` is an expression.
  bool condition_declaration_ahead() {
    if (!starts_declaration(true)) return false;
    int depth = 0;
    for (u32 j = 0;; ++j) {
      switch (kind(j)) {
      case Tk::eof: case Tk::semi: return false;
      case Tk::equal: if (depth == 0) return true; break;
      case Tk::l_brace: if (depth == 0) return true; ++depth; break;
      case Tk::l_paren: case Tk::l_square: ++depth; break;
      case Tk::r_paren: case Tk::r_square: case Tk::r_brace:
        // `T x)`: a declaration missing its initializer, which `condition_operand` reports.
        if (depth-- == 0) return j >= 2 && kind(j - 1) == Tk::identifier && kind(j - 2) != Tk::l_paren;
        break;
      default: break;
      }
    }
  }

  // `using identifier attribute-specifier-seq_opt =`: an alias-declaration ([dcl.pre]).
  bool alias_declaration_ahead() { return kind(1) == Tk::identifier && kind(past_attributes(2)) == Tk::equal; }

  // Whether an init-statement comes before the condition: a `;` before the closing `)`.
  bool init_statement_ahead() const {
    int depth = 0;
    for (u32 j = 0;; ++j) {
      switch (kind(j)) {
      case Tk::eof: return false;
      case Tk::l_paren: case Tk::l_square: case Tk::l_brace: ++depth; break;
      case Tk::r_paren: case Tk::r_square: case Tk::r_brace:
        if (depth-- == 0) return false;
        break;
      case Tk::semi:
        if (depth == 0) return true;
        break;
      default: break;
      }
    }
  }

  // [stmt.pre]: an init-statement is an expression-statement, a simple-declaration or (C++23) an
  // alias-declaration, each with its `;`.
  Ex init_statement() {
    u32 begin = pos();
    if (is(Tk::kw_using) && !alias_declaration_ahead()) {
      s_->diag(begin, Diag::expected_expression);  // [stmt.pre]: not a using-directive or -declaration
      skip_to_semi();
      accept(Tk::semi);
      return {NodeId::none, begin, pos()};
    }
    if (is(Tk::kw_using)) return statement();
    skip_attributes();  // of the simple-declaration ([dcl.pre]/5)
    if (starts_declaration(true)) return declaration_statement();
    Ex e = is(Tk::semi) ? Ex{} : s_->discarded(expression());
    if (!expect(Tk::semi)) skip_to_semi();
    return s_->node(ast::ExprStmt{sema::Sema::at(e, begin)}, begin, pos());
  }

  void record_local_constant_initializer(u32 local, u32 declaration, Ex init) {
    if (!s_->needs_immediate_reference_check() || !init.ok()) return;
    QualType target = s_->local(local).type;
    if (s_->types().is_reference(target) && s_->types().is_function(s_->types().referee(target))) {
      auto v = sema::Evaluator(*s_, questions()).evaluate_reference_initializer(init);
      if (v.value) s_->permit_constant_initializer(declaration, target, *v.value);
    } else if (auto value = s_->local_values()[local]) {
      s_->permit_constant_initializer(declaration, target, *value);
    } else if (sema::Types::is_const(target) && !sema::Types::is_volatile(target) &&
               (s_->types().is_integral(target) || s_->types().is_enum(target))) {
      auto v = sema::Evaluator(*s_, questions()).evaluate(init);
      if (v.value) s_->permit_constant_initializer(declaration, target, *v.value);
    }
  }

  // A local usable in constant expressions ([expr.const]): constexpr, or a const integral one
  // with a constant initializer; a constexpr one must have one ([dcl.constexpr]).
  void constant_local(u32 local, QualType t, bool is_constexpr, Ex init) {
    bool usable_kind = sema::Types::is_const(t) && !sema::Types::is_volatile(t) && s_->types().is_integral(t);
    if (!init.ok() || !(is_constexpr || usable_kind)) return;
    sema::Evaluated v = sema::Evaluator(*s_, questions()).evaluate(init);
    if (v.value) s_->set_local_value(local, *v.value);
    else if (v.dependent) s_->set_local_value_dependent(local);
    else if (is_constexpr) s_->diag(init.begin, Diag::not_constant, v.reason);
  }

  // [stmt.while]
  Ex while_statement() {
    u32 begin = pos();
    bump();
    s_->push_scope();
    s_->guard_scope();
    Condition c = condition(false);
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    s_->pop_scope();
    return s_->node(ast::While{sema::Sema::at(c.node, begin), sema::Sema::at(body, begin)}, begin, pos());
  }

  // [stmt.do]
  Ex do_statement() {
    u32 begin = pos();
    bump();
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    expect(Tk::kw_while);
    Ex cond;
    if (expect(Tk::l_paren)) {  // [stmt.do]: an expression, never a declaration
      Ex e = expression();
      cond = s_->to_bool(e, e.begin);
      expect(Tk::r_paren);
    }
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
    if (auto r = range_for_ahead()) return range_for(begin, *r);
    Ex init;
    skip_attributes();  // of a simple-declaration ([dcl.pre]/5)
    if (is(Tk::kw_using)) {
      init = init_statement();  // an alias-declaration (C++23)
    } else if (starts_declaration(true)) {
      init = declaration_statement();
    } else {
      u32 b = pos();
      Ex e = is(Tk::semi) ? Ex{} : s_->discarded(expression());
      expect(Tk::semi);
      init = s_->node(ast::ExprStmt{sema::Sema::at(e, b)}, b, pos());
    }
    Ex cond, value;
    if (!is(Tk::semi)) cond = condition_operand(CondKind::boolean, value);
    expect(Tk::semi);
    Ex inc;
    if (!is(Tk::r_paren)) inc = s_->discarded(expression());  // a discarded-value expression ([stmt.for])
    expect(Tk::r_paren);
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    s_->pop_scope();
    return s_->node(ast::For{sema::Sema::at(init, begin), sema::Sema::at(cond, begin), sema::Sema::at(inc, begin),
                             sema::Sema::at(body, begin)},
                    begin, pos());
  }

  // Whether a range-based `for` follows the `(` ([stmt.ranged]): a `:` before the `)`, outside
  // brackets and not a `?:`'s, after at most one `;` (that of an init-statement); with whether
  // there is that init-statement.
  std::optional<bool> range_for_ahead() const {
    int depth = 0, conditionals = 0;
    bool init = false;
    for (u32 j = 0;; ++j) {
      switch (kind(j)) {
      case Tk::eof: return std::nullopt;
      case Tk::kw_enum: case Tk::kw_struct: case Tk::kw_class: case Tk::kw_union:
        // Skip a base-clause or enum-base directly after the type name. An elaborated
        // type followed by a declarator (`struct T& x : range`) still starts a range-for.
        if (depth == 0 && kind(j + 1) == Tk::identifier && kind(j + 2) == Tk::colon) j += 2;
        break;
      case Tk::l_paren: case Tk::l_square: case Tk::l_brace: ++depth; break;
      case Tk::r_paren: case Tk::r_square: case Tk::r_brace:
        if (depth-- == 0) return std::nullopt;
        break;
      case Tk::question: if (depth == 0) ++conditionals; break;
      case Tk::colon:
        if (depth == 0 && conditionals > 0) --conditionals;
        else if (depth == 0) return init;
        break;
      case Tk::semi:
        if (depth == 0) {
          if (init) return std::nullopt;
          init = true;
        }
        break;
      default: break;
      }
    }
  }

  // [stmt.ranged]: `for ( init-statement_opt for-range-declaration : for-range-initializer )
  // statement` is the loop the draft expands it to, built from the existing kinds:
  //   { init-statement  auto&& __range = initializer;  auto __begin = begin-expr;  auto __end = end-expr;
  //     for ( ; __begin != __end; ++__begin ) { for-range-declaration = *__begin; statement } }
  // where begin-expr is `__range` for an array, `__range.begin()` for a class whose member lookup
  // finds both `begin` and `end` (P0962), and `begin(__range)` otherwise (as `end`). The for-init
  // scope is open.
  Ex range_for(u32 begin, bool has_init) {
    std::vector<ast::Child> outer;
    if (has_init) {
      Ex init = init_statement();
      if (init.ok()) outer.push_back(sema::Sema::at(init, begin));
    }
    skip_attributes();
    u32 dbegin = pos();
    DeclSpec spec = decl_specifiers();
    // [stmt.pre]/5 (as for a condition): only type-specifiers and `constexpr`.
    if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_typedef || spec.is_consteval || spec.is_constinit) {
      s_->diag(spec.begin, Diag::condition_specifier,
               spec.is_static ? Tk::kw_static : spec.is_extern ? Tk::kw_extern : spec.is_inline ? Tk::kw_inline
               : spec.is_typedef ? Tk::kw_typedef : spec.is_consteval ? Tk::kw_consteval : Tk::kw_constinit);
    }
    Declarator d = declarator(spec.type, false);
    if (!expect(Tk::colon) || !d.ok || !spec.ok) {
      skip_rest();
      s_->pop_scope();
      return {NodeId::none, begin, pos()};
    }
    if (is(Tk::l_brace)) {  // a braced list deduces std::initializer_list (#329)
      s_->diag(pos(), Diag::not_supported, Tk::l_brace);
      skip_rest();
      s_->pop_scope();
      return {NodeId::none, begin, pos()};
    }
    Ex range = expression();
    expect(Tk::r_paren);
    sema::Types& types = s_->types();
    // In a template's definition, or after an error in the header: the body is analysed with the
    // variable's type as written (dependent if a placeholder), in the guarded scope; an
    // instantiation expands the loop.
    auto finish_dependent = [&](bool ok) -> Ex {
      s_->declare_local(d.name, types.has_placeholder(d.type) ? s_->builtin(Builtin::dependent) : d.type, 0, d.name_at);
      ++s_->loop_depth;
      Ex body = substatement();
      --s_->loop_depth;
      s_->pop_scope();
      if (!ok) return {NodeId::none, begin, pos()};
      if (body.ok()) outer.push_back(sema::Sema::at(body, begin));
      return s_->node(ast::Compound{s_->builder().list(outer)}, begin, pos());
    };
    if (!range.ok()) return finish_dependent(false);
    // If this becomes the nonmember branch, its synthesized calls use the instantiation
    // context as well as the lexical context ([temp.dep.candidate]).
    if (types.is_dependent(range.type())) s_->dependent_calls.push_back(begin);
    if (s_->dependent(range) || (types.is_dependent(d.type) && !types.has_placeholder(d.type))) return finish_dependent(true);
    // auto&& __range = initializer;
    auto hidden = [&](QualType declared, Ex e) -> std::optional<std::pair<u32, Ex>> {
      auto t = s_->deduce_placeholder(declared, e, e.begin);
      if (!t) return std::nullopt;
      u32 l = s_->declare_local(s_->no_name(), *t, 0, e.begin, false);
      Ex init = types.is_reference(*t) ? s_->bind_reference(e, *t, e.begin, true) : s_->convert(e, *t, e.begin);
      if (!init.ok()) return std::nullopt;
      ast::EntityId dtor = types.is_reference(*t) ? ast::EntityId::none : s_->destructor_of(*t);
      Ex var = s_->node(ast::LocalVar{l, sema::Sema::at(init, e.begin), dtor}, e.begin, e.end);
      s_->set_local_decl(l, var.id);
      return std::pair{l, var};
    };
    auto r = hidden(types.rvalue_ref(types.placeholder(false)), range);
    if (!r) return finish_dependent(false);
    auto ref = [&](u32 l) { return s_->local_ref(l, range.begin, range.end); };
    QualType ct = s_->local(r->first).type;
    ct = types.referee(ct);
    // begin-expr and end-expr ([stmt.ranged]/1).
    Ex b, e;
    NameId begin_name = s_->ins().names.intern("begin"), end_name = s_->ins().names.intern("end");
    u32 at = range.begin, to = range.end;
    if (types.is_array(ct)) {
      if (!s_->require_complete(ct, at)) return finish_dependent(false);
      b = s_->rvalue(ref(r->first));
      QualType size = s_->builtin(Builtin::unsigned_long);
      Ex n = s_->literal(size, s_->world().values.integer(size, types.rec(ct).bound), ast::ValueCategory::prvalue, at, to);
      e = s_->binary(sema::Sema::BinOp::add, s_->rvalue(ref(r->first)), n, at);
    } else if (types.is_class(ct) && s_->find_member(types.class_of(ct), begin_name) &&
               s_->find_member(types.class_of(ct), end_name)) {
      b = s_->call(s_->member(ref(r->first), false, begin_name, to, at, at), {}, to, to);
      e = s_->call(s_->member(ref(r->first), false, end_name, to, at, at), {}, to, to);
    } else {
      if (types.is_class(ct) && !s_->require_complete(ct, at)) return finish_dependent(false);
      b = s_->adl_call(begin_name, ref(r->first), at, to, begin);
      e = s_->adl_call(end_name, ref(r->first), at, to, begin);
    }
    auto bv = b.ok() ? hidden(types.placeholder(false), b) : std::nullopt;
    auto ev = e.ok() ? hidden(types.placeholder(false), e) : std::nullopt;
    if (!bv || !ev) return finish_dependent(false);
    ast::Child decls[] = {sema::Sema::at(r->second, begin), sema::Sema::at(bv->second, begin), sema::Sema::at(ev->second, begin)};
    outer.push_back(sema::Sema::at(s_->node(ast::DeclStmt{s_->builder().list(decls)}, at, to), begin));
    Ex cond = s_->to_bool(s_->binary(sema::Sema::BinOp::ne, ref(bv->first), ref(ev->first), at), at);
    Ex inc = s_->discarded(s_->unary(ast::UnaryOp::pre_inc, ref(bv->first), at, to, at));
    // { for-range-declaration = *__begin; statement }: declared in the guarded scope with the
    // init-statement's names, neither is redeclared in the statement's outermost block
    // ([basic.scope.block]/2).
    Ex deref = s_->unary(ast::UnaryOp::deref, ref(bv->first), at, to, at);
    if (spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);
    u32 local = s_->declare_local(d.name, d.type, 0, d.name_at);
    Ex init;
    if (deref.ok() && types.has_placeholder(d.type)) {
      if (auto t = s_->deduce_placeholder(d.type, deref, at)) {
        d.type = *t;
        s_->set_local_type(local, d.type);
      } else {
        deref = {};
        s_->set_local_type(local, s_->builtin(Builtin::dependent));  // its uses say nothing more
      }
    }
    if (deref.ok()) init = types.is_reference(d.type) ? s_->bind_reference(deref, d.type, at, true) : s_->convert(deref, d.type, at);
    if (!types.is_reference(d.type)) {
      constant_local(local, d.type, spec.is_constexpr, init);
    } else if (init.ok() && spec.is_constexpr) {
      // A built-in dereference reads the non-constexpr iterator even for a reference.
      // For a class iterator, check the direct call while full evaluation waits for #25.
      if (types.is_pointer(s_->local(bv->first).type)) constant_local(local, d.type, true, init);
      else sema::Evaluator(*s_, questions()).check_constexpr_call(deref);
    }
    ast::EntityId dtor = types.is_reference(d.type) ? ast::EntityId::none : s_->destructor_of(d.type);
    Ex var = s_->node(ast::LocalVar{local, sema::Sema::at(init, dbegin), dtor}, dbegin, d.name_at + 1);
    s_->set_local_decl(local, var.id);
    if (init.ok() || dtor != ast::EntityId::none) s_->set_initialized(local);
    ++s_->loop_depth;
    Ex body = substatement();
    --s_->loop_depth;
    s_->pop_scope();
    if (!cond.ok() || !inc.ok() || !init.ok()) return {NodeId::none, begin, pos()};
    ast::Child loop_var[] = {sema::Sema::at(var, dbegin)};
    Ex decl = s_->node(ast::DeclStmt{s_->builder().list(loop_var)}, dbegin, d.name_at + 1);
    ast::Child inner[] = {sema::Sema::at(decl, begin), sema::Sema::at(body, begin)};
    Ex block = s_->node(ast::Compound{s_->builder().list(std::span(inner, body.ok() ? 2 : 1))}, begin, pos());
    Ex loop = s_->node(ast::For{{}, sema::Sema::at(cond, begin), sema::Sema::at(inc, begin), sema::Sema::at(block, begin)}, begin, pos());
    outer.push_back(sema::Sema::at(loop, begin));
    return s_->node(ast::Compound{s_->builder().list(outer)}, begin, pos());
  }

  // [stmt.return]: the operand copy-initializes the result; a void function may return a void
  // expression.
  Ex return_statement() {
    u32 begin = pos();
    bump();
    QualType rt = s_->return_type();
    Ex value;
    // A declared return type with a placeholder is deduced from each return ([dcl.spec.auto.general]/3).
    if (s_->deduces_return()) {
      if (is(Tk::l_brace)) {  // [dcl.type.auto.deduct]/2: not from a braced-init-list
        auto b = braced_init_list();
        s_->diag(b->begin, Diag::auto_list, u64(b->clauses.size()));
        if (!expect(Tk::semi)) skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      std::optional<Ex> e;
      if (!is(Tk::semi)) {
        const auto operand = s_->checks_lambda_result_expression() ? std::optional(pos()) : std::nullopt;
        auto outer = std::exchange(lambda_result_operand_, operand);
        e = expression();
        lambda_result_operand_ = outer;
      }
      auto t = s_->deduce_return(e ? &*e : nullptr, e ? e->begin : begin);
      if (!t || (e && !e->ok())) {
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      rt = *t;
      if (e && s_->types().is_void(rt)) value = *e;
      else if (e && s_->types().is_reference(rt)) value = s_->bind_reference(*e, rt, e->begin);
      else if (e) value = s_->init_deduced_return(*e, rt);
      if (!expect(Tk::semi)) skip_to_semi();
      return s_->node(ast::Return{sema::Sema::at(value, begin)}, begin, pos());
    }
    if (!is(Tk::semi)) {
      if (is(Tk::l_brace)) {
        // `return {…};` copy-list-initializes the result ([stmt.return]/2).
        auto b = braced_init_list();
        if (s_->types().is_void(rt)) s_->diag(begin, Diag::return_value_in_void);
        else value = s_->list_init(rt, *b, sema::Construction::copy_list);
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

  // A block-scope variable's node: a `LocalVar`, or for a static one a `StaticVar`, an entity of its
  // own initialized the first time control passes its declaration ([stmt.dcl]/3), with the
  // constant it is initialized with when its initializer is a constant expression
  // ([basic.start.static]).
  Ex variable_node(const DeclSpec& spec, u32 local, Ex init, ast::EntityId dtor, u32 dbegin) {
    record_local_constant_initializer(local, dbegin, init);
    if (!spec.is_static) return s_->node(ast::LocalVar{local, sema::Sema::at(init, dbegin), dtor}, dbegin, pos());
    if (s_->types().is_reference(s_->local(local).type)) {
      s_->diag(dbegin, Diag::not_supported, Tk::kw_static);  // a static reference (#367)
      return {NodeId::none, dbegin, pos()};
    }
    s_->static_local(local);
    ast::ConstId value = ast::ConstId::none;
    if (init.ok() && !s_->dependent(init) && !s_->types().is_dependent(s_->local(local).type)) {
      sema::Evaluator evaluator(*s_, questions());
      sema::Evaluated v = s_->as<ast::InitList>(init) ? evaluator.evaluate_initializer(init) : evaluator.evaluate(init);
      if (v.value) value = *v.value;
      else if (spec.is_constinit && !v.dependent) s_->diag(init.begin, Diag::not_constant, v.reason);
    }
    s_->permit_constant_initializer(dbegin, s_->local(local).type, value);
    s_->static_value(local, value == ast::ConstId::none ? std::nullopt : std::optional(value));
    return s_->node(ast::StaticVar{local, sema::Sema::at(init, dbegin), value, dtor}, dbegin, pos());
  }

  // A block-scope structured binding ([dcl.pre], [dcl.struct.bind]).
  Ex structured_binding(const DeclSpec& spec, u32 begin) {
    auto& types = s_->types();
    QualType declared = spec.type;
    u32 at = pos();
    std::vector<std::pair<u32, u32>> names;  // local and token offset
    auto fail = [&](bool recover = true) {
      for (auto [local, name_at] : names) s_->set_local_type(local, s_->builtin(Builtin::dependent));
      if (recover) skip_to_semi();
      return Ex{NodeId::none, begin, pos()};
    };
    if (!spec.ok) return fail();
    if (types.kind(declared) != sema::TypeKind::placeholder || types.rec(declared).decltype_auto) {
      s_->diag(at, Diag::binding_requires_auto);
      return fail();
    }
    if (spec.is_static || spec.is_extern || spec.is_inline || spec.is_constexpr || spec.is_constinit ||
        spec.is_consteval || spec.is_typedef) {
      s_->diag(at, Diag::not_supported, Tk::l_square);
      return fail();
    }
    if (accept(Tk::amp)) declared = types.lvalue_ref(declared);
    else if (accept(Tk::ampamp)) declared = types.rvalue_ref(declared);
    bump();  // '['
    do {
      if (!is(Tk::identifier)) {
        s_->diag(pos(), Diag::expected_unqualified_id);
        if (is(Tk::r_square)) bump();
        return fail();
      }
      u32 local = s_->declare_local(name_of(tok()), spec.type, ast::local_flag::structured_binding, pos());
      names.emplace_back(local, pos());
      bump();
    } while (accept(Tk::comma));
    if (!expect(Tk::r_square)) return fail();

    // The names already participate in lookup. Referring to any of them in the
    // initializer is ill-formed ([dcl.struct.bind]/5).
    u32 init_at = pos();
    auto form = sema::Construction::copy;
    Ex source;
    if (accept(Tk::equal)) {
      source = assignment_expression();
    } else if (is(Tk::l_paren) || is(Tk::l_brace)) {
      bool brace = is(Tk::l_brace);
      form = brace ? sema::Construction::direct_list : sema::Construction::direct;
      bump();
      source = assignment_expression();
      if (!expect(brace ? Tk::r_brace : Tk::r_paren)) return fail();
    } else {
      s_->diag(at, Diag::binding_requires_initializer);
      return fail();
    }
    if (!source.ok()) return fail();

    QualType type;
    Ex init;
    const sema::ClassRec* record = nullptr;
    if (types.is_class(source.type())) {
      auto binding = s_->class_binding(declared, source, names.size(), at);
      if (!binding) return fail();
      std::tie(type, record) = *binding;
      const bool reference = types.is_reference(type);
      const bool braced = form == sema::Construction::direct_list;
      if (braced && (reference || !s_->is_aggregate(type))) {
        sema::Braced list{init_at, pos(), {}};
        list.clauses.push_back({source, {}, {}, false, source.begin});
        init = s_->list_init(type, list, form, /*extend_reference=*/true);
      } else if (reference) {
        init = s_->bind_reference(source, type, source.begin, /*extend=*/true);
      } else {
        // For a same-type aggregate element, [dcl.init.list] uses direct-initialization.
        init = s_->construct(type, {source}, init_at, pos(), braced ? sema::Construction::direct : form);
      }
      if (!init.ok()) return fail();
    } else {
      auto binding = s_->array_binding(declared, source, names.size(), at);
      if (!binding) return fail();
      std::tie(type, init) = *binding;
    }
    if (!expect(Tk::semi)) return fail();

    u32 hidden = s_->declare_local(s_->no_name(), type, 0, at, false);
    std::vector<ast::Child> vars;
    auto append = [&](u32 local, const Ex& value, u32 from, u32 end, ast::EntityId dtor = ast::EntityId::none) {
      Ex var = s_->node(ast::LocalVar{local, sema::Sema::at(value, from), dtor}, from, end);
      s_->set_local_decl(local, var.id);
      s_->set_initialized(local);
      vars.push_back(sema::Sema::at(var, begin));
    };
    ast::EntityId dtor = record && !types.is_reference(type) ? s_->destructor_of(type) : ast::EntityId::none;
    append(hidden, init, begin, pos(), dtor);
    QualType index_type = s_->builtin(s_->world().target.ptrdiff_type);
    for (u64 i = 0; i < names.size(); ++i) {
      auto [local, name_at] = names[i];
      u32 end = name_at + 1;
      Ex object = s_->local_ref(hidden, name_at, end);
      Ex element;
      if (record) {
        const auto& field = record->fields[i];
        element = s_->member(object, false, field.name, end, name_at, name_at);
        if (!element.ok()) return fail(false);
        s_->set_binding_type(local, types.is_reference(field.type) ? field.type : element.type());
      } else {
        Ex index = s_->literal(index_type, s_->world().values.integer(index_type, i), ast::ValueCategory::prvalue, name_at, end);
        element = s_->subscript(object, index, end, name_at);
        s_->set_binding_type(local, element.type());
      }
      append(local, element, name_at, end);
    }
    return s_->node(ast::DeclStmt{s_->builder().list(vars)}, begin, pos());
  }

  // A block-scope simple-declaration ([stmt.dcl], [dcl.pre]): each declarator is a local
  // variable, in scope from the end of its declarator ([basic.scope.pdecl]).
  Ex declaration_statement() {
    u32 begin = pos();
    DeclSpec spec = decl_specifiers();
    if (is(Tk::l_square) || ((is(Tk::amp) || is(Tk::ampamp)) && kind(1) == Tk::l_square))
      return structured_binding(spec, begin);
    if (spec.is_extern || spec.is_inline) {
      s_->diag(begin, Diag::not_supported, spec.is_extern ? Tk::kw_extern : Tk::kw_inline);
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    if (spec.is_constinit && !spec.is_static) s_->diag(begin, Diag::constinit_automatic);  // [dcl.constinit]/1
    if (spec.is_consteval) {
      s_->diag(begin, Diag::consteval_not_function);
      skip_to_semi();
      return {NodeId::none, begin, pos()};
    }
    if (spec.is_typedef) {
      // Block-scope typedef-names ([dcl.typedef]).
      do {
        Declarator d = declarator(spec.type, false, true);
        if (!d.ok || !spec.ok || !d.named || d.unknown_bound) {
          if (d.ok && d.unknown_bound) s_->diag(d.name_at, Diag::not_supported, Tk::l_square);
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        if (s_->ins().names.spelling(d.name).starts_with(lex::literal_operator_prefix)) {
          s_->diag(d.name_at, Diag::literal_operator_scope);
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        if (s_->types().has_placeholder(d.type)) {
          s_->diag(d.name_at, Diag::auto_not_allowed);  // [dcl.spec.auto.general]: not in a typedef
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        s_->local_type_alias(d.name, d.type, d.name_at);
      } while (accept(Tk::comma));
      expect(Tk::semi);
      return s_->node(ast::ExprStmt{}, begin, pos());
    }
    std::vector<Ex> vars;
    std::optional<QualType> replaced;  // what the declarators' placeholder deduced ([dcl.type.auto.deduct]/2)
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
      if (s_->ins().names.spelling(d.name).starts_with(lex::literal_operator_prefix)) {
        s_->diag(d.name_at, Diag::literal_operator_scope);
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (s_->types().is_void(d.type)) {
        s_->diag(d.name_at, Diag::void_variable, d.name);
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (!s_->require_supported_virtual_layout(d.type, d.name_at)) {
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      sema::Sema::ImmediateReferenceScope references(*s_, s_->constant_reference_context || spec.is_constexpr);
      // [dcl.constexpr]: a constexpr variable is const.
      if (spec.is_constexpr) d.type = s_->types().add_cv(d.type, sema::Cv::const_);
      // `char s[] = "…"`: the bound from the literal, null included ([dcl.init.string]/1).
      if (d.unknown_bound && is(Tk::equal) && kind(1) == Tk::string_lit) {
        d.type = s_->types().array(d.type, literal_at(1).units.size() + 1);
        d.unknown_bound = false;
      }
      // A placeholder type, deduced from the initializer ([dcl.type.auto.deduct]); the name is
      // declared first, so that its initializer cannot use it ([dcl.spec.auto.general]/11).
      if (s_->types().has_placeholder(d.type)) {
        u32 local = s_->declare_local(d.name, d.type, 0, d.name_at);
        Ex init = placeholder_variable(d, local, spec.is_constexpr, replaced);
        if (!init.ok()) {
          // Its uses say nothing more ([temp.dep.expr]: as a dependent name would).
          if (s_->types().has_placeholder(s_->local(local).type)) s_->set_local_type(local, s_->builtin(Builtin::dependent));
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        if (!s_->require_supported_virtual_layout(d.type, d.name_at)) {
          skip_to_semi();
          return {NodeId::none, begin, pos()};
        }
        ast::EntityId dtor = s_->types().is_reference(d.type) ? ast::EntityId::none : s_->destructor_of(d.type);
        Ex var = variable_node(spec, local, init, dtor, dbegin);
        s_->set_local_decl(local, var.id);
        vars.push_back(var);
        continue;
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
        const auto form = accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
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
        if (s_->types().is_reference(d.type) && !s_->types().is_class(s_->types().referee(d.type)) &&
            b->clauses.size() == 1 && !b->clauses[0].list && !b->clauses[0].designated)
          init = s_->bind_reference(b->clauses[0].expr, d.type, b->clauses[0].begin, true);
        else
          init = s_->list_init(d.type, *b, form, /*extend_reference=*/true);
        constant_local(local, d.type, spec.is_constexpr, init);
      } else if (accept(Tk::equal)) {
        Ex e = assignment_expression();
        if (s_->types().is_array(d.type)) {
          init = s_->convert(e, d.type, e.begin);  // from a string literal ([dcl.init.string])
        } else if (s_->types().is_reference(d.type)) {
          // A temporary bound to the reference lives as long as it ([class.temporary]/6).
          init = s_->bind_reference(e, d.type, e.begin, true);
        } else {
          init = s_->convert(e, d.type, e.begin);
          constant_local(local, d.type, spec.is_constexpr, init);
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
      } else if (s_->default_deleted(d.type)) {
        s_->diag(d.name_at, Diag::deleted_constructor, d.type);
      } else if (s_->has_member_inits(d.type)) {
        init = s_->default_init(d.type, d.name_at, pos());
        if (s_->as<ast::DefaultInit>(init)) constant_local(local, d.type, spec.is_constexpr, init);
      } else if (spec.is_constexpr) {
        s_->diag(d.name_at, Diag::constexpr_uninitialized, d.name);
      } else if (sema::Types::is_const(d.type)) {
        s_->diag(d.name_at, Diag::const_uninitialized, d.name);
      }
      // The destructor that ends the variable at scope exit ([stmt.dcl]); a reference has none.
      ast::EntityId dtor = s_->types().is_reference(d.type) ? ast::EntityId::none : s_->destructor_of(d.type);
      Ex var = variable_node(spec, local, init, dtor, dbegin);
      s_->set_local_decl(local, var.id);
      // A jump may bypass a static declaration ([stmt.dcl]/2). Default-initialization that
      // initializes nothing is vacuous ([basic.life]/2).
      if (!spec.is_static && ((init.ok() && !s_->as<ast::DefaultInit>(init)) || dtor != ast::EntityId::none))
        s_->set_initialized(local);
      vars.push_back(var);
    } while (accept(Tk::comma));
    if (!expect(Tk::semi)) skip_to_semi();
    std::vector<ast::Child> cs;
    for (const Ex& v : vars) cs.push_back(sema::Sema::at(v, begin));
    return s_->node(ast::DeclStmt{s_->builder().list(cs)}, begin, pos());
  }

  // The initializer of block-scope variable `d`, declared as `local` with a placeholder type:
  // `= e`, `(e)` or `{e}` ([dcl.type.auto.deduct]/2), from which its type is deduced, then the
  // variable initialized as one of that type. `replaced` holds what an earlier declarator's
  // placeholder deduced. An invalid `Ex` after a diagnosed error.
  Ex placeholder_variable(Declarator& d, u32 local, bool constexpr_, std::optional<QualType>& replaced) {
    u32 at = pos();
    Ex e;
    if (accept(Tk::equal)) {
      if (is(Tk::l_brace)) {  // `auto x = {…}` deduces std::initializer_list, not supported yet
        s_->diag(pos(), Diag::not_supported, Tk::l_brace);
        return {NodeId::none, at, pos()};
      }
      e = assignment_expression();
    } else if (is(Tk::l_brace) || is(Tk::l_paren)) {
      bool brace = is(Tk::l_brace);
      u32 open = pos();
      std::vector<Ex> args;
      if (brace) {
        auto b = braced_init_list();
        for (auto& c : b->clauses) {
          if (c.list || c.designated) {
            s_->diag(c.begin, Diag::auto_list, u64(b->clauses.size()));
            return {NodeId::none, at, pos()};
          }
          args.push_back(c.expr);
        }
      } else {
        bump();
        if (!is(Tk::r_paren))
          do args.push_back(assignment_expression());
          while (accept(Tk::comma));
        if (!expect(Tk::r_paren)) return {NodeId::none, at, pos()};
      }
      if (args.size() != 1) {  // [dcl.type.auto.deduct]/2.1: a single element
        s_->diag(open, Diag::auto_list, u64(args.size()));
        return {NodeId::none, at, pos()};
      }
      e = args[0];
    } else {
      s_->diag(d.name_at, Diag::auto_uninitialized, d.name);
      return {NodeId::none, at, pos()};
    }
    if (!e.ok()) return e;
    QualType r{};
    auto t = s_->deduce_placeholder(d.type, e, e.begin, &r);
    if (!t) return {NodeId::none, at, pos()};
    if (replaced && !s_->types().is_dependent(r) && !s_->types().is_dependent(*replaced) &&
        s_->types().canonical(*replaced) != s_->types().canonical(r)) {
      s_->diag(d.name_at, Diag::auto_inconsistent, d.type, r, *replaced);
      return {NodeId::none, at, pos()};
    }
    replaced = r;
    if (s_->types().is_void(*t)) {
      s_->diag(d.name_at, Diag::void_variable, d.name);
      return {NodeId::none, at, pos()};
    }
    d.type = *t;
    s_->set_local_type(local, d.type);
    Ex init = s_->types().is_reference(d.type) ? s_->bind_reference(e, d.type, e.begin, true) : s_->convert(e, d.type, e.begin);
    constant_local(local, d.type, constexpr_, init);
    return init;
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
  u32 deferred_argument_checks_ = 0;  // checks a dependent canonical argument may not retain
  bool half_ = false;           // the current `>>` has had its first `>` taken by a template-id
public:
  bool body = false;  // the part is a function body, not a declaration part
  bool substituting_partial_arguments = false;
private:
  std::optional<u32> address_op_;  // the token of an explicit unary `&`
  std::optional<u32> lambda_result_operand_;  // first token of a deducing lambda's return operand
};

}  // namespace ycxx::parse
