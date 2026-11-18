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
};

// What a declarator declares ([dcl.decl]): its name and type, and, for a function declarator,
// the parameters of the function type closest to the name.
struct Declarator {
  NameId name{};
  bool named = false;
  u32 name_at = 0;
  // `C::name`: the class named by the nested-name-specifier ([dcl.meaning]).
  bool qualified = false;
  NameId qualifier{};
  u32 qualifier_at = 0;
  u32 begin = 0;
  QualType type{};
  std::vector<Param> params;
  bool variadic = false;
  // `T x[] = {…}`: the outermost array bound is left to the initializer ([dcl.array]/8); `type`
  // is then the element type.
  bool unknown_bound = false;
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

  Tk kind(u32 ahead = 0) const { return i_ + ahead < end_ ? r_->tokens[i_ + ahead].kind : Tk::eof; }
  const Tok& tok(u32 ahead = 0) const { return r_->tokens[i_ + ahead]; }
  bool is(Tk k) const { return kind() == k; }
  u32 pos() const { return i_ - begin_; }  // the current token's offset in the part
  void bump() {
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

  // Whether a declaration starts here: a decl-specifier keyword, or a name that denotes a class
  // ([stmt.ambig]: what can be a declaration is one).
  bool starts_declaration() {
    if (starts_decl_specifier(kind())) return true;
    if (!is(Tk::identifier) || kind(1) == Tk::coloncolon || !s_->class_name(name_of(tok()))) return false;
    // `T(args).f()` and `T{…}.m` are expressions; `T (x);` declares x ([stmt.ambig]).
    if (kind(1) == Tk::l_brace) return false;
    if (kind(1) != Tk::l_paren) return true;
    u32 j = 1, depth = 0;
    for (; i_ + j < end_; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren) ++depth;
      if (k == Tk::r_paren && --depth == 0) break;
    }
    Tk after = kind(j + 1);
    return after != Tk::period && after != Tk::arrow && after != Tk::l_paren && after != Tk::l_square;
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
    int n_char8 = 0, n_char16 = 0, n_char32 = 0, n_wchar = 0;
    bool c = false, v = false;
    std::optional<QualType> cls;  // a class-name or elaborated-type-specifier
    auto any_type = [&] {
      return cls || n_char + n_short + n_int + n_long + n_signed + n_unsigned + n_void + n_bool + n_char8 + n_char16 +
                        n_char32 + n_wchar > 0;
    };
    auto once = [&](bool& flag, Tk k) {
      if (flag) s_->diag(pos(), Diag::duplicate_specifier, k);
      flag = true;
    };
    for (;;) {
      Tk k = kind();
      switch (k) {
      case Tk::kw_void: ++n_void; break;
      case Tk::kw_bool: ++n_bool; break;
      case Tk::kw_char: ++n_char; break;
      case Tk::kw_char8_t: ++n_char8; break;
      case Tk::kw_char16_t: ++n_char16; break;
      case Tk::kw_char32_t: ++n_char32; break;
      case Tk::kw_wchar_t: ++n_wchar; break;
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
      case Tk::kw_constexpr: once(d.is_constexpr, k); break;
      case Tk::kw_consteval: once(d.is_consteval, k); break;
      case Tk::kw_constinit: once(d.is_constinit, k); break;
      case Tk::kw_struct:
      case Tk::kw_class: {
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
        auto t = s_->class_name(n, true);
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
      case Tk::identifier:
        if (!any_type()) {
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
    if (int(d.is_constexpr) + int(d.is_consteval) + int(d.is_constinit) > 1) {  // [dcl.spec]/2
      s_->diag(d.begin, Diag::constexpr_specifiers);
      d.ok = false;
    }
    // [dcl.type.general]: the combinations of simple-type-specifiers, as in its table.
    int total = n_char + n_short + n_int + n_long + n_signed + n_unsigned + n_void + n_bool + n_char8 + n_char16 +
                n_char32 + n_wchar;
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
    bool variadic = false;
    sema::Cv cv = sema::Cv::none;  // a member function's cv-qualifier-seq ([dcl.fct])
    u64 bound = 0;
    bool unknown_bound = false;  // `[]`: in a parameter, or the outermost bound of a variable
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
      d.params = fn->params;
      d.variadic = fn->variadic;
    }
    return d;
  }

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
    if (!abstract && is(Tk::identifier) && kind(1) == Tk::coloncolon && kind(2) == Tk::identifier) {
      // A qualified declarator-id `C::name`, defining a member of class C ([class.mfct],
      // [class.static.data]).
      l.qualified = true;
      l.qualifier = name_of(tok());
      l.qualifier_at = pos();
      bump();
      bump();
    }
    if (is(Tk::identifier)) {
      l.name = name_of(tok());
      l.named = true;
      l.name_at = pos();
      bump();
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
        // ref-qualifiers, noexcept-specifiers and trailing return types are not supported yet.
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
        auto v = sema::Evaluator(*s_, questions()).evaluate(e).value;
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

  // Whether the `(` here begins a parameter-declaration-clause rather than an initializer
  // ([dcl.ambig.res]: what can be a declaration is one).
  bool params_ahead() {
    Tk k = kind(1);
    if (k == Tk::r_paren || k == Tk::ellipsis || starts_decl_specifier(k)) return true;
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
        // The outermost bound of a variable, which its initializer gives; any other is not
        // supported yet.
        if (&*s != &l.suffixes.front() || !l.pointers.empty() || d.unknown_bound || &l != top_) {
          s_->diag(d.begin, Diag::not_supported, Tk::l_square);
          d.ok = false;
        }
        d.unknown_bound = true;
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
        if (s.params[i].name == s.params[j].name && s.params[i].name != s_->no_name())
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
      Declarator d = declarator(spec.type, true);
      ok = ok && spec.ok && d.ok;
      QualType t = d.type;
      // [dcl.fct]/5: arrays and functions decay; top-level cv is dropped.
      if (s_->types().is_array(t)) t = s_->types().pointer(s_->types().rec(t).inner);
      else if (s_->types().is_function(t)) t = s_->types().pointer(t);
      if (s_->types().is_void(t)) {
        s_->diag(at, Diag::void_parameter);
        ok = false;
      }
      // The parameter keeps its top-level cv as a variable of the body; the function type drops
      // it (`function_type` adjusts).
      s.params.push_back({t, d.named ? d.name : s_->no_name(), d.named ? d.name_at : at});
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
    if (is(Tk::kw_template) || (type_param && kind(1) == Tk::ellipsis)) {
      s_->diag(pos() + (is(Tk::kw_template) ? 0 : 1), Diag::not_supported, is(Tk::kw_template) ? Tk::kw_template : Tk::ellipsis);
      ok = false;
      return std::nullopt;
    }
    if (type_param) {
      // A type-parameter ([temp.param]/1), with a default type-id.
      bump();
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
    Declarator d = declarator(spec.type, true);
    if (!spec.ok || !d.ok) {
      ok = false;
      return std::nullopt;
    }
    QualType t = d.type;
    if (s_->types().is_array(t)) t = s_->types().pointer(s_->types().rec(t).inner);
    else if (s_->types().is_function(t)) t = s_->types().pointer(t);
    t = sema::Types::unqualified(t);
    // [temp.param]/7: a structural type; class types are not supported yet.
    if (!s_->types().is_integral(t) && !s_->types().is_pointer(t) && !s_->types().is_reference(t) &&
        !s_->types().is(t, Builtin::nullptr_t) && !s_->types().is_dependent(t)) {
      s_->diag(at, s_->types().is_class(t) ? Diag::not_supported : Diag::invalid_template_parameter_type,
               s_->types().is_class(t) ? Tk::kw_class : Tk::kw_template);
      ok = false;
      return std::nullopt;
    }
    if (d.named) p.name = d.name;
    p.kind = ast::TemplateArgKind::value;
    p.type = t;
    if (accept(Tk::equal)) {
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
    if (!is(Tk::greater)) {
      do {
        if (starts_type_id()) {
          auto t = type_id();
          if (!t) ok = false;
          else args.push_back({ast::TemplateArgKind::type, s_->types().canonical(*t), sema::ConstId::none});
          continue;
        }
        Ex e = conditional_expression();
        if (!e.ok()) {
          ok = false;
          continue;
        }
        // Converted to its parameter's type when the template is known ([temp.arg.nontype]).
        auto v = template_value(e, sema::Types::unqualified(s_->rvalue(e).type()));
        if (!v) ok = false;
        else args.push_back({ast::TemplateArgKind::value, s_->types().canonical(sema::Types::unqualified(s_->rvalue(e).type())), *v});
      } while (accept(Tk::comma));
    }
    --angle_;
    if (!expect(Tk::greater) || !ok) return std::nullopt;
    return args;
  }
  bool starts_type_id() {
    if (starts_decl_specifier(kind())) return !is(Tk::kw_static) && !is(Tk::kw_extern);
    return is(Tk::identifier) && kind(1) != Tk::coloncolon && s_->class_name(name_of(tok())).has_value();
  }

  // The language linkage function types get: from an enclosing linkage-specification.
  void set_linkage(LangLinkage l) { linkage_ = l; }
  LangLinkage linkage() const { return linkage_; }

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
    if (is(Tk::l_paren) && starts_decl_specifier(kind(1))) {
      u32 at = pos();
      s_->diag(at, Diag::not_supported, Tk::l_paren);
      bump();
      while (!is(Tk::r_paren) && !at_end()) bump();
      accept(Tk::r_paren);
      cast_expression();
      return {NodeId::none, at, pos()};
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
    case Tk::kw_alignof:
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
            if (is(Tk::l_brace)) {
              s_->diag(pos(), Diag::not_supported, Tk::l_brace);
              return {NodeId::none, e.begin, pos()};
            }
            args.push_back(assignment_expression());
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
      bump();
      int saved = std::exchange(angle_, 0);
      Ex e = expression();
      angle_ = saved;
      if (!expect(Tk::r_paren)) return {NodeId::none, begin, pos()};
      return s_->paren(e, begin, pos());
    }
    case Tk::identifier: {
      NameId n = name_of(tok());
      if ((kind(1) == Tk::l_paren || kind(1) == Tk::l_brace))
        if (auto t = s_->class_name(n)) {
          // `T(args)` or `T{…}` ([expr.type.conv]).
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
      bump();
      if (is(Tk::less) && s_->is_template_name(n)) {
        // A template-id ([temp.names]): `f<args>`.
        auto args = template_argument_list();
        if (!args) return {NodeId::none, begin, pos()};
        return s_->template_id(n, std::move(*args), begin, pos());
      }
      if (is(Tk::coloncolon) && kind(1) == Tk::identifier) {
        // A qualified-id `C::name` ([expr.prim.id.qual]).
        bump();
        u32 name_at = pos();
        NameId m = name_of(tok());
        bump();
        if (is(Tk::coloncolon)) {
          s_->diag(pos(), Diag::not_supported, Tk::coloncolon);
          return {NodeId::none, begin, pos()};
        }
        return s_->qualified_name(n, m, begin, name_at, pos());
      }
      return s_->name_expr(n, begin, pos(), is(Tk::l_paren));
    }
    case Tk::kw_this: bump(); return s_->this_expr(begin, pos());
    case Tk::float_lit:
    case Tk::l_square:
    case Tk::kw_requires:
    case Tk::coloncolon:
    case Tk::kw_operator:
    case Tk::tilde:
    case Tk::kw_typeid:
    case Tk::kw_static_cast:
    case Tk::kw_dynamic_cast:
    case Tk::kw_const_cast:
    case Tk::kw_reinterpret_cast:
    case Tk::kw_throw:
      s_->diag(begin, Diag::not_supported, kind());
      bump();
      return {NodeId::none, begin, pos()};
    default:
      s_->diag(begin, Diag::expected_expression);
      return {NodeId::none, begin, begin};
    }
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
      if (is(Tk::l_brace)) c.list = braced_init_list();
      else c.expr = assignment_expression();
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
    case Tk::kw_co_return:
    case Tk::kw_using:
    case Tk::kw_namespace:
    case Tk::kw_asm:
    case Tk::kw_contract_assert:
    case Tk::kw_template:
      s_->diag(begin, Diag::not_supported, kind());
      skip_statement();
      return {NodeId::none, begin, pos()};
    default: break;
    }
    if (is(Tk::l_square) && kind(1) == Tk::l_square) {
      s_->diag(begin, Diag::not_supported, Tk::l_square);  // attributes
      skip_statement();
      return {NodeId::none, begin, pos()};
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
        s_->diag(pos(), Diag::not_supported, Tk::l_brace);
        skip_to_semi();
        return {NodeId::none, begin, pos()};
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
    std::vector<Ex> vars;
    do {
      u32 dbegin = pos();
      Declarator d = declarator(spec.type, false);
      if (!d.ok || !spec.ok) {
        skip_to_semi();
        return {NodeId::none, begin, pos()};
      }
      if (s_->types().is_function(d.type)) {
        s_->diag(d.name_at, Diag::not_supported, Tk::l_paren);  // block-scope function declarations
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
          s_->diag(dbegin, Diag::not_supported, Tk::l_square);  // array initialization
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
  const Level* top_ = nullptr;  // the declarator being built
};

}  // namespace ycxx::parse
