// ycxx.parse: the per-item queries of docs/design/ast.md §3.6 and query-engine.md §B1.3, over one
// region's items (#18):
// - `item_sema(item)` parses and analyses an item's declaration part: an `ItemSema` whose
//   `decls` are in the order of the skeleton's declared names, so `decls[ord]` is the record of
//   `DeclId{item, ord}`;
// - `decl_sema(d)` is `item_sema(d.item).decls[d.ord]`;
// - `analyze_body(item)` parses and analyses a function body: a `BodySema` whose first locals
//   are the parameters.
//
// Until contextual queries exist (#16), `Program` plays the engine: it memoizes each cell for one
// translation unit, and answers sema's questions (`visible_decls` at an item's prefix) from the
// skeletons. Each function below keeps the signature it will have as a query, with `Program` in
// place of the context; diagnostics are values held per cell.
export module ycxx.parse;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pp;
import ycxx.items;
import ycxx.sema;
export import :parser;

export namespace ycxx::parse {

using ast::DeclId;
using ast::EntityId;
using ast::ItemId;
using items::ItemKind;
using sema::ConstId;

// The record of a declaration that failed to be declared; its diagnostic says why.
ast::DeclSema invalid_decl() {
  ast::DeclSema d{};
  d.entity = EntityId::none;
  d.constraint = ast::NodeId::none;
  d.contracts = ast::NodeId::none;
  return d;
}

// A cell's value and its diagnostics.
template <class V>
struct Result {
  V value{};
  std::vector<engine::Diagnostic> diags;
};

class Program {
public:
  Program(sema::World& w, const pp::Region& r, const items::Items& its)
      : w_(&w), r_(&r), items_(&its), item_cells_(its.items.size()), body_cells_(its.items.size()) {
    for (const items::Item& it : its.items) {
      ItemId id = w.ins.items.add<ItemId>(it.id);
      ids_.push_back(id);
      index_.emplace(std::to_underlying(id), static_cast<u32>(ids_.size() - 1));
    }
  }

  sema::World& world() { return *w_; }
  const pp::Region& region() const { return *r_; }
  const items::Items& items() const { return *items_; }
  ItemId id(u32 item) const { return ids_[item]; }
  u32 index(ItemId id) const { return index_.at(std::to_underlying(id)); }

  const Result<ast::ItemSema>& item_sema(u32 item);
  std::optional<ast::DeclSema> decl_sema(DeclId d);
  const Result<ast::BodySema>& analyze_body(u32 item);

  // `visible_decls(::, name, at item)` (§B2): the global-namespace declarations of `name` in the
  // items before `item`, and in `item` itself when `self`.
  std::vector<DeclId> visible_decls(std::string_view name, u32 item, bool self) const {
    std::vector<DeclId> r;
    for (u32 j = 0; j < item + (self ? 1 : 0); ++j) {
      const items::Item& it = items_->items[j];
      if (!it.scope.empty()) continue;
      for (const items::DeclName& n : it.names)
        if (n.name == name && !(n.flags & items::name_flag::friend_))
          r.push_back({ids_[j], ast::make_decl_ord(ast::DeclClass::syntactic, n.ordinal)});
    }
    return r;
  }

  // The constant a variable usable in constant expressions was initialized with ([expr.const]).
  std::optional<ConstId> constant_value(EntityId e) const {
    if (auto it = constants_.find(std::to_underlying(e)); it != constants_.end()) return it->second;
    return std::nullopt;
  }
  void set_constant(EntityId e, ConstId c) { constants_[std::to_underlying(e)] = c; }

