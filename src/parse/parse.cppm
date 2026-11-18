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
import ycxx.ir;
import ycxx.irgen;
import ycxx.interp;
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
      : w_(&w), r_(&r), items_(&its), item_cells_(its.items.size()), body_cells_(its.items.size()),
        class_cells_(its.items.size()) {
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
  // `class_def(C)` (ast.md §3.6) of the class item `item`: the records of its members, each its
  // own item, analysed at the class's prefix (§4.2). It also fills the world's class table, which
  // member lookup and the layout read until #33 has them.
  const Result<ast::ClassDef>& class_def(u32 item);

  // Whether class `c` is complete at `item` ([class.mem.general]): its definition is an earlier
  // item that does not enclose `item`.
  // A function body inside the class is a complete-class context ([class.mem.general]); past
  // the closing brace of the class an item defines (`own`), that class is complete too.
  bool is_complete(EntityId c, u32 item, bool body = false, bool own = false) {
    auto it = class_items_.find(std::to_underlying(c));
    if (it == class_items_.end()) return false;
    u32 d = it->second;
    bool inside = item > d && item <= last_member(d);
    if ((item < d) || (item == d && !own) || (inside && !body)) return false;
    class_def(d);
    return w_->classes.is_defined(c);
  }
  // The last item nested in `k`, or `k` (items are in preorder).
  u32 last_member(u32 k) const {
    while (!items_->items[k].members.empty()) k = items_->items[k].members.back();
    return k;
  }

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

  // What item_sema learns of each entity, for constant evaluation (§B7): its declarations, the
  // constant a variable usable in constant expressions was initialized with ([expr.const]), and a
  // function's defining item, each with the item that said it. An answer at `item` sees only what
  // items up to it said, so it does not depend on which other cells have run (§A1).
  void declared(const ast::DeclSema& d, u32 item) {
    decls_[std::to_underlying(d.entity)].push_back({item, d});
    if (d.kind == ast::DeclKind::function && (d.flags & sema::decl_flag::definition))
      definitions_[std::to_underlying(d.entity)] = item;
    if (d.kind == ast::DeclKind::class_ && (d.flags & sema::decl_flag::definition)) {
      class_items_.emplace(std::to_underlying(d.entity), item);
      class_entities_.emplace(item, d.entity);
      class_heads_.emplace(item, d);
    }
  }
  void set_constant(EntityId e, ConstId c, u32 item) { constants_[std::to_underlying(e)] = {item, c}; }

  std::optional<ast::DeclSema> declaration(EntityId e, u32 item) const {
    auto it = decls_.find(std::to_underlying(e));
    if (it == decls_.end()) return std::nullopt;
    std::optional<ast::DeclSema> r;
    for (const auto& [k, d] : it->second)
      if (k <= item) r = d;
    return r;
  }
  std::optional<ConstId> constant_value(EntityId e, u32 item) const {
    auto it = constants_.find(std::to_underlying(e));
    if (it == constants_.end() || it->second.first > item) return std::nullopt;
    return it->second.second;
  }

  // `function_ir(F)` for a function defined in an item before `item` ([expr.const]: a function
  // defined later is not defined at the point of evaluation).
  interp::Callee function_ir(EntityId f, u32 item);
  // A namespace-scope variable as the interpreter sees it at `item`: its type and, if it is usable
  // in constant expressions, its value.
  std::optional<interp::GlobalVar> global(EntityId v, u32 item) const {
    auto d = declaration(v, item);
    if (!d || d->kind != ast::DeclKind::variable) return std::nullopt;
    auto c = constant_value(v, item);
    return interp::GlobalVar{d->type, c, c.has_value()};
  }
  bool is_constexpr(EntityId f, u32 item) const {
    auto d = declaration(f, item);
    return d && (d->flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_));
  }

  // The type of `this` in a non-static member function ([expr.prim.this]): a pointer to its class,
  // with the function's cv-qualifiers.
  std::optional<QualType> this_type(const ast::DeclSema& f) {
    if (f.kind != ast::DeclKind::function || !(f.flags & sema::decl_flag::member) || (f.flags & sema::decl_flag::static_))
      return std::nullopt;
    EntityId c = w_->entities.rec(f.entity).scope;
    return w_->types.pointer(w_->types.add_cv(w_->types.class_type(c), w_->types.rec(f.type).fn_cv));
  }

  // Every item and body analysed, in item order; the diagnostics of each cell in order.
  void analyze_all() {
    for (u32 k = 0; k < items_->items.size(); ++k) {
      // Members of a class or namespace wait for those (M2); the enclosing item was diagnosed.
      if (!in_global_scope(k)) continue;
      item_sema(k);
      if (items_->items[k].kind == ItemKind::class_def) {
        class_def(k);
        // The members' function bodies, complete-class contexts.
        for (u32 m : items_->items[k].members)
          if (items_->items[m].kind == ItemKind::function && items_->items[m].has_body()) analyze_body(m);
      }
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
      if (class_cells_[k]) r.insert(r.end(), class_cells_[k]->diags.begin(), class_cells_[k]->diags.end());
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
    return r_->site(tok).offset;  // a macro's token: where it was expanded
  }

  // Spells a diagnostic argument: a name, a type, a string literal or why an evaluation failed.
  std::string spell(char how, u64 raw) const {
    if (how == 'n') return std::string(w_->ins.names.spelling(ast::NameId(u32(raw))));
    if (how == 'r') return sema::spell_reason(raw);
    if (how == 'c') {
      std::string t = w_->values.rec(ConstId(u32(raw))).units;
      if (!t.empty() && t.back() == '\0') t.pop_back();  // the terminating null
      return t;
    }
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
  std::vector<std::optional<Result<ast::ClassDef>>> class_cells_;
  std::map<u32, u32> class_items_;     // by class entity: its defining item
  std::map<u32, EntityId> class_entities_;  // by class item: the class it defines
  std::map<u32, ast::DeclSema> class_heads_;  // by class item: the class's record, for its members

public:
  // The class a class item defines, none if its head failed. The class's own declarators ask it
  // while its item is analysed, so it is read from what the item declared before them.
  EntityId class_entity(u32 item) {
    if (!class_entities_.contains(item)) item_sema(item);
    auto it = class_entities_.find(item);
    return it == class_entities_.end() ? EntityId::none : it->second;
  }

private:
  std::vector<u32> running_;
  std::map<u32, std::pair<u32, ConstId>> constants_;                // by entity: (item, value)
  std::map<u32, std::vector<std::pair<u32, ast::DeclSema>>> decls_;  // by entity: (item, record)
  std::map<u32, u32> definitions_;               // by function entity: its defining item
  std::map<u32, std::optional<irgen::FunctionIr>> irs_;  // by item: function_ir, none if it failed
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
  interp::Callee function_ir(EntityId f) override { return p_->function_ir(f, item_); }
  std::optional<interp::GlobalVar> global(EntityId v) override { return p_->global(v, item_); }
  bool is_constexpr(EntityId f) override { return p_->is_constexpr(f, item_); }
  bool is_complete(EntityId c) override { return p_->is_complete(c, item_, self_, own_class_complete); }
  std::optional<ConstId> constant_value(EntityId v) override { return p_->constant_value(v, item_); }
  std::optional<ast::DeclSema> earlier_member(ast::NameId name) override {
    const items::Item& it = p_->items().items[item_];
    if (it.parent == items::none || p_->items().items[it.parent].kind != ItemKind::class_def) return std::nullopt;
    std::optional<ast::DeclSema> r;
    for (u32 m : p_->items().items[it.parent].members) {
      if (m == item_) break;
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(p_->item_sema(m).value.decls))
        if (d.entity != EntityId::none && d.kind != ast::DeclKind::class_ && p_->world().entities.rec(d.entity).name == name)
          r = d;
    }
    return r;
  }
  // Past the closing brace of the class the item defines, that class is complete.
  bool own_class_complete = false;

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
    // An elaborated-type-specifier naming no class declares one in the namespace
    // ([basic.scope.pdecl]); its entity is the one its definition will have.
    ps_.declare_class = [this](NameId n) -> std::optional<QualType> {
      if (p_->items().items[k_].parent != items::none &&
          p_->items().items[p_->items().items[k_].parent].kind == ItemKind::class_def)
        return std::nullopt;
      return s_.types().class_type(class_entity(n));
    };
  }

  Result<ast::ItemSema> run() {
    bool member = it_->parent != items::none && p_->items().items[it_->parent].kind == ItemKind::class_def;
    switch (it_->kind) {
    case ItemKind::empty:
    case ItemKind::access_spec:
    case ItemKind::linkage_block: break;  // its members are items of their own
    case ItemKind::declaration:
      if (member) member_declaration();
      else declaration();
      break;
    case ItemKind::function:
      if (member) member_declaration();
      else declaration();
      break;
    case ItemKind::class_def:
      if (member) {
        s_.diag(0, Diag::not_supported, p_->region().tokens[it_->begin].kind);  // nested classes
        bad();
      } else {
        class_definition();
      }
      break;
    default:
      s_.diag(0, Diag::not_supported, p_->region().tokens[it_->begin].kind);
      bad();
      break;
    }
    NodeId root = tree_;
    if (root == NodeId::none) {
      std::vector<ast::Child> cs;
      for (const Ex& d : declarators_) cs.push_back(sema::Sema::at(d, 0));
      root = s_.node(ast::DeclGroup{b_.list(cs)}, 0, it_->decl_length()).id;
    }
    ast::ItemSema v{root, b_.seq(std::span<const ast::DeclSema>(decls_))};
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
    if (ps_.is(Tk::kw_static_assert)) {
      Ex sa = ps_.static_assert_declaration();
      tree_ = sa.id;
      if (!ps_.at_end()) s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
      return;
    }
    if (!ps_.starts_declaration()) {
      if (lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon))
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // templates, using, static_assert, …
      else
        s_.diag(ps_.pos(), Diag::expected_declaration);
      return bad();
    }
    DeclSpec spec = ps_.decl_specifiers();
    if (spec.elaborated && ps_.is(Tk::semi)) {
      // `class-key identifier ;` declares the class ([dcl.type.elab]); it stays incomplete until
      // its definition.
      ps_.bump();
      if (spec.ok) decls_.push_back(class_decl(s_.types().class_of(spec.type), false));
      if (!spec.ok || sema::Types::cv(spec.type) != sema::Cv::none || spec.is_static || spec.is_extern) bad();
      return;
    }
    bool is_function_def = it_->kind == ItemKind::function;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false);
      if (!d.ok || !spec.ok || !d.named || !declare(spec, d, lang, explicit_lang, is_function_def)) {
        // Later declarators keep their skeleton ordinals with placeholder records.
        bad();
        if (!is_function_def) ps_.skip_to_semi();
        return;
      }
    } while (!is_function_def && ps_.accept(Tk::comma));
    if (!is_function_def) {
      if (!ps_.expect(Tk::semi)) bad();
    } else if (!ps_.at_end()) {
      if (ps_.is(Tk::identifier)) s_.diag(ps_.pos(), Diag::unexpected_identifier, ps_.name_of(ps_.tok()));
      else s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
    }
  }

  // The entity of the namespace-scope class named `n` (§4.3: scope, name).
  EntityId class_entity(NameId n) {
    return s_.world().entities.entity({sema::EntityKind::class_, n, {}, LangLinkage::cxx, ast::Linkage::external, {}});
  }
  ast::DeclSema class_decl(EntityId e, bool definition) {
    ast::DeclSema sem{};
    sem.entity = e;
    sem.type = s_.types().class_type(e);
    sem.kind = ast::DeclKind::class_;
    sem.linkage = ast::Linkage::external;
    sem.storage = ast::Storage::none;
    sem.access = ast::Access::none;
    sem.flags = definition ? sema::decl_flag::definition : 0;
    sem.attributes = b_.seq(std::span<const NodeId>{});
    sem.constraint = NodeId::none;
    sem.contracts = NodeId::none;
    sem.params = b_.seq(std::span<const ast::ParamSema>{});
    return sem;
  }

  // A class definition at namespace scope ([class.pre]): `class-key identifier { … }`, then
  // its declarators. The member-specification is the members' items (§4.2); `class_def` reads
  // them.
  void class_definition() {
    u32 at = ps_.pos();
    Tk key = ps_.kind();
    if (key == Tk::kw_union || (key != Tk::kw_struct && key != Tk::kw_class)) {
      s_.diag(at, Diag::not_supported, key);  // unions, and specifiers before the class-key
      return bad();
    }
    ps_.bump();
    if (!ps_.is(Tk::identifier)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // unnamed classes, attributes
      return bad();
    }
    NameId name = ps_.name_of(ps_.tok());
    u32 name_at = ps_.pos();
    ps_.bump();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // base classes, `final`
      return bad();
    }
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->kind == ast::DeclKind::class_ && (ds->flags & sema::decl_flag::definition)) {
        s_.diag(name_at, Diag::class_redefinition, name);
        return bad();
      }
    ast::DeclSema sem = class_decl(class_entity(name), true);
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    // The declarators after the closing brace: `struct S { … } s, *p;`, where S is complete.
    ps_.seek(it_->brace_end);
    q_.own_class_complete = true;
    if (ps_.accept(Tk::semi)) return;
    DeclSpec spec;
    spec.begin = at;
    spec.type = sem.type;
    spec.has_type = true;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false);
      if (!d.ok || !d.named || !declare(spec, d, LangLinkage::cxx, false, false)) {
        bad();
        ps_.skip_to_semi();
        return;
      }
    } while (ps_.accept(Tk::comma));
    if (!ps_.expect(Tk::semi)) bad();
  }

  // A member-declaration ([class.mem.general]) of the class item enclosing this one: non-static
  // data members, so far.
  void member_declaration() {
    EntityId ce = p_->class_entity(it_->parent);
    if (ce == EntityId::none) return bad();
    ast::DeclSema c = class_decl(ce, true);
    s_.current_class = ce;  // names in a member declaration are looked up in the class first
    if (ps_.accept(Tk::semi)) return;
    if (!Parser::starts_decl_specifier(ps_.kind()) && !ps_.starts_declaration()) {
      s_.diag(ps_.pos(), lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon) || ps_.is(Tk::tilde)
                             ? Diag::not_supported
                             : Diag::expected_declaration,
              ps_.kind());
      return bad();
    }
    DeclSpec spec = ps_.decl_specifiers();
    if (spec.is_extern || spec.is_constinit) {
      s_.diag(spec.begin, Diag::not_supported, spec.is_extern ? Tk::kw_extern : Tk::kw_constinit);
      return bad();
    }
    if (!spec.ok) return bad();
    if (spec.elaborated && ps_.is(Tk::semi)) {
      s_.diag(spec.begin, Diag::not_supported, Tk::kw_struct);  // a member class declaration
      return bad();
    }
    sema::Types& types = s_.types();
    do {
      parse::Declarator d = ps_.declarator(spec.type, false);
      if (!d.ok || !d.named) return bad();
      if (d.unknown_bound) {
        s_.diag(d.name_at, Diag::unknown_bound_member);  // [class.mem.general]: a complete type
        return bad();
      }
      if (d.qualified) {
        s_.diag(d.qualifier_at, Diag::not_supported, Tk::coloncolon);
        return bad();
      }
      if (types.is_function(d.type) || spec.is_static) {
        if (!member_function_or_static(spec, d, c)) return bad();
        if (it_->kind == ItemKind::function) return;  // its body follows the declaration part
        continue;
      }
      if (ps_.is(Tk::colon) || spec.is_inline || spec.is_constexpr || spec.is_consteval) {
        // Bit-fields; inline or constexpr non-static data members are ill-formed.
        s_.diag(ps_.is(Tk::colon) ? ps_.pos() : spec.begin, Diag::not_supported, ps_.is(Tk::colon) ? Tk::colon : Tk::kw_constexpr);
        return bad();
      }
      // [class.mem.general]: a non-static data member has a complete object type; the class is
      // incomplete inside its own member-specification.
      if (types.is_void(d.type)) {
        s_.diag(d.name_at, Diag::void_variable, d.name);
        return bad();
      }
      if (!s_.require_complete(d.type, d.name_at)) return bad();
      if (member_declared(d.name)) {
        s_.diag(d.name_at, Diag::redeclared_member, d.name);
        return bad();
      }
      // A default member initializer ([class.mem.general]): kept as its value, which must be a
      // constant of scalar type so far.
      Ex init;
      ConstId value = ConstId::none;
      if (ps_.is(Tk::equal) || ps_.is(Tk::l_brace)) {
        u32 at = ps_.pos();
        if (ps_.is(Tk::l_brace) || ps_.kind(1) == Tk::l_brace) {
          ps_.accept(Tk::equal);
          init = s_.list_init(d.type, *ps_.braced_init_list());
        } else {
          ps_.bump();
          Ex e = ps_.assignment_expression();
          init = s_.convert(e, d.type, e.begin);
        }
        if (!init.ok()) return bad();
        auto v = types.is_scalar(d.type) ? constant_initializer(init) : std::nullopt;
        if (!v) {
          s_.diag(at, Diag::not_supported, Tk::equal);  // a default member initializer that is not a constant
          return bad();
        }
        value = *v;
      }
      ast::DeclSema sem = class_decl(c.entity, false);
      sem.kind = ast::DeclKind::data_member;
      sem.type = d.type;
      sem.linkage = ast::Linkage::none;
      sem.flags = sema::decl_flag::definition;
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::data_member, d.name, d.type, LangLinkage::cxx, ast::Linkage::none, {}, c.entity});
      s_.own.push_back({d.name, sem});
      declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
      decls_.push_back(sem);
    } while (ps_.accept(Tk::comma));
    if (!ps_.expect(Tk::semi)) bad();
  }

  // A qualified declarator `C::m` outside its class: the definition of a member function or a
  // static data member C declares ([class.mfct], [class.static.data]), with its entity and flags.
  bool define_member(const DeclSpec& spec, const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition,
                     bool has_init) {
    if (spec.is_static) {
      s_.diag(spec.begin, Diag::static_outside_class);  // [class.static.mfct], [class.static.data]
      return false;
    }
    auto t = s_.class_name(d.qualifier, true);
    if (!t) {
      s_.diag(d.qualifier_at, Diag::not_a_type, d.qualifier);
      return false;
    }
    if (!s_.require_complete(*t, d.qualifier_at)) return false;
    const sema::ClassRec* cr = s_.world().classes.find(s_.types().class_of(*t));
    const sema::MemberDeclRec* m = nullptr;
    for (const sema::MemberDeclRec& r : cr->members)
      if (r.name == d.name && r.is_function == fn && s_.types().canonical(r.type) == s_.types().canonical(sem.type)) m = &r;
    if (!m || (!fn && !m->is_static)) {
      s_.diag(d.name_at, Diag::no_member_declared, d.name, *t);
      return false;
    }
    auto mem = p_->decl_sema(m->decl);
    if (!mem) return false;
    // [class.mfct]/1, [class.static.data]/3: outside its class a member is only defined, once.
    if (!definition) {
      s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
      return false;
    }
    // [depr.static.constexpr]: a constexpr static data member may be redeclared without an
    // initializer; it stays defined in its class.
    if (!fn && (mem->flags & sema::decl_flag::constexpr_) && !has_init) {
      sem.flags = u16((sem.flags & ~sema::decl_flag::definition) | sema::decl_flag::member | sema::decl_flag::static_);
      sem.entity = m->entity;
      sem.storage = mem->storage;
      return true;
    }
    // [basic.def.odr]: one definition, in the class or after it.
    auto latest = p_->declaration(m->entity, k_);
    if ((mem->flags & sema::decl_flag::definition) || (latest && (latest->flags & sema::decl_flag::definition))) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    constexpr u16 kept = sema::decl_flag::member | sema::decl_flag::static_ | sema::decl_flag::constexpr_ |
                         sema::decl_flag::consteval_;
    sem.flags = u16((sem.flags & ~kept) | (mem->flags & kept));
    sem.entity = m->entity;
    sem.linkage = mem->linkage;
    sem.storage = mem->storage;
    return true;
  }

  // A member function or static data member of class `c` declared in its class ([class.mfct],
  // [class.static]).
  bool member_function_or_static(const DeclSpec& spec, parse::Declarator& d, ast::DeclSema c) {
    sema::Types& types = s_.types();
    bool fn = types.is_function(d.type);
    bool is_def = it_->kind == ItemKind::function;
    ast::DeclSema sem = class_decl(c.entity, false);
    sem.type = d.type;
    sem.linkage = ast::Linkage::external;
    u16 flags = sema::decl_flag::member;
    if (spec.is_static) flags |= sema::decl_flag::static_;
    if (spec.is_constexpr) flags |= sema::decl_flag::constexpr_;
    if (spec.is_consteval) flags |= sema::decl_flag::consteval_;
    if (spec.is_inline || spec.is_constexpr || spec.is_consteval || is_def) flags |= sema::decl_flag::inline_;
    if (member_declared(d.name)) {
      s_.diag(d.name_at, fn ? Diag::overload_not_supported : Diag::redeclared_member, d.name);
      return false;
    }
    Ex init;
    ConstId value = ConstId::none;
    if (fn) {
      const sema::TypeRec& r = types.rec(d.type);
      if (spec.is_static && r.fn_cv != sema::Cv::none) {
        s_.diag(d.name_at, Diag::cv_on_non_member);  // [class.static.mfct]/2
        return false;
      }
      if (ps_.is(Tk::equal)) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::equal);  // pure, defaulted and deleted functions
        return false;
      }
      sem.kind = ast::DeclKind::function;
      if (is_def) flags |= sema::decl_flag::definition;
      if (r.variadic) flags |= sema::decl_flag::variadic;
      std::vector<ast::ParamSema> params;
      for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, NodeId::none, 0});
      sem.params = b_.seq(std::span<const ast::ParamSema>(params));
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::function, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
    } else {
      // A static data member: a declaration, unless inline or constexpr ([class.static.data]).
      // One that is const integral or constexpr may give its value in the class, and is then
      // usable in constant expressions ([expr.const]).
      sem.kind = ast::DeclKind::variable;
      sem.storage = ast::Storage::static_;
      if (spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);
      sem.type = d.type;
      if (spec.is_inline || spec.is_constexpr) flags |= sema::decl_flag::definition;
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::variable, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
      if (ps_.accept(Tk::equal)) {
        Ex e = ps_.assignment_expression();
        init = s_.convert(e, d.type, e.begin);
        if (!init.ok()) return false;
        bool usable = spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                            types.is_integral(d.type));
        sema::Evaluated v = usable ? sema::Evaluator(s_, q_).evaluate(init) : sema::Evaluated{};
        if (!v.value) {
          s_.diag(e.begin, usable ? Diag::not_constant : Diag::not_supported, usable ? v.reason : u64(Tk::equal));
          return false;
        }
        value = *v.value;
        p_->set_constant(sem.entity, value, k_);
      } else if (spec.is_constexpr) {
        s_.diag(d.name_at, Diag::constexpr_uninitialized, d.name);
        return false;
      }
    }
    sem.flags = flags;
    s_.own.push_back({d.name, sem});
    p_->declared(sem, k_);
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
    decls_.push_back(sem);
    return true;
  }

  // Whether the class already has a member named `n`: in this item, or an earlier member item
  // ([class.mem.general]: a member is declared once).
  bool member_declared(NameId n) {
    for (const auto& o : s_.own)
      if (o.name == n) return true;
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_) break;
      const ast::ItemSema& ms = p_->item_sema(m).value;
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(ms.decls))
        if (d.entity != EntityId::none && d.kind != ast::DeclKind::class_ && p_->world().entities.rec(d.entity).name == n)
          return true;
    }
    return false;
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
  // One init-declarator at namespace scope; false after an error that leaves no record.
  bool declare(const DeclSpec& spec, parse::Declarator d, LangLinkage lang, bool explicit_lang,
               bool is_function_def) {
    sema::Types& types = s_.types();
    u32 ord = static_cast<u32>(decls_.size());
    // `T a[] = {…}`: the bound from the list ([dcl.array]/8), parsed here and used below.
    std::unique_ptr<sema::Braced> early;
    if (d.unknown_bound) {
      if (!(ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
        s_.diag(d.name_at, Diag::not_supported, Tk::l_square);  // an array of unknown bound
        return false;
      }
      ps_.accept(Tk::equal);
      early = ps_.braced_init_list();
      u64 n = s_.deduce_bound(d.type, *early);
      if (n == 0) {
        s_.diag(early->begin, Diag::bound_not_constant);
        return false;
      }
      d.type = types.array(d.type, n);
    }
    bool fn = types.is_function(d.type);
    if (!fn && spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);  // [dcl.constexpr]
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
    if (spec.is_constinit && fn) {
      s_.diag(spec.begin, Diag::constinit_function);  // [dcl.constinit]/1
      return false;
    }
    if (spec.is_consteval && !fn) {
      s_.diag(spec.begin, Diag::consteval_not_function);  // [dcl.constexpr]/1
      return false;
    }
    // [dcl.constexpr]/1: a constexpr or consteval function is inline; a constexpr variable const.
    if (fn && (spec.is_constexpr || spec.is_consteval)) flags |= sema::decl_flag::inline_;
    if (spec.is_consteval) flags |= sema::decl_flag::consteval_;

    bool has_init = early || ps_.is(Tk::equal) || ps_.is(Tk::l_brace) || (!fn && ps_.is(Tk::l_paren));
    bool definition = fn ? is_function_def : !(spec.is_extern && !has_init);
    if (definition) flags |= sema::decl_flag::definition;
    if (!fn && types.is_void(d.type)) {
      s_.diag(d.name_at, Diag::void_variable, d.name);
      return false;
    }
    if (!fn && p_->world().ins.names.spelling(d.name) == "main") {
      s_.diag(d.name_at, Diag::main_not_function);  // [basic.start.main]/3
      return false;
    }

    // Earlier declarations of the name: in this item, then before it ([basic.link], [dcl.fct]). A
    // qualified name redeclares a member of its class instead.
    std::vector<ast::DeclSema> prior;
    // A class and a variable or function may share a name; the class is hidden
    // ([basic.scope.scope]/4, [basic.scope.hiding]/2), so it is not an earlier declaration.
    if (!d.qualified) {
      for (DeclId p : q_.visible_decls(d.name))
        if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && ds->kind != ast::DeclKind::class_)
          prior.push_back(*ds);
      for (const auto& o : s_.own)
        if (o.name == d.name && o.sema.kind != ast::DeclKind::class_) prior.push_back(o.sema);
    }
    if (fn) {
      if (!explicit_lang && !prior.empty() && prior.back().kind == ast::DeclKind::function)
        lang = s_.world().entities.rec(prior.back().entity).lang;  // [dcl.link]/6: inherited
      const sema::TypeRec& r = types.rec(d.type);
      if (r.fn_cv != sema::Cv::none && !d.qualified) {
        s_.diag(d.name_at, Diag::cv_on_non_member);  // [dcl.fct]/6
        return false;
      }
      std::vector<QualType> ps = r.params;
      sem.type = types.function(r.inner, ps, r.variadic, lang, r.fn_cv);
      if (lang == LangLinkage::c) flags |= sema::decl_flag::lang_c;
      if (r.variadic) flags |= sema::decl_flag::variadic;
      std::vector<ast::ParamSema> params;
      for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, NodeId::none, 0});
      sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    } else {
      sem.params = b_.seq(std::span<const ast::ParamSema>{});
    }
    sem.flags = flags;

    if (d.qualified) {
      if (!define_member(spec, d, sem, fn, definition, has_init)) return false;
      // The rest of the declarator, its initializer included, is in the class's scope
      // ([basic.scope.class]).
      s_.current_class = s_.world().entities.rec(sem.entity).scope;
    } else if (!prior.empty()) {
      const ast::DeclSema& p = prior.back();
      if ((p.kind == ast::DeclKind::function) != fn) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return false;
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
        return false;
      }
      if (!fn && p.type != sem.type) {  // [basic.link]/11: the same type, cv included
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return false;
      }
      // [dcl.stc]: a later declaration may not give internal linkage to a name with external.
      if (spec.is_static && p.linkage == ast::Linkage::external) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return false;
      }
      sem.linkage = p.linkage;
      // [dcl.constexpr]/1: every declaration of a function says constexpr, or consteval, or neither.
      if (fn && (p.flags & sema::decl_flag::consteval_) != (flags & sema::decl_flag::consteval_)) {
        s_.diag(d.name_at, Diag::consteval_redeclared, d.name);
        return false;
      }
      if (fn && (p.flags & sema::decl_flag::constexpr_) != (flags & sema::decl_flag::constexpr_)) {
        s_.diag(d.name_at, Diag::constexpr_redeclared, d.name);
        return false;
      }
      if (definition && std::ranges::any_of(prior, [](const ast::DeclSema& o) { return o.flags & sema::decl_flag::definition; })) {
        s_.diag(d.name_at, Diag::redefinition, d.name);
        return false;
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
    p_->declared(sem, k_);
    Ex init;
    sema::ConstId value = sema::ConstId::none;
    if (fn && has_init && !is_function_def) {
      s_.diag(ps_.pos(), Diag::function_initialized, d.name);
      ps_.skip_to_semi();
    } else if (!fn && (early || ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
      // List-initialization; constant initialization when every element is a constant
      // ([basic.start.static]).
      ps_.accept(Tk::equal);
      auto b = early ? std::move(early) : ps_.braced_init_list();
      init = s_.list_init(d.type, *b);
      if (init.ok()) {
        if (auto v = constant_initializer(init)) {
          value = *v;
          if (spec.is_constexpr) p_->set_constant(sem.entity, *v, k_);
        } else {
          s_.diag(b->begin, spec.is_constexpr || spec.is_constinit ? Diag::not_constant : Diag::nonconstant_initializer,
                  sema::reason_unsupported);
        }
      }
    } else if (!fn && ps_.accept(Tk::equal)) {
      if (types.is_array(d.type)) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        ps_.skip_to_semi();
      } else {
        Ex e = ps_.assignment_expression();
        init = s_.convert(e, d.type, e.begin);
        if (init.ok()) {
          // [basic.start.static]: constant initialization, or dynamic, which the α subset lacks.
          // A constexpr or constinit variable must be constant-initialized ([dcl.constexpr],
          // [dcl.constinit]); a constexpr one, and a const integral one that is, are usable in
          // constant expressions ([expr.const]).
          sema::Evaluated v = sema::Evaluator(s_, q_).evaluate(init);
          if (v.value) {
            value = *v.value;
            if (spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                      types.is_integral(d.type)))
              p_->set_constant(sem.entity, *v.value, k_);
          } else {
            s_.diag(e.begin, spec.is_constexpr || spec.is_constinit ? Diag::not_constant : Diag::nonconstant_initializer,
                    v.reason);
          }
        }
      }
    } else if (!fn && ps_.is(Tk::l_paren)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      ps_.skip_to_semi();
    } else if (!fn && definition && !s_.require_complete(d.type, d.name_at)) {
      // A definition needs a complete object type ([basic.def]).
    } else if (!fn && definition && s_.has_member_inits(d.type)) {
      // Zero-initialization, then the default member initializers ([basic.start.static]).
      value = *constant_initializer(s_.default_init(d.type, d.name_at, ps_.pos()));
    } else if (!fn && d.qualified) {
      // [class.static.data]/4: the definition of a member initialized in its class repeats no
      // initializer, and has the class's value.
      if (auto v = p_->constant_value(sem.entity, k_)) value = *v;
      else if (sema::Types::is_const(d.type)) s_.diag(d.name_at, Diag::const_uninitialized, d.name);
    } else if (!fn && spec.is_constexpr) {
      s_.diag(d.name_at, Diag::constexpr_uninitialized, d.name);
    } else if (!fn && sema::Types::is_const(d.type) && !spec.is_extern) {
      s_.diag(d.name_at, Diag::const_uninitialized, d.name);
    }
    declarators_.push_back(s_.node(ast::Declarator{ord, sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
    decls_.push_back(sem);
    return true;
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
    if (spec.is_constexpr) s_.diag(spec.begin, Diag::main_specifier, Tk::kw_constexpr);
    if (spec.is_consteval) s_.diag(spec.begin, Diag::main_specifier, Tk::kw_consteval);
    if (lang == LangLinkage::c) s_.diag(d.name_at, Diag::main_linkage);
  }

  // The constant an analysed initializer gives its object, if it is a constant expression
  // ([expr.const]): a list-initialized aggregate's elements one by one, each a constant.
  std::optional<ConstId> constant_initializer(Ex e) {
    if (s_.as<ast::InitList>(e)) {
      std::vector<ConstId> elems;
      for (const Ex& sub : s_.list_elements(e)) {
        auto v = constant_initializer(sub);
        if (!v) return std::nullopt;
        elems.push_back(*v);
      }
      return s_.world().values.aggregate(e.type(), elems);
    }
    if (s_.as<ast::ValueInit>(e) || s_.as<ast::DefaultInit>(e)) return zero_constant(e.type());
    if (s_.types().is_class(e.type())) return std::nullopt;  // a copy: dynamic initialization
    return sema::Evaluator(s_, q_).evaluate(e).value;
  }
  // The value of a zero-initialized object of type `t` ([dcl.init.general]).
  std::optional<ConstId> zero_constant(QualType t) {
    if (s_.is_aggregate(t)) {
      std::vector<ConstId> elems;
      for (const sema::Sema::Element& e : s_.elements(t)) {
        auto v = zero_constant(e.type);
        if (!v) return std::nullopt;
        elems.push_back(*v);
      }
      return s_.world().values.aggregate(t, elems);
    }
    if (s_.types().is_pointer(t)) return s_.world().values.null_pointer(t);
    if (s_.types().is_integral(t)) return s_.world().values.integer(sema::Types::unqualified(t), 0);
    return std::nullopt;
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
  NodeId tree_ = NodeId::none;  // the root when it is not a DeclGroup (a static_assert)
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
  // The members of a class whose item is still being analysed (its declarators after the
  // brace ask for the class's completeness) see its head, which was declared first.
  if (u32 k = index(d.item); std::ranges::find(running_, k) != running_.end()) {
    if (ast::number_of(d.ord) == 0 && class_heads_.contains(k)) return class_heads_.at(k);
    return std::nullopt;
  }
  const ast::ItemSema& s = item_sema(index(d.item)).value;
  u32 n = ast::number_of(d.ord);
  if (n >= w_->ins.seqs.size(s.decls)) return std::nullopt;
  return w_->ins.seqs.at(s.decls, n);
}

// ---------------------------------------------------------------- class_def

const Result<ast::ClassDef>& Program::class_def(u32 item) {
  if (class_cells_[item]) return *class_cells_[item];
  u32 key = item | 0x80000000u;  // apart from item_sema(item), which may be running and ask for it
  contract_assert(std::ranges::find(running_, key) == running_.end());
  running_.push_back(key);
  Result<ast::ClassDef> out;
  ast::ClassDef def{};
  def.injected_class_name = EntityId::none;
  std::vector<ast::MemberRec> members;
  std::optional<EntityId> cls = class_entity(item);
  const items::Item& it = items_->items[item];
  // [class.access.spec]: members of a class defined with `class` are private by default.
  ast::Access access = r_->tokens[it.begin].kind == lex::Tk::kw_class ? ast::Access::private_ : ast::Access::public_;
  std::vector<sema::FieldRec> fields;
  std::vector<sema::MemberDeclRec> statics;
  for (u32 m : it.members) {
    const items::Item& mi = items_->items[m];
    if (mi.kind == ItemKind::access_spec) {
      lex::Tk k = r_->tokens[mi.begin].kind;
      access = k == lex::Tk::kw_public ? ast::Access::public_ : k == lex::Tk::kw_protected ? ast::Access::protected_
                                                                                          : ast::Access::private_;
      continue;
    }
    const ast::ItemSema& ms = item_sema(m).value;
    // The default member initializers' values, by declaration ordinal.
    std::map<u32, ConstId> inits;
    if (ms.tree != ast::NodeId::none && w_->ins.ast.is<ast::DeclGroup>(ms.tree))
      for (ast::Child c : w_->ins.ast.children(w_->ins.ast.get<ast::DeclGroup>(ms.tree).declarators)) {
        auto dr = w_->ins.ast.get<ast::Declarator>(c.id);
        inits[dr.decl] = dr.value;
      }
    for (u32 i = 0; i < w_->ins.seqs.size(ms.decls); ++i) {
      ast::DeclSema d = w_->ins.seqs.at(ms.decls, i);
      if (d.entity == EntityId::none) continue;
      members.push_back({{id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.entity, access});
      if (d.kind == ast::DeclKind::data_member)
        fields.push_back({w_->entities.rec(d.entity).name, d.type, d.entity, inits.contains(i) ? inits[i] : ConstId::none});
      else if (d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable)
        statics.push_back({w_->entities.rec(d.entity).name, d.entity, d.type,
                           {id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.kind == ast::DeclKind::function,
                           (d.flags & sema::decl_flag::static_) != 0});
    }
  }
  if (cls && *cls != EntityId::none) {
    sema::ClassRec& rec = w_->classes.rec(*cls);
    if (!rec.defined) {
      rec.fields = std::move(fields);
      rec.members = std::move(statics);
      rec.defined = true;
    }
  }
  ast::Builder b(w_->ins, &arena_);
  def.bases = b.seq(std::span<const ast::BaseSpec>{});
  def.members = b.seq(std::span<const ast::MemberRec>(members));
  def.friends = b.seq(std::span<const ast::MemberRec>{});
  def.synthesized = b.seq(std::span<const ast::DeclSema>{});
  def.nested = b.seq(std::span<const EntityId>{});
  out.value = b.commit(def);
  running_.pop_back();
  class_cells_[item] = std::move(out);
  return *class_cells_[item];
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
      // A consteval function's body is an immediate function context ([expr.const]).
      if (f->flags & sema::decl_flag::consteval_) s_.immediate_context = 1;
      // A member function's body sees its class's members ([basic.scope.class]) and `this`.
      if (f->flags & sema::decl_flag::member) {
        s_.current_class = p_->world().entities.rec(f->entity).scope;
        s_.this_type = p_->this_type(*f);
      }
      // The parameters are the first locals, in their own scope ([basic.scope.param]).
      s_.push_scope();
      s_.guard_scope();
      for (const ast::ParamSema& prm : p_->world().ins.seqs.elements(f->params))
        s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, 0, false);
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

interp::Callee Program::function_ir(EntityId f, u32 item) {
  interp::Callee c;
  c.is_constexpr = is_constexpr(f, item);
  auto def = definitions_.find(std::to_underlying(f));
  if (def == definitions_.end() || def->second >= item) return c;
  u32 k = def->second;
  if (!irs_.contains(k)) {
    // A body with errors has no L1: the evaluation stops at the call.
    const Result<ast::BodySema>& body = analyze_body(k);
    std::optional<irgen::FunctionIr> ir;
    const ast::ItemSema& is = item_sema(k).value;
    if (body.diags.empty() && body.value.tree != NodeId::none && w_->ins.seqs.size(is.decls) > 0) {
      ast::DeclSema d = w_->ins.seqs.at(is.decls, 0);
      const items::Item& it = items_->items[k];
      irgen::FunctionInput input{d.entity, d, body.value, id(k), it.body_begin - it.begin};
      input.this_type = this_type(d);
      AtItem q(*this, k, true);
      sema::EvalFacts facts(*w_, q);
      if (auto r = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*r);
    }
    irs_.emplace(k, std::move(ir));
  }
  const auto& ir = irs_.at(k);
  if (!ir) c.unsupported = true;
  else c.body = &ir->body;
  return c;
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
  pp::Preprocessed pre = pp::preprocess(text, path);
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
  // Phase 4's diagnostics name their file.
  std::vector<pp::Message> pp_messages;
  for (const pp::Diag& d : pre.diags) {
    if (d.file == 0) lines.push_back({d.offset, lex::message_of(d.kind).severity, lex::render(d.kind, d.args)});
    else pp_messages.push_back(pp::message(pp::session(), pp::session().files.id(pre.region.files[d.file]), pre.region.file_names[d.file], d));
  }
  for (const pp::Message& m : pp_messages)
    std::println(err, "{}:{}:{}: {}: {}", m.file, m.line, m.column, m.error ? "error" : "warning", m.text);
  for (const engine::Diagnostic& d : p.diagnostics()) {
    auto kind = sema::sema_diag_of(d.id);
    if (!kind) continue;
    std::vector<u64> args;
    for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
    lines.push_back({p.byte_offset(d.at), sema::message_of(*kind).severity,
                     sema::render(*kind, args, [&](char how, u64 raw) { return p.spell(how, raw); })});
  }
  std::ranges::stable_sort(lines, {}, &Line::offset);
  int errors = static_cast<int>(std::ranges::count_if(pp_messages, &pp::Message::error));
  for (const Line& l : lines) {
    auto [line, col] = line_column(text, l.offset);
    bool error = l.severity == lex::Severity::error;
    errors += error;
    std::println(err, "{}:{}:{}: {}: {}", path, line, col, error ? "error" : "warning", l.text);
  }
  return errors;
}

}  // namespace ycxx::parse