  // Every item and body analysed, in item order; the diagnostics of each cell in order.
  void analyze_all() {
    for (u32 k = 0; k < items_->items.size(); ++k) {
      // Members of a class or namespace wait for those (M2); the enclosing item was diagnosed.
      if (!in_global_scope(k)) continue;
      item_sema(k);
      if (items_->items[k].has_body() && items_->items[k].kind == ItemKind::function) analyze_body(k);
    }
  }
  bool in_global_scope(u32 k) const {
    for (u32 j = items_->items[k].parent; j != items::none; j = items_->items[j].parent)
      if (items_->items[j].kind != ItemKind::linkage_block) return false;
    return true;
  }
  std::vector<engine::Diagnostic> diagnostics() const {
    std::vector<engine::Diagnostic> r;
    for (u32 k = 0; k < items_->items.size(); ++k) {
      if (item_cells_[k]) r.insert(r.end(), item_cells_[k]->diags.begin(), item_cells_[k]->diags.end());
      if (body_cells_[k]) r.insert(r.end(), body_cells_[k]->diags.begin(), body_cells_[k]->diags.end());
    }
    std::ranges::stable_sort(r, {}, [&](const engine::Diagnostic& d) {
      return std::pair(index(ItemId(d.at.item)), d.at.offset);
    });
    return r;
  }

  // The byte offset in the file of a diagnostic's location (ast.md §8.3, steps 1–3).
  u32 byte_offset(engine::Loc at) const {
    const items::Item& it = items_->items[index(ItemId(at.item))];
    u32 tok = std::min<u32>(it.begin + at.offset, static_cast<u32>(r_->locs.size() - 1));
    return r_->locs[tok].offset;
  }

  // Spells a diagnostic argument: a name or a type.
  std::string spell(char how, u64 raw) const {
    if (how == 'n') return std::string(w_->ins.names.spelling(ast::NameId(u32(raw))));
    return w_->types.spell(ast::QualType(u32(raw)));
  }

private:
  enum class State : u8 { empty, running, done };
  template <class V>
  struct Cell {
    State state = State::empty;
    Result<V> result;
  };

  sema::World* w_;
  const pp::Region* r_;
  const items::Items* items_;
  std::vector<ItemId> ids_;
  std::unordered_map<u32, u32> index_;
  std::vector<std::optional<Result<ast::ItemSema>>> item_cells_;
  std::vector<std::optional<Result<ast::BodySema>>> body_cells_;
  std::vector<u32> running_;
  std::map<u32, ConstId> constants_;
  std::pmr::unsynchronized_pool_resource arena_;  // the frames' arena (§9.3)

  friend class ItemAnalysis;
  friend class BodyAnalysis;
};

// Sema's questions, asked at an item's prefix (§B1.4).
class AtItem final : public sema::Questions {
public:
  AtItem(Program& p, u32 item, bool self) : p_(&p), item_(item), self_(self) {}
  std::vector<DeclId> visible_decls(ast::NameId name) override {
    return p_->visible_decls(p_->world().ins.names.spelling(name), item_, self_);
  }
  ast::DeclSema decl_sema(DeclId d) override {
    if (auto s = p_->decl_sema(d)) return *s;
    return invalid_decl();
  }
  std::optional<ConstId> constant_value(EntityId e) override { return p_->constant_value(e); }

private:
  Program* p_;
  u32 item_;
  bool self_;
};

// ---------------------------------------------------------------- item_sema

// [dcl.link]: the language linkage a linkage-specification's string names.
std::optional<LangLinkage> linkage_named(const pp::Region& r, u32 tok) {
  const lex::Literal& l = r.literal(r.tokens[tok]);
  std::string s;
  for (u32 u : l.units) s.push_back(static_cast<char>(u));
  if (s == "C") return LangLinkage::c;
  if (s == "C++") return LangLinkage::cxx;
  return std::nullopt;
}

class ItemAnalysis {
public:
  ItemAnalysis(Program& p, u32 item)
      : p_(&p), k_(item), it_(&p.items().items[item]), b_(p.world().ins, &p.arena_), q_(p, item, false),
        s_(p.world(), b_, q_, out_.diags, p.id(item), 0),
        ps_(p.region(), it_->begin, it_->begin + it_->decl_length(), s_) {
    ps_.set_questions(q_);
  }

  Result<ast::ItemSema> run() {
    switch (it_->kind) {
    case ItemKind::empty:
    case ItemKind::linkage_block: break;  // its members are items of their own
    case ItemKind::declaration:
    case ItemKind::function: declaration(); break;
    default:
      s_.diag(0, Diag::not_supported, p_->region().tokens[it_->begin].kind);
      bad();
      break;
    }
    std::vector<ast::Child> cs;
    for (const Ex& d : declarators_) cs.push_back(sema::Sema::at(d, 0));
    Ex root = s_.node(ast::DeclGroup{b_.list(cs)}, 0, it_->decl_length());
    ast::ItemSema v{root.id, b_.seq(std::span<const ast::DeclSema>(decls_))};
    out_.value = b_.commit(v);
    return std::move(out_);
  }

private:
  // The language linkage of the enclosing linkage-specifications ([dcl.link]): the innermost.
  LangLinkage enclosing_linkage() {
    for (u32 j = it_->parent; j != items::none; j = p_->items().items[j].parent) {
      const items::Item& outer = p_->items().items[j];
      if (outer.kind == ItemKind::linkage_block)
        if (auto l = linkage_named(p_->region(), outer.begin + 1)) return *l;
    }
    return LangLinkage::cxx;
  }

  // A simple-declaration or the declaration part of a function-definition ([dcl.pre],
  // [dcl.fct.def.general]), possibly after `extern "C"` ([dcl.link]).
  void declaration() {
    LangLinkage lang = enclosing_linkage();
    bool explicit_lang = lang != LangLinkage::cxx;
    if (ps_.is(Tk::kw_extern) && ps_.kind(1) == Tk::string_lit) {
      u32 at = ps_.pos() + 1;
      auto l = linkage_named(p_->region(), it_->begin + at);
      if (!l) s_.diag(at, Diag::unknown_linkage);
      lang = l.value_or(LangLinkage::cxx);
      explicit_lang = true;
      ps_.bump();
      ps_.bump();
      while (ps_.is(Tk::string_lit) && ps_.tok().has(lex::tok_flag::continued)) ps_.bump();
    }
    ps_.set_linkage(lang);
    if (ps_.is(Tk::semi)) {
      ps_.bump();
      return;
    }
    if (!Parser::starts_decl_specifier(ps_.kind())) {
      if (lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon))
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // templates, using, static_assert, …
      else
        s_.diag(ps_.pos(), Diag::expected_declaration);
      return bad();
    }
    DeclSpec spec = ps_.decl_specifiers();
    bool is_function_def = it_->kind == ItemKind::function;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false);
      if (!d.ok || !spec.ok || !d.named) {
        bad();
        return;
      }
      declare(spec, d, lang, explicit_lang, is_function_def);
    } while (!is_function_def && ps_.accept(Tk::comma));
    if (!is_function_def) {
      if (!ps_.expect(Tk::semi)) bad();
    } else if (!ps_.at_end()) {
      s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
    }
  }

  // The rest of the skeleton's names get empty records, so ordinals stay aligned.
  void bad() {
    while (decls_.size() < it_->names.size()) {
      ast::DeclSema d = invalid_decl();
      d.type = s_.builtin(sema::Builtin::void_);  // an interned type, for the record's fingerprint
      d.attributes = b_.seq(std::span<const NodeId>{});
      d.params = b_.seq(std::span<const ast::ParamSema>{});
      decls_.push_back(d);
    }
  }

  // One init-declarator at namespace scope.
  void declare(const DeclSpec& spec, const parse::Declarator& d, LangLinkage lang, bool explicit_lang,
               bool is_function_def) {
    sema::Types& types = s_.types();
    u32 ord = static_cast<u32>(decls_.size());
    bool fn = types.is_function(d.type);
    ast::DeclSema sem{};
    sem.kind = fn ? ast::DeclKind::function : ast::DeclKind::variable;
    sem.type = d.type;
    sem.storage = spec.is_static ? ast::Storage::static_ : spec.is_extern ? ast::Storage::extern_ : ast::Storage::none;
    // [basic.link]/3: `static`, and a non-volatile const variable not declared extern, give
    // internal linkage.
    bool internal = spec.is_static || (!fn && sema::Types::is_const(d.type) && !spec.is_extern && !spec.is_inline);
    sem.linkage = internal ? ast::Linkage::internal : ast::Linkage::external;
    sem.access = ast::Access::none;
    sem.constraint = NodeId::none;
    sem.contracts = NodeId::none;
    sem.attributes = b_.seq(std::span<const NodeId>{});
    u16 flags = 0;
    if (spec.is_inline) flags |= sema::decl_flag::inline_;
    if (spec.is_constexpr) flags |= sema::decl_flag::constexpr_;
    if (spec.is_constexpr) {
      s_.diag(spec.begin, Diag::not_supported, Tk::kw_constexpr);
    }
    bool has_init = ps_.is(Tk::equal) || ps_.is(Tk::l_brace) || (!fn && ps_.is(Tk::l_paren));
    bool definition = fn ? is_function_def : !(spec.is_extern && !has_init);
    if (definition) flags |= sema::decl_flag::definition;
    if (!fn && types.is_void(d.type)) {
      s_.diag(d.name_at, Diag::void_variable, d.name);
      return bad();
    }

    // Earlier declarations of the name: in this item, then before it ([basic.link], [dcl.fct]).
    std::vector<ast::DeclSema> prior;
    for (const auto& o : s_.own)
      if (o.name == d.name) prior.push_back(o.sema);
    if (prior.empty())
      for (DeclId p : q_.visible_decls(d.name))
        if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none) prior.push_back(*ds);
    if (fn) {
      if (!explicit_lang && !prior.empty() && prior.back().kind == ast::DeclKind::function)
        lang = s_.world().entities.rec(prior.back().entity).lang;  // [dcl.link]/6: inherited
      const sema::TypeRec& r = types.rec(d.type);
      std::vector<QualType> ps = r.params;
      sem.type = types.function(r.inner, ps, r.variadic, lang);
      if (lang == LangLinkage::c) flags |= sema::decl_flag::lang_c;
      if (r.variadic) flags |= sema::decl_flag::variadic;
      std::vector<ast::ParamSema> params;
      for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, NodeId::none, 0});
      sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    } else {
      sem.params = b_.seq(std::span<const ast::ParamSema>{});
    }
    sem.flags = flags;

    if (!prior.empty()) {
      const ast::DeclSema& p = prior.back();
      if ((p.kind == ast::DeclKind::function) != fn) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return bad();
      }
      if (fn && p.type != sem.type) {
        const sema::TypeRec& a = types.rec(p.type);
        const sema::TypeRec& c = types.rec(sem.type);
        if (a.params == c.params && a.variadic == c.variadic && a.inner == c.inner)
          s_.diag(d.name_at, Diag::linkage_conflict, d.name);
        else if (a.params == c.params && a.variadic == c.variadic)
          s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        else
          s_.diag(d.name_at, Diag::overload_not_supported, d.name);
        return bad();
      }
      if (!fn && sema::Types::unqualified(p.type) != sema::Types::unqualified(sem.type)) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return bad();
      }
      // [dcl.stc]: a later declaration may not give internal linkage to a name with external.
      if (spec.is_static && p.linkage == ast::Linkage::external) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return bad();
      }
      sem.linkage = p.linkage;
      if (definition && std::ranges::any_of(prior, [](const ast::DeclSema& o) { return o.flags & sema::decl_flag::definition; })) {
        s_.diag(d.name_at, Diag::redefinition, d.name);
        return bad();
      }
      sem.entity = p.entity;
    } else {
      sema::EntityRec e{fn ? sema::EntityKind::function : sema::EntityKind::variable,
                        d.name,
                        sem.type,
                        fn ? lang : LangLinkage::cxx,
                        sem.linkage,
                        {p_->id(k_), ast::make_decl_ord(ast::DeclClass::syntactic, ord)}};
      sem.entity = s_.world().entities.entity(e);
    }

    if (fn && p_->world().ins.names.spelling(d.name) == "main") check_main(spec, sem, d, lang);

    // The initializer ([dcl.init]); the name is in scope from the end of its declarator.
    s_.own.push_back({d.name, sem});
    Ex init;
    sema::ConstId value = sema::ConstId::none;
    if (fn && has_init && !is_function_def) {
      s_.diag(ps_.pos(), Diag::function_initialized, d.name);
      ps_.skip_to_semi();
    } else if (!fn && ps_.accept(Tk::equal)) {
      if (ps_.is(Tk::l_brace) || types.is_array(d.type)) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        ps_.skip_to_semi();
      } else {
        Ex e = ps_.assignment_expression();
        init = s_.convert(e, d.type, e.begin);
        if (init.ok()) {
          // [basic.start.static]: constant initialization, or dynamic, which the α subset lacks.
          if (auto v = sema::Folder(s_, q_).fold(init)) {
            value = *v;
            if (sema::Types::is_const(d.type) && types.is_integral(d.type)) p_->set_constant(sem.entity, *v);
          } else {
            s_.diag(e.begin, Diag::nonconstant_initializer);
          }
        }
      }
    } else if (!fn && (ps_.is(Tk::l_paren) || ps_.is(Tk::l_brace))) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      ps_.skip_to_semi();
    } else if (!fn && sema::Types::is_const(d.type) && !spec.is_extern) {
      s_.diag(d.name_at, Diag::const_uninitialized, d.name);
    }
    declarators_.push_back(s_.node(ast::Declarator{ord, sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
    decls_.push_back(sem);
  }

  // [basic.start.main]
  void check_main(const DeclSpec& spec, const ast::DeclSema& sem, const parse::Declarator& d, LangLinkage lang) {
    sema::Types& types = s_.types();
    const sema::TypeRec& r = types.rec(sem.type);
    if (!types.is(r.inner, Builtin::int_) || sema::Types::cv(r.inner) != sema::Cv::none)
      s_.diag(d.name_at, Diag::main_return_type);
    QualType charpp = types.pointer(types.pointer(s_.builtin(Builtin::char_)));
    bool ok = !r.variadic && (r.params.empty() || (r.params.size() == 2 && types.is(r.params[0], Builtin::int_) &&
                                                   r.params[1] == charpp));
    if (!ok) s_.diag(d.name_at, Diag::main_parameters);
    if (spec.is_static) s_.diag(spec.begin, Diag::main_specifier, Tk::kw_static);
    if (spec.is_inline) s_.diag(spec.begin, Diag::main_specifier, Tk::kw_inline);
    if (lang == LangLinkage::c) s_.diag(d.name_at, Diag::main_linkage);
  }

  Program* p_;
  u32 k_;
  const items::Item* it_;
  Result<ast::ItemSema> out_;
  ast::Builder b_;
  AtItem q_;
  sema::Sema s_;
  Parser ps_;
  std::vector<ast::DeclSema> decls_;
  std::vector<Ex> declarators_;
};

const Result<ast::ItemSema>& Program::item_sema(u32 item) {
  if (!item_cells_[item]) {
    // A cycle is an internal error (§A2).
    contract_assert(std::ranges::find(running_, item) == running_.end());
    running_.push_back(item);
    item_cells_[item] = ItemAnalysis(*this, item).run();
    running_.pop_back();
  }
  return *item_cells_[item];
}

std::optional<ast::DeclSema> Program::decl_sema(DeclId d) {
  if (ast::class_of(d.ord) != ast::DeclClass::syntactic) return std::nullopt;
  const ast::ItemSema& s = item_sema(index(d.item)).value;
  u32 n = ast::number_of(d.ord);
  if (n >= w_->ins.seqs.size(s.decls)) return std::nullopt;
  return w_->ins.seqs.at(s.decls, n);
}

// ---------------------------------------------------------------- analyze_body

class BodyAnalysis {
public:
  BodyAnalysis(Program& p, u32 item)
      : p_(&p), k_(item), it_(&p.items().items[item]), b_(p.world().ins, &p.arena_), q_(p, item, true),
        s_(p.world(), b_, q_, out_.diags, p.id(item), it_->body_begin - it_->begin),
        ps_(p.region(), it_->body_begin, it_->body_end, s_) {
    ps_.set_questions(q_);
  }

  Result<ast::BodySema> run() {
    const ast::ItemSema& is = p_->item_sema(k_).value;
    std::optional<ast::DeclSema> f;
    if (p_->world().ins.seqs.size(is.decls) > 0) f = p_->world().ins.seqs.at(is.decls, 0);
    Ex root;
    if (f && f->entity != EntityId::none && f->kind == ast::DeclKind::function) {
      s_.set_return_type(s_.types().rec(f->type).inner);
      // The parameters are the first locals, in their own scope ([basic.scope.param]).
      s_.push_scope();
      s_.mark_params_scope();
      for (const ast::ParamSema& prm : p_->world().ins.seqs.elements(f->params))
        s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, 0);
      if (!ps_.is(lex::Tk::l_brace)) {
        // `= default`, `= delete`, a ctor-initializer or a function-try-block.
        s_.diag(0, sema::Diag::not_supported, ps_.kind());
      } else {
        root = ps_.compound_statement();
        if (!ps_.at_end()) s_.diag(ps_.pos(), sema::Diag::unexpected_token, ps_.kind());
      }
      s_.pop_scope();
    }
    std::vector<ast::LocalRec> locals(s_.locals().begin(), s_.locals().end());
    ast::BodySema v{root.id, b_.seq(std::span<const ast::LocalRec>(locals))};
    out_.value = b_.commit(v);
    return std::move(out_);
  }

private:
  Program* p_;
  u32 k_;
  const items::Item* it_;
  Result<ast::BodySema> out_;
  ast::Builder b_;
  AtItem q_;
  sema::Sema s_;
  Parser ps_;
};

const Result<ast::BodySema>& Program::analyze_body(u32 item) {
  if (!body_cells_[item]) body_cells_[item] = BodyAnalysis(*this, item).run();
  return *body_cells_[item];
}

// ---------------------------------------------------------------- one file, for the driver

// Line and column (both from 1) of a byte offset in `text` (ast.md §8.3, step 4).
std::pair<u32, u32> line_column(std::string_view text, u32 offset) {
  u32 line = 1, col = 1;
  for (u32 i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
  return {line, col};
}

// Preprocesses, splits and analyses one file, and renders every diagnostic as
// `file:line:col: error: message`. Returns the number of errors.
int check_file(std::string_view path, std::string_view text, std::ostream& err) {
  pp::Preprocessed pre = pp::preprocess(text, pp::FileId{0});
  items::Items its = items::split(pre.region, ycxx::fingerprint128(path));
  ast::Interners ins;
  sema::World w(ins);
  Program p(w, pre.region, its);
  p.analyze_all();
  struct Line {
    u32 offset;
    lex::Severity severity;
    std::string text;
  };
  std::vector<Line> lines;
  for (const lex::LexDiagnostic& d : pre.diags)
    lines.push_back({d.offset, lex::message_of(d.kind).severity, lex::render(d.kind, d.args)});
  for (const engine::Diagnostic& d : p.diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<u64> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    lines.push_back({p.byte_offset(d.at), sema::message_of(*kind).severity,
                     sema::render(*kind, args, [&](char how, u64 raw) { return p.spell(how, raw); })});
  }
  std::ranges::stable_sort(lines, {}, &Line::offset);
  int errors = 0;
  for (const Line& l : lines) {
    auto [line, col] = line_column(text, l.offset);
    bool error = l.severity == lex::Severity::error;
    errors += error;
    std::println(err, "{}:{}:{}: {}: {}", path, line, col, error ? "error" : "warning", l.text);
  }
  return errors;
}

}  // namespace ycxx::parse
