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
  d.constraints = ast::ConstId::none;
  d.contracts = ast::NodeId::none;
  return d;
}

// A cell's value and its diagnostics.
template <class V>
struct Result {
  V value{};
  std::vector<engine::Diagnostic> diags;
  std::vector<sema::Demand> demands;  // in program order (query-engine.md §B1.4)
};

class Program {
public:
  // `template_depth` is the limit on nested instantiations ([temp.inst]/16), `-ftemplate-depth`.
  Program(sema::World& w, const pp::Region& r, const items::Items& its, u32 template_depth = pp::Config::default_template_depth)
      : instantiation_limit(template_depth), w_(&w), r_(&r), items_(&its), item_cells_(its.items.size()), body_cells_(its.items.size()),
        class_cells_(its.items.size()) {
    w.install_builtin_classes(classes_);
    for (const items::Item& it : its.items) {
      ItemId id = w.ins.items.add<ItemId>(it.id);
      ids_.push_back(id);
      index_.emplace(std::to_underlying(id), static_cast<u32>(ids_.size() - 1));
    }
    for (u32 j = 0; j < its.items.size(); ++j)
      for (u32 k = 0; k < its.items[j].names.size(); ++k)
        if (!its.items[j].names[k].name.empty()) by_name_[its.items[j].names[k].name].push_back({j, k});
    namespaces();
  }

  // ------------------------------------------------------------ namespaces ([basic.namespace])
  //
  // A namespace is named by its path, `a::b`, the global one by "". Each item belongs to the
  // namespace whose definition encloses it (`ns_of`), and, for lookup, to that namespace with
  // its inline and unnamed components dropped (`eff_ns`): their members are found in the
  // enclosing namespace ([namespace.def]/7, [namespace.unnamed]).

  const std::string& ns_of(u32 k) const { return ns_of_[k]; }
  bool in_class(u32 k) const { return in_class_[k]; }
  // The entity of namespace `path`, none for the global one.
  EntityId namespace_entity(const std::string& path) {
    if (path.empty()) return EntityId::none;
    if (auto it = ns_entities_.find(path); it != ns_entities_.end()) return it->second;
    auto cut = path.rfind("::");
    std::string parent = cut == std::string::npos ? "" : path.substr(0, cut);
    std::string name = cut == std::string::npos ? path : path.substr(cut + 2);
    sema::EntityRec r{sema::EntityKind::namespace_, w_->ins.names.intern(name), {}, LangLinkage::cxx,
                      ast::Linkage::external, {}, namespace_entity(parent)};
    // Each unit's unnamed namespace is its own ([namespace.unnamed]): named by the unit's main file.
    if (name == "(anonymous)") r.head = ast::seed("ycxx.sema.unnamed-namespace:" + r_->files.at(0));
    EntityId e = w_->entities.entity(std::move(r));
    ns_entities_[path] = e;
    ns_paths_[std::to_underlying(e)] = path;
    return e;
  }
  std::string namespace_path(EntityId e) const {
    if (e == EntityId::none) return "";
    auto it = ns_paths_.find(std::to_underlying(e));
    return it == ns_paths_.end() ? "" : it->second;
  }
  bool is_namespace(EntityId e) const { return e == EntityId::none || ns_paths_.contains(std::to_underlying(e)); }
  // Whether `path` is inside an unnamed namespace, whose members have internal linkage
  // ([basic.link]/4).
  static bool unnamed(const std::string& path) { return path.find("(anonymous)") != std::string::npos; }

  // A using-directive at namespace scope ([namespace.udir]) or a namespace alias
  // ([namespace.alias]): where it is, and the namespace it names once resolved.
  struct Directive {
    u32 item;
    std::string at;
    std::optional<NameId> alias;  // an alias's name; none for a using-directive
    std::optional<std::string> target;
  };
  void set_directive_target(u32 item, std::string target) {
    for (Directive& d : directives_)
      if (d.item == item) d.target = std::move(target);
  }

  sema::World& world() { return *w_; }
  const pp::Region& region() const { return *r_; }
  const items::Items& items() const { return *items_; }
  ItemId id(u32 item) const { return ids_[item]; }
  u32 index(ItemId id) const { return index_.at(std::to_underlying(id)); }

  const Result<ast::ItemSema>& item_sema(u32 item);
  std::optional<ast::DeclSema> decl_sema(DeclId d);
  const Result<ast::BodySema>& analyze_body(u32 item);

  bool defaulted_body(u32 item) const {
    const auto& it = items_->items[item];
    return it.body_end - it.body_begin == 3 &&
           r_->tokens[it.body_begin].kind == lex::Tk::equal &&
           r_->tokens[it.body_begin + 1].kind == lex::Tk::kw_default &&
           r_->tokens[it.body_begin + 2].kind == lex::Tk::semi;
  }
  // An accepted in-class defaulted special member has a definition and no token body to analyse.
  bool defaulted_member_definition(u32 item, const ast::DeclSema& d) const {
    const auto& it = items_->items[item];
    if (d.entity == EntityId::none || !(d.flags & sema::decl_flag::member) ||
        it.parent == items::none || items_->items[it.parent].kind != ItemKind::class_def || !defaulted_body(item))
      return false;
    return (d.flags & sema::decl_flag::constructor) ||
           (d.is_virtual && (d.flags & sema::decl_flag::destructor)) ||
           (d.kind == ast::DeclKind::function && w_->ins.names.spelling(w_->entities.rec(d.entity).name) == "operator=");
  }
  // `class_def(C)` (ast.md §3.6) of the class item `item`: the records of its members, each its
  // own item, analysed at the class's prefix (§4.2). It also fills this unit's class table
  // (`classes`), which member lookup and the layout read until #33 has them.
  const Result<ast::ClassDef>& class_def(u32 item);
  // The records of the classes and enumerations this unit has analysed.
  const sema::Classes& classes() const { return classes_; }
  // The defining pattern selected for an instantiated class, including a partial specialization.
  std::optional<u32> class_pattern_item(EntityId c) const {
    auto it = class_patterns_.find(std::to_underlying(c));
    return it == class_patterns_.end() ? std::nullopt : std::optional(it->second);
  }
  // This unit's declaration of member `e` in its class, once the class is analysed here.
  std::optional<DeclId> member_decl(EntityId e) const {
    auto it = member_decls_.find(std::to_underlying(e));
    return it == member_decls_.end() ? std::nullopt : std::optional(it->second);
  }

  // Whether class `c` is complete at `item` ([class.mem.general]): its definition is an earlier
  // item that does not enclose `item`.
  // A function body inside the class is a complete-class context ([class.mem.general]); past
  // the closing brace of the class an item defines (`own`), that class is complete too.
  // Whether the class item `item` is a union: its class-head's class-key ([class.union]), the
  // last one before the brace (a template-head's `class T` comes earlier).
  bool class_rec_is_union(u32 item) const {
    for (u32 t = items_->items[item].brace_begin; t-- > items_->items[item].begin;)
      if (lex::Tk k = r_->tokens[t].kind; k == lex::Tk::kw_union || k == lex::Tk::kw_struct || k == lex::Tk::kw_class)
        return k == lex::Tk::kw_union;
    return false;
  }
  // Whether class `c` was declared with `union`, complete or not: the class-key of its first
  // declaration before `item` (a specialization's, its template's).
  bool is_union_class(EntityId c, u32 item) {
    if (EntityId t = w_->entities.rec(c).templ; t != EntityId::none) c = t;
    auto it = decls_.find(std::to_underlying(c));
    if (it == decls_.end() || it->second.empty() || it->second.front().first > item) return false;
    u32 k = it->second.front().first;
    const items::Item& i = items_->items[k];
    if (i.kind == ItemKind::class_def) return class_rec_is_union(k);
    for (u32 t = i.end; t-- > i.begin;)
      if (lex::Tk kk = r_->tokens[t].kind; kk == lex::Tk::kw_union || kk == lex::Tk::kw_struct || kk == lex::Tk::kw_class)
        return kk == lex::Tk::kw_union;
    return false;
  }
  // An entity's GNU asm label: its symbol as written (docs/EXTENSIONS.md); empty if it has none.
  std::string asm_label(EntityId e) const {
    auto it = asm_labels_.find(std::to_underlying(e));
    return it == asm_labels_.end() ? std::string() : it->second;
  }
  bool is_complete(EntityId c, u32 item, bool body = false, bool own = false) {
    EntityId templ = w_->entities.rec(c).templ;
    if (templ != EntityId::none) specializations_before(item);
    auto it = class_items_.find(std::to_underlying(c));
    // A class template specialization is instantiated where it must be complete ([temp.inst]/2),
    // after its template's definition, unless it is explicitly specialized ([temp.expl.spec]).
    if (it == class_items_.end()) {
      // Built-in classes are complete from the start. A closure type is complete once its lambda's
      // body ends, where its record is published.
      if (templ == EntityId::none && classes_.is_defined(c)) return true;
      if (templ == EntityId::none || explicitly_specialized(c, item)) return false;
      const sema::ClassRec* r = instantiate_class(c, item);
      return r && r->defined;
    }
    u32 d = it->second;
    bool inside = item > d && item <= last_member(d);
    if ((item < d) || (item == d && !own) || (inside && !body)) return false;
    if (!classes_.is_defined(c)) class_def(d);
    return classes_.is_defined(c);
  }
  // The last item nested in `k`, or `k` (items are in preorder).
  u32 last_member(u32 k) const {
    while (!items_->items[k].members.empty()) k = items_->items[k].members.back();
    return k;
  }

  // `visible_decls(::, name, at item)` (§B2): the global-namespace declarations of `name` in the
  // items before `item`, and in `item` itself when `self`.
  // Unqualified lookup ([basic.lookup.unqual]) of `name` at `item` (and in it, when `self`),
  // starting in namespace `from`: the first enclosing namespace that declares it before the
  // point, counting the namespaces using-directives nominate where [namespace.udir]/2 puts them.
  // `block` are the namespaces block-scope using-directives at the point nominate.
  std::vector<DeclId> visible_decls(std::string_view name, u32 item, bool self, const std::string& from,
                                    std::span<const std::string> block = {}) {
    // Each nominated namespace, and those its own directives nominate ([namespace.udir]/4), with
    // the namespace that encloses both it and the directive: where its names appear.
    std::vector<std::pair<std::string, std::string>> nominated;  // (namespace, level)
    auto add = [&](const std::string& at, const std::string& target) {
      for (const std::string& t : closure(target, item)) nominated.push_back({t, common_ancestor(at, t)});
    };
    for (const Directive& d : directives_) {
      if (!d.target && !d.alias && d.item < item) item_sema(d.item);  // resolves it
      if (!d.alias && d.item < item && d.target && encloses(d.at, from)) add(d.at, *d.target);
    }
    for (const std::string& t : block) add(from, t);
    for (std::string level = from;; level = parent_of(level)) {
      std::vector<DeclId> r = declared_in(name, level, item, self);
      for (const auto& [t, at] : nominated)
        if (at == level) {
          std::vector<DeclId> more = declared_in(name, t, item, self);
          r.insert(r.end(), more.begin(), more.end());
        }
      if (!r.empty() || level.empty()) return r;
    }
  }
  std::vector<DeclId> visible_decls(std::string_view name, u32 item, bool self) {
    return visible_decls(name, item, self, ns_of_[item]);
  }
  // `ns` and the namespaces its using-directives nominate before `item`, transitively.
  std::vector<std::string> closure(const std::string& ns, u32 item) {
    std::vector<std::string> out{ns};
    for (std::size_t i = 0; i < out.size(); ++i)
      for (const Directive& d : directives_) {
        if (!d.target && !d.alias && d.item < item) item_sema(d.item);
        if (!d.alias && d.item < item && d.target && d.at == out[i] && !std::ranges::contains(out, *d.target))
          out.push_back(*d.target);
      }
    return out;
  }
  // Qualified lookup in namespace `ns` ([namespace.qual]/2): its own declarations, or, when it has
  // none, those of the namespaces its using-directives nominate.
  std::vector<DeclId> qualified_decls(std::string_view name, const std::string& ns, u32 item, bool self) {
    std::vector<DeclId> r = declared_in(name, ns, item, self);
    if (!r.empty()) return r;
    for (const std::string& t : closure(ns, item))
      if (t != ns) {
        std::vector<DeclId> more = declared_in(name, t, item, self);
        r.insert(r.end(), more.begin(), more.end());
      }
    return r;
  }
  // What the using-declarations that inhabit namespace `path` before `item` introduce under
  // `name`; an inline or unnamed member namespace's are not among them ([basic.scope.scope]/4).
  std::vector<DeclId> used_in(std::string_view name, const std::string& path, u32 item) {
    std::vector<DeclId> r;
    for (auto [j, k] : named(name)) {
      if (j >= item) break;
      const items::DeclName& n = items_->items[j].names[k];
      if (in_class_[j] || ns_of_[j] != path) continue;
      if ((n.flags & items::name_flag::unknown) && std::ranges::find(running_, j) == running_.end()) {
        item_sema(j);
        if (auto t = using_targets_.find({j, n.ordinal}); t != using_targets_.end())
          r.insert(r.end(), t->second.begin(), t->second.end());
      }
    }
    return r;
  }
  // The names items declare under `name`, as (item, index in its names), in order.
  std::span<const std::pair<u32, u32>> named(std::string_view name) const {
    auto f = by_name_.find(name);
    return f == by_name_.end() ? std::span<const std::pair<u32, u32>>{} : std::span{f->second};
  }
  // [basic.scope.scope], [basic.link]: friend declarations can correspond in their target
  // namespace without binding the name there. Read earlier source items even if nothing else
  // has asked for the containing class yet.
  std::vector<DeclId> hidden_friends_before(std::string_view name, u32 item,
                                           std::optional<u32> enclosing_class = std::nullopt) {
    std::vector<DeclId> result;
    for (auto [j, k] : named(name)) {
      if (j >= item) break;
      const items::Item& candidate = items_->items[j];
      if (enclosing_class && candidate.parent != *enclosing_class) continue;
      const items::DeclName& n = candidate.names[k];
      if (!(n.flags & items::name_flag::friend_) || ns_of_[j] != ns_of_[item] || !namespace_class_friend(j)) continue;
      DeclId d{id(j), ast::make_decl_ord(ast::DeclClass::syntactic, n.ordinal)};
      if (decl_sema(d) && ordinary_friend_items_.contains(j)) result.push_back(d);
    }
    return result;
  }
  bool namespace_class_friend(u32 item) const {
    u32 parent = items_->items[item].parent;
    return parent != items::none && items_->items[parent].kind == ItemKind::class_def &&
           in_global_scope(parent) && items_->items[parent].template_depth == 0;
  }
  // The declarations of `name` that are members of namespace `path` ([namespace.memdef]) before
  // the point: of its own definitions, and, unless `inhabit`, of its inline and unnamed members.
  // A using-declaration's name stands for what it introduces (query-engine.md, "Using-declarations").
  // The ADL search uses `bound_functions` to check where direct declarations bind their names.
  std::vector<DeclId> declared_in(std::string_view name, const std::string& path, u32 item, bool self,
                                  bool inhabit = false, bool bound_functions = false) {
    std::vector<DeclId> r;
    for (auto [j, k] : named(name)) {
      if (j >= item + (self ? 1 : 0)) break;
      const items::Item& it = items_->items[j];
      const items::DeclName& n = it.names[k];
      const bool nested = it.kind == ItemKind::namespace_def;  // only these have per-name scopes
      const std::string& ns = nested ? name_ns(j, k) : ns_of_[j];
      if (in_class_[j] || (ns != path && (inhabit || (nested ? name_eff_ns(j, k) : eff_ns_[j]) != path))) continue;
      if (n.flags & items::name_flag::unknown) {
        if (std::ranges::find(running_, j) != running_.end()) continue;
        item_sema(j);  // resolves its targets
        if (auto t = using_targets_.find({j, n.ordinal}); t != using_targets_.end())
          r.insert(r.end(), t->second.begin(), t->second.end());
        continue;
      }
      // A scoped enumeration's enumerators are in its own scope ([dcl.enum]/11); the declarators
      // after its braces are not.
      if ((n.flags & (items::name_flag::friend_ | items::name_flag::specialization)) ||
          (it.kind == ItemKind::enum_def && n.ordinal > 0 && !(n.flags & items::name_flag::enumerator) &&
           it.begin + n.offset < it.brace_end))
        continue;
      DeclId id{ids_[j], ast::make_decl_ord(ast::DeclClass::syntactic, n.ordinal)};
      // A C-linkage definition qualified into another namespace does not bind a lexical name.
      // Its earlier declaration is indexed in the target namespace.
      const bool qualified = it.qualifier_begin < it.qualifier_end;
      auto d = bound_functions || qualified ? decl_sema(id) : std::nullopt;
      if (qualified)
        if (auto target = qualified_c_functions_.find({j, n.ordinal});
            target != qualified_c_functions_.end() && target->second != ns)
          continue;
      if (bound_functions) {
        // A qualified definition can inhabit this namespace while binding its name elsewhere
        // ([dcl.meaning.general]). Targets of using-declarators were collected above.
        if (!d || d->entity == EntityId::none || d->kind != ast::DeclKind::function ||
            (d->flags & sema::decl_flag::member))
          continue;
        if (!(d->flags & sema::decl_flag::lang_c) && namespace_path(w_->entities.rec(d->entity).scope) != path)
          continue;
      }
      r.push_back(id);
    }
    for (const Directive& d : directives_)
      if (d.alias && d.item < item + (self ? 1 : 0) && d.at == path && w_->ins.names.spelling(*d.alias) == name)
        r.push_back({ids_[d.item], ast::make_decl_ord(ast::DeclClass::syntactic, 0)});
    return r;
  }
  // [basic.lookup.argdep]/4: search the innermost non-inline namespace and its inline namespace
  // set. In each namespace search only its own declarations, including using-declarator targets
  // fixed at their locus ([basic.lookup.general], [namespace.udecl]).
  std::vector<ast::DeclSema> adl_functions(EntityId ns, ast::NameId name, u32 item, bool self) {
    const u32 limit = item + (self ? 1 : 0);
    std::string path = namespace_path(ns);
    while (is_inline_namespace(path, limit)) path = parent_of(path);
    std::vector<std::string> scopes{std::move(path)};
    std::vector<DeclId> decls;
    std::string_view spelling = w_->ins.names.spelling(name);
    for (std::size_t i = 0; i < scopes.size(); ++i) {
      auto own = declared_in(spelling, scopes[i], item, self, /*inhabit=*/true, /*bound_functions=*/true);
      decls.insert(decls.end(), own.begin(), own.end());
      if (auto children = inline_namespaces_.find(scopes[i]); children != inline_namespaces_.end())
        for (const InlineNamespace& child : children->second)
          if (child.item < limit) scopes.push_back(child.path);
    }
    // A using-declarator contributes the original declarations. Prefer the latest redeclaration
    // actually found for each function, which also has the defaults known at that declaration.
    std::ranges::sort(decls, {}, [&](DeclId d) { return std::pair{index(d.item), ast::number_of(d.ord)}; });
    std::vector<ast::DeclSema> result;
    for (DeclId d : decls | std::views::reverse)
      if (auto s = decl_sema(d); s && s->entity != EntityId::none && s->kind == ast::DeclKind::function &&
          !std::ranges::contains(result, s->entity, &ast::DeclSema::entity))
        result.push_back(*s);
    return result;
  }
  static std::string parent_of(const std::string& p) {
    auto cut = p.rfind("::");
    return cut == std::string::npos ? "" : p.substr(0, cut);
  }
  bool is_inline_namespace(const std::string& path, u32 limit) const {
    if (path.empty()) return false;
    auto inlines = inline_namespaces_.find(parent_of(path));
    if (inlines == inline_namespaces_.end()) return false;
    auto child = std::ranges::find(inlines->second, path, &InlineNamespace::path);
    return child != inlines->second.end() && child->item < limit;
  }
  static bool encloses(const std::string& outer, const std::string& inner) {
    return outer.empty() || inner == outer || inner.starts_with(outer + "::");
  }
  static std::string common_ancestor(std::string a, const std::string& b) {
    while (!encloses(a, b)) a = parent_of(a);
    return a;
  }

  // What item_sema learns of each entity, for constant evaluation (§B7): its declarations, the
  // constant a variable usable in constant expressions was initialized with ([expr.const]), and a
  // function's defining item, each with the item that said it. An answer at `item` sees only what
  // items up to it said, so it does not depend on which other cells have run (§A1).
  void declared(const ast::DeclSema& d, u32 item) {
    decls_[std::to_underlying(d.entity)].push_back({item, d});
    if (d.flags & sema::decl_flag::specialization) explicit_specs_.emplace(std::to_underlying(d.entity), item);
    // A partial specialization ([temp.spec.partial]): a pattern its template's specializations may
    // be instantiated from (`choose_partial`).
    if (d.flags & sema::decl_flag::partial) {
      std::vector<Partial>& ps = partials_[std::to_underlying(w_->entities.rec(d.entity).templ)];
      bool defined = (d.flags & sema::decl_flag::definition) != 0;
      auto p = std::ranges::find(ps, d.entity, &Partial::entity);
      if (p == ps.end()) ps.push_back({item, d.entity, defined});
      else if (defined && !p->defined) *p = {item, d.entity, true};  // its definition, after a declaration
      return;
    }
    if (d.kind == ast::DeclKind::function && (d.flags & sema::decl_flag::definition))
      definitions_[std::to_underlying(d.entity)] = item;
    if (d.kind == ast::DeclKind::class_ && (d.flags & sema::decl_flag::template_)) {
      // A class template's definition: its members are analysed as the current instantiation's
      // (`current_instantiation`), and each specialization from them (`instantiate_class`).
      if (d.flags & sema::decl_flag::definition) class_templates_.emplace(std::to_underlying(d.entity), item);
      return;
    }
    // A variable template's definition: each specialization's initializer is analysed from it
    // (`variable_instance`).
    if (d.kind == ast::DeclKind::variable && (d.flags & sema::decl_flag::template_) && (d.flags & sema::decl_flag::definition))
      var_templates_.emplace(std::to_underlying(d.entity), item);
    if (d.kind == ast::DeclKind::class_ && (d.flags & sema::decl_flag::definition)) {
      class_items_.emplace(std::to_underlying(d.entity), item);
      class_entities_.emplace(item, d.entity);
      class_heads_.emplace(item, d);
    }
  }
  // Template operations at the end of the unit, whose templates' declarations it knows.
  sema::Templates templates() {
    sema::Templates t(*w_, classes_);
    t.template_decl = [this](EntityId e) { return declaration(e, end()); };
    return t;
  }
  void set_constant(EntityId e, ConstId c, u32 item) { constants_[std::to_underlying(e)] = {item, c}; }
  bool has_constant(EntityId e) const { return constants_.contains(std::to_underlying(e)); }
  // The current instantiation of the class template item `item` defines ([temp.dep.type]): the
  // class its members belong to while the template is analysed.
  void current_instantiation(EntityId spec, u32 item, const ast::DeclSema& head) {
    class_items_.emplace(std::to_underlying(spec), item);
    class_entities_.emplace(item, spec);
    class_heads_.emplace(item, head);
  }
  // Whether `e` is explicitly specialized before `item` ([temp.expl.spec]): then it is never
  // instantiated from its template.
  bool explicitly_specialized(EntityId e, u32 item) {
    specializations_before(item);
    auto it = explicit_specs_.find(std::to_underlying(e));
    return it != explicit_specs_.end() && it->second < item;
  }
  // Analyses the explicit and partial specializations before `item`, so what is said of a
  // specialization there does not depend on which cells ran first (§A1).
  void specializations_before(u32 item) {
    for (; spec_scan_ < item && spec_scan_ < items_->items.size(); ++spec_scan_) {
      const items::Item& it = items_->items[spec_scan_];
      bool spec = it.specialization || std::ranges::any_of(it.names, [](const items::DeclName& n) {
                    return (n.flags & items::name_flag::specialization) != 0;
                  });
      if (in_class_[spec_scan_] || !spec || std::ranges::contains(running_, spec_scan_)) continue;
      item_sema(spec_scan_);
    }
  }
  // A partial specialization of a template: its item, and the specialization of its parameters it
  // is the pattern of, whose arguments are the partial specialization's.
  struct Partial {
    u32 item;  // its definition's, or its first declaration's while it has none
    EntityId entity;
    bool defined;
  };
  struct PartialReplay {
    u32 begin;
    ast::DeclSema primary;
    std::vector<u32> dependent_calls;
  };
  // Whether class `c` is the pattern of a partial specialization defined before `item`, whose
  // members may be defined outside it ([temp.spec.partial.member]).
  bool partial_pattern(EntityId c, u32 item) {
    specializations_before(item);
    auto it = partials_.find(std::to_underlying(w_->entities.rec(c).templ));
    return it != partials_.end() &&
           std::ranges::any_of(it->second, [&](const Partial& p) { return p.entity == c && p.defined && p.item < item; });
  }
  // What a specialization is instantiated from ([temp.spec.partial.match]): the most specialized of
  // its template's partial specializations declared before `point` that match it, and the
  // arguments of that one's parameters; none for the primary template. `ambiguous` when no match
  // is more specialized than the others.
  struct Chosen {
    u32 item = 0;
    sema::Bindings args;
    bool ambiguous = false;
    bool defined = true;  // a partial specialization only declared leaves the specialization incomplete
  };
  std::optional<std::vector<ast::TemplateArg>> substitute_partial_args(
      u32 item, u32 point, std::span<const ast::TemplateParamSema> params, std::span<const ast::TemplateArg> pattern,
      sema::Bindings args);
  std::optional<Chosen> choose_partial(EntityId spec, u32 point) {
    specializations_before(point);
    const sema::EntityRec rec = w_->entities.rec(spec);
    auto it = partials_.find(std::to_underlying(rec.templ));
    if (it == partials_.end()) return std::nullopt;
    if (too_deep(partial_matching_.size(), it->second.front().item)) return std::nullopt;
    const u32 key = std::to_underlying(spec);
    partial_matching_.insert(key);
    struct Matching {
      std::set<u32>& active;
      u32 key;
      ~Matching() { active.erase(key); }
    } matching{partial_matching_, key};
    // Substitution can instantiate a class and discover more partial specializations.
    const std::vector<Partial> candidates = it->second;
    sema::Templates t = templates();
    struct Match {
      const Partial* p;
      std::vector<ast::TemplateParamSema> params;
      std::vector<ast::TemplateArg> pattern, args;
    };
    std::vector<Match> matches;
    for (const Partial& p : candidates) {
      if (p.item >= point) continue;
      std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(w_->ins.seqs.at(item_sema(p.item).value.decls, 0).template_params);
      std::vector<ast::TemplateArg> pattern = w_->entities.rec(p.entity).args;
      auto a = t.deduce_partial(params, pattern, rec.args);
      if (!a) continue;
      auto back = substitute_partial_args(p.item, point, params, pattern, sema::Bindings::of(params[0].depth, *a));
      if (back && std::ranges::equal(*back, rec.args))
        matches.push_back({&p, std::move(params), std::move(pattern), std::move(*a)});
    }
    if (matches.empty()) return std::nullopt;
    auto best = std::ranges::find_if(matches, [&](const Match& m) {
      return std::ranges::all_of(matches, [&](const Match& o) {
        return &m == &o || t.more_specialized_partial(m.params, m.pattern, o.params, o.pattern);
      });
    });
    if (best == matches.end()) return Chosen{matches[0].p->item, {}, true, true};
    return Chosen{best->p->item, sema::Bindings::of(best->params[0].depth, best->args), false, best->p->defined};
  }
  // An ambiguous choice of partial specialization, said once for each specialization.
  // ------------------------------------------------------------ constraints ([temp.constr])

  // Whether normal form `c` is satisfied at `point` ([temp.constr.sat]): a conjunction when both
  // are, a disjunction when either is, an atom by `atom_satisfied`; none while an atom's mapping
  // still names a template parameter.
  std::optional<bool> satisfied(ConstId c, u32 point) {
    if (c == ConstId::none) return true;
    if (w_->ins.values.is<ast::VConstraint>(c)) {
      auto v = w_->ins.values.get<ast::VConstraint>(c);
      bool any_unknown = false;
      for (ConstId o : w_->ins.seqs.elements(v.operands)) {
        auto r = satisfied(o, point);
        if (!r) any_unknown = true;
        else if (*r == (v.op == 1)) return *r;  // a false conjunct, a true disjunct
      }
      if (any_unknown) return std::nullopt;
      return v.op == 0;
    }
    return atom_satisfied(c, point);
  }
  // An atomic constraint's satisfaction ([temp.constr.atomic]/3), memoized (`atoms_`): the first
  // evaluation is the program's (/3: a different result elsewhere is ill-formed, no diagnostic
  // required).
  std::optional<bool> atom_satisfied(ConstId c, u32 point);
  std::map<u32, std::optional<bool>> atoms_;
  std::set<u32> atom_errors_;  // the atoms whose satisfaction check raised a hard error
  // Whether an atom of normal form `c` is among them.
  bool satisfaction_failed(ConstId c) {
    if (c == ConstId::none) return false;
    if (w_->ins.values.is<ast::VConstraint>(c))
      return std::ranges::any_of(w_->ins.seqs.elements(w_->ins.values.get<ast::VConstraint>(c).operands),
                                 [&](ConstId o) { return satisfaction_failed(o); });
    return atom_errors_.contains(std::to_underlying(c));
  }

  // The type of variable template specialization `v` at `point` when it is not its template's:
  // its explicit specialization's, or that of the partial specialization it is instantiated
  // from, with that one's arguments.
  std::optional<QualType> specialization_type(EntityId v, u32 point) {
    if (explicitly_specialized(v, point)) {
      auto d = declaration(v, point);
      return d ? std::optional(d->type) : std::nullopt;
    }
    if (partial_matching_.contains(std::to_underlying(v))) return std::nullopt;
    std::optional<Chosen> c = choose_partial(v, point);
    if (c && c->ambiguous) return std::nullopt;
    // A placeholder type is what the specialization's instantiation deduced ([dcl.spec.auto]).
    auto deduced = [&]() -> std::optional<QualType> {
      if (const VarInstance* in = variable_instance(v, point))
        for (const ast::DeclSema& d : w_->ins.seqs.elements(in->cell.value.decls))
          if (d.entity == v) return d.type;
      return std::nullopt;
    };
    if (!c) {
      auto t = var_templates_.find(std::to_underlying(w_->entities.rec(v).templ));
      if (t != var_templates_.end() && t->second < point && w_->ins.seqs.size(item_sema(t->second).value.decls) > 0 &&
          w_->types.has_placeholder(w_->ins.seqs.at(item_sema(t->second).value.decls, 0).type))
        return deduced();
      return std::nullopt;
    }
    auto t = templates().substitute(w_->ins.seqs.at(item_sema(c->item).value.decls, 0).type, c->args);
    if (t && w_->types.has_placeholder(*t)) return deduced();
    return t;
  }
  // At the end of the unit, it is said where the demand being instantiated came from.
  void ambiguous_partial(EntityId spec, u32 point) {
    if (!ambiguous_.insert(std::to_underlying(spec)).second) return;
    engine::Loc at = point < end() ? engine::Loc{std::to_underlying(id(point)), 0} : end_at_;
    ast::NameId name = w_->entities.rec(spec).name;
    end_diags_.push_back({at, sema::diag_id(sema::Diag::ambiguous_partial),
                          {{std::to_underlying(name), w_->ins.names.fp(name)}}});
  }

  // The template-heads of the class templates enclosing `item`, outermost first.
  // The leading heads of a member template of a class template defined outside it, before its
  // own ([temp.mem]/1): its body is in their scope too.
  void set_outer_heads(u32 item, std::vector<std::vector<ast::TemplateParamSema>> heads) { outer_heads_[item] = std::move(heads); }
  std::vector<std::vector<ast::TemplateParamSema>> enclosing_heads(u32 item) {
    if (auto o = outer_heads_.find(item); o != outer_heads_.end()) {
      item_sema(item);
      return o->second;
    }
    std::vector<std::vector<ast::TemplateParamSema>> r;
    u32 parent = items_->items[item].parent;
    if (parent != items::none && items_->items[parent].kind == ItemKind::class_def && items_->items[parent].template_depth > 0) {
      const ast::ItemSema& ps = item_sema(parent).value;
      if (w_->ins.seqs.size(ps.decls) > 0) {
        ast::DeclSema h = w_->ins.seqs.at(ps.decls, 0);
        if (h.flags & sema::decl_flag::template_) r.push_back(w_->ins.seqs.elements(h.template_params));
      }
    }
    return r;
  }

  std::optional<ast::DeclSema> declaration(EntityId e, u32 item) const {
    auto it = decls_.find(std::to_underlying(e));
    if (it == decls_.end()) return std::nullopt;
    std::optional<ast::DeclSema> r;
    for (const auto& [k, d] : it->second)
      if (k <= item) r = d;
    return r;
  }
  // A previous definition of this entity in this TU ([basic.def.odr]). C language linkage
  // can make a definition in another namespace correspond without ordinary lookup finding it.
  bool function_defined_before(EntityId e, u32 item) {
    if (auto known = definitions_.find(std::to_underlying(e)); known != definitions_.end() && known->second < item)
      return true;
    for (auto [j, k] : named(w_->ins.names.spelling(w_->entities.rec(e).name))) {
      if (j >= item) break;
      const items::Item& it = items_->items[j];
      if (!in_global_scope(j) || it.kind != ItemKind::function) continue;
      DeclId id{ids_[j], ast::make_decl_ord(ast::DeclClass::syntactic, it.names[k].ordinal)};
      if (auto d = decl_sema(id); d && d->entity == e && (d->flags & sema::decl_flag::definition)) return true;
    }
    return false;
  }
  std::optional<ConstId> constant_value(EntityId e, u32 item) {
    variable_instance(e, item);
    member_value(e);
    auto it = constants_.find(std::to_underlying(e));
    if (it == constants_.end() || it->second.first > item) return std::nullopt;
    return it->second.second;
  }

  // `function_ir(F)` for a function defined in an item before `item` ([expr.const]: a function
  // defined later is not defined at the point of evaluation).
  interp::Callee function_ir(EntityId f, u32 item);
  // A closure type's record, published by the analysis of `item`, its lambda's
  // (docs/design/closures.md), with its implicit special members ([expr.prim.lambda.closure]/14).
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r, u32 item) {
    if (const sema::ClassRec* done = classes_.find(c)) return *done;
    special_members(item, c, r);
    return classes_.publish(c, std::move(r));
  }
  interp::Callee closure_ir(EntityId f, u32 item);
  // A generic lambda's function call operator specialized ([temp.inst]): its body re-analysed
  // from the lambda's tokens with the arguments bound, an `Instance`; its type with the return
  // type deduced, none when it fails.
  struct GenericInst {
    sema::GenericClosure g;
    EntityId spec;
    QualType spec_type;
    std::vector<ast::TemplateArg> args;
  };
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, u32 point);

  // The function type a definition before `item` deduced for `f` ([dcl.spec.auto.general]/11).
  std::optional<QualType> deduced_type(EntityId f, u32 item);
  // A namespace-scope variable as the interpreter sees it at `item`: its type and, if it is usable
  // in constant expressions, its value.
  std::optional<interp::GlobalVar> global(EntityId v, u32 item) {
    // A static local: usable in constant expressions when const and constant-initialized
    // ([expr.const]); reading or writing another is not constant.
    if (auto s = static_locals_.find(std::to_underlying(v)); s != static_locals_.end())
      return interp::GlobalVar{s->second.first, s->second.second,
                               s->second.second.has_value() && sema::Types::is_const(s->second.first)};
    variable_instance(v, item);
    auto d = declaration(v, item);
    if (!d || d->kind != ast::DeclKind::variable) return std::nullopt;
    auto c = constant_value(v, item);
    QualType type = d->type;
    if (w_->types.is_reference(type) && !w_->types.is_function(w_->types.referee(type))) {
      const sema::EntityRec& entity = w_->entities.rec(v);
      bool in_namespace = entity.scope == EntityId::none || w_->entities.rec(entity.scope).kind == sema::EntityKind::namespace_;
      if (in_namespace && entity.templ == EntityId::none) type = w_->types.pointer(w_->types.referee(type));
    }
    return interp::GlobalVar{type, c, c.has_value()};
  }
  bool is_constexpr(EntityId f, u32 item) {
    if (auto copy = copy_assignments_.find(f); copy != copy_assignments_.end())
      return copy->second.definition.item <= item;
    // A specialization is as its template is declared, unless explicitly specialized ([temp.expl.spec]/14).
    if (EntityId t = w_->entities.rec(f).templ; t != EntityId::none && !explicitly_specialized(f, item)) f = t;
    auto d = declaration(f, item);
    return d && (d->flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_));
  }

  // A default member initializer's tokens in the region ([class.mem.general]), analysed again in
  // each constructor that uses it.
  struct Dmi {
    u32 begin = 0;
    u32 end = 0;
    bool braced = false;
  };
  std::map<u32, std::pair<QualType, std::optional<ConstId>>> static_locals_;  // by entity (`global`)
  void set_dmi(EntityId field, Dmi d) { dmis_[std::to_underlying(field)] = d; }
  // A class's base-clause ([class.derived.general]), recorded when its head is analysed.
  void set_bases(EntityId cls, std::vector<sema::BaseRec> bases) { class_bases_[std::to_underlying(cls)] = std::move(bases); }
  std::optional<Dmi> dmi(EntityId field) const {
    auto it = dmis_.find(std::to_underlying(field));
    if (it == dmis_.end()) return std::nullopt;
    return it->second;
  }

  // The implicitly defined special members needed by the unit ([special]).
  enum class SpecialMember { destructor, copy_constructor, copy_assignment };
  struct Synthesized {
    u32 item;  // the class's
    ast::DeclSema decl;
    SpecialMember kind;
  };
  const std::deque<Synthesized>& synthesized() const { return synth_; }
  // Its body, analysed on first use.
  const ast::BodySema& synthesized_body(std::size_t i);
  void special_members(u32 item, EntityId cls, sema::ClassRec& rec);
  void declare_copy_assignment(u32 item, EntityId cls, sema::ClassRec& rec, bool implicit, bool user_move);
  sema::CopyAssignment require_copy_assignment(EntityId f);
  Ex copy_from(sema::Sema& sm, Ex src, QualType t);

  // The type of `this` in a non-static member function ([expr.prim.this]): a pointer to its class,
  // with the function's cv-qualifiers.
  std::optional<QualType> this_type(const ast::DeclSema& f) {
    if (f.kind != ast::DeclKind::function || !(f.flags & sema::decl_flag::member) || (f.flags & sema::decl_flag::static_))
      return std::nullopt;
    EntityId c = w_->entities.rec(f.entity).scope;
    return w_->types.pointer(w_->types.add_cv(w_->types.class_type(c), w_->types.rec(f.type).fn_cv));
  }

  // ------------------------------------------------------------ instantiation (#88)

  // What instantiating a function template specialization analyses its template's body with
  // (ast.md §6.4 as amended by #88): the specialization's declaration, and the arguments bound to
  // the template's parameters.
  struct Instantiation {
    ast::DeclSema decl;
    sema::Bindings args;
  };
  // The bodies being analysed, innermost last: each function's item and its instantiation, which a
  // generic lambda defined there and specialized there is instantiated in.
  struct Running {
    EntityId function;
    u32 item;
    std::optional<Instantiation> inst;
  };
  std::vector<Running> running_bodies_;
  std::map<u32, sema::GenericClosure> generic_defs_;  // generic lambdas defined so far, by operator
  // What instantiating a class template specialization analyses each member item with: the
  // specialization, and the arguments bound to the template's parameters.
  struct ClassInstantiation {
    EntityId spec;
    sema::Bindings args;
    u32 point;  // the item whose demand instantiates it ([temp.point])
  };
  // Static data members of class templates ([temp.inst]/3). The template's analysis records where
  // each in-class initializer ends; a specialization declares the member and skips it
  // (`lazy_member`); its value is instantiated on first use (`member_value`), and the members used
  // are defined in the unit (`used_members`).
  void set_static_init_end(u32 item, u32 ordinal, u32 end) { static_init_ends_[{item, ordinal}] = end; }
  std::optional<u32> static_init_end(u32 item, u32 ordinal) {
    item_sema(item);  // the template's analysis records it
    auto it = static_init_ends_.find({item, ordinal});
    if (it == static_init_ends_.end()) return std::nullopt;
    return it->second;
  }
  struct LazyMember {
    u32 item;
    u32 ordinal;
    ClassInstantiation inst;
    enum class State : u8 { pending, running, done } state = State::pending;
  };
  void lazy_member(EntityId e, u32 item, u32 ordinal, const ClassInstantiation& inst) {
    lazy_members_.try_emplace(std::to_underlying(e), LazyMember{item, ordinal, inst});
  }
  bool is_lazy_member(EntityId e) const { return lazy_members_.contains(std::to_underlying(e)); }
  void member_value(EntityId e);
  void use_member(EntityId e) {
    if (!is_lazy_member(e) || std::ranges::contains(used_members_, e)) return;
    used_members_.push_back(e);
    member_value(e);
  }
  std::span<const EntityId> used_members() const { return used_members_; }
  // Whether a member's initializer was instantiated and gave no value: its error was reported.
  bool member_failed(EntityId e) const {
    auto it = lazy_members_.find(std::to_underlying(e));
    return it != lazy_members_.end() && it->second.state == LazyMember::State::done && !has_constant(e) &&
           !member_inits_.contains(std::to_underlying(e));
  }
  // A used member's initial value, which a member not usable in constant expressions has too.
  void set_member_init(EntityId e, ConstId v) { member_inits_[std::to_underlying(e)] = v; }
  ConstId member_init(EntityId e) const {
    auto it = member_inits_.find(std::to_underlying(e));
    return it == member_inits_.end() ? ConstId::none : it->second;
  }
  // A template's static data member whose value depends on its arguments ([temp.dep.constexpr]).
  void value_dependent_member(EntityId e) { value_dependent_members_.insert(std::to_underlying(e)); }
  bool is_value_dependent_member(EntityId e) const { return value_dependent_members_.contains(std::to_underlying(e)); }
  // `instantiate_fn(spec, END)` (query-engine.md §B6.2): the template's definition item, the
  // specialization's declaration and its body. `failed` when the template's definition had
  // errors already: nothing more is said about it.
  struct Instance {
    EntityId spec;
    u32 item;
    ast::DeclSema decl;
    Result<ast::BodySema> body;
    bool failed = false;
    // A generic lambda operator's: the instantiation of the function its lambda is in, if any.
    std::optional<Instantiation> enclosing{};
  };
  // The instance of a specialization, instantiated on first request; none when its template has
  // no definition, or the instantiations nest too deeply ([temp.inst]/16).
  const Instance* instance(EntityId spec, u32 point);
  // What instantiating a variable template specialization analyses its template's declaration
  // with: the specialization, and the arguments bound to the template's parameters.
  struct VarInstantiation {
    EntityId spec;
    sema::Bindings args;
    u32 point;  // the item whose demand instantiates it ([temp.point])
  };
  // `instantiate_var(spec)` ([temp.inst]/7): the declaration of a variable template specialization,
  // its initializer analysed and evaluated with the arguments bound; none if `v` is no such
  // specialization, or its template is not defined before `point`.
  struct VarInstance;
  const VarInstance* variable_instance(EntityId v, u32 point);
  // The variable template specializations instantiated, in order: their item and record.
  struct VarInstance {
    EntityId spec;
    u32 item;
    Result<ast::ItemSema> cell;
  };
  const std::deque<VarInstance>& variable_instances() const { return var_instances_; }
  // The members of class template specializations analysed, in order: (member item, specialization)
  // and the member's record for that specialization ([temp.inst]/3).
  struct MemberInstance {
    u32 item;
    EntityId spec;
    const Result<ast::ItemSema>* cell;
  };
  std::vector<MemberInstance> member_instances() const {
    std::vector<MemberInstance> r;
    for (const auto& key : class_member_order_)
      r.push_back({key.first, EntityId(key.second), &class_member_cells_.at(key)});
    return r;
  }
  // A class's members, from its member items' records (`class_def` and `instantiate_class`).
  struct ClassContent {
    std::vector<ast::MemberRec> members;
    std::vector<sema::FieldRec> fields;
    std::vector<sema::MemberDeclRec> statics;
    std::vector<sema::NestedType> nested;
    std::vector<EntityId> friends;
    std::vector<ast::MemberRec> adl_friends;
  };
  ClassContent class_content(u32 item, const std::function<const ast::ItemSema&(u32)>& sema_of);
  // `instantiate_class(spec)` ([temp.inst]/2, §B6.2): the class record of a class template
  // specialization, its member items analysed with the arguments bound, built whole and then
  // published; none when the template has no definition before `item`, or for a dependent one.
  const sema::ClassRec* instantiate_class(EntityId spec, u32 point);
  // The member of a class template specialization a member function entity is, if it is one:
  // its pattern's item and the specialization.
  struct MemberOf {
    u32 item;
    EntityId spec;
  };
  std::map<u32, MemberOf> member_patterns_;
  std::optional<u32> out_of_class_definition(EntityId spec, MemberOf of);
  // A class template specialization's record of its member item `m`, analysed already.
  const ast::ItemSema& class_member_sema(u32 m, EntityId spec) {
    return class_member_cells_.at(std::pair(m, std::to_underlying(spec))).value;
  }

  // The end of the translation unit (§B6.2): every specialization whose definition a cell
  // requires is instantiated, in order of first demand, and so are those its instance requires.
  void instantiate_all();
  void references(NodeId n, std::vector<EntityId>& out) const;
  std::span<const Instance* const> instances() const { return instance_order_; }
  // Every specialization whose definition the unit requires, in order of first demand.
  std::span<const EntityId> required() const { return required_; }
  std::span<const EntityId> implicit_allocation_functions() const { return implicit_allocation_functions_; }
  // The calls of a template's body that were dependent at its definition, by the template's item
  // ([temp.dep.candidate]); its instantiations add argument-dependent lookup to them.
  // Whether a call of the templated body of `item` at `offset` was dependent, read from the
  // body's analysis (ast.md §6.4).
  bool dependent_call(u32 item, u32 offset) {
    const ast::BodySema& b = analyze_body(item).value;
    for (u32 o : w_->ins.seqs.elements(b.dependent_calls))
      if (o == offset) return true;
    return false;
  }
  // The pack expansions of a templated definition's part (0 its declaration part, 1 its body), read
  // from that part's analysis (ast.md §6.4), for the instantiations that read their patterns once
  // per element. A cache of the values.
  const std::vector<sema::Expansion>* expansions(u32 item, u32 part) {
    auto key = std::pair(item, part);
    if (auto it = expansions_.find(key); it != expansions_.end()) return it->second.empty() ? nullptr : &it->second;
    ast::SeqId<ast::ExpansionRec> list = part == 0 ? item_sema(item).value.expansions : analyze_body(item).value.expansions;
    std::vector<sema::Expansion> out;
    for (const ast::ExpansionRec& e : w_->ins.seqs.elements(list)) {
      sema::Expansion x{e.begin, e.end, {}};
      for (const ast::PackRefRec& r : w_->ins.seqs.elements(e.packs)) x.packs.push_back({r.depth, r.index, r.fn});
      out.push_back(std::move(x));
    }
    auto& v = expansions_[key] = std::move(out);
    return v.empty() ? nullptr : &v;
  }
  // Whether the body of `item` is a template's definition or a member of one: a pattern.
  bool templated_body(u32 item) {
    const ast::ItemSema& is = item_sema(item).value;
    bool templ = w_->ins.seqs.size(is.decls) > 0 && w_->ins.seqs.size(w_->ins.seqs.at(is.decls, 0).template_params) > 0;
    return templ || !enclosing_heads(item).empty();
  }
  std::map<std::pair<u32, u32>, std::vector<sema::Expansion>> expansions_;
  u32 end() const { return static_cast<u32>(items_->items.size()); }
  const u32 instantiation_limit;  // [temp.inst]/16 ([implimits]: at least 1024)
  // Whether an instantiation `depth` deep may not begin: the limit; reported once, at item `k`. The
  // stack is guarded in the parse of each instantiation's body (`Parser::deep_enough`, #187).
  // `offset`: where in item `k` to report it.
  bool too_deep(std::size_t depth, u32 k, u32 offset = 0) {
    if (depth < instantiation_limit) return false;
    if (!depth_reported_)
      end_diags_.push_back({engine::Loc{std::to_underlying(id(k)), offset}, sema::diag_id(sema::Diag::instantiation_depth),
                            {engine::diag_arg(instantiation_limit)}});
    depth_reported_ = true;
    return true;
  }
  bool instantiation_gave_up() const { return depth_reported_; }

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
    instantiate_all();
  }
  // An item at namespace scope: in no class, whichever namespaces enclose it.
  bool in_global_scope(u32 k) const { return !in_class_[k]; }
  // What analysis reached, without analysing more (#170): an item's semantics and its body, if
  // analysed.
  const ast::ItemSema* analysed_item(u32 k) const { return item_cells_[k] ? &item_cells_[k]->value : nullptr; }
  const ast::BodySema* analysed_body(u32 k) const { return body_cells_[k] ? &body_cells_[k]->value : nullptr; }
  std::vector<engine::Diagnostic> diagnostics() const {
    std::vector<engine::Diagnostic> r;
    for (u32 k = 0; k < items_->items.size(); ++k) {
      if (item_cells_[k]) r.insert(r.end(), item_cells_[k]->diags.begin(), item_cells_[k]->diags.end());
      if (class_cells_[k]) r.insert(r.end(), class_cells_[k]->diags.begin(), class_cells_[k]->diags.end());
      if (body_cells_[k]) r.insert(r.end(), body_cells_[k]->diags.begin(), body_cells_[k]->diags.end());
    }
    // A class template specialization's member declarations', then each function instance's, in
    // the template's tokens (ast.md §6.4), in order of instantiation.
    for (const auto& key : class_member_order_) {
      const auto& cell = class_member_cells_.at(key);
      r.insert(r.end(), cell.diags.begin(), cell.diags.end());
    }
    for (const Instance* in : instance_order_) r.insert(r.end(), in->body.diags.begin(), in->body.diags.end());
    for (const VarInstance& v : var_instances_) r.insert(r.end(), v.cell.diags.begin(), v.cell.diags.end());
    r.insert(r.end(), end_diags_.begin(), end_diags_.end());
    std::ranges::stable_sort(r, {}, [&](const engine::Diagnostic& d) {
      return std::pair(index(ItemId(d.at.item)), d.at.offset);
    });
    return r;
  }

  // The file and byte offset of a diagnostic's location (ast.md §8.3, steps 1–3); a macro's
  // token is where it was expanded.
  pp::TokLoc site(engine::Loc at) const { return r_->site(token(at)); }
  // The region token of a diagnostic's location.
  u32 token(engine::Loc at) const {
    const items::Item& it = items_->items[index(ItemId(at.item))];
    return std::min<u32>(it.begin + at.offset, static_cast<u32>(r_->locs.size() - 1));
  }
  // The byte offset in its file of a diagnostic's location.
  u32 byte_offset(engine::Loc at) const { return site(at).offset; }

  // Spells a diagnostic argument: a name, a type, a string literal or why an evaluation failed.
  std::string spell(char how, u64 raw) const {
    if (how == 'n') return std::string(w_->ins.names.spelling(ast::NameId(u32(raw))));
    if (how == 'r') return sema::spell_reason(raw);
    if (how == 'c') {
      std::string t = w_->values.narrow_string(ConstId(u32(raw))).value_or("");
      if (!t.empty() && t.back() == '\0') t.pop_back();  // the terminating null
      return t;
    }
    return w_->types.spell(ast::QualType(u32(raw)));
  }

private:
  std::optional<QualType> assignment_parameter(const sema::MemberDeclRec& member, QualType owner) const;

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
  // By member function or static data member entity: its declaration in this unit's class, an
  // item of this unit (#156).
  std::map<u32, DeclId> member_decls_;
  sema::Classes classes_{w_->ins, w_->types};  // this unit's class and enumeration records (#115)
  void defaulted_copy(EntityId cls, sema::ClassRec& rec, std::span<const QualType> subobjects, bool implicit);
  // The enumeration whose enumerator list is being analysed, and its record so far.
  std::pair<EntityId, const sema::EnumRec*> defining_enum_{EntityId::none, nullptr};
  // What each namespace-scope using-declaration introduces, by (item, name ordinal): the
  // declarations qualified lookup found at it ([namespace.udecl]/1).
  std::map<std::pair<u32, u32>, std::vector<DeclId>> using_targets_;
  std::map<u32, std::string> asm_labels_;  // by entity: its GNU asm label (docs/EXTENSIONS.md)
  std::map<u32, ast::DeclSema> class_heads_;  // by class item: the class's record, for its members
  std::map<u32, u32> class_templates_;        // by class template entity: its defining item
  std::map<std::pair<u32, u32>, Result<ast::ItemSema>> class_member_cells_;  // by (member item, specialization)
  std::set<u32> classes_instantiating_;
  std::set<u32> classes_instantiated_;  // by specialization: instantiated in this unit
  std::vector<std::pair<u32, u32>> class_member_order_;  // the cells above, in order of analysis
  std::map<u32, Dmi> dmis_;                       // by field entity
  std::map<u32, std::vector<sema::BaseRec>> class_bases_;  // by class entity
  std::vector<std::string> ns_of_, eff_ns_;       // by item
  // A namespace definition's components' namespaces, as declared and for lookup, by name
  // (`namespace a::b` declares `a` in the enclosing namespace and `b` in `a`).
  std::map<u32, std::vector<std::pair<std::string, std::string>>> name_scopes_;
  const std::string& name_ns(u32 item, u32 name) const {
    auto it = name_scopes_.find(item);
    return it != name_scopes_.end() && name < it->second.size() ? it->second[name].first : ns_of_[item];
  }
  const std::string& name_eff_ns(u32 item, u32 name) const {
    auto it = name_scopes_.find(item);
    return it != name_scopes_.end() && name < it->second.size() ? it->second[name].second : eff_ns_[item];
  }
  std::vector<std::string> inner_of_;             // by namespace definition item: the namespace
  std::vector<bool> in_class_;                    // by item
  // By declared name: (item, index in its names), in order; a lookup visits only its own name's.
  std::unordered_map<std::string_view, std::vector<std::pair<u32, u32>>> by_name_;
  std::map<std::string, EntityId> ns_entities_;
  std::map<u32, std::string> ns_paths_;           // by namespace entity
  struct InlineNamespace { std::string path; u32 item; };
  std::map<std::string, std::vector<InlineNamespace>> inline_namespaces_;  // by enclosing namespace
  // The target namespace of a qualified C-linkage function, by (item, declarator ordinal).
  std::map<std::pair<u32, u32>, std::string> qualified_c_functions_;
  std::vector<Directive> directives_;

  // Each item's namespace, from the namespace definitions that enclose it; and the using-
  // directives and aliases at namespace scope, whose targets are resolved when their items are
  // analysed.
  void namespaces() {
    const auto& its = items_->items;
    ns_of_.assign(its.size(), "");
    eff_ns_.assign(its.size(), "");
    in_class_.assign(its.size(), false);
    inner_of_.assign(its.size(), "");
    std::vector<std::string> inner_eff(its.size());
    for (u32 k = 0; k < its.size(); ++k) {  // preorder: a parent comes first
      u32 p = its[k].parent;
      if (p != items::none) {
        in_class_[k] = in_class_[p] || its[p].kind == ItemKind::class_def || its[p].kind == ItemKind::enum_def;
        bool ns = its[p].kind == ItemKind::namespace_def;
        ns_of_[k] = ns ? inner_of_[p] : ns_of_[p];
        eff_ns_[k] = ns ? inner_eff[p] : eff_ns_[p];
      }
      if (its[k].kind != ItemKind::namespace_def) continue;
      // The identifiers of `namespace a::inline b`, or none for an unnamed one; `inline` before
      // a component makes it transparent to lookup ([namespace.def]/7). Component i is declared
      // in the namespace the ones before it name.
      std::vector<std::pair<std::string, bool>> names;
      bool is_inline = false;
      for (u32 t = its[k].begin; t < its[k].brace_begin; ++t) {
        // Past attributes, `[[…]]` and `__attribute__((…))`.
        if (r_->tokens[t].kind == lex::Tk::l_square || r_->tokens[t].kind == lex::Tk::l_paren) {
          int depth = 0;
          for (; t < its[k].brace_begin; ++t) {
            lex::Tk x = r_->tokens[t].kind;
            depth += (x == lex::Tk::l_square || x == lex::Tk::l_paren) - (x == lex::Tk::r_square || x == lex::Tk::r_paren);
            if (depth == 0) break;
          }
          continue;
        }
        if (r_->tokens[t].kind == lex::Tk::identifier && t + 1 < its[k].brace_begin && r_->tokens[t + 1].kind == lex::Tk::l_paren)
          continue;  // `__attribute__`, whose parentheses come next
        if (r_->tokens[t].kind == lex::Tk::kw_inline) is_inline = true;
        if (r_->tokens[t].kind == lex::Tk::identifier) {
          names.push_back({std::string(r_->name(r_->tokens[t])), is_inline});
          is_inline = false;
        }
      }
      bool anonymous = names.empty();
      if (anonymous) names.push_back({"(anonymous)", true});
      inner_of_[k] = ns_of_[k];
      inner_eff[k] = eff_ns_[k];
      for (const auto& [n, inl] : names) {
        const std::string parent = inner_of_[k];
        if (!anonymous) name_scopes_[k].push_back({parent, inner_eff[k]});
        inner_of_[k] += (inner_of_[k].empty() ? "" : "::") + n;
        // An unnamed namespace is transparent to ordinary lookup even without `inline`.
        // Only explicitly inline namespaces form the set used by ADL.
        if (anonymous ? is_inline : inl) {
          auto& children = inline_namespaces_[parent];
          if (!std::ranges::contains(children, inner_of_[k], &InlineNamespace::path))
            children.push_back({inner_of_[k], k});
        }
        if (!inl) inner_eff[k] += (inner_eff[k].empty() ? "" : "::") + n;
      }
    }
    for (u32 k = 0; k < its.size(); ++k) {
      if (in_class_[k] || its[k].kind != ItemKind::declaration) continue;
      lex::Tk a = r_->tokens[its[k].begin].kind;
      lex::Tk b = its[k].begin + 1 < its[k].end ? r_->tokens[its[k].begin + 1].kind : lex::Tk::eof;
      if (a == lex::Tk::kw_using && b == lex::Tk::kw_namespace) directives_.push_back({k, ns_of_[k], std::nullopt, std::nullopt});
      if (a == lex::Tk::kw_namespace && b == lex::Tk::identifier)
        directives_.push_back({k, ns_of_[k], w_->ins.names.intern(r_->name(r_->tokens[its[k].begin + 1])), std::nullopt});
    }
  }
  // A deque: analysing a synthesized body can instantiate a class, which synthesizes more members
  // while the body's record is referred to.
  std::deque<Synthesized> synth_;
  std::map<std::size_t, ast::BodySema> synth_bodies_;
  struct PendingCopyAssignment {
    Synthesized definition;
    sema::CopyAssignment state;
    std::optional<ast::BodySema> body = std::nullopt;
    bool checking = false;
    bool emitted = false;
  };
  std::map<EntityId, PendingCopyAssignment> copy_assignments_;
  std::deque<Instance> instances_;               // a deque: instances are referred to as they are added
  std::vector<const Instance*> instance_order_;  // the completed ones, in order of instantiation
  std::map<u32, std::size_t> instance_index_;    // by specialization: its slot in `instances_`
  std::set<u32> instantiating_;                  // specializations whose instantiation is running
  std::map<u32, u32> var_templates_;             // by variable template entity: its defining item
  std::map<u32, u32> explicit_specs_;            // by specialization: the item of its first explicit specialization
  std::map<u32, u32> class_points_;              // by class template specialization: its point of instantiation
  u32 spec_scan_ = 0;                            // the items `specializations_before` has analysed
  std::map<u32, std::vector<Partial>> partials_;  // by template entity, in order of declaration
  std::map<u32, PartialReplay> partial_replays_; // by item with deferred checks in its template arguments
  std::set<u32> partial_matching_;              // specializations whose arguments are being substituted
  std::set<u32> ambiguous_;                      // specializations whose choice was diagnosed
  engine::Loc end_at_{};                         // the END worklist's demand being instantiated
public:
  // Where in its item an explicit specialization names its specialization, for diagnostics.
  void specialization_named_at(EntityId e, u32 offset) { spec_names_.emplace(std::to_underlying(e), offset); }
private:
  std::map<u32, u32> spec_names_;
  std::map<u32, u32> class_patterns_;            // by class specialization: the item it was instantiated from
  std::map<u32, sema::Bindings> class_args_;     // by class specialization: its pattern's arguments
  std::deque<VarInstance> var_instances_;        // a deque: their records are referred to as they are added
  std::map<u32, std::size_t> var_index_;         // by specialization: its slot in `var_instances_`
  std::set<u32> var_instantiating_;

  bool depth_reported_ = false;
  u32 members_running_ = 0;  // static data member initializers being instantiated, nested
  std::vector<engine::Diagnostic> end_diags_;    // the END phase's own
  std::vector<EntityId> required_;
  std::vector<EntityId> implicit_allocation_functions_;  // in order of first use
  std::map<u32, std::optional<irgen::FunctionIr>> instance_irs_;  // instantiated and implicit bodies, by entity
  std::map<u32, std::optional<irgen::FunctionIr>> closure_irs_;  // by closure member

public:
  // The class a class item defines, none if its head failed. The class's own declarators ask it
  // while its item is analysed, so it is read from what the item declared before them.
  EntityId class_entity(u32 item) {
    if (!class_entities_.contains(item)) item_sema(item);
    auto it = class_entities_.find(item);
    return it == class_entities_.end() ? EntityId::none : it->second;
  }

private:
  static constexpr u32 class_def_key(u32 item) { return item | 0x80000000u; }
  std::vector<u32> running_;
  std::set<u32> ordinary_friend_items_;  // the source item of each supported unqualified friend
  bool friend_body_ready(u32 definition) const {
    if (!ordinary_friend_items_.contains(definition)) return true;
    const u32 parent = items_->items[definition].parent;
    if (classes_.is_defined(class_entities_.at(parent))) return true;
    // Earlier friends can be found inside the member list. A deduction or constant evaluation
    // there cannot ask for the body while that body's complete class is still being assembled.
    return std::ranges::none_of(running_, [&](u32 key) {
      if (key == class_def_key(parent)) return true;
      return key < items_->items.size() && (key == parent || items_->items[key].parent == parent);
    });
  }
  std::map<EntityId, std::vector<std::optional<std::pair<u32, u32>>>> member_default_tokens_;
  void bind_member_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out);
  std::map<u32, std::vector<std::vector<ast::TemplateParamSema>>> outer_heads_;  // by item
  std::map<std::pair<u32, u32>, u32> static_init_ends_;  // by (member item, ordinal): its initializer's end
  std::map<u32, LazyMember> lazy_members_;                 // by member entity
  std::vector<EntityId> used_members_;                     // in order of first use
  std::set<u32> value_dependent_members_;
  std::map<u32, ConstId> member_inits_;  // by lazily initialized member
  std::map<u32, std::pair<u32, ConstId>> constants_;                // by entity: (item, value)
  std::map<u32, std::vector<std::pair<u32, ast::DeclSema>>> decls_;  // by entity: (item, record)
  std::map<u32, u32> definitions_;               // by function entity: its defining item
  std::map<u32, std::optional<irgen::FunctionIr>> irs_;  // by item: function_ir, none if it failed
  std::pmr::unsynchronized_pool_resource arena_;  // the frames' arena (§9.3)

  friend class ItemAnalysis;
  friend class AtItem;
  friend class AtomAnalysis;
  friend class BodyAnalysis;
};

// Sema's questions, asked at an item's prefix (§B1.4).
class AtItem final : public sema::Questions {
public:
  AtItem(Program& p, u32 item, bool self) : from(p.ns_of(item)), p_(&p), item_(item), self_(self) {}
  const sema::Classes& classes() override { return p_->classes(); }
  sema::CopyAssignment require_copy_assignment(EntityId f) override { return p_->require_copy_assignment(f); }
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r) override { return p_->publish_class(c, std::move(r), item_); }
  const sema::EnumRec* defining_enum(EntityId e) override {
    return p_->defining_enum_.first == e ? p_->defining_enum_.second : nullptr;
  }
  std::vector<DeclId> visible_decls(ast::NameId name, std::span<const EntityId> directives = {}) override {
    std::vector<std::string> paths;
    for (EntityId ns : directives) paths.push_back(p_->namespace_path(ns));
    return p_->visible_decls(p_->world().ins.names.spelling(name), item_, self_, from, paths);
  }
  std::optional<EntityId> namespace_named(ast::NameId name, std::span<const EntityId> directives = {}) override {
    for (DeclId d : visible_decls(name, directives))
      if (auto s = p_->decl_sema(d); s && s->kind == ast::DeclKind::namespace_) return s->entity;
    return std::nullopt;
  }
  std::vector<ast::DeclSema> namespace_members(EntityId ns, ast::NameId name) override {
    std::vector<ast::DeclSema> r;
    for (DeclId d : p_->qualified_decls(p_->world().ins.names.spelling(name), p_->namespace_path(ns), item_, self_))
      if (auto s = p_->decl_sema(d); s && s->entity != EntityId::none) r.push_back(*s);
    return r;
  }
  std::vector<ast::DeclSema> adl_functions(EntityId ns, ast::NameId name, bool at_instantiation) override {
    return at_instantiation ? p_->adl_functions(ns, name, point(), false) : p_->adl_functions(ns, name, item_, self_);
  }
  const sema::ClassRec* adl_class(EntityId c, bool at_instantiation) override {
    bool complete = at_instantiation ? is_complete(c) : is_complete_at_item(c);
    return complete ? p_->classes().find(c) : nullptr;
  }
  std::vector<DeclId> earlier_adl_friends(EntityId c, NameId name) override {
    const auto def = p_->class_items_.find(std::to_underlying(c));
    if (def == p_->class_items_.end() || p_->items().items[item_].parent != def->second) return {};
    return p_->hidden_friends_before(p_->world().ins.names.spelling(name), item_, def->second);
  }
  // The namespace unqualified lookup starts in: the item's, or the one a qualified declarator
  // names ([basic.lookup.unqual]/9).
  std::string from;
  ast::DeclSema decl_sema(DeclId d) override {
    if (auto s = p_->decl_sema(d)) return *s;
    return invalid_decl();
  }
  void use_allocation_function(EntityId f) override {
    auto& functions = p_->implicit_allocation_functions_;
    if (!std::ranges::contains(functions, f)) functions.push_back(f);
  }
  interp::Callee function_ir(EntityId f) override { return p_->function_ir(f, point()); }
  std::optional<QualType> deduced_type(EntityId f) override { return p_->deduced_type(f, point()); }
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, const sema::GenericBinding&) override {
    return p_->generic_instance(spec, known, point());
  }
  void generic_defined(const sema::GenericClosure& g) override { p_->generic_defs_[std::to_underlying(g.call)] = g; }
  std::optional<interp::GlobalVar> global(EntityId v) override { return p_->global(v, value_point(v)); }
  bool is_constexpr(EntityId f) override { return p_->is_constexpr(f, point()); }
  bool is_complete(EntityId c) override {
    return at_point ? p_->is_complete(c, *at_point, true, true) : is_complete_at_item(c);
  }
  std::optional<ConstId> constant_value(EntityId v) override { return p_->constant_value(v, value_point(v)); }
  bool instantiation_gave_up() override { return p_->instantiation_gave_up(); }
  std::optional<QualType> specialization_type(EntityId v) override { return p_->specialization_type(v, point()); }
  const std::vector<sema::Expansion>* pattern_expansions() override { return at_point ? p_->expansions(item_, 0) : nullptr; }
  bool is_union_class(EntityId c) override { return p_->is_union_class(c, point()); }
  std::optional<bool> satisfied(ConstId c) override { return p_->satisfied(c, point()); }
  bool satisfaction_failed(ConstId c) override { return p_->satisfaction_failed(c); }
  bool value_dependent_member(EntityId v) override { return p_->is_value_dependent_member(v); }
  std::optional<ast::DeclSema> template_decl(EntityId t) override { return p_->declaration(t, p_->end()); }
  bool use_static_member(EntityId v) override {
    p_->use_member(v);
    return !p_->member_failed(v);
  }
  std::vector<EntityId> defining_bases(EntityId c) override {
    std::vector<EntityId> r;
    if (auto b = p_->class_bases_.find(std::to_underlying(c)); b != p_->class_bases_.end())
      for (const sema::BaseRec& x : b->second) r.push_back(x.cls);
    return r;
  }
  void static_local(EntityId v, QualType t, std::optional<ConstId> value) override {
    p_->static_locals_[std::to_underlying(v)] = {t, value};
  }
  std::optional<ast::DeclSema> earlier_member(ast::NameId name) override {
    const items::Item& it = p_->items().items[item_];
    if (it.parent == items::none || p_->items().items[it.parent].kind != ItemKind::class_def) return std::nullopt;
    std::optional<ast::DeclSema> r;
    for (u32 m : p_->items().items[it.parent].members) {
      if (m == item_) break;
      // In a class template's instantiation, the specialization's own records of its members.
      const ast::ItemSema& ms = instantiating != EntityId::none && p_->items().items[m].kind != ItemKind::access_spec
                                    ? p_->class_member_sema(m, instantiating)
                                    : p_->item_sema(m).value;
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(ms.decls))
        if (d.entity != EntityId::none && d.kind != ast::DeclKind::class_ && !(d.flags & sema::decl_flag::friend_) &&
            p_->world().entities.rec(d.entity).name == name)
          r = d;
    }
    return r;
  }
  // Past the closing brace of the class the item defines, that class is complete.
  bool own_class_complete = false;
  EntityId completing_class = EntityId::none;
  // In a class template's instantiation (#88): completeness and constant evaluation are answered
  // at its point of instantiation ([temp.point]); lookup stays at the item.
  std::optional<u32> at_point;
  EntityId instantiating = EntityId::none;  // the class specialization whose member this is
  // In a re-analysed operand: the offsets of calls that were dependent where it appears,
  // which argument-dependent lookup at the point of use completes ([temp.dep.candidate]).
  std::vector<u32> instantiation_calls;
  bool dependent_call(sema::u32 offset) override { return std::ranges::contains(instantiation_calls, offset); }

private:
  bool is_complete_at_item(EntityId c) {
    return (completing_class != EntityId::none && completing_class == c) ||
           p_->is_complete(c, item_, self_, own_class_complete);
  }
  u32 value_point(EntityId v) const {
    if (completing_class == EntityId::none || v == EntityId::none) return point();
    const auto& entities = p_->world().entities;
    EntityId scope = entities.rec(v).scope;
    if (entities.rec(v).kind == sema::EntityKind::enumerator) scope = entities.rec(scope).scope;
    if (scope != completing_class) return point();
    return p_->last_member(p_->class_items_.at(std::to_underlying(completing_class)));
  }
  u32 point() const { return at_point ? *at_point : item_; }
  Program* p_;
  u32 item_;
  bool self_;
};


// Sema's questions in an instantiation of the function template that item `item` defines, at
// the end of the translation unit (§B6.2). Unqualified names are looked up at the template's own
// item, as its definition was ([temp.res], [temp.nondep]); argument-dependent lookup for the
// calls that were dependent, completeness and constant evaluation are answered at the end.
// (Fable's decision on #88 asks non-dependent constructs at the template's context; the end
// answers the same unless a later declaration changes them, which [temp.res.general]/6 makes
// ill-formed, no diagnostic required.)
class AtInstantiation final : public sema::Questions {
public:
  AtInstantiation(Program& p, u32 item) : p_(&p), item_(item), at_(p, item, true) {}
  const sema::Classes& classes() override { return p_->classes(); }
  sema::CopyAssignment require_copy_assignment(EntityId f) override { return p_->require_copy_assignment(f); }
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r) override { return p_->publish_class(c, std::move(r), item_); }
  std::vector<DeclId> visible_decls(ast::NameId name, std::span<const EntityId> directives = {}) override {
    return at_.visible_decls(name, directives);
  }
  std::optional<EntityId> namespace_named(ast::NameId name, std::span<const EntityId> directives = {}) override {
    return at_.namespace_named(name, directives);
  }
  std::vector<ast::DeclSema> namespace_members(EntityId ns, ast::NameId name) override { return at_.namespace_members(ns, name); }
  std::vector<ast::DeclSema> adl_functions(EntityId ns, ast::NameId name, bool at_instantiation) override {
    return at_instantiation ? p_->adl_functions(ns, name, p_->end(), false) : at_.adl_functions(ns, name, false);
  }
  const sema::ClassRec* adl_class(EntityId c, bool at_instantiation) override {
    if (!at_instantiation) return at_.adl_class(c, false);
    return is_complete(c) ? p_->classes().find(c) : nullptr;
  }
  ast::DeclSema decl_sema(DeclId d) override { return at_.decl_sema(d); }
  void use_allocation_function(EntityId f) override { at_.use_allocation_function(f); }
  interp::Callee function_ir(EntityId f) override { return p_->function_ir(f, p_->end()); }
  std::optional<QualType> deduced_type(EntityId f) override { return p_->deduced_type(f, p_->end()); }
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, const sema::GenericBinding&) override {
    return p_->generic_instance(spec, known, p_->end());
  }
  void generic_defined(const sema::GenericClosure& g) override { p_->generic_defs_[std::to_underlying(g.call)] = g; }
  std::optional<interp::GlobalVar> global(EntityId v) override { return p_->global(v, p_->end()); }
  bool is_constexpr(EntityId f) override { return p_->is_constexpr(f, p_->end()); }
  bool is_complete(EntityId c) override { return p_->is_complete(c, p_->end(), true, true); }
  std::optional<ConstId> constant_value(EntityId v) override { return p_->constant_value(v, p_->end()); }
  std::optional<ast::DeclSema> earlier_member(ast::NameId) override { return std::nullopt; }
  bool instantiation_gave_up() override { return p_->instantiation_gave_up(); }
  std::optional<QualType> specialization_type(EntityId v) override { return p_->specialization_type(v, p_->end()); }
  const std::vector<sema::Expansion>* pattern_expansions() override { return p_->expansions(item_, 1); }
  bool is_union_class(EntityId c) override { return p_->is_union_class(c, p_->end()); }
  std::optional<bool> satisfied(ConstId c) override { return p_->satisfied(c, p_->end()); }
  bool satisfaction_failed(ConstId c) override { return p_->satisfaction_failed(c); }
  bool value_dependent_member(EntityId v) override { return p_->is_value_dependent_member(v); }
  std::optional<ast::DeclSema> template_decl(EntityId t) override { return p_->declaration(t, p_->end()); }
  bool use_static_member(EntityId v) override {
    p_->use_member(v);
    return !p_->member_failed(v);
  }
  void static_local(EntityId v, QualType t, std::optional<ConstId> value) override { p_->static_locals_[std::to_underlying(v)] = {t, value}; }
  bool dependent_call(sema::u32 offset) override {
    return p_->dependent_call(item_, offset);
  }

  AtItem& pattern_context() { return at_; }

private:
  Program* p_;
  u32 item_;
  AtItem at_;  // the template's own context
};

// ---------------------------------------------------------------- item_sema

// A part's pack expansions as the records its value keeps (ast.md §6.4).
ast::SeqId<ast::ExpansionRec> expansion_list(ast::Builder& b, const std::vector<sema::Expansion>& es) {
  std::vector<ast::ExpansionRec> out;
  for (const sema::Expansion& e : es) {
    std::vector<ast::PackRefRec> packs;
    for (const sema::PackRef& r : e.packs) packs.push_back({r.depth, r.index, r.fn});
    out.push_back({e.begin, e.end, b.seq(std::span<const ast::PackRefRec>(packs))});
  }
  return b.seq(std::span<const ast::ExpansionRec>(out));
}

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
  // `inst` analyses a member item of a class template for the specialization it names, with the
  // template's arguments bound (ast.md §6.4 as amended by #88).
  ItemAnalysis(Program& p, u32 item, std::optional<Program::ClassInstantiation> inst = std::nullopt)
      : p_(&p), k_(item), it_(&p.items().items[item]), b_(p.world().ins, &p.arena_), q_(p, item, false),
        s_(p.world(), b_, q_, out_.diags, p.id(item), 0),
        ps_(p.region(), it_->begin, it_->begin + it_->decl_length(), s_), inst_(std::move(inst)) {
    ps_.set_questions(q_);
    if (inst_) {
      q_.at_point = inst_->point;
      q_.instantiating = inst_->spec;
    }
    // A member of a class template is in the scope of the template's head ([basic.scope.temp]).
    for (auto& head : p_->enclosing_heads(item)) s_.template_scopes.push_back(std::move(head));
    if (inst_) s_.template_args = inst_->args;
    // An elaborated-type-specifier naming no class declares one in the namespace
    // ([basic.scope.pdecl]); its entity is the one its definition will have.
    ps_.qualified_hook = [this](const sema::Sema::Scope& sc) {
      if (sc.ns) {
        q_.from = p_->namespace_path(*sc.ns);
      } else if (sc.type && s_.types().is_class(*sc.type)) {
        EntityId c = s_.types().class_of(*sc.type);
        s_.current_class = c;
        q_.from = p_->namespace_path(s_.world().entities.rec(c).scope);
      }
    };
    ps_.declare_class = [this](NameId n) -> std::optional<QualType> {
      if (p_->items().items[k_].parent != items::none &&
          p_->items().items[p_->items().items[k_].parent].kind == ItemKind::class_def)
        return std::nullopt;
      return s_.types().class_type(class_entity(n));
    };
  }

  // Analyses a member item of a class template specialization again, instantiating the
  // initializer of its static data member with declarator `ordinal` ([temp.inst]/3).
  ItemAnalysis(Program& p, u32 item, Program::ClassInstantiation inst, u32 ordinal) : ItemAnalysis(p, item, std::move(inst)) {
    member_value_ = ordinal;
  }

  // `var` analyses a variable template's definition for the specialization it names, with the
  // template's arguments bound ([temp.inst]/7).
  ItemAnalysis(Program& p, u32 item, Program::VarInstantiation var) : ItemAnalysis(p, item) {
    var_ = std::move(var);
    q_.at_point = var_->point;
    s_.template_args = var_->args;
  }

  Result<ast::ItemSema> run() {
    try {
      analyse();
    } catch (const TooDeep&) {
      // Diagnosed: the item fails, its scopes unwound with the parse.
      tree_ = NodeId::none;
      declarators_.clear();
      bad();
    }
    return finish();
  }

  void analyse() {
    bool member = it_->parent != items::none && p_->items().items[it_->parent].kind == ItemKind::class_def;
    switch (it_->kind) {
    case ItemKind::empty:
    case ItemKind::access_spec: break;
    case ItemKind::stray:  // a `}` that closes nothing ([gram]: no declaration begins with it)
      s_.diag(0, Diag::extraneous_brace);
      bad();
      break;
    case ItemKind::linkage_block:  // its members are items of their own
      braces_closed();
      break;
    case ItemKind::declaration:
      if (member) member_declaration();
      else declaration();
      break;
    case ItemKind::function:
      if (member) member_declaration();
      else declaration();
      break;
    case ItemKind::enum_def:
      enum_declaration(member ? (inst_ ? inst_->spec : p_->class_entity(it_->parent)) : EntityId::none);
      break;
    case ItemKind::namespace_def: namespace_definition(); break;
    case ItemKind::class_def:
      if (member && it_->unnamed_type_ordinal != items::none) {
        unnamed_union_member();
      } else if (member) {
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
  }

  // The item's record and tree, from what the analysis left.
  Result<ast::ItemSema> finish() {
    NodeId root = tree_;
    if (root == NodeId::none) {
      std::vector<ast::Child> cs;
      for (const Ex& d : declarators_) cs.push_back(sema::Sema::at(d, 0));
      root = s_.node(ast::DeclGroup{b_.list(cs)}, 0, it_->decl_length()).id;
    }
    std::vector<sema::Expansion> none;
    ast::ItemSema v{root, b_.seq(std::span<const ast::DeclSema>(decls_)), expansion_list(b_, inst_ || var_ ? none : s_.expansions),
                    b_.seq(std::span<const ast::ClosureDef>(s_.closures))};
    out_.value = b_.commit(v);
    s_.check_immediate_addresses(out_.value.tree);
    if (!s_.closures.empty()) s_.check_labels();
    for (const ast::ClosureDef& c : p_->world().ins.seqs.elements(out_.value.closures))
      s_.check_immediate_addresses(c.body, c.body_begin);
    out_.demands = std::move(s_.demands);
    return std::move(out_);
  }

private:
  // The language linkage of the enclosing linkage-specifications ([dcl.link]): the innermost.
  std::optional<LangLinkage> enclosing_linkage() {
    for (u32 j = it_->parent; j != items::none; j = p_->items().items[j].parent) {
      const items::Item& outer = p_->items().items[j];
      if (outer.kind == ItemKind::linkage_block)
        if (auto l = linkage_named(p_->region(), outer.begin + 1)) return *l;
    }
    return std::nullopt;
  }

  // A simple-declaration or the declaration part of a function-definition ([dcl.pre],
  // [dcl.fct.def.general]), possibly after `extern "C"` ([dcl.link]).
  void declaration() {
    const auto enclosing = enclosing_linkage();
    LangLinkage lang = enclosing.value_or(LangLinkage::cxx);
    bool explicit_lang = enclosing.has_value();
    u32 attrs_at = ps_.pos();
    // [dcl.link]: a linkage-specification has no attribute-specifier-seq.
    // (GNU attributes there are accepted, as GCC and Clang do.)
    if ((ps_.skip_attributes() & parse::Parser::attr_std) && ps_.is(Tk::kw_extern) && ps_.kind(1) == Tk::string_lit) {
      s_.diag(attrs_at, Diag::attributes_not_allowed);
      return bad();
    }
    if (ps_.is(Tk::kw_extern) && ps_.kind(1) == Tk::string_lit) {
      u32 at = ps_.pos() + 1;
      auto l = linkage_named(p_->region(), it_->begin + at);
      if (!l) s_.diag(at, Diag::unknown_linkage);
      lang = l.value_or(LangLinkage::cxx);
      explicit_lang = true;
      single_linkage_spec_ = true;  // [dcl.link]/7: it declares as if `extern` were given
      ps_.bump();
      ps_.bump();
      while (ps_.is(Tk::string_lit) && ps_.tok().has(lex::tok_flag::continued)) ps_.bump();
      ps_.skip_attributes();
    }
    ps_.set_linkage(lang);
    // `template<>`: an explicit specialization ([temp.expl.spec]) of a class, function or variable
    // template. Specializations of members of class templates are not supported yet.
    if (ps_.is(Tk::kw_template) && ps_.kind(1) == Tk::less && ps_.kind(2) == Tk::greater) {
      if (ps_.kind(3) == Tk::kw_template) {
        s_.diag(ps_.pos() + 3, Diag::not_supported, Tk::kw_template);
        return bad();
      }
      ps_.bump();
      ps_.bump();
      ps_.bump();
      explicit_ = true;
      ps_.template_ids_in_declarators = true;
      // `template<> class-key X<args>;` declares the specialization; it is incomplete until defined.
      if ((ps_.is(Tk::kw_struct) || ps_.is(Tk::kw_class)) && ps_.kind(1) == Tk::identifier && ps_.kind(2) != Tk::less) {
        s_.diag(ps_.pos() + 1, Diag::specialization_needs_arguments, ps_.name_of(ps_.tok(1)));
        return bad();
      }
      if ((ps_.is(Tk::kw_struct) || ps_.is(Tk::kw_class)) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::less) {
        ps_.bump();
        NameId name = ps_.name_of(ps_.tok());
        u32 name_at = ps_.pos();
        ps_.bump();
        auto spec = explicit_class(name, name_at);
        if (!spec) return bad();
        if (!ps_.expect(Tk::semi)) return bad();
        ast::DeclSema sem = class_decl(*spec, false);
        sem.flags |= sema::decl_flag::specialization;
        decls_.push_back(sem);
        p_->declared(sem, k_);
        return;
      }
    }
    // A template-head ([temp.pre]): what follows declares a function or variable template, or
    // declares a class template. Alias templates and nested heads are not supported yet.
    else if (ps_.is(Tk::kw_template)) {
      if (ps_.kind(1) != Tk::less) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_template);
        return bad();
      }
      if (!ps_.template_head(0)) return bad();
      head_requires_clause();
      // `template<class U> template<class T> R X<U>::f(T)`: a member template of a class template
      // defined outside it ([temp.mem]/1); its body is in the scope of both heads.
      if (ps_.is(Tk::kw_template) && ps_.kind(1) == Tk::less) {
        p_->set_outer_heads(k_, {s_.template_scopes.front()});
        if (!ps_.template_head(1)) return bad();
        head_requires_clause();
        member_template_ = true;
        two_heads_ = true;
      }
      ps_.skip_attributes();
      template_ = true;
      ps_.template_ids_in_declarators = true;  // `template<…> T v<args>`: a partial specialization
      // `template<…> concept C = constraint-expression;` ([temp.concept]).
      if (ps_.is(Tk::kw_concept)) return concept_definition();
      // `template<…> struct X;` declares a class template ([temp.class]), defined later.
      if ((ps_.is(Tk::kw_struct) || ps_.is(Tk::kw_class)) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::semi) {
        NameId name = ps_.name_of(ps_.tok(1));
        ps_.bump();
        ps_.bump();
        ps_.bump();
        ast::DeclSema sem = class_template_decl(name, false);
        decls_.push_back(sem);
        p_->declared(sem, k_);
        return;
      }
      // `template<…> using N = type-id;` declares an alias template ([temp.alias]).
      if (ps_.is(Tk::kw_using) && ps_.kind(1) == Tk::identifier && ps_.kind(ps_.past_attributes(2)) == Tk::equal) {
        ps_.bump();
        NameId name = ps_.name_of(ps_.tok());
        u32 name_at = ps_.pos();
        ps_.bump();
        ps_.skip_attributes();
        ps_.bump();
        auto t = ps_.type_id();
        if (!t || !alias_template(name, name_at, *t)) return bad();
        if (!ps_.expect(Tk::semi)) bad();
        return;
      }
      // `template<…> struct X<args>;` declares a partial specialization ([temp.spec.partial]).
      if ((ps_.is(Tk::kw_struct) || ps_.is(Tk::kw_class)) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::less) {
        ps_.bump();
        NameId name = ps_.name_of(ps_.tok());
        u32 name_at = ps_.pos();
        ps_.bump();
        auto pattern = partial_class(name, name_at);
        if (!pattern) return bad();
        if (!ps_.expect(Tk::semi)) return bad();
        ast::DeclSema sem = class_decl(*pattern, false);
        sem.flags |= sema::decl_flag::template_ | sema::decl_flag::partial;
        sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
        decls_.push_back(sem);
        p_->declared(sem, k_);
        return;
      }
      if (ps_.is(Tk::kw_template) || ps_.is(Tk::kw_struct) || ps_.is(Tk::kw_class) || ps_.is(Tk::kw_union) ||
          ps_.is(Tk::kw_using) || ps_.is(Tk::kw_concept) || (it_->kind != ItemKind::function && !ps_.starts_declaration())) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        return bad();
      }
      template_ = true;
    }
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
    if (!template_ && !explicit_ && ps_.is(Tk::kw_enum)) {
      bool scoped = ps_.kind(1) == Tk::kw_class || ps_.kind(1) == Tk::kw_struct;
      u32 name = ps_.past_attributes(scoped ? 2 : 1);
      bool qualified = ps_.kind(name) == Tk::coloncolon;
      if (qualified) ++name;
      while (ps_.kind(name) == Tk::identifier && ps_.kind(name + 1) == Tk::coloncolon) {
        qualified = true;
        name += 2;
      }
      if (ps_.kind(name) == Tk::identifier && (ps_.kind(name + 1) == Tk::semi || ps_.kind(name + 1) == Tk::colon)) {
        if (qualified) {
          s_.diag(ps_.pos() + name, Diag::qualified_opaque_enum);  // [dcl.enum]: except explicit specializations
          return bad();
        }
        return enum_declaration(EntityId::none);
      }
    }
    // `using namespace N;` and `namespace A = N;` at namespace scope ([namespace.udir],
    // [namespace.alias]).
    if (ps_.is(Tk::kw_using) && ps_.kind(1) == Tk::kw_namespace) {
      ps_.bump();
      ps_.bump();
      if (auto ns = ps_.namespace_spec()) p_->set_directive_target(k_, p_->namespace_path(*ns));
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    if (ps_.is(Tk::kw_namespace) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::equal) {
      ps_.bump();
      ps_.bump();
      ps_.bump();
      auto ns = ps_.namespace_spec();
      if (!ns) return bad();
      ast::DeclSema sem = class_decl(*ns, true);
      sem.kind = ast::DeclKind::namespace_;
      sem.type = s_.builtin(sema::Builtin::void_);
      decls_.push_back(sem);
      p_->set_directive_target(k_, p_->namespace_path(*ns));
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    // A using-declaration ([namespace.udecl]): `using N::x, ::y;`, each declarator introducing
    // what qualified lookup finds at this point.
    if (ps_.is(Tk::kw_using) && ps_.kind(1) != Tk::kw_namespace && ps_.kind(1) != Tk::kw_enum &&
        !(ps_.kind(1) == Tk::identifier && ps_.kind(ps_.past_attributes(2)) == Tk::equal)) {
      ps_.bump();
      do {
        if (!using_declarator()) {
          bad();
          ps_.skip_to_semi();
          return;
        }
      } while (ps_.accept(Tk::comma));
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    // An alias-declaration ([dcl.typedef]/2): `using T = type-id;`.
    if (ps_.is(Tk::kw_using) && ps_.kind(1) == Tk::identifier && ps_.kind(ps_.past_attributes(2)) == Tk::equal) {
      ps_.bump();
      NameId n = ps_.name_of(ps_.tok());
      u32 at = ps_.pos();
      ps_.bump();
      ps_.skip_attributes();  // appertaining to the alias ([dcl.pre]/1)
      ps_.bump();
      auto t = ps_.type_id();
      if (!t || !declare_alias(n, at, *t)) return bad();
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    const u32 ctor_at = constructor_start();
    if (ctor_at == 0 ? ps_.qualified_ahead() : qualified_constructor_ahead(ctor_at)) {
      u32 next = 0;
      auto sc = ps_.nested_name(ctor_at, next);
      if (sc && sc->type && s_.types().is_class(*sc->type)) {
        NameId cname = p_->world().entities.rec(s_.types().class_of(*sc->type)).name;
        bool ctor = ps_.kind(next) == Tk::identifier && ps_.name_of(ps_.tok(next)) == cname && ps_.kind(next + 1) == Tk::l_paren;
        bool dtor = ps_.kind(next) == Tk::tilde && ps_.kind(next + 1) == Tk::identifier &&
                    ps_.name_of(ps_.tok(next + 1)) == cname;
        if (ctor || dtor) {
          auto flags = constructor_flags(ctor_at);
          if (!flags) return bad();
          return out_of_class_special(*sc->type, next - ctor_at, *flags);
        }
      }
    }
    if (!ps_.starts_declaration()) {
      if (lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon))
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // templates, using, static_assert, …
      else
        s_.diag(ps_.pos(), Diag::expected_declaration);
      return bad();
    }
    s_.defer_type_access = true;
    DeclSpec spec = ps_.decl_specifiers();
    if (spec.ok && spec.elaborated && s_.types().is_enum(spec.type)) {
      EntityId e = s_.types().enum_of(spec.type);
      if (!ps_.is(Tk::semi) || s_.world().entities.rec(e).scope == ns_entity()) {
        s_.settle_type_access(EntityId::none);
        s_.diag(spec.class_at, Diag::conflicting_declaration, spec.class_name);
        return bad();
      }
      // A standalone class declaration in an inner namespace introduces a name there.
      spec.type = s_.types().class_type(class_entity(spec.class_name));
      if (clashes_with_using(spec.class_name, spec.class_at, ast::DeclKind::class_, spec.type)) {
        s_.settle_type_access(EntityId::none);
        return bad();
      }
    }
    if (spec.elaborated && ps_.is(Tk::semi)) {
      s_.settle_type_access(EntityId::none);
      // `class-key identifier ;` declares the class ([dcl.type.elab]); it stays incomplete until
      // its definition.
      ps_.bump();
      if (spec.ok) {
        decls_.push_back(class_decl(s_.types().class_of(spec.type), false));
        p_->declared(decls_.back(), k_);  // its class-key says whether it is a union
      }
      if (!spec.ok || sema::Types::cv(spec.type) != sema::Cv::none || spec.is_static || spec.is_extern) bad();
      return;
    }
    bool is_function_def = it_->kind == ItemKind::function;
    u32 declarators = 0;
    do {
      s_.defer_type_access = true;
      parse::Declarator d = ps_.declarator(spec.type, false, spec.is_typedef);
      // [dcl.spec.auto.general]: a function with a placeholder type is the only declarator.
      if (d.ok && s_.types().has_placeholder(spec.type) && s_.types().is_function(d.type) && (declarators > 0 || ps_.is(Tk::comma))) {
        s_.diag(d.name_at, Diag::auto_not_allowed);
        s_.settle_type_access(EntityId::none);
        bad();
        ps_.skip_to_semi();
        return;
      }
      ++declarators;
      bool declared = d.ok && spec.ok && d.named && declare(spec, d, lang, explicit_lang, is_function_def);
      s_.settle_type_access(EntityId::none);  // what `declare` did not settle
      if (!declared) {
        // Later declarators keep their skeleton ordinals with placeholder records.
        bad();
        if (!is_function_def) ps_.skip_to_semi();
        return;
      }
      // [temp.pre]/5: a template-declaration declares one name.
      if (template_ && ps_.is(Tk::comma)) {
        s_.diag(ps_.pos(), Diag::unexpected_token, Tk::comma);
        bad();
        ps_.skip_to_semi();
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

  // One using-declarator at namespace scope: `N::x` or `::x` ([namespace.udecl]).
  bool using_declarator() {
    ps_.accept(Tk::kw_typename);
    u32 next = 0;
    auto sc = ps_.nested_name(0, next);
    if (!sc || !ps_.lookup_id_ahead(next)) {
      s_.diag(ps_.pos(), Diag::not_a_namespace, ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : s_.no_name());
      return false;
    }
    for (u32 j = 0; j < next; ++j) ps_.bump();
    u32 at = ps_.pos();
    auto name = ps_.lookup_id();
    if (!name) return false;
    NameId n = *name;
    if (!sc->ns) {
      // [namespace.udecl]/8: a class member's only as a member-declaration; an enumerator's
      // (/7) is not supported yet.
      if (sc->type && s_.types().is_class(*sc->type)) s_.diag(at, Diag::using_class_member, n);
      else s_.diag(at, Diag::not_supported, Tk::kw_using);
      return false;
    }
    std::vector<DeclId> found;
    ast::DeclSema first{};
    for (DeclId d : p_->qualified_decls(p_->world().ins.names.spelling(n), p_->namespace_path(*sc->ns), k_, false))
      if (auto ds = p_->decl_sema(d); ds && ds->entity != EntityId::none) {
        if (ds->kind == ast::DeclKind::namespace_) {  // [namespace.udecl]/3: it names no namespace
          s_.diag(at, Diag::using_namespace_name, n);
          return false;
        }
        if (found.empty()) first = *ds;
        found.push_back(d);
      }
    if (found.empty()) {
      s_.diag(at, Diag::no_namespace_member, n);
      return false;
    }
    // [namespace.udecl]/10: a target may not conflict with a declaration of its name in this scope,
    // nor with an earlier declarator's of this using-declaration.
    std::vector<DeclId> here = p_->declared_in(p_->world().ins.names.spelling(n), p_->ns_of(k_), k_, false, true);
    for (const auto& [key, targets] : p_->using_targets_)
      if (key.first == k_ && p_->items().items[k_].names[key.second].name == p_->world().ins.names.spelling(n))
        here.insert(here.end(), targets.begin(), targets.end());
    for (DeclId p : here)
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none)
        for (DeclId f : found)
          if (auto t = p_->decl_sema(f); t && conflict(*ds, *t)) {
            s_.diag(at, Diag::conflicting_declaration, n);
            return false;
          }
    // Its own record stands for the first of them; lookup takes them all from `using_targets_`.
    p_->using_targets_[{k_, static_cast<u32>(decls_.size())}] = std::move(found);
    ast::DeclSema own = class_decl(EntityId::none, false);  // built in this item's frame
    own.kind = first.kind;
    own.type = first.type;
    decls_.push_back(own);
    return true;
  }

  // Whether two declarations of one name in one scope conflict ([basic.scope.scope]/4): not the
  // same entity, not two names of one type (typedef-names, classes, enumerations: [dcl.typedef]/5),
  // not both functions (overloads), and not a class or enumeration and a variable or function,
  // which hides it.
  bool conflict(const ast::DeclSema& a, const ast::DeclSema& b) {
    using K = ast::DeclKind;
    auto type = [](K k) { return k == K::class_ || k == K::enum_; };
    auto names_type = [&](K k) { return type(k) || k == K::type_alias; };
    auto object = [](K k) { return k == K::variable || k == K::function; };
    if (a.entity == b.entity || (a.kind == K::function && b.kind == K::function)) return false;
    if (names_type(a.kind) && names_type(b.kind)) return s_.types().canonical(a.type) != s_.types().canonical(b.type);
    return !((type(a.kind) && object(b.kind)) || (object(a.kind) && type(b.kind)));
  }
  // Whether a declaration `d` of `n` here conflicts with what an earlier using-declaration of this
  // scope introduced ([namespace.udecl]/10).
  bool clashes_with_using(NameId n, u32 at, ast::DeclKind kind, QualType type = {}) {
    ast::DeclSema d{};
    d.kind = kind;
    d.type = type;
    d.entity = EntityId::none;
    for (DeclId u : p_->used_in(p_->world().ins.names.spelling(n), p_->ns_of(k_), k_))
      if (auto t = p_->decl_sema(u); t && conflict(d, *t)) {
        s_.diag(at, Diag::conflicting_declaration, n);
        return true;
      }
    return false;
  }

  // A member operator function ([over.oper.general]/7–10, [over.unary], [over.binary],
  // [over.assign]): not static except `()` and `[]` ([over.call], [over.sub]), and with the
  // operator's arity less the object.
  bool member_operator_ok(const parse::Declarator& d, bool is_static) {
    std::string_view n = p_->world().ins.names.spelling(d.name);
    if (n == lex::conversion_function_name) {
      if (is_static) s_.diag(d.name_at, Diag::conversion_static);
      else if (!d.params.empty() || d.variadic) s_.diag(d.name_at, Diag::conversion_parameters);
      else return true;
      return false;
    }
    if (!n.starts_with("operator") || n.size() == 8) return true;
    std::string_view op = n.substr(8);
    if (op == "()" || op == "[]") return true;
    if (is_static) {
      s_.diag(d.name_at, Diag::operator_static, d.name);
      return false;
    }
    std::size_t count = d.params.size();
    bool ok = op == "->"                                  ? count == 0
              : op == "~" || op == "!"                    ? count == 0
              : op == "+" || op == "-" || op == "*" || op == "&" || op == "++" || op == "--" ? count <= 1
                                                                                             : count == 1;
    if (!ok) s_.diag(d.name_at, Diag::operator_arity, d.name);
    return ok;
  }

  // A non-member operator function ([over.oper.general]/7–10): a parameter of class or enumeration
  // type (or a reference to one), one or two of them, and not `=`, `()`, `[]` or `->`, which only
  // a member may be.
  // A redeclaration of function `f` ([dcl.fct.default]/4): it inherits the default arguments of
  // the earlier declarations `prior` found, may not give one again, and, with them, every parameter
  // after one with a default argument has one.
  bool merge_defaults(std::vector<ast::ParamSema>& params, std::span<const ast::DeclSema> prior, EntityId f,
                      const parse::Declarator& d) {
    auto given = [&](std::size_t i) { return i < d.params.size() && d.params[i].default_arg != NodeId::none; };
    // An earlier declarator of this declaration: its drafts ([dcl.fct.default]/4).
    if (auto o = own_defaults_.find(std::to_underlying(f)); o != own_defaults_.end())
      for (std::size_t i = 0; i < params.size() && i < o->second.size(); ++i) {
        if (o->second[i] == NodeId::none) continue;
        if (given(i)) {
          s_.diag(d.params[i].at, Diag::default_arg_redefined);
          return false;
        }
        params[i].default_arg = o->second[i];
      }
    for (const ast::DeclSema& o : prior) {
      if (o.entity != f) continue;
      std::vector<ast::ParamSema> earlier = p_->world().ins.seqs.elements(o.params);
      for (std::size_t i = 0; i < params.size() && i < earlier.size(); ++i) {
        // A function template's later declarations add none ([dcl.fct.default]/4).
        if (given(i) && (o.flags & sema::decl_flag::template_) && earlier[i].default_arg == NodeId::none) {
          s_.diag(d.params[i].at, Diag::default_arg_template_redeclared);
          return false;
        }
        if (earlier[i].default_arg == NodeId::none) continue;
        if (given(i)) {
          s_.diag(d.params[i].at, Diag::default_arg_redefined);
          return false;
        }
        if (params[i].default_arg == NodeId::none) params[i].default_arg = b_.adopt(earlier[i].default_arg);
      }
    }
    return defaults_trail(params, d.params, d.name_at);
  }
  // [dcl.fct.default]/4: after a parameter with a default argument, each has one, but for a pack.
  bool defaults_trail(std::span<const ast::ParamSema> params, std::span<const parse::Param> written, u32 name_at) {
    bool seen = false;
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (params[i].default_arg != NodeId::none || (i < written.size() && written[i].default_tokens)) seen = true;
      else if (seen && !(params[i].flags & sema::param_flag::pack_element) &&
               s_.types().kind(params[i].type) != sema::TypeKind::pack_expansion) {
        s_.diag(i < written.size() ? written[i].at : name_at, Diag::default_arg_missing);
        return false;
      }
    }
    return true;
  }
  // Default arguments are allowed only in a function declaration's own parameters
  // ([dcl.fct.default]/3), and of operator functions only for `operator()` and `operator[]`
  // ([over.oper.general]/10).
  bool defaults_allowed(std::span<const parse::Param> written, bool function, NameId name) {
    auto d = std::ranges::find_if(written, [](const parse::Param& p) { return p.default_arg != NodeId::none || p.default_tokens.has_value(); });
    if (d == written.end()) return true;
    std::string_view n = p_->world().ins.names.spelling(name);
    bool op = n.starts_with("operator") && n.size() > 8 && !std::isalnum(static_cast<unsigned char>(n[8])) && n[8] != '_';
    if (function && (!op || n == "operator()" || n == "operator[]")) return true;
    s_.diag(d->at, Diag::default_arg_not_allowed);
    return false;
  }

  bool operator_function_ok(const parse::Declarator& d) {
    std::string_view n = p_->world().ins.names.spelling(d.name);
    if (!n.starts_with("operator") || n.size() == 8 || n.starts_with(lex::literal_operator_prefix)) return true;
    std::string_view op = n.substr(8);
    if (sema::is_allocation_name(n)) return true;  // checked against the implicit declarations
    sema::Types& types = s_.types();
    if (op == "=" || op == "()" || op == "[]" || op == "->") {
      s_.diag(d.name_at, Diag::operator_member_only, d.name);
      return false;
    }
    std::size_t count = d.params.size();
    bool unary_only = op == "~" || op == "!", binary_only = !(unary_only || op == "+" || op == "-" || op == "*" || op == "&" ||
                                                              op == "++" || op == "--");
    if (count == 0 || count > 2 || (unary_only && count != 1) || (binary_only && count != 2)) {
      s_.diag(d.name_at, Diag::operator_arity, d.name);
      return false;
    }
    if (!std::ranges::any_of(d.params, [&](const parse::Param& p) {
          QualType t = types.referee(p.type);
          return types.is_class(t) || types.is_enum(t) || types.is_dependent(t);
        })) {
      s_.diag(d.name_at, Diag::operator_needs_class, d.name);
      return false;
    }
    return true;
  }

  bool literal_operator_named(NameId n) { return p_->world().ins.names.spelling(n).starts_with(lex::literal_operator_prefix); }

  bool literal_operator_ok(const DeclSpec& spec, const parse::Declarator& d, LangLinkage lang, bool templ) {
    sema::Types& types = s_.types();
    if (spec.is_typedef || !types.is_function(d.type) || (d.qualified && !d.scope.ns)) {
      s_.diag(d.name_at, Diag::literal_operator_scope);
      return false;
    }
    if (lang == LangLinkage::c) {
      s_.diag(d.name_at, Diag::literal_operator_linkage);
      return false;
    }
    if (templ) {
      auto f = types.rec(d.type);
      const auto& head = s_.template_scopes.back();
      bool string_template = head.size() == 1 && head[0].kind == ast::TemplateArgKind::value && !head[0].pack &&
                             types.is_class(head[0].type);
      if (!f.variadic && f.params.empty() && (s_.numeric_literal_template(head) || string_template)) return true;
    } else if (s_.literal_operator_form(d.type) != sema::Sema::LiteralOperatorForm::invalid) {
      return true;
    }
    s_.diag(d.name_at, Diag::literal_operator_parameters);
    return false;
  }

  // A typedef-name or alias-declaration at namespace scope ([dcl.typedef]). In its scope it may
  // only redeclare a typedef-name, class or enumeration as the type it already is (/5, /6).
  bool declare_alias(NameId name, u32 at, QualType type) {
    sema::Types& types = s_.types();
    if (types.has_placeholder(type)) {  // [dcl.spec.auto.general]/6: no placeholder in a typedef
      s_.diag(at, Diag::auto_not_allowed);
      return false;
    }
    auto clashes = [&](const ast::DeclSema& o) {
      bool same_type = (o.kind == ast::DeclKind::type_alias || o.kind == ast::DeclKind::class_ || o.kind == ast::DeclKind::enum_) &&
                       types.canonical(o.type) == types.canonical(type);
      return !same_type;
    };
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && same_scope(*ds) && clashes(*ds)) {
        s_.diag(at, Diag::conflicting_declaration, name);
        return false;
      }
    for (const auto& o : s_.own)
      if (o.name == name && clashes(o.sema)) {
        s_.diag(at, Diag::conflicting_declaration, name);
        return false;
      }
    if (clashes_with_using(name, at, ast::DeclKind::type_alias, type)) return false;
    ast::DeclSema sem = class_decl(s_.world().entities.entity({sema::EntityKind::type_alias, name, {}, LangLinkage::cxx,
                                                                 ast::Linkage::none, {}, ns_entity()}),
                                   false);
    sem.kind = ast::DeclKind::type_alias;
    sem.type = type;
    sem.linkage = ast::Linkage::none;
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    return true;
  }

  // Whether integral type `t` represents `v` (`big`: the unsigned value of v's bits, 2^63 or more).
  bool holds(QualType t, std::int64_t v, bool big) {
    sema::IntShape sh = sema::shape(s_.world().target, *s_.types().builtin_of(t));
    if (big) return !sh.is_signed && sh.bits == 64;
    if (sh.is_signed) return sh.bits >= 64 || (v >= -(std::int64_t(1) << (sh.bits - 1)) && v < (std::int64_t(1) << (sh.bits - 1)));
    return v >= 0 && (sh.bits >= 64 || v < (std::int64_t(1) << sh.bits));
  }

  // `typedef class-specifier X …;` with an unnamed class or enumeration: X names it for linkage
  // purposes ([dcl.typedef]/9), and so is its name here; the declarator after the braces.
  std::optional<std::pair<NameId, u32>> typedef_name_after_braces() {
    u32 back = it_->begin + ps_.pos();  // `seek` takes a region index, `pos` is in the item
    ps_.seek(it_->brace_end);
    std::optional<std::pair<NameId, u32>> r;
    if (ps_.is(Tk::identifier)) r = std::pair(ps_.name_of(ps_.tok()), ps_.pos());
    ps_.seek(back);
    return r;
  }

  // Whether a typedef-name of this scope names `n` as another type than `t`, which a class or
  // enumeration named `n` would clash with ([basic.scope.scope]/4, [dcl.typedef]/6).
  // With no type (a class template's name), any typedef-name of that name clashes ([temp.pre]/7).
  bool alias_clash(NameId n, std::optional<QualType> t) {
    for (DeclId p : q_.visible_decls(n))
      if (auto ds = p_->decl_sema(p); ds && ds->kind == ast::DeclKind::type_alias && same_scope(*ds) &&
                                      (!t || s_.types().canonical(ds->type) != s_.types().canonical(*t)))
        return true;
    return false;
  }

  // The entity of the namespace-scope class named `n` (§4.3: scope, name).
  EntityId class_entity(NameId n) {
    return s_.world().entities.entity({sema::EntityKind::class_, n, {}, LangLinkage::cxx, ast::Linkage::external, {}, ns_entity()});
  }
  // The namespace this item is a member of ([basic.namespace]).
  EntityId ns_entity() { return p_->namespace_entity(p_->ns_of(k_)); }
  // Whether an earlier declaration found by lookup is in this item's own scope, the only one a
  // declaration here can redeclare or clash with.
  bool same_scope(const ast::DeclSema& d) { return s_.world().entities.rec(d.entity).scope == ns_entity(); }
  bool same_function_parameters(QualType a, QualType b) {
    const auto& types = s_.types();
    const auto& interner = s_.world().ins.types;
    const auto first = interner.get<ast::TFunction>(types.canonical(a));
    const auto second = interner.get<ast::TFunction>(types.canonical(b));
    return first.params == second.params && first.variadic == second.variadic && first.cv == second.cv;
  }
  // The arguments of a partial specialization of `tmpl` ([temp.spec.partial]), given as `args`:
  // completed, and checked against [temp.spec.partial]/9; none, diagnosed, if they break it.
  std::optional<std::vector<ast::TemplateArg>> partial_arguments(const ast::DeclSema& tmpl, std::span<const ast::TemplateArg> args,
                                                                 u32 at) {
    NameId name = s_.world().entities.rec(tmpl.entity).name;
    auto full = s_.template_arguments(tmpl, args, at);
    if (!full) return std::nullopt;
    std::vector<ast::TemplateParamSema> params = s_.world().ins.seqs.elements(tmpl.template_params);
    const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.back();
    sema::Templates t = s_.templates();
    auto& values = s_.world().ins.values;
    // /9.3: the arguments are not the primary template's own, its parameters in order.
    bool own = full->size() == head.size();
    for (std::size_t i = 0; own && i < full->size(); ++i) {
      const ast::TemplateArg& a = (*full)[i];
      sema::TypeRec r = s_.types().rec(a.type);
      own = a.kind == ast::TemplateArgKind::type
                ? r.kind == sema::TypeKind::template_param && r.index == i && head[i].kind == a.kind &&
                      sema::Types::cv(s_.types().canonical(a.type)) == sema::Cv::none
                : values.is<ast::VDepParam>(a.value) && values.get<ast::VDepParam>(a.value).index == i;
    }
    if (own) {
      s_.diag(at, Diag::partial_same_as_primary, name);
      return std::nullopt;
    }
    for (std::size_t i = 0; i < full->size(); ++i) {
      const ast::TemplateArg& a = (*full)[i];
      if (a.kind != ast::TemplateArgKind::value || values.is<ast::VDepParam>(a.value)) continue;
      // /9.1: a non-type argument is a parameter, or depends on none.
      if (values.is_dependent(a.value)) {
        s_.diag(at, Diag::partial_argument, name);
        return std::nullopt;
      }
      // /9.2: nor does its parameter's type, for the specialization's arguments.
      auto pt = t.substitute(params[i].type, sema::Bindings::of(params.empty() ? 0 : params[0].depth, *full));
      if (pt && s_.types().is_dependent(*pt)) {
        s_.diag(at, Diag::partial_argument_type, name);
        return std::nullopt;
      }
    }
    // Each of its parameters is deduced from its arguments, and it is more specialized than the
    // primary template ([temp.spec.partial.general]).
    if (!t.deducible(head, *full)) {
      s_.diag(at, Diag::partial_not_deducible, name);
      return std::nullopt;
    }
    std::vector<ast::TemplateArg> primary = t.own_args(params);
    if (!t.more_specialized_partial(head, *full, params, primary)) {
      s_.diag(at, Diag::partial_not_more_specialized, name);
      return std::nullopt;
    }
    return full;
  }
  // `X<args>` after `template<…> class-key X`: the specialization of the partial specialization's
  // parameters it is the pattern of; none, diagnosed, if X is no class template.
  std::optional<EntityId> partial_class(NameId name, u32 name_at) {
    auto tmpl = s_.class_template(name);
    if (!tmpl) {
      s_.diag(name_at, Diag::not_a_template, name);
      return std::nullopt;
    }
    std::optional<u32> replay_at;
    auto args = ps_.template_argument_list(&replay_at);
    if (!args) return std::nullopt;
    auto full = partial_arguments(*tmpl, *args, name_at);
    if (!full) return std::nullopt;
    if (replay_at) p_->partial_replays_.emplace(k_, Program::PartialReplay{*replay_at, *tmpl, s_.dependent_calls});
    return s_.class_specialization(tmpl->entity, *full);
  }
  // `template<…> class-key X<args> { … }`: a partial specialization of class template X
  // ([temp.spec.partial]). Its members are analysed as those of `X<args>`, the specialization of
  // its own parameters it is the pattern of, as a class template's are as its current
  // instantiation's.
  void partial_class_definition(NameId name, u32 name_at) {
    auto spec = partial_class(name, name_at);
    if (!spec) return bad();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // base classes, `final`
      return bad();
    }
    EntityId pattern = *spec;
    if (auto prior = p_->declaration(pattern, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(name_at, Diag::class_redefinition, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(pattern, true);
    sem.flags |= sema::decl_flag::template_ | sema::decl_flag::partial;
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    decls_.push_back(sem);
    p_->declared(sem, k_);
    p_->current_instantiation(pattern, k_, sem);
    ps_.seek(it_->brace_end);
    q_.own_class_complete = true;
    if (ps_.at_end()) {
      ps_.expect(Tk::semi);  // the `;` is missing ([class.pre]/1)
    } else if (!ps_.accept(Tk::semi)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // declarators after a partial specialization
      bad();
    }
  }
  // `template<…> T v<args> …`: a partial specialization of variable template v
  // ([temp.spec.partial]), whose declaration is the pattern of the specializations it matches.
  bool partial_variable(const parse::Declarator& d, ast::DeclSema& sem, u16 flags, bool fn) {
    auto tmpl = fn ? std::nullopt : s_.variable_template(d.name);
    if (!tmpl) {
      s_.diag(d.name_at, Diag::not_a_template, d.name);  // functions have no partial specializations
      return false;
    }
    auto full = partial_arguments(*tmpl, *d.template_args, d.template_args_at);
    if (!full) return false;
    sem.entity = s_.variable_specialization(tmpl->entity, *full);
    sem.flags = u16(flags | sema::decl_flag::template_ | sema::decl_flag::partial);
    sem.params = b_.seq(std::span<const ast::ParamSema>{});
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    if (auto prior = p_->declaration(sem.entity, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    if (d.replay_template_args_at)
      p_->partial_replays_.emplace(k_, Program::PartialReplay{*d.replay_template_args_at, *tmpl, s_.dependent_calls});
    return true;
  }

  // `X<args>` after `template<> class-key X` ([temp.expl.spec]/1): the specialization of class
  // template X it declares; none, diagnosed, if X is no class template.
  std::optional<EntityId> explicit_class(NameId name, u32 name_at) {
    auto tmpl = s_.class_template(name);
    if (!tmpl) {
      s_.diag(name_at, Diag::not_a_template, name);
      return std::nullopt;
    }
    if (!ps_.is(Tk::less)) {
      s_.diag(name_at, Diag::specialization_needs_arguments, name);
      return std::nullopt;
    }
    auto args = ps_.template_argument_list();
    if (!args) return std::nullopt;
    auto t = s_.class_template_id(*tmpl, *args, name_at);
    if (!t) return std::nullopt;
    p_->specialization_named_at(s_.types().class_of(*t), name_at);
    return s_.types().class_of(*t);
  }
  // `template<> class-key X<args> { … }`: the definition of a class template specialization
  // ([temp.expl.spec]), a class like any other, whose entity is the specialization's.
  void explicit_class_definition(NameId name, u32 name_at) {
    auto spec = explicit_class(name, name_at);
    if (!spec) return bad();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // base classes, `final`
      return bad();
    }
    if (auto prior = p_->declaration(*spec, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(name_at, Diag::class_redefinition, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(*spec, true);
    sem.flags |= sema::decl_flag::specialization;
    decls_.push_back(sem);
    p_->declared(sem, k_);
    ps_.seek(it_->brace_end);
    q_.own_class_complete = true;
    if (!ps_.accept(Tk::semi)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // declarators after an explicit specialization
      bad();
    }
  }

  // An alias template ([temp.alias]): its entity is (scope, name, head); a use of it is the type it
  // stands for with the arguments substituted (`Sema::class_template_id`). It is the only
  // declaration of its name in the namespace ([temp.alias]/1, [basic.scope.scope]).
  bool alias_template(NameId name, u32 at, QualType type) {
    const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.back();
    if (!pack_last(head)) {
      s_.diag(0, Diag::pack_not_last);
      return false;
    }
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && same_scope(*ds)) {
        s_.diag(at, Diag::conflicting_declaration, name);
        return false;
      }
    sema::EntityRec e{sema::EntityKind::alias_template, name, {}, LangLinkage::cxx, ast::Linkage::none, {}, ns_entity()};
    e.head = head_fp(head);
    ast::DeclSema sem = class_decl(s_.world().entities.entity(e), false);
    sem.kind = ast::DeclKind::type_alias;
    sem.type = type;
    sem.linkage = ast::Linkage::none;
    sem.flags = sema::decl_flag::template_;
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(head));
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    return true;
  }
  // A template-head's requires-clause ([temp.pre]/9), after it: its normal form, and its tokens'
  // fingerprint for the declaration's identity.
  void head_requires_clause() {
    if (!ps_.is(Tk::kw_requires)) return;
    ps_.bump();
    u32 begin = ps_.pos();
    head_requires_ = ps_.constraint_expression(true);
    constraints_fp_ = ast::mix(constraints_fp_, clause_fp(begin, ps_.pos()));
  }
  // A declarator's trailing requires-clause ([dcl.decl]/4).
  void trailing_requires_clause() {
    if (!ps_.is(Tk::kw_requires)) return;
    ps_.bump();
    u32 begin = ps_.pos();
    trailing_requires_ = ps_.constraint_expression(true);
    constraints_fp_ = ast::mix(ast::mix(constraints_fp_, u64(1)), clause_fp(begin, ps_.pos()));
  }
  // The fingerprint of a clause's tokens with each template parameter as its position: equal for
  // equivalent clauses of redeclarations ([temp.over.link]/5, as tokens, not as expressions).
  sema::Fp128 clause_fp(u32 begin, u32 end) {
    sema::Fp128 f = ast::seed("ycxx.parse.clause");
    const pp::Region& r = p_->region();
    for (u32 j = it_->begin + begin; j < it_->begin + end; ++j) {
      const lex::Tok& t = r.tokens[j];
      if (t.kind == Tk::identifier) {
        NameId n = s_.world().ins.names.intern(r.name(t));
        if (const ast::TemplateParamSema* p = s_.template_param(n)) {
          f = ast::mix(ast::mix(ast::mix(f, u64(0xA11)), u64(p->depth)), u64(p->index));
          continue;
        }
      }
      f = ast::mix(f, std::to_underlying(t.kind));
      std::string_view sp = r.spelling(t);
      f = ast::mix(f, s_.world().ins.names.fp(s_.world().ins.names.intern(sp)));
    }
    return f;
  }
  // The declaration's associated constraints ([temp.constr.decl]/3): its head's type-constraints
  // in order, its head's requires-clause, its trailing requires-clause; with their fingerprint.
  ConstId associated_constraints() {
    sema::Templates t = s_.templates();
    ConstId c = ConstId::none;
    for (ConstId tc : s_.head_constraints) {
      c = t.constraint_op(0, c, tc);
      constraints_fp_ = ast::mix(constraints_fp_, s_.world().ins.values.fp(tc));
    }
    c = t.constraint_op(0, c, head_requires_);
    return t.constraint_op(0, c, trailing_requires_);
  }
  // `template<…> concept C = constraint-expression;` ([temp.concept]): C's normal form is its
  // constraint-expression's. A concept has no associated constraints of its own, and its name
  // is the only one of its kind in its scope ([temp.concept]/4, /5).
  void concept_definition() {
    ps_.bump();
    if (!ps_.is(Tk::identifier)) {
      s_.diag(ps_.pos(), Diag::expected_unqualified_id);
      return bad();
    }
    NameId name = ps_.name_of(ps_.tok());
    u32 name_at = ps_.pos();
    ps_.bump();
    ps_.skip_attributes();
    if (!ps_.expect(Tk::equal)) return bad();
    if (!s_.head_constraints.empty() || head_requires_ != ConstId::none) {
      s_.diag(name_at, Diag::concept_constrained, name);
      return bad();
    }
    ConstId nf = ps_.constraint_expression(false);
    if (!ps_.expect(Tk::semi)) return bad();
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && same_scope(*ds)) {
        s_.diag(name_at, Diag::conflicting_declaration, name);
        return bad();
      }
    const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.back();
    sema::EntityRec e{sema::EntityKind::concept_, name, {}, LangLinkage::cxx, ast::Linkage::none, {}, ns_entity()};
    e.head = head_fp(head);
    ast::DeclSema sem = class_decl(s_.world().entities.entity(e), true);
    sem.kind = ast::DeclKind::concept_;
    sem.type = s_.builtin(sema::Builtin::bool_);
    sem.linkage = ast::Linkage::none;
    sem.flags = u16(sema::decl_flag::template_ | sema::decl_flag::definition);
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(head));
    sem.constraints = nf;
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
  }
  // Whether a template-head's pack, if any, is its last parameter ([temp.param]).
  static bool pack_last(std::span<const ast::TemplateParamSema> head) {
    for (std::size_t i = 0; i + 1 < head.size(); ++i)
      if (head[i].pack) return false;
    return true;
  }
  // A class template's definition ([temp.class]): the template's entity, (scope, name, head), and
  // as its type the current instantiation `X<T…>` ([temp.dep.type]), whose class its members are.
  ast::DeclSema class_template_decl(NameId name, bool definition = true) {
    std::vector<ast::TemplateParamSema> head = s_.template_scopes.back();
    // [temp.param]/14: in a class template's head, the parameters after one with a default
    // argument have default arguments.
    bool defaulted = false;
    for (const ast::TemplateParamSema& p : head) {
      if (defaulted && !p.has_default && !p.pack) {
        s_.diag(0, Diag::default_not_last);
        break;
      }
      defaulted = defaulted || p.has_default;
    }
    if (!pack_last(head)) s_.diag(0, Diag::pack_not_last);
    sema::EntityRec t{sema::EntityKind::class_template, name, {}, LangLinkage::cxx, ast::Linkage::external, {}, ns_entity()};
    t.head = head_fp(head);
    EntityId templ = s_.world().entities.entity(t);
    std::vector<ast::TemplateArg> own = sema::Templates(s_.world(), p_->classes_).own_args(head);
    // Another declaration of a class template of the name in this scope: the same template, which
    // gives each default argument once ([temp.param]/12); a different head declares another
    // template of one name, which [temp.pre]/7 forbids.
    for (DeclId d : q_.visible_decls(name)) {
      auto prior = p_->decl_sema(d);
      if (!prior || prior->kind != ast::DeclKind::class_ || !same_scope(*prior) || !(prior->flags & sema::decl_flag::template_))
        continue;
      if (prior->entity != templ) {
        s_.diag(0, Diag::conflicting_declaration, name);
        break;
      }
      std::vector<ast::TemplateParamSema> earlier = s_.world().ins.seqs.elements(prior->template_params);
      for (std::size_t i = 0; i < head.size() && i < earlier.size(); ++i)
        if (head[i].has_default && earlier[i].has_default) {
          s_.diag(0, Diag::default_redefined);
          break;
        }
    }
    ast::DeclSema sem = class_decl(s_.class_specialization(templ, own), definition);
    sem.entity = templ;
    sem.flags |= sema::decl_flag::template_;
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(head));
    return sem;
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
    sem.constraints = ast::ConstId::none;
    sem.contracts = NodeId::none;
    sem.params = b_.seq(std::span<const ast::ParamSema>{});
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>{});
    return sem;
  }

  // A class definition at namespace scope ([class.pre]): `class-key identifier { … }`, then
  // its declarators. The member-specification is the members' items (§4.2); `class_def` reads
  // them.
  void class_definition() {
    // A class template ([temp.class]): its head, then the class-head. `template<>` defines an
    // explicit specialization ([temp.expl.spec]).
    if (ps_.is(Tk::kw_template) && ps_.kind(1) == Tk::less && ps_.kind(2) == Tk::greater) {
      ps_.bump();
      ps_.bump();
      ps_.bump();
      explicit_ = true;
    } else if (ps_.is(Tk::kw_template)) {
      if (ps_.kind(1) != Tk::less) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_template);
        return bad();
      }
      if (!ps_.template_head(0)) return bad();
      head_requires_clause();
      template_ = true;
    }
    u32 at = ps_.pos();
    ps_.skip_attributes();
    bool td = ps_.accept(Tk::kw_typedef);  // `typedef class-specifier declarators;` ([dcl.typedef])
    Tk key = ps_.kind();
    if (key != Tk::kw_struct && key != Tk::kw_class && key != Tk::kw_union) {
      s_.diag(ps_.pos(), Diag::not_supported, key);  // other specifiers before the class-key
      return bad();
    }
    ps_.bump();
    ps_.skip_attributes();
    auto named = td ? typedef_name_after_braces() : std::nullopt;
    if (!ps_.is(Tk::identifier) && !named) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // unnamed classes, attributes
      return bad();
    }
    NameId name = ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : named->first;
    u32 name_at = ps_.is(Tk::identifier) ? ps_.pos() : named->second;
    if (ps_.is(Tk::identifier)) ps_.bump();
    if (explicit_) return explicit_class_definition(name, name_at);
    if (template_ && ps_.is(Tk::less)) return partial_class_definition(name, name_at);
    std::vector<sema::BaseRec> bases;
    if (ps_.is(Tk::colon) && !template_) {
      if (key == Tk::kw_union) {
        s_.diag(ps_.pos(), Diag::union_with_base);  // [class.union.general]/1
        return bad();
      }
      if (!base_clause(key, name, bases)) return bad();
    }
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // bases of templates (#308), `final`
      return bad();
    }
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && same_scope(*ds)) {
        if (ds->kind == ast::DeclKind::enum_) {
          s_.diag(name_at, Diag::conflicting_declaration, name);
          return bad();
        }
        if (ds->kind == ast::DeclKind::class_ && (ds->flags & sema::decl_flag::definition)) {
          s_.diag(name_at, Diag::class_redefinition, name);
          return bad();
        }
      }
    if (template_ ? alias_clash(name, std::nullopt) : alias_clash(name, s_.types().class_type(class_entity(name)))) {
      s_.diag(name_at, Diag::conflicting_declaration, name);
      return bad();
    }
    if (!template_ && clashes_with_using(name, name_at, ast::DeclKind::class_, s_.types().class_type(class_entity(name))))
      return bad();
    ast::DeclSema sem = template_ ? class_template_decl(name) : class_decl(class_entity(name), true);
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    if (!template_) p_->set_bases(sem.entity, std::move(bases));
    if (template_) p_->current_instantiation(s_.types().class_of(sem.type), k_, sem);
    // The declarators after the closing brace: `struct S { … } s, *p;`, where S is complete.
    ps_.seek(it_->brace_end);
    q_.own_class_complete = true;
    if (ps_.accept(Tk::semi)) return;
    if (ps_.at_end()) {  // the `;` is missing; the definition stands ([class.pre]/1)
      ps_.expect(Tk::semi);
      return;
    }
    DeclSpec spec;
    spec.begin = at;
    spec.type = sem.type;
    spec.has_type = true;
    spec.is_typedef = td;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false, spec.is_typedef);
      if (!d.ok || !d.named || !declare(spec, d, LangLinkage::cxx, false, false)) {
        bad();
        ps_.skip_to_semi();
        return;
      }
    } while (ps_.accept(Tk::comma));
    if (!ps_.expect(Tk::semi)) bad();
  }

  // An unnamed union with a declarator in a class ([class.pre], [class.mem.general]). The head
  // occupies declaration slot 0, and the declarators that follow it belong to the enclosing class.
  void unnamed_union_member() {
    const items::Item& outer = p_->items().items[it_->parent];
    ps_.skip_attributes();
    u32 at = ps_.pos();
    Tk key = ps_.kind();
    if (it_->names.size() < 2 || inst_ || outer.template_depth != 0 || outer.specialization || !ps_.accept(Tk::kw_union)) {
      s_.diag(at, Diag::not_supported, key);
      return bad();
    }
    ps_.skip_attributes();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(at, Diag::not_supported, key);
      return bad();
    }
    EntityId owner = p_->class_entity(it_->parent);
    if (owner == EntityId::none) return bad();
    sema::EntityRec entity{.kind = sema::EntityKind::class_, .name = s_.no_name(), .scope = owner,
                           .unnamed_type_ordinal = it_->unnamed_type_ordinal};
    ast::DeclSema head = class_decl(s_.world().entities.entity(std::move(entity)), true);
    decls_.push_back(head);
    p_->declared(head, k_);
    p_->class_def(k_);

    ps_.seek(it_->brace_end);
    s_.current_class = owner;
    q_.own_class_complete = true;
    DeclSpec spec;
    spec.begin = at;
    spec.type = head.type;
    spec.has_type = true;
    member_declarators(spec, class_decl(owner, true));
  }

  // A base-clause ([class.derived.general]): `: [virtual] [access] class-name, …`. Each base is a
  // complete class other than this one, named once ([class.mi]/3); its access defaults to
  // private for a `class`, public for a `struct` ([class.access.base]/2).
  bool base_clause(Tk key, NameId name, std::vector<sema::BaseRec>& bases) {
    ps_.bump();
    sema::Types& types = s_.types();
    do {
      ps_.skip_attributes();
      if (ps_.is(Tk::kw_virtual) || (ps_.kind(1) == Tk::kw_virtual && (ps_.is(Tk::kw_public) || ps_.is(Tk::kw_protected) ||
                                                                         ps_.is(Tk::kw_private)))) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_virtual);  // virtual bases (#310)
        return false;
      }
      ast::Access access = key == Tk::kw_class ? ast::Access::private_ : ast::Access::public_;
      if (ps_.accept(Tk::kw_public)) access = ast::Access::public_;
      else if (ps_.accept(Tk::kw_protected)) access = ast::Access::protected_;
      else if (ps_.accept(Tk::kw_private)) access = ast::Access::private_;
      u32 at = ps_.pos();
      DeclSpec spec = ps_.decl_specifiers();
      if (!spec.ok) return false;
      if (!types.is_class(spec.type) || sema::Types::cv(spec.type) != sema::Cv::none) {
        s_.diag(at, Diag::base_not_class, spec.type);
        return false;
      }
      EntityId b = types.class_of(spec.type);
      if (p_->world().entities.rec(b).name == name && p_->world().entities.rec(b).scope == ns_entity()) {
        s_.diag(at, Diag::incomplete_type, spec.type);  // the class itself: incomplete here
        return false;
      }
      if (!s_.require_complete(spec.type, at)) return false;
      if (const sema::ClassRec* r = p_->classes_.find(b)) {
        if (r->union_) {
          s_.diag(at, Diag::base_not_class, spec.type);  // [class.union.general]/1: not a union
          return false;
        }
        if (r->polymorphic) {
          s_.diag(at, Diag::virtual_base_class, spec.type);
          return false;
        }
      }
      if (std::ranges::any_of(bases, [&](const sema::BaseRec& r) { return r.cls == b; })) {
        s_.diag(at, Diag::duplicate_base, spec.type);
        return false;
      }
      bases.push_back({b, access});
    } while (ps_.accept(Tk::comma));
    return true;
  }

  // [dcl.enum]: an opaque declaration or a definition of an enumeration in this namespace.
  bool enum_redeclaration_ok(NameId name, u32 at, bool scoped, std::optional<QualType> fixed, bool definition) {
    sema::Types& types = s_.types();
    for (DeclId p : q_.visible_decls(name)) {
      auto ds = p_->decl_sema(p);
      if (!ds || ds->entity == EntityId::none || !same_scope(*ds)) continue;
      if (ds->kind == ast::DeclKind::class_ || ds->kind == ast::DeclKind::namespace_) {
        s_.diag(at, Diag::conflicting_declaration, name);
        return false;
      }
      if (ds->kind != ast::DeclKind::enum_) continue;
      const sema::EnumRec* old = p_->classes_.find_enum(ds->entity);
      if (!old) continue;
      if (old->scoped != scoped) {
        s_.diag(at, Diag::enum_scope_mismatch, name);
        return false;
      }
      if (old->fixed && !fixed) {
        s_.diag(at, Diag::enum_base_required, name);
        return false;
      }
      if (old->fixed != fixed.has_value() ||
          (fixed && old->underlying && types.canonical(*old->underlying) != types.canonical(*fixed))) {
        s_.diag(at, Diag::enum_base_mismatch, name);
        return false;
      }
      if (definition && (ds->flags & sema::decl_flag::definition)) {
        s_.diag(at, Diag::enum_redefinition, name);
        return false;
      }
    }
    return true;
  }

  // An enumeration definition, or a namespace-scope opaque declaration ([dcl.enum]). A definition
  // can have declarators after the enumerator list. In a class (`cls`), it is a member.
  void enum_declaration(EntityId cls) {
    sema::Types& types = s_.types();
    u32 at = ps_.pos();
    bool definition = it_->kind == ItemKind::enum_def;
    bool td = cls == EntityId::none && ps_.accept(Tk::kw_typedef);  // `typedef enum … { … } E;`
    if (!ps_.is(Tk::kw_enum)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      return bad();
    }
    ps_.bump();
    bool scoped = ps_.accept(Tk::kw_class) || ps_.accept(Tk::kw_struct);
    ps_.skip_attributes();
    auto named = td ? typedef_name_after_braces() : std::nullopt;
    if (!ps_.is(Tk::identifier) && !named) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // unnamed enumerations
      return bad();
    }
    NameId name = ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : named->first;
    u32 name_at = ps_.is(Tk::identifier) ? ps_.pos() : named->second;
    if (ps_.is(Tk::identifier)) ps_.bump();
    std::optional<QualType> fixed;
    if (ps_.accept(Tk::colon)) {
      // [dcl.enum]/2: a fixed underlying type, an integral type.
      DeclSpec u = ps_.decl_specifiers();
      if (!u.ok) return bad();
      if (!types.is_integral(u.type) || u.is_static || u.is_extern || u.is_inline || u.is_typedef ||
          u.is_constexpr || u.is_consteval || u.is_constinit) {
        s_.diag(u.begin, Diag::enum_base_not_integral);
        return bad();
      }
      fixed = sema::Types::unqualified(u.type);
    } else if (scoped) {
      fixed = s_.builtin(sema::Builtin::int_);  // [dcl.enum]/5: int unless given
    }
    if (!definition && !fixed) {
      s_.diag(name_at, Diag::enum_base_required, name);
      return bad();
    }
    if (!ps_.expect(definition ? Tk::l_brace : Tk::semi)) return bad();
    if (cls == EntityId::none && !enum_redeclaration_ok(name, name_at, scoped, fixed, definition)) return bad();
    EntityId e = s_.world().entities.entity(
        {sema::EntityKind::enum_, name, {}, LangLinkage::cxx, ast::Linkage::external, {}, cls != EntityId::none ? cls : ns_entity()});
    QualType t = types.enum_type(e);
    if (cls == EntityId::none && alias_clash(name, t)) {
      s_.diag(name_at, Diag::conflicting_declaration, name);
      return bad();
    }
    if (cls == EntityId::none && clashes_with_using(name, name_at, ast::DeclKind::enum_, t)) return bad();
    sema::EnumRec er{scoped, fixed.has_value(), definition, fixed.value_or(s_.builtin(sema::Builtin::int_)), std::nullopt, {}};
    ast::DeclSema sem = class_decl(e, definition);
    sem.kind = ast::DeclKind::enum_;
    sem.type = t;
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    if (!definition) {
      p_->classes_.publish_enum(e, std::move(er));
      return;
    }
    // The enumerator list sees the definition's record as the list grows (#156).
    p_->defining_enum_ = {e, &er};
    // The enumerators: each the previous one's value plus one, or its initializer's, a converted
    // constant expression of the underlying type when it is fixed ([dcl.enum]/5).
    std::int64_t next = 0;
    bool next_big = false;  // `next` is an unsigned value of 2^63 or more, its bits in `next`
    // Before the closing brace an enumerator has its initializer's type, or the previous one's,
    // or the fixed underlying type ([dcl.enum]/5); its value is kept until the type is chosen.
    QualType prev_type = fixed.value_or(s_.builtin(sema::Builtin::int_));
    std::vector<std::int64_t> values;
    std::vector<bool> big;  // per value: unsigned, 2^63 or more (its bits are in `values`)
    while (!ps_.is(Tk::r_brace) && !ps_.at_end()) {
      if (!ps_.is(Tk::identifier)) {
        s_.diag(ps_.pos(), Diag::expected_unqualified_id);
        break;
      }
      NameId en = ps_.name_of(ps_.tok());
      u32 en_at = ps_.pos();
      ps_.bump();
      ps_.skip_attributes();  // an enumerator's attribute-specifier-seq ([dcl.enum]/1)
      std::int64_t value = next;
      bool value_big = next_big;
      if (!ps_.is(Tk::equal) && !values.empty() && big.back() && values.back() == -1) {
        s_.diag(en_at, Diag::enum_no_type, name);  // [dcl.enum]/5: no type holds the incremented value
        value_big = false;
      }
      // [dcl.enum]/5: an incremented value the previous enumerator's type cannot represent has a
      // type that can: the first of int, unsigned int, long, unsigned long that holds it.
      if (!ps_.is(Tk::equal) && !fixed && !values.empty() && !holds(prev_type, value, value_big)) {
        for (sema::Builtin b : {sema::Builtin::int_, sema::Builtin::unsigned_int, sema::Builtin::long_, sema::Builtin::unsigned_long})
          if (holds(s_.builtin(b), value, value_big)) {
            prev_type = s_.builtin(b);
            break;
          }
      }
      if (!ps_.is(Tk::equal) && fixed && !values.empty()) {
        // An implicit value must fit the fixed type too.
        QualType ll = s_.builtin(sema::Builtin::long_long);
        Ex lit = s_.literal(ll, s_.world().values.integer(ll, sema::u64(value)), ast::ValueCategory::prvalue, en_at, en_at);
        if (s_.narrows(lit, *fixed)) s_.diag(en_at, Diag::narrowing, ll, *fixed);
      }
      if (ps_.accept(Tk::equal)) {
        Ex x = ps_.constant_expression();
        Ex v = s_.promote(x);
        auto c = v.ok() ? sema::Evaluator(s_, q_).evaluate(v).value : std::nullopt;
        if (!c || s_.world().values.rec(*c).kind != sema::ValueKind::integer || !types.is_integral(v.type())) {
          if (x.ok()) s_.diag(x.begin, Diag::bound_not_constant);
        } else {
          sema::IntShape sh = sema::shape(s_.world().target, *types.builtin_of(v.type()));
          sema::u64 bits = s_.world().values.rec(*c).bits;
          value = sh.is_signed && sh.bits < 64 && ((bits >> (sh.bits - 1)) & 1) ? std::int64_t(bits | ~((sema::u64{1} << sh.bits) - 1))
                                                                               : std::int64_t(bits);
          value_big = !sh.is_signed && (bits >> 63) != 0;
          if (fixed) {
            Ex lit = s_.literal(v.type(), *c, ast::ValueCategory::prvalue, x.begin, x.end);
            if (s_.narrows(lit, *fixed)) s_.diag(x.begin, Diag::narrowing, v.type(), *fixed);
          } else {
            prev_type = sema::Types::unqualified(v.type());
          }
        }
      }
      // The next implicit value, past INT64_MAX into the unsigned values.
      next_big = value_big || value == std::numeric_limits<std::int64_t>::max();
      next = std::int64_t(sema::u64(value) + 1);
      // [basic.scope.scope]: an unscoped enumerator is declared in the enclosing scope.
      if (!scoped && cls == EntityId::none)
        for (DeclId p : q_.visible_decls(en))
          if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && ds->kind != ast::DeclKind::class_ &&
                                          ds->kind != ast::DeclKind::enum_ && ds->kind != ast::DeclKind::namespace_ &&
                                          (ds->kind == ast::DeclKind::enumerator || same_scope(*ds)))
            s_.diag(en_at, Diag::conflicting_declaration, en);
      EntityId x = s_.world().entities.entity(
          {sema::EntityKind::enumerator, en, {}, LangLinkage::cxx, ast::Linkage::none, {}, e});
      p_->set_constant(x, s_.world().values.integer(prev_type, sema::u64(value)), k_);
      er.enumerators.push_back({en, x, ConstId::none});
      values.push_back(value);
      big.push_back(value_big);
      ast::DeclSema xs = class_decl(e, true);
      xs.kind = ast::DeclKind::enumerator;
      xs.type = t;
      xs.entity = x;
      decls_.push_back(xs);
      xs.type = prev_type;  // inside the list
      s_.own.push_back({en, xs});
      if (!ps_.accept(Tk::comma)) {
        // [dcl.enum]/1: the enumerator-list is comma-separated.
        if (!ps_.is(Tk::r_brace) && !ps_.at_end()) s_.diag(ps_.pos(), Diag::expected_token, Tk::comma);
        break;
      }
    }
    // [dcl.enum]/7: without a fixed type, the type holds every value, and is no larger than int
    // unless a value fits neither int nor unsigned int; which one is the implementation's. As the
    // Itanium C++ ABI compilers choose (#220): with no negative value, `unsigned int` if every
    // value fits, else `unsigned long`; else `int` if every value fits, else `long`. An empty
    // enumeration has the values of `unsigned int`.
    if (!fixed) {
      using sema::Builtin;
      bool negative = false;
      for (std::size_t i = 0; i < values.size(); ++i) negative = negative || (values[i] < 0 && !big[i]);
      Builtin u = Builtin::unsigned_int;
      if (!negative) {
        sema::u64 hi = 0;
        for (std::int64_t v : values) hi = std::max(hi, sema::u64(v));
        if (hi > std::numeric_limits<std::uint32_t>::max()) u = Builtin::unsigned_long;
      } else if (std::ranges::contains(big, true)) {
        s_.diag(name_at, Diag::enum_no_type, name);  // [dcl.enum]/7: no integral type holds them all
        u = Builtin::long_;
      } else {
        auto [lo, hi] = std::ranges::minmax(values);
        u = lo >= std::numeric_limits<std::int32_t>::min() && hi <= std::numeric_limits<std::int32_t>::max() ? Builtin::int_
                                                                                                             : Builtin::long_;
      }
      er.underlying = s_.builtin(u);
      // [conv.prom]/3: the first of int, unsigned int, long, unsigned long that holds every
      // value of the enumeration (its range, [dcl.enum]/8: these hold it when they hold the
      // enumerators).
      Builtin p = Builtin::unsigned_long;
      for (Builtin c : {Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long}) {
        bool all = true;
        for (std::size_t i = 0; i < values.size(); ++i) all = all && holds(s_.builtin(c), values[i], big[i]);
        if (all) {
          p = c;
          break;
        }
      }
      er.promotion = s_.builtin(p);
    }
    // Past the closing brace an enumerator has the enumeration's type ([dcl.enum]/5), in the
    // declarators that follow too.
    for (auto& o : s_.own)
      if (o.sema.kind == ast::DeclKind::enumerator && s_.world().entities.rec(o.sema.entity).scope == e) o.sema.type = t;
    for (std::size_t i = 0; i < er.enumerators.size(); ++i) {
      er.enumerators[i].value = s_.world().values.integer(*er.underlying, sema::u64(values[i]));
      p_->set_constant(er.enumerators[i].entity, er.enumerators[i].value, k_);
    }
    p_->defining_enum_ = {EntityId::none, nullptr};
    p_->classes_.publish_enum(e, std::move(er));
    ps_.seek(it_->brace_end);
    if (ps_.accept(Tk::semi)) return;
    // The item ended at the brace: the `;` is missing, and the definition stands
    // ([class.pre]/1, [dcl.enum]/1).
    if (ps_.at_end()) {
      ps_.expect(Tk::semi);
      return;
    }
    DeclSpec spec;
    spec.begin = at;
    spec.type = t;
    spec.has_type = true;
    spec.is_typedef = td;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false, spec.is_typedef);
      if (!d.ok || !d.named || cls != EntityId::none || !declare(spec, d, LangLinkage::cxx, false, false)) {
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
    EntityId ce = inst_ ? inst_->spec : p_->class_entity(it_->parent);
    if (ce == EntityId::none) return bad();
    ast::DeclSema c = class_decl(ce, true);
    s_.current_class = ce;  // names in a member declaration are looked up in the class first
    if (ps_.accept(Tk::semi)) return;
    // An access-specifier missing its `:` ([class.access.spec]/1).
    if ((ps_.is(Tk::kw_public) || ps_.is(Tk::kw_protected) || ps_.is(Tk::kw_private)) && ps_.kind(1) != Tk::colon) {
      ps_.bump();
      ps_.expect(Tk::colon);
      return bad();
    }
    // A constructor or destructor ([class.ctor], [class.dtor]): the class's name, or `~` and it,
    // then a parameter list.
    NameId cname = p_->world().entities.rec(ce).name;
    // A member alias-declaration ([class.mem], [dcl.typedef]/2): a type nested in the class.
    if (ps_.is(Tk::kw_using) && ps_.kind(1) == Tk::identifier && ps_.kind(ps_.past_attributes(2)) == Tk::equal) {
      ps_.bump();
      NameId n = ps_.name_of(ps_.tok());
      u32 at = ps_.pos();
      ps_.bump();
      ps_.skip_attributes();
      ps_.bump();
      auto t = ps_.type_id();
      if (!t || !member_alias(c, n, at, *t)) return bad();
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    if (ps_.is(Tk::kw_friend) || std::ranges::any_of(it_->names, [](const items::DeclName& n) {
          return (n.flags & items::name_flag::friend_) != 0;
        }))
      return friend_declaration();
    // A member template ([temp.mem]): its head one level inside the class's.
    if (ps_.is(Tk::kw_template)) {
      if (ps_.kind(1) != Tk::less) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_template);  // an explicit instantiation (#315)
        return bad();
      }
      if (!ps_.template_head(u16(p_->enclosing_heads(k_).size()))) return bad();
      // [temp.local]/6: a member template's parameter does not take the class template's names.
      for (std::size_t h = 0; h + 1 < s_.template_scopes.size(); ++h)
        for (const ast::TemplateParamSema& outer : s_.template_scopes[h])
          for (const ast::TemplateParamSema& own : s_.template_scopes.back())
            if (own.name == outer.name && own.name != s_.no_name()) {
              s_.diag(0, Diag::redeclared_local, own.name);
              return bad();
            }
      head_requires_clause();
      member_template_ = true;
    }
    if (!member_template_ && s_.template_scopes.empty() && !inst_)
      ps_.member_template_name = [this](NameId name) -> std::optional<bool> {
        std::string_view spelling = p_->world().ins.names.spelling(name);
        bool found = false;
        for (auto [item, ordinal] : p_->named(spelling)) {
          const auto& member = p_->items().items[item];
          if (member.parent != it_->parent) continue;
          const auto& n = member.names[ordinal];
          if (n.flags & (items::name_flag::friend_ | items::name_flag::specialization)) continue;
          if (n.flags & items::name_flag::template_) return true;
          found = true;
        }
        if (found) return false;
        return std::nullopt;
      };
    const u32 ctor_at = constructor_start();
    const bool ctor = ps_.kind(ctor_at) == Tk::identifier && ps_.kind(ctor_at + 1) == Tk::l_paren &&
                      ps_.name_of(ps_.tok(ctor_at)) == cname && (ctor_at == 0 || ps_.params_ahead(ctor_at + 1));
    const bool dtor = ps_.kind(ctor_at) == Tk::tilde && ps_.kind(ctor_at + 1) == Tk::identifier &&
                      ps_.name_of(ps_.tok(ctor_at + 1)) == cname;
    if (ctor || dtor) {
      std::optional<u32> virtual_at;
      auto flags = constructor_flags(ctor_at, dtor ? &virtual_at : nullptr);
      if (!flags) return bad();
      return special_member(c, ctor, *flags, virtual_at);
    }
    if (ps_.is(Tk::l_square) && ps_.kind(1) == Tk::l_square) {
      u32 attrs_at = ps_.pos();
      if (!ps_.skip_balanced_attributes()) return bad();  // [class.mem.general]
      // [class.access.spec]: an access label has no attribute-specifier-seq.
      if (ps_.is(Tk::kw_public) || ps_.is(Tk::kw_protected) || ps_.is(Tk::kw_private)) {
        s_.diag(attrs_at, Diag::attributes_not_allowed);
        return bad();
      }
    }
    if (!ps_.conversion_id_ahead() && !Parser::starts_decl_specifier(ps_.kind()) && !ps_.starts_declaration()) {
      s_.diag(ps_.pos(), lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon) || ps_.is(Tk::tilde)
                             ? Diag::not_supported
                             : Diag::expected_declaration,
              ps_.kind());
      return bad();
    }
    DeclSpec spec = ps_.decl_specifiers(/*in_type_id=*/false, nullptr, /*allow_conversion_id=*/true, /*allow_virtual=*/true);
    if (spec.is_extern || spec.is_constinit) {
      s_.diag(spec.begin, Diag::not_supported, spec.is_extern ? Tk::kw_extern : Tk::kw_constinit);
      return bad();
    }
    if (!spec.ok) return bad();
    if (spec.elaborated && ps_.is(Tk::semi)) {
      s_.diag(spec.begin, Diag::not_supported, Tk::kw_struct);  // a member class declaration
      return bad();
    }
    // A member typedef ([class.mem], [dcl.typedef]): each declarator a type nested in the class.
    if (spec.is_typedef) {
      do {
        parse::Declarator d = ps_.declarator(spec.type, false, true);
        if (!d.ok || !d.named || d.qualified || !member_alias(c, d.name, d.name_at, d.type)) return bad();
      } while (ps_.accept(Tk::comma));
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    member_declarators(spec, c);
  }

  void member_declarators(const DeclSpec& spec, const ast::DeclSema& c) {
    sema::Types& types = s_.types();
    do {
      bool conversion = ps_.conversion_id_ahead();
      if (conversion && spec.has_type) {
        s_.diag(spec.begin, Diag::conversion_decl_type);
        return bad();
      }
      if (!conversion && !spec.has_type) {
        s_.diag(ps_.pos(), Diag::missing_type_specifier);
        return bad();
      }
      if (conversion && member_template_) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_template);
        return bad();
      }
      using enum Parser::NamedExceptionSpec;
      auto exception = spec.virtual_at ? virtual_member : p_->defaulted_body(k_) ? defaulted_assignment : ordinary;
      parse::Declarator d = ps_.declarator(spec.type, /*abstract=*/false, /*typedef_=*/false, conversion, exception);
      if (!d.ok || !d.named) return bad();
      if (conversion && !d.function) {
        s_.diag(d.name_at, Diag::conversion_parameters);
        return bad();
      }
      if (literal_operator_named(d.name)) {
        s_.diag(d.name_at, Diag::literal_operator_scope);
        return bad();
      }
      if (sema::is_allocation_name(s_.ins().names.spelling(d.name))) {
        s_.diag(d.name_at, Diag::allocation_declaration, d.name);
        return bad();
      }
      if (!d.asm_label.empty()) {
        s_.diag(d.name_at, Diag::not_supported, Tk::kw_asm);  // a member's asm label
        return bad();
      }
      if (d.unknown_bound) {
        s_.diag(d.name_at, Diag::unknown_bound_member);  // [class.mem.general]: a complete type
        return bad();
      }
      if (d.qualified) {
        s_.diag(d.qualifier_at, Diag::not_supported, Tk::coloncolon);
        return bad();
      }
      if (spec.virtual_at) {
        if (!types.is_function(d.type) || spec.is_static) {
          s_.diag(*spec.virtual_at, Diag::virtual_placement);
          return bad();
        }
        if (!virtual_member_scope(c.entity, *spec.virtual_at)) return bad();
        if (types.has_placeholder(types.rec(d.type).inner)) {
          s_.diag(d.name_at, Diag::virtual_placeholder_return);
          return bad();
        }
        if (spec.is_consteval) {
          s_.diag(spec.begin, Diag::not_supported, Tk::kw_consteval);
          return bad();
        }
        if (ps_.is(Tk::kw_requires)) {
          s_.diag(ps_.pos(), Diag::virtual_constraints);
          return bad();
        }
      }
      if (types.is_function(d.type) && !member_virt_specifier(c.entity)) return bad();
      // A member whose type became a function type through substitution is a data member of
      // function type, which cannot exist ([temp.inst], [class.mem.general]).
      if (inst_ && types.is_function(d.type) && !d.function) {
        s_.diag(d.name_at, Diag::member_of_function_type, d.type);
        return bad();
      }
      u8 bit_width = 0;
      if (ps_.is(Tk::colon)) {
        auto width = bit_field(spec, d, c.entity);
        if (!width) return bad();
        bit_width = *width;
      }
      if (types.is_function(d.type) || spec.is_static) {
        if (it_->kind == ItemKind::class_def || p_->items().items[it_->parent].unnamed_type_ordinal != items::none) {
          s_.diag(d.name_at, Diag::not_supported, Tk::kw_union);
          return bad();
        }
        if (!member_function_or_static(spec, d, c)) return bad();
        if (it_->kind == ItemKind::function) return;  // its body follows the declaration part
        continue;
      }
      if (spec.is_inline || spec.is_constexpr || spec.is_consteval) {
        // Inline or constexpr non-static data members are ill-formed.
        s_.diag(spec.begin, Diag::not_supported, Tk::kw_constexpr);
        return bad();
      }
      // [dcl.spec.auto.general]: a non-static data member's type has no placeholder.
      if (types.has_placeholder(d.type)) {
        s_.diag(d.name_at, Diag::auto_not_allowed);
        return bad();
      }
      // [class.mem.general]: a non-static data member has a complete object type; the class is
      // incomplete inside its own member-specification.
      if (types.is_void(d.type)) {
        s_.diag(d.name_at, Diag::void_variable, d.name);
        return bad();
      }
      if (!s_.require_complete(d.type, d.name_at)) return bad();
      if (p_->class_rec_is_union(it_->parent) && !variant_member(d)) return bad();
      if (member_declared(d.name)) {
        s_.diag(d.name_at, Diag::redeclared_member, d.name);
        return bad();
      }
      ast::DeclSema sem = class_decl(c.entity, false);
      sem.kind = ast::DeclKind::data_member;
      sem.type = d.type;
      sem.bit_width = bit_width;
      sem.linkage = ast::Linkage::none;
      sem.flags = sema::decl_flag::definition;
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::data_member, d.name, d.type, LangLinkage::cxx, ast::Linkage::none, {}, c.entity});
      // A default member initializer ([class.mem.general]) is a complete-class context: it is
      // analysed again where it is used (a constructor). Here it is kept as its value when it is
      // a constant of scalar type, and its tokens are recorded for the rest; what it says is
      // diagnosed where it is used.
      Ex init;
      ConstId value = ConstId::none;
      if (ps_.is(Tk::equal) || ps_.is(Tk::l_brace)) {
        bool braced = ps_.is(Tk::l_brace) || ps_.kind(1) == Tk::l_brace;
        if (!braced) ps_.bump();
        else ps_.accept(Tk::equal);
        u32 from = ps_.pos();
        std::size_t n = s_.diag_count();
        if (braced) init = s_.list_init(d.type, *ps_.braced_init_list());
        else init = s_.convert(ps_.assignment_expression(), d.type, from);
        auto v = init.ok() && types.is_scalar(d.type) ? constant_initializer(init) : std::nullopt;
        s_.truncate_diags(n);
        if (v) value = *v;
        else init = {};
        p_->set_dmi(sem.entity, {it_->begin + from, it_->begin + ps_.pos(), braced});
      }
      s_.own.push_back({d.name, sem});
      declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
      decls_.push_back(sem);
    } while (ps_.accept(Tk::comma));
    if (!ps_.expect(Tk::semi)) bad();
  }

  std::optional<u8> bit_field(const DeclSpec& spec, const parse::Declarator& d, EntityId owner) {
    const items::Item& record = p_->items().items[it_->parent];
    // The first slice covers a non-template record without bases or layout attributes.
    auto attributed = [&](u32 begin, u32 end) {
      const pp::Region& region = p_->region();
      for (u32 i = begin; i < end; ++i) {
        const auto token = region.tokens[i];
        if (token.kind == Tk::kw_alignas ||
            (token.kind == Tk::l_square && i + 1 < end && region.tokens[i + 1].kind == Tk::l_square) ||
            (token.kind == Tk::identifier && region.name(token) == "__attribute__")) return true;
      }
      return false;
    };
    if (d.name_at != d.begin || d.template_args || member_template_ || spec.is_static || !q_.defining_bases(owner).empty() ||
        p_->class_rec_is_union(it_->parent) || record.template_depth != 0 || record.specialization ||
        sema::Types::cv(s_.types().canonical(d.type)) != sema::Cv::none ||
        !(s_.types().is(d.type, sema::Builtin::int_) || s_.types().is(d.type, sema::Builtin::unsigned_int)) ||
        attributed(record.begin, record.brace_begin) || attributed(it_->begin, ps_.index())) {
      s_.diag(ps_.pos(), Diag::bit_field_declaration);
      return std::nullopt;
    }
    ps_.bump();
    u32 at = ps_.pos();
    Ex e = s_.promote(ps_.constant_expression());
    if (!e.ok()) return std::nullopt;
    std::optional<ConstId> v;
    if (s_.types().is_integral(e.type())) v = sema::Evaluator(s_, q_).evaluate(e).value;
    auto& values = s_.world().values;
    u64 width = v && values.rec(*v).kind == sema::ValueKind::integer ? values.rec(*v).bits : 0;
    if (width == 0 || width > 32) {
      s_.diag(at, Diag::bit_field_width);
      return std::nullopt;
    }
    if (ps_.is(Tk::equal) || ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::bit_field_initializer);
      return std::nullopt;
    }
    return u8(width);
  }

  // A friend declaration in class `c` ([class.friend]): `friend class X;` or `friend X;`, a class
  // (declared in the enclosing namespace when lookup finds none, [dcl.type.elab]/4), or a function
  // declaration, of a namespace-scope function or of another class's member. The friend is not a
  // member, and the declaration makes no name visible to ordinary lookup ([class.friend]/7).
  void friend_declaration() {
    std::optional<u32> friend_at;
    if (ps_.is(Tk::kw_friend)) {
      friend_at = ps_.pos();
      ps_.bump();
    }
    const bool definition = it_->kind == ItemKind::function;
    if (ps_.is(Tk::kw_template) || !p_->enclosing_heads(k_).empty() || inst_) {
      s_.diag(friend_at.value_or(ps_.pos()), Diag::not_supported, Tk::kw_friend);  // in or of a template (#259)
      return bad();
    }
    sema::Types& types = s_.types();
    ast::DeclSema sem = class_decl(EntityId::none, false);
    sem.flags = sema::decl_flag::friend_;
    // `friend class X;`: the class lookup finds, else one the declaration introduces in the
    // innermost enclosing namespace ([dcl.type.elab]/4, [namespace.memdef]/3).
    if (friend_at && (ps_.is(Tk::kw_class) || ps_.is(Tk::kw_struct)) && ps_.kind(1) == Tk::identifier &&
        ps_.kind(2) == Tk::semi) {
      NameId n = ps_.name_of(ps_.tok(1));
      auto t = s_.class_name(n, false);
      sem.entity = t && types.is_class(*t) ? types.class_of(*t) : class_entity(n);
      decls_.push_back(sem);
      ps_.bump();
      ps_.bump();
      ps_.bump();
      return;
    }
    DeclSpec spec = ps_.decl_specifiers(false, &friend_at);
    if (!spec.ok || !friend_at) return bad();
    if (ps_.accept(Tk::semi)) {
      if (!types.is_class(spec.type)) {
        s_.diag(spec.begin, Diag::not_supported, Tk::kw_friend);  // a friend that is not a class
        return bad();
      }
      sem.entity = types.class_of(spec.type);
      decls_.push_back(sem);
      return;
    }
    parse::Declarator d = ps_.declarator(spec.type, false);
    if (!d.ok || !d.named) return bad();
    if (literal_operator_named(d.name) &&
        (!defaults_allowed(d.params, types.is_function(d.type), d.name) || !literal_operator_ok(spec, d, LangLinkage::cxx, false)))
      return bad();
    const bool c_context = enclosing_linkage() == LangLinkage::c;
    const bool ordinary = (definition || !c_context) && !d.qualified && !d.template_args && p_->namespace_class_friend(k_) &&
                          p_->region().tokens[it_->begin + d.name_at].kind == Tk::identifier;
    if (!types.is_function(d.type) ||
        (definition ? !ordinary || !d.function || !ps_.at_end() || !it_->has_body() ||
                        p_->region().tokens[it_->body_begin].kind != Tk::l_brace
                    : !ps_.is(Tk::semi))) {
      s_.diag(d.name_at, Diag::not_supported, Tk::kw_friend);
      return bad();
    }
    if (ordinary) {
      if (c_context) {
        s_.diag(*friend_at, Diag::friend_c_linkage);
        return bad();
      }
      // [dcl.fct.default]/4 imposes separate requirements on defaults written in a friend.
      if (spec.is_static || spec.is_extern || spec.is_typedef ||
          std::ranges::any_of(d.params, [](const parse::Param& p) {
            return p.default_arg != NodeId::none || p.default_tokens.has_value();
          })) {
        s_.diag(*friend_at, Diag::not_supported, Tk::kw_friend);
        return bad();
      }
      if (!declare(spec, std::move(d), LangLinkage::cxx, false, definition, /*ordinary_friend=*/true)) return bad();
      p_->ordinary_friend_items_.insert(k_);
      if (!definition) ps_.expect(Tk::semi);
      return;
    }
    sem.kind = ast::DeclKind::function;
    sem.type = d.type;
    if (d.qualified && d.scope.ns) {
      // `friend R ::f(…);`, `friend R N::f(…);`: a function the namespace declared earlier.
      auto f = s_.namespace_function(*d.scope.ns, d.name, d.type);
      if (!f) {
        s_.diag(d.name_at, Diag::no_friend_match, d.name);
        return bad();
      }
      sem.entity = *f;
    } else if (d.qualified) {
      // A member function of another, complete class: its declaration with this type.
      if (!d.scope.type || !types.is_class(*d.scope.type) || !s_.require_complete(*d.scope.type, d.qualifier_at)) return bad();
      EntityId other = types.class_of(*d.scope.type);
      const sema::ClassRec* r = p_->classes_.find(other);
      const sema::MemberDeclRec* m = nullptr;
      if (r)
        for (const sema::MemberDeclRec& x : r->members)
          if (x.name == d.name && x.is_function && types.canonical(x.type) == types.canonical(d.type)) m = &x;
      if (!m) {
        s_.diag(d.name_at, Diag::no_member_declared, d.name, *d.scope.type);
        return bad();
      }
      sem.entity = m->entity;
    } else {
      // The function of the innermost enclosing namespace with this name and type
      // ([namespace.memdef]/3), whose namespace-scope declarations name the same entity.
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::function, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, ns_entity()});
    }
    decls_.push_back(sem);
    ps_.expect(Tk::semi);
  }

  // A typedef-name or alias-declaration that is a member of class `c` ([class.mem],
  // [dcl.typedef]): a type of the class's scope, `C::name` outside it.
  bool member_alias(const ast::DeclSema& c, NameId name, u32 at, QualType type) {
    if (literal_operator_named(name)) {
      s_.diag(at, Diag::literal_operator_scope);
      return false;
    }
    if (s_.types().has_placeholder(type)) {  // [dcl.spec.auto.general]/6
      s_.diag(at, Diag::auto_not_allowed);
      return false;
    }
    // [temp.local]/6: not a template parameter's name; [class.mem.general]: not the class's.
    if (s_.template_param(name)) {
      s_.diag(at, Diag::redeclared_local, name);
      return false;
    }
    if (name == s_.world().entities.rec(c.entity).name) {
      s_.diag(at, Diag::member_named_as_class, name);
      return false;
    }
    if (member_declared(name)) {
      s_.diag(at, Diag::redeclared_member, name);
      return false;
    }
    ast::DeclSema sem = class_decl(s_.world().entities.entity({sema::EntityKind::type_alias, name, {}, LangLinkage::cxx,
                                                                 ast::Linkage::none, {}, c.entity}),
                                   false);
    sem.kind = ast::DeclKind::type_alias;
    sem.type = type;
    sem.linkage = ast::Linkage::none;
    sem.flags = sema::decl_flag::member;
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    return true;
  }

  // Whether the item's "body" is `= delete` ([dcl.fct.def.delete]).
  bool deleted_body() const {
    const pp::Region& r = p_->region();
    return it_->has_body() && r.tokens[it_->body_begin].kind == Tk::equal && it_->body_begin + 1 < it_->body_end &&
           r.tokens[it_->body_begin + 1].kind == Tk::kw_delete;
  }

  // A qualified definition of a namespace member redeclares an earlier declaration in its
  // target namespace or inline namespace set ([dcl.meaning.general], [basic.scope.scope]).
  bool define_namespace_member(const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition,
                               u32 ordinal, bool explicit_lang) {
    const std::string ns = p_->namespace_path(*d.scope.ns);
    const QualType type = s_.types().canonical(sem.type);
    std::optional<ast::DeclSema> mem;
    std::optional<std::string> c_namespace;
    bool linkage_conflict = false;
    for (DeclId p : p_->declared_in(p_->world().ins.names.spelling(d.name), ns, k_, false)) {
      auto ds = p_->decl_sema(p);
      if (!ds || ds->entity == EntityId::none || (ds->kind == ast::DeclKind::function) != fn) continue;
      const QualType prior_type = s_.types().canonical(ds->type);
      std::optional<std::string> target;
      if (fn && ((ds->flags | sem.flags) & sema::decl_flag::lang_c)) {
        if ((ds->flags | sem.flags) & sema::decl_flag::template_) continue;
        // A using-declaration can expose a name without making the function a member. The
        // C-linkage entity's shared identity cannot identify the earlier declaration's scope.
        const std::string& owner = p_->ns_of(p_->index(p.item));
        std::string candidate = owner;
        while (candidate != ns && p_->is_inline_namespace(candidate, k_)) candidate = Program::parent_of(candidate);
        if (candidate != ns || !Program::encloses(p_->ns_of(k_), owner)) continue;
        if (type != prior_type) {
          // Match the whole function type with only its outer language linkage substituted.
          // An unspecified linkage inherits that of this redeclaration ([dcl.link]).
          auto& ins = p_->world().ins;
          auto inherited = ins.types.get<ast::TFunction>(type);
          inherited.linkage = ins.types.get<ast::TFunction>(prior_type).linkage;
          if (ins.intern_type(inherited) != prior_type) continue;
          if (explicit_lang) {
            linkage_conflict = true;
            continue;
          }
        }
        target = owner;
      } else if (fn && type != prior_type) {
        continue;
      }
      mem = ds;
      c_namespace = std::move(target);
    }
    if (!mem) {
      s_.diag(d.name_at, linkage_conflict ? Diag::linkage_conflict : Diag::no_namespace_member, d.name);
      return false;
    }
    auto latest = p_->declaration(mem->entity, k_);
    if (definition && latest && (latest->flags & sema::decl_flag::definition)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    sem.entity = mem->entity;
    sem.linkage = mem->linkage;
    if (c_namespace && (mem->flags & sema::decl_flag::lang_c)) {
      sem.type = mem->type;
      sem.flags |= sema::decl_flag::lang_c;
      p_->qualified_c_functions_.emplace(std::pair{k_, ordinal}, std::move(*c_namespace));
    }
    return true;
  }

  // Whether member `r` is what a definition outside the class defines: a member template when
  // the definition's innermost head is the member's own, with an equivalent head and
  // constraints ([temp.over.link]/4-6), otherwise a member that is no template.
  bool same_template(const sema::MemberDeclRec& r, bool member_template) {
    if (r.is_template != member_template) return false;
    if (!member_template) return true;
    const sema::EntityRec& e = p_->world().entities.rec(r.entity);
    return e.head == head_fp(s_.template_scopes.back()) && e.constraints == constraints_fp_;
  }
  // A constructor or destructor defined outside its class: `N::C::C(…)`, `C::~C()`.
  void out_of_class_special(QualType cls, u32 next, u16 flags) {
    EntityId c = s_.types().class_of(cls);
    for (u32 j = 0; j < next; ++j) ps_.bump();
    bool ctor = !ps_.is(Tk::tilde);
    u32 name_at = ps_.pos();
    if (!ctor) ps_.bump();
    ps_.bump();
    EntityId owner = s_.world().entities.rec(c).scope;
    if (flags & sema::decl_flag::constexpr_) {
      while (!p_->is_namespace(owner)) owner = s_.world().entities.rec(owner).scope;
      if (!Program::encloses(p_->ns_of(k_), p_->namespace_path(owner))) {
        s_.diag(name_at, Diag::constructor_namespace);
        return bad();
      }
    }
    s_.current_class = c;
    q_.from = p_->namespace_path(owner);
    bool ok = true;
    auto fs = ps_.function_suffix(ok);
    if (!ok) return bad();
    std::vector<QualType> ps;
    for (const parse::Param& p : fs.params) ps.push_back(p.type);
    QualType type = s_.types().function(s_.builtin(sema::Builtin::void_), ps, fs.variadic, LangLinkage::cxx, fs.cv);
    // Complete here: this unit has analysed the class, so it knows the members' declarations.
    const sema::ClassRec* cr = q_.is_complete(c) ? p_->classes_.find(c) : nullptr;
    // A constructor template's definition: the innermost head is its own ([temp.mem]/1).
    const bool member_template = member_template_ || (template_ && s_.world().entities.rec(c).templ == EntityId::none);
    const sema::MemberDeclRec* m = nullptr;
    if (cr)
      for (const sema::MemberDeclRec& r : cr->members)
        if ((ctor ? r.is_ctor : r.is_dtor) && (s_.types().canonical(r.type) == s_.types().canonical(type) || r.is_virtual) &&
            same_template(r, member_template))
          m = &r;
    if (!m) {
      s_.diag(name_at, Diag::no_member_declared, p_->world().entities.rec(c).name, cls);
      return bad();
    }
    auto md = p_->member_decl(m->entity);
    auto mem = md ? p_->decl_sema(*md) : std::nullopt;
    if (!mem || it_->kind != ItemKind::function) return bad();
    if ((mem->flags & sema::decl_flag::constexpr_) != (flags & sema::decl_flag::constexpr_)) {
      s_.diag(name_at, Diag::constexpr_redeclared, m->name);
      return bad();
    }
    if ((flags & sema::decl_flag::constexpr_) && !finish_constructor_declarator(false)) return bad();
    if (mem->flags & sema::decl_flag::definition) {
      s_.diag(name_at, Diag::redefinition, m->name);
      return bad();
    }
    if (m->is_virtual) {
      s_.diag(name_at, Diag::virtual_definition);
      return bad();
    }
    // Its record, rebuilt in this frame (the declaration's lists belong to the class's item).
    ast::DeclSema sem = *mem;
    sem.flags |= flags | sema::decl_flag::definition;
    sem.attributes = b_.seq(std::span<const NodeId>{});
    std::vector<ast::ParamSema> params;
    for (const parse::Param& p : fs.params) params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
    sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    // A class template's constructor or destructor defined outside it: the head is its body's; a
    // constructor template's, its own.
    if (member_template) sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    else if (template_) sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.front()));
    p_->declared(sem, k_);
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), {}, ConstId::none}, name_at, ps_.pos()));
    decls_.push_back(sem);
  }

  // `namespace N { … }` ([namespace.def]): the namespace its members belong to.
  // A namespace's or linkage block's `}` ([namespace.def], [dcl.link]): the splitter cut the item
  // at the end of the file without one.
  void braces_closed() {
    const pp::Region& r = p_->region();
    if (it_->brace_end <= it_->brace_begin) return;
    int depth = 0;
    for (u32 t = it_->brace_begin; t < it_->brace_end; ++t) {
      Tk k = r.tokens[t].kind;
      depth += k == Tk::l_brace ? 1 : k == Tk::r_brace ? -1 : 0;
    }
    if (depth > 0) {
      ps_.seek(it_->brace_end);
      ps_.expect(Tk::r_brace);
    }
  }

  void namespace_definition() {
    braces_closed();
    if (!namespace_head_ok()) return bad();
    if (it_->names.empty()) return;  // an unnamed namespace declares no name
    // [basic.scope.scope]/4, [namespace.def]: a namespace name names nothing else in its scope.
    NameId name = p_->world().ins.names.intern(it_->names.front().name);
    for (DeclId p : q_.visible_decls(name))
      if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && ds->kind != ast::DeclKind::namespace_ &&
                                      same_scope(*ds)) {
        s_.diag(it_->names.front().offset, Diag::conflicting_declaration, name);
        return bad();
      }
    // `namespace a::b`: one declaration of each namespace it names ([namespace.def]/2); a later
    // component names nothing else in the namespace before it either.
    std::string path = p_->ns_of(k_);
    for (const items::DeclName& n : it_->names) {
      if (&n != &it_->names.front())
        for (DeclId p : p_->declared_in(n.name, path, k_, false, true))
          if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && ds->kind != ast::DeclKind::namespace_) {
            s_.diag(n.offset, Diag::conflicting_declaration, p_->world().ins.names.intern(n.name));
            return bad();
          }
      path += (path.empty() ? "" : "::") + n.name;
      ast::DeclSema sem = class_decl(p_->namespace_entity(path), true);
      sem.kind = ast::DeclKind::namespace_;
      sem.type = s_.builtin(sema::Builtin::void_);
      decls_.push_back(sem);
    }
  }

  // The head of a namespace definition ([namespace.def.general]): `inline_opt namespace attributes_opt
  // identifier_opt attributes_opt {`, or a nested-namespace-definition, `namespace a :: inline_opt b
  // … {`, which has no attributes and no leading `inline`.
  bool namespace_head_ok() {
    const pp::Region& r = p_->region();
    const u32 end = it_->brace_begin;
    u32 t = it_->begin;
    auto k = [&](u32 j) { return j < end ? r.tokens[j].kind : Tk::l_brace; };
    auto fail = [&](u32 j, Diag d) {
      s_.diag(std::min(j, end) - it_->begin, d);
      return false;
    };
    auto attributes = [&] {
      bool any = false;
      for (;;) {
        u32 open = k(t) == Tk::l_square ? t
                   : k(t) == Tk::identifier && r.name(r.tokens[t]) == "__attribute__" && k(t + 1) == Tk::l_paren ? t + 1
                                                                                                                 : end;
        if (open == end) return any;
        int depth = 0;
        for (t = open; t < end; ++t) {
          Tk x = k(t);
          depth += (x == Tk::l_square || x == Tk::l_paren) - (x == Tk::r_square || x == Tk::r_paren);
          if (depth == 0) break;
        }
        ++t;
        any = true;
      }
    };
    if (k(t) == Tk::kw_export) ++t;
    bool leading_inline = k(t) == Tk::kw_inline;
    if (leading_inline) ++t;
    if (k(t) != Tk::kw_namespace) return true;
    ++t;
    bool before = attributes();
    if (t == end) return true;  // an unnamed namespace
    if (k(t) != Tk::identifier) return fail(t, Diag::expected_namespace_name);
    ++t;
    if (k(t) == Tk::coloncolon) {
      if (before) return fail(it_->begin, Diag::nested_namespace_attributes);
      if (leading_inline) return fail(it_->begin, Diag::nested_namespace_inline);
      while (k(t) == Tk::coloncolon) {
        ++t;
        if (k(t) == Tk::kw_inline) ++t;
        if (k(t) != Tk::identifier) return fail(t, Diag::expected_namespace_name);
        ++t;
      }
      if (t != end) return fail(t, Diag::nested_namespace_attributes);
      return true;
    }
    attributes();
    if (t != end) return fail(t, Diag::expected_namespace_name);
    return true;
  }

  // An array's innermost element type; any other type itself.
  QualType element_type(QualType t) {
    while (s_.types().is_array(t)) t = s_.types().rec(t).inner;
    return t;
  }

  // Constructors and destructors have no return type. Look past their specifiers before
  // ordinary declaration parsing can read the class's name as a type.
  u32 constructor_start() {
    u32 next = ps_.past_attributes(0);
    while (ps_.kind(next) == Tk::kw_constexpr || ps_.kind(next) == Tk::kw_inline || ps_.kind(next) == Tk::kw_static ||
           ps_.kind(next) == Tk::kw_virtual)
      next = ps_.past_attributes(next + 1);
    return next;
  }

  // Check the shape of a qualified constructor before looking up its scope. A declaration with
  // a qualified return type normally has a declarator between that type and the parameter list.
  bool qualified_constructor_ahead(u32 next) {
    if (!ps_.qualified_ahead(next)) return false;
    if (ps_.kind(next) == Tk::coloncolon) ++next;
    while (ps_.kind(next) == Tk::identifier) {
      u32 after = next + 1;
      if (ps_.kind(after) == Tk::less) after = ps_.past_template_args(after);
      if (ps_.kind(after) != Tk::coloncolon) break;
      next = after + 1;
    }
    if (ps_.kind(next) == Tk::tilde) ++next;
    return ps_.kind(next) == Tk::identifier && ps_.kind(next + 1) == Tk::l_paren;
  }

  std::optional<u16> constructor_flags(u32 count, std::optional<u32>* virtual_at = nullptr) {
    using namespace sema::decl_flag;
    u16 flags = 0;
    u32 end = ps_.pos() + count;
    while (ps_.pos() < end) {
      if (ps_.skip_attributes()) continue;
      Tk kind = ps_.kind();
      if (kind == Tk::kw_static) {
        s_.diag(ps_.pos(), Diag::static_constructor);
        return std::nullopt;
      }
      if (kind == Tk::kw_virtual) {
        if (!virtual_at) {
          s_.diag(ps_.pos(), Diag::virtual_constructor);
          return std::nullopt;
        }
        if (*virtual_at) {
          s_.diag(ps_.pos(), Diag::duplicate_specifier, kind);
          return std::nullopt;
        }
        *virtual_at = ps_.pos();
        ps_.bump();
        continue;
      }
      u16 flag = kind == Tk::kw_constexpr ? constexpr_ : inline_;
      if (flags & flag) s_.diag(ps_.pos(), Diag::duplicate_specifier, kind);
      flags |= flag;
      ps_.bump();
    }
    if (flags & constexpr_) flags |= inline_;  // [dcl.constexpr]
    return flags;
  }

  // The declarator's tokens end at the body. An in-class definition can spell bare noexcept;
  // named exception specifications beyond that spelling are tracked in #219.
  bool finish_constructor_declarator(bool in_class, bool* written_noexcept = nullptr) {
    ps_.skip_attributes();
    const bool bare_noexcept = in_class && ps_.accept(Tk::kw_noexcept);
    if (bare_noexcept) ps_.skip_attributes();
    if (written_noexcept) *written_noexcept = bare_noexcept;
    if (ps_.at_end()) return true;
    s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
    return false;
  }

  bool virtual_member_scope(EntityId cls, u32 at) {
    if (member_template_) {
      s_.diag(at, Diag::virtual_member_template);
      return false;
    }
    if (p_->class_rec_is_union(it_->parent)) {
      s_.diag(at, Diag::virtual_union_member);
      return false;
    }
    if (!s_.template_scopes.empty() || inst_ || !q_.defining_bases(cls).empty() ||
        !p_->is_namespace(s_.world().entities.rec(cls).scope) || p_->items().items[it_->parent].specialization) {
      s_.diag(at, Diag::not_supported, Tk::kw_virtual);
      return false;
    }
    return true;
  }

  bool member_virt_specifier(EntityId cls) {
    if (!ps_.is(Tk::identifier)) return true;
    const auto name = s_.ins().names.spelling(ps_.name_of(ps_.tok()));
    if (name == "override" && q_.defining_bases(cls).empty() && s_.template_scopes.empty()) {
      s_.diag(ps_.pos(), Diag::override_without_base);
      return false;
    }
    if (name == "override" || name == "final") {
      s_.diag(ps_.pos(), Diag::not_supported, Tk::identifier);
      return false;
    }
    return true;
  }

  // A constructor or destructor declared in class `c` ([class.ctor], [class.dtor]), possibly
  // deleted ([dcl.fct.def.delete]).
  void special_member(const ast::DeclSema& c, bool ctor, u16 flags, std::optional<u32> virtual_at) {
    sema::Types& types = s_.types();
    u32 name_at = ps_.pos();
    if (!ctor) ps_.bump();
    NameId name = ctor ? p_->world().entities.rec(c.entity).name
                       : p_->world().ins.names.intern("~" + std::string(p_->world().ins.names.spelling(p_->world().entities.rec(c.entity).name)));
    ps_.bump();
    bool ok = true;
    auto fs = ps_.function_suffix(ok);
    if (!ok) return bad();
    if (ctor && (fs.cv != sema::Cv::none || ps_.is(Tk::amp) || ps_.is(Tk::ampamp))) {
      s_.diag(name_at, Diag::qualified_constructor);
      return bad();
    }
    const bool defaulted = p_->defaulted_body(k_);
    const bool defaulted_copy = defaulted && !virtual_at;
    bool written_noexcept = false;
    if (virtual_at) {
      if (!virtual_member_scope(c.entity, *virtual_at)) return bad();
      if (fs.cv != sema::Cv::none || ps_.is(Tk::amp) || ps_.is(Tk::ampamp)) {
        s_.diag(name_at, Diag::not_supported, Tk::tilde);
        return bad();
      }
      written_noexcept = ps_.accept(Tk::kw_noexcept);
      ps_.skip_attributes();
      if (!member_virt_specifier(c.entity)) return bad();
      if (!ps_.at_end() && !ps_.is(Tk::semi)) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        return bad();
      }
      if (defaulted) flags |= sema::decl_flag::constexpr_ | sema::decl_flag::inline_;
    } else if (defaulted_copy) {
      // [dcl.fct.def.default]: first declaration of an ordinary copy constructor.
      if (!finish_constructor_declarator(true, &written_noexcept)) return bad();
      auto copy_reference = [&] {
        if (fs.params.size() != 1 || fs.variadic) return false;
        const parse::Param& p = fs.params.front();
        if (p.default_arg != NodeId::none || p.default_tokens) return false;
        const auto ref = types.rec(p.type);
        return ref.kind == sema::TypeKind::reference && !ref.rvalue_ref &&
               !sema::Types::is_volatile(types.canonical(ref.inner)) &&
               sema::Types::unqualified(types.canonical(ref.inner)) == types.canonical(c.type);
      };
      if (!ctor || member_template_ || !s_.template_scopes.empty() || inst_ ||
          !p_->is_namespace(p_->world().entities.rec(c.entity).scope) || !copy_reference()) {
        s_.diag(name_at, Diag::defaulted_copy_declaration);
        return bad();
      }
      // A first-declaration defaulted special member is inline and, when constexpr-suitable,
      // implicitly constexpr ([dcl.fct.def.default]).
      flags |= sema::decl_flag::constexpr_ | sema::decl_flag::inline_;
    } else if ((flags & sema::decl_flag::constexpr_) && it_->has_body() && !finish_constructor_declarator(true)) {
      return bad();
    }
    if (!ctor && (!fs.params.empty() || fs.variadic)) {
      s_.diag(name_at, Diag::not_supported, Tk::tilde);  // [class.dtor]/1: a destructor has no parameters
      return bad();
    }
    if (!virtual_at && !member_virt_specifier(c.entity)) return bad();
    std::vector<QualType> ps;
    for (const parse::Param& p : fs.params) ps.push_back(p.type);
    QualType type = types.function(s_.builtin(sema::Builtin::void_), ps, fs.variadic, LangLinkage::cxx, fs.cv,
                                   ast::RefQual::none, written_noexcept);
    flags |= sema::decl_flag::member | (ctor ? sema::decl_flag::constructor : sema::decl_flag::destructor);
    if (it_->kind == ItemKind::function) flags |= sema::decl_flag::definition | sema::decl_flag::inline_;
    if (deleted_body()) {
      flags |= sema::decl_flag::deleted | sema::decl_flag::definition;
    } else if (ps_.is(Tk::equal)) {
      s_.diag(ps_.pos(), Diag::not_supported, Tk::equal);  // `= default`
      return bad();
    }
    ast::DeclSema sem = class_decl(c.entity, false);
    sem.kind = ast::DeclKind::function;
    sem.type = type;
    sem.linkage = ast::Linkage::external;
    sem.flags = flags;
    sem.is_virtual = virtual_at.has_value();
    std::vector<ast::ParamSema> params;
    for (const parse::Param& p : fs.params) params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
    if (!defaults_trail(params, fs.params, name_at)) return bad();
    sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    if (member_template_) {
      // A constructor template ([temp.mem]/1): a function template named as its class, whose
      // specializations are constructors (`Sema::specialization`). A destructor is none.
      if (!ctor) {
        s_.diag(name_at, Diag::not_supported, Tk::kw_template);
        return bad();
      }
      sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
      sem.flags |= sema::decl_flag::template_;
      sema::EntityRec e{sema::EntityKind::function_template, name, type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
      e.head = head_fp(s_.template_scopes.back());
      sem.constraints = associated_constraints();
      e.constraints = constraints_fp_;
      sem.entity = s_.world().entities.entity(e);
    } else {
      sem.entity = s_.world().entities.entity({ctor ? sema::EntityKind::constructor : sema::EntityKind::destructor, name, type,
                                              LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
    }
    // One declaration of each ([class.mem.general]/5). An exception specification cannot
    // distinguish overloads of a constructor ([over.load]).
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_) break;
      for (const ast::DeclSema& o : p_->world().ins.seqs.elements(earlier(m).decls)) {
        const bool redeclared_copy =
            ctor && !member_template_ && (o.flags & sema::decl_flag::constructor) &&
            !(o.flags & sema::decl_flag::template_) && (defaulted_copy || p_->defaulted_member_definition(m, o)) &&
            same_function_parameters(o.type, sem.type);
        const bool redeclared_virtual_destructor = !ctor && (sem.is_virtual || o.is_virtual) &&
                                                  (o.flags & sema::decl_flag::destructor);
        if (o.entity == sem.entity || redeclared_copy || redeclared_virtual_destructor) {
          s_.diag(name_at, Diag::redeclared_member, name);
          return bad();
        }
      }
    }
    remember_member_defaults(sem.entity, fs.params);
    p_->declared(sem, k_);
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), {}, ConstId::none}, name_at, ps_.pos()));
    decls_.push_back(sem);
    if (it_->kind != ItemKind::function && !ps_.expect(Tk::semi)) bad();
  }

  // A qualified declarator `C::m` outside its class: the definition of a member function or a
  // static data member C declares ([class.mfct], [class.static.data]), with its entity and flags.
  bool define_member(const DeclSpec& spec, const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition,
                     bool has_init) {
    if (spec.is_static) {
      s_.diag(spec.begin, Diag::static_outside_class);  // [class.static.mfct], [class.static.data]
      return false;
    }
    std::optional<QualType> t = d.scope.type;
    if (!t || !s_.types().is_class(*t)) {
      s_.diag(d.qualifier_at, Diag::not_a_type, d.qualifier);
      return false;
    }
    if (!s_.require_complete(*t, d.qualifier_at)) return false;
    const sema::ClassRec* cr = p_->classes_.find(s_.types().class_of(*t));
    // A dependent class other than the current instantiation: a partial specialization's
    // member ([temp.spec.partial.member]), not supported yet (#88).
    if (!cr) {
      s_.diag(d.qualifier_at, Diag::not_supported, Tk::kw_template);
      return false;
    }
    // `typename C<U>::T` in the definition names the class's own member type T ([temp.dep.type]/1).
    EntityId cls = s_.types().class_of(*t);
    QualType own = s_.templates().resolve_dep_names(sem.type, [&](QualType q, NameId n) -> std::optional<QualType> {
      if (!s_.types().is_class(q) || s_.types().class_of(q) != cls) return std::nullopt;
      for (const auto& [name, mt, me] : cr->types)
        if (name == n) return mt;
      return std::nullopt;
    });
    if (s_.types().canonical(own) != s_.types().canonical(sem.type)) sem.type = own;
    const sema::MemberDeclRec* m = nullptr;
    for (const sema::MemberDeclRec& r : cr->members) {
      if (r.name != d.name || r.is_function != fn || !same_template(r, member_template_)) continue;
      const auto& types = s_.types();
      if (types.canonical(r.type) == types.canonical(sem.type) ||
          (r.is_virtual && same_function_parameters(r.type, sem.type) &&
           types.canonical(types.rec(r.type).inner) == types.canonical(types.rec(sem.type).inner)))
        m = &r;
    }
    if (!m || (!fn && !m->is_static)) {
      s_.diag(d.name_at, Diag::no_member_declared, d.name, *t);
      return false;
    }
    auto md = p_->member_decl(m->entity);
    auto mem = md ? p_->decl_sema(*md) : std::nullopt;
    if (!mem) return false;
    // [class.mfct]/1, [class.static.data]/3: outside its class a member is only defined, once. An
    // explicit specialization of a member of a class template specialization may be declared,
    // and replaces the member the class's instantiation declared or defined ([temp.expl.spec]).
    if (explicit_) {
      auto latest = p_->declaration(m->entity, k_);
      // A static data member initialized in its class has its initializer ([class.static.data]/4);
      // a specialization cannot give it another.
      if (!fn && has_init && ((mem->flags & sema::decl_flag::definition) || p_->is_lazy_member(m->entity) || p_->has_constant(m->entity))) {
        s_.diag(d.name_at, Diag::redefinition, d.name);
        return false;
      }
      if (definition && latest && (latest->flags & sema::decl_flag::specialization) && (latest->flags & sema::decl_flag::definition)) {
        s_.diag(d.name_at, Diag::redefinition, d.name);
        return false;
      }
      constexpr u16 kept = sema::decl_flag::member | sema::decl_flag::static_;
      sem.flags = u16((sem.flags & ~kept) | (mem->flags & kept));
      sem.entity = m->entity;
      sem.linkage = mem->linkage;
      sem.storage = mem->storage;
      return true;
    }
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
    if (m->is_virtual) {
      s_.diag(d.name_at, Diag::virtual_definition);
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

  void remember_member_defaults(EntityId entity, std::span<const parse::Param> params) {
    if (std::ranges::none_of(params, [](const parse::Param& p) { return p.default_tokens.has_value(); })) return;
    auto& tokens = p_->member_default_tokens_[entity];
    for (const auto& p : params) tokens.push_back(p.default_tokens);
  }

  // A member function or static data member of class `c` declared in its class ([class.mfct],
  // [class.static]).
  bool member_function_or_static(const DeclSpec& spec, parse::Declarator& d, ast::DeclSema c) {
    sema::Types& types = s_.types();
    bool fn = types.is_function(d.type);
    if (fn && !member_operator_ok(d, spec.is_static)) return false;
    if (!defaults_allowed(d.params, fn, d.name)) return false;
    bool is_def = it_->kind == ItemKind::function;
    const bool assignment = fn && s_.ins().names.spelling(d.name) == "operator=";
    const bool defaulted = assignment && p_->defaulted_body(k_);
    const bool deleted = assignment && deleted_body() && it_->body_end - it_->body_begin == 3 &&
                         p_->region().tokens[it_->body_end - 1].kind == Tk::semi;
    if (defaulted || deleted) {
      using sema::TypeKind;
      const auto parameter = p_->assignment_parameter({.name = d.name, .type = d.type, .is_function = true,
                                                       .is_static = spec.is_static, .is_template = member_template_,
                                                       .defaults = {}}, c.type);
      const bool ordinary = s_.template_scopes.empty() && !inst_ &&
                            p_->is_namespace(s_.world().entities.rec(c.entity).scope);
      if (!ordinary || !parameter || types.rec(*parameter).rvalue_ref) {
        s_.diag(d.name_at, Diag::not_supported, defaulted ? Tk::kw_default : Tk::kw_delete);
        return false;
      }
      if (defaulted) {
        const auto f = types.rec(d.type);
        const auto source = types.rec(*parameter);
        if (source.kind != TypeKind::reference || sema::Types::is_volatile(types.canonical(source.inner)) ||
            f.fn_cv != sema::Cv::none || f.fn_ref != ast::RefQual::none || spec.is_consteval ||
            types.canonical(f.inner) != types.lvalue_ref(c.type)) {
          s_.diag(d.name_at, Diag::defaulted_copy_assignment_declaration);
          return false;
        }
      }
    }
    ast::DeclSema sem = class_decl(c.entity, false);
    sem.type = d.type;
    sem.linkage = ast::Linkage::external;
    sem.is_virtual = spec.virtual_at.has_value();
    u16 flags = sema::decl_flag::member;
    if (spec.is_static) flags |= sema::decl_flag::static_;
    if (spec.is_constexpr || defaulted) flags |= sema::decl_flag::constexpr_;
    if (spec.is_consteval) flags |= sema::decl_flag::consteval_;
    if (deleted) flags |= sema::decl_flag::deleted;
    if (spec.is_inline || spec.is_constexpr || spec.is_consteval || is_def) flags |= sema::decl_flag::inline_;
    if (member_declared(d.name, fn ? std::optional(d.type) : std::nullopt, assignment || sem.is_virtual)) {
      if (fn && s_.ins().names.spelling(d.name) == lex::conversion_function_name)
        s_.diag(d.name_at, Diag::conversion_redeclared, types.rec(d.type).inner);
      else
        s_.diag(d.name_at, Diag::redeclared_member, d.name);
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
      for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
      if (!defaults_trail(params, d.params, d.name_at)) return false;
      sem.params = b_.seq(std::span<const ast::ParamSema>(params));
      if (member_template_) {
        // A member function template: its head, positional, and constraints are part of it
        // ([temp.over.link]).
        sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
        flags |= sema::decl_flag::template_;
        sema::EntityRec e{sema::EntityKind::function_template, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
        e.head = head_fp(s_.template_scopes.back());
        sem.constraints = associated_constraints();
        e.constraints = constraints_fp_;
        sem.entity = s_.world().entities.entity(e);
      } else {
        sem.entity = s_.world().entities.entity(
            {sema::EntityKind::function, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
      }
      remember_member_defaults(sem.entity, d.params);
    } else if (member_template_) {
      s_.diag(d.name_at, Diag::not_supported, Tk::kw_template);  // a static data member template
      return false;
    } else {
      // A static data member: a declaration, unless inline or constexpr ([class.static.data]).
      // One that is const integral or constexpr may give its value in the class, and is then
      // usable in constant expressions ([expr.const]).
      sem.kind = ast::DeclKind::variable;
      sem.storage = ast::Storage::static_;
      if (spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);
      // A placeholder type is deduced from the initializer in the class ([dcl.type.auto.deduct]),
      // read here, before the member is declared.
      std::optional<Ex> early;
      if (types.has_placeholder(d.type)) {
        std::unique_ptr<sema::Braced> list;
        if (ps_.is(Tk::l_brace) || !deduce_variable(d, list, early) || list) {
          if (ps_.is(Tk::l_brace)) s_.diag(ps_.pos(), Diag::not_supported, Tk::l_brace);
          return false;
        }
      }
      sem.type = d.type;
      if (spec.is_inline || spec.is_constexpr) {
        if (!s_.require_supported_virtual_layout(d.type, d.name_at)) return false;
        flags |= sema::decl_flag::definition;
      }
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::variable, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
      // A member whose initializer is in error is still declared, so its uses say nothing more.
      const u32 ordinal = u32(decls_.size());
      auto declare_member = [&](ConstId v, const Ex& e) {
        sem.flags = flags;
        s_.own.push_back({d.name, sem});
        if (!member_value_) p_->declared(sem, k_);
        declarators_.push_back(s_.node(ast::Declarator{ordinal, sema::Sema::at(e, d.begin), v}, d.begin, ps_.pos()));
        decls_.push_back(sem);
      };
      // [temp.inst]/3: a class template specialization declares its static data members; the
      // initializer of one defined in the class (inline or constexpr) is instantiated when the
      // member is used (`Program::member_value`), from the extent the template's analysis recorded.
      if (inst_ && !early && (spec.is_inline || spec.is_constexpr) && ps_.is(Tk::equal) && member_value_ != ordinal) {
        if (auto end = p_->static_init_end(k_, ordinal)) {
          ps_.seek(*end);
          p_->lazy_member(sem.entity, k_, ordinal, *inst_);
          declare_member(ConstId::none, {});
          return true;
        }
      }
      if (early || ps_.accept(Tk::equal)) {
        Ex e = early ? *early : ps_.assignment_expression();
        if (!inst_) p_->set_static_init_end(k_, ordinal, ps_.index());
        init = s_.convert(e, d.type, e.begin);
        if (!init.ok()) {
          declare_member(ConstId::none, {});
          return false;
        }
        // Of a dependent type, it may be integral in a specialization, which checks it again.
        bool usable = spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                            (types.is_integral(d.type) || types.is_dependent(d.type)));
        // In a class template, a value-dependent initializer is evaluated in each specialization
        // that uses the member; the template's own uses of it are value-dependent
        // ([temp.dep.constexpr]/2.3, `Sema::static_member`).
        if (s_.dependent(init) || types.is_dependent(d.type)) {
          if (usable) p_->value_dependent_member(sem.entity);
          declare_member(ConstId::none, {});
          return true;
        }
        // An inline one that is not usable in constant expressions is still constant-initialized
        // when it can be ([basic.start.static]); dynamic initialization is not supported yet.
        if (!usable && !spec.is_inline) {
          s_.diag(e.begin, Diag::not_supported, Tk::equal);
          declare_member(ConstId::none, {});
          return false;
        }
        sema::Evaluated v = sema::Evaluator(s_, q_).evaluate(init);
        if (!v.value) {
          s_.diag(e.begin, usable ? Diag::not_constant : Diag::nonconstant_initializer, v.reason);
          declare_member(ConstId::none, {});
          return false;
        }
        value = *v.value;
        if (usable) p_->set_constant(sem.entity, value, k_);
        if (member_value_) p_->set_member_init(sem.entity, value);
      } else if (spec.is_constexpr) {
        s_.diag(d.name_at, Diag::constexpr_uninitialized, d.name);
        declare_member(ConstId::none, {});
        return false;
      } else if (spec.is_inline && !types.is_dependent(d.type) && s_.has_member_inits(d.type)) {
        // Default-initialized, as a namespace-scope variable is: its constructor or default member
        // initializers ([dcl.init.general]/7, [basic.start.static]).
        if (!s_.require_complete(d.type, d.name_at)) return false;
        value = *constant_initializer(s_.default_init(d.type, d.name_at, ps_.pos()));
      }
      declare_member(value, init);
      return true;
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
  // A member function `function` (its type) clashes only with a member that is not a function or
  // one of the same type ([class.mem.general]/5, [over.load]).
  // A union's member ([class.union.general]): no reference (/2). Variant members whose class has
  // non-trivial special members or default member initializers (which make the union's deleted,
  // /3), and a default member initializer on a member other than the first, are not supported
  // yet.
  bool variant_member(const parse::Declarator& d) {
    sema::Types& types = s_.types();
    if (types.is_reference(d.type)) {
      s_.diag(d.name_at, Diag::union_reference, d.name);
      return false;
    }
    QualType et = d.type;
    while (types.is_array(et)) et = types.rec(et).inner;
    if (s_.has_member_inits(et) || s_.destructor_of(et) != EntityId::none || q_.classes().copy_of(et).first) {
      s_.diag(d.name_at, Diag::not_supported, Tk::kw_union);
      return false;
    }
    bool earlier = std::ranges::any_of(s_.own, [](const auto& o) { return o.sema.kind == ast::DeclKind::data_member; });
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_ || earlier) break;
      for (const ast::DeclSema& o : p_->world().ins.seqs.elements(earlier_sema(m).decls))
        earlier = earlier || o.kind == ast::DeclKind::data_member;
    }
    if (earlier && (ps_.is(Tk::equal) || ps_.is(Tk::l_brace))) {
      s_.diag(ps_.pos(), Diag::not_supported, Tk::equal);
      return false;
    }
    return true;
  }
  const ast::ItemSema& earlier_sema(u32 m) { return earlier(m); }

  bool member_declared(NameId n, std::optional<QualType> function = std::nullopt, bool compare_parameters = false) {
    // Functions clash when they have the same type and are both templates with equivalent heads and
    // constraints, or both not templates ([temp.over.link]/4-6, [over.load]).
    auto clash = [&](const ast::DeclSema& d) {
      if (!function || d.kind != ast::DeclKind::function) return true;
      if (s_.types().canonical(d.type) != s_.types().canonical(*function) &&
          (!(compare_parameters || d.is_virtual) || !same_function_parameters(d.type, *function))) return false;
      bool templ = (d.flags & sema::decl_flag::template_) != 0;
      if (templ != member_template_) return false;
      if (!templ) return true;
      const sema::EntityRec& e = p_->world().entities.rec(d.entity);
      return e.head == head_fp(s_.template_scopes.back()) && e.constraints == constraints_fp_;
    };
    for (const auto& o : s_.own)
      if (o.name == n && clash(o.sema)) return true;
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_) break;
      const ast::ItemSema& ms = earlier(m);
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(ms.decls))
        if (d.entity != EntityId::none && d.kind != ast::DeclKind::class_ && p_->world().entities.rec(d.entity).name == n &&
            clash(d))
          return true;
    }
    return false;
  }

  // A template-head's identity ([temp.over.link]): its parameters' kinds and non-type parameters'
  // types, not their names or defaults.
  sema::Fp128 head_fp(std::span<const ast::TemplateParamSema> ps) {
    sema::Fp128 f = ast::seed("ycxx.sema.TemplateHead");
    for (const ast::TemplateParamSema& p : ps) {
      f = ast::mix(ast::mix(f, std::to_underlying(p.kind)), p.pack);
      if (p.kind == ast::TemplateArgKind::value) f = ast::mix(f, s_.types().fp(s_.types().canonical(p.type)));
      // A template template parameter's own head ([temp.over.link]/6).
      if (p.kind == ast::TemplateArgKind::template_) f = ast::mix(f, head_fp(s_.world().ins.seqs.elements(p.params)));
    }
    return f;
  }

  // An earlier member item of the same class: the class's own record of it, or in an
  // instantiation the specialization's, which `class_content` analysed first. An
  // access-specifier declares nothing, and has no instantiation of its own.
  const ast::ItemSema& earlier(u32 m) {
    if (inst_ && p_->items().items[m].kind != ItemKind::access_spec) return p_->class_member_sema(m, inst_->spec);
    return p_->item_sema(m).value;
  }

  // The rest of the skeleton's names get empty records, so ordinals stay aligned.
  void bad() {
    while (decls_.size() < it_->names.size()) {
      ast::DeclSema d = invalid_decl();
      d.type = s_.builtin(sema::Builtin::void_);  // an interned type, for the record's fingerprint
      d.attributes = b_.seq(std::span<const NodeId>{});
      d.params = b_.seq(std::span<const ast::ParamSema>{});
      d.template_params = b_.seq(std::span<const ast::TemplateParamSema>{});
      decls_.push_back(d);
    }
  }

  // One init-declarator at namespace scope.
  // One init-declarator at namespace scope; false after an error that leaves no record.
  bool declare(const DeclSpec& spec, parse::Declarator d, LangLinkage lang, bool explicit_lang,
               bool is_function_def, bool ordinary_friend = false) {
    sema::Types& types = s_.types();
    if (!defaults_allowed(d.params, types.is_function(d.type) && !spec.is_typedef, d.name)) return false;
    if (literal_operator_named(d.name) && !literal_operator_ok(spec, d, lang, template_)) return false;
    std::string_view name = s_.ins().names.spelling(d.name);
    if (sema::is_allocation_name(name)) {
      auto matches = [&](const ast::DeclSema& f) {
        return s_.world().entities.rec(f.entity).name == d.name && types.canonical(f.type) == types.canonical(d.type);
      };
      if (!d.function || d.qualified || ns_entity() != EntityId::none || lang != LangLinkage::cxx || template_ ||
          explicit_ || spec.is_static || spec.is_inline || spec.is_typedef || spec.is_constexpr || spec.is_consteval ||
          deleted_body() || !d.asm_label.empty() || !std::ranges::any_of(s_.world().allocation_functions(), matches)) {
        s_.diag(d.name_at, Diag::allocation_declaration, d.name);
        return false;
      }
    }
    if (spec.is_typedef) {
      if (is_function_def) {  // [dcl.typedef]/1: a typedef declares no function definition
        s_.diag(spec.begin, Diag::typedef_definition);
        return false;
      }
      if (d.qualified || d.unknown_bound) {  // [dcl.typedef]; arrays of unknown bound are not supported yet
        if (d.qualified) s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        else s_.diag(d.name_at, Diag::not_supported, Tk::l_square);
        return false;
      }
      return declare_alias(d.name, d.name_at, d.type);
    }
    u32 ord = static_cast<u32>(decls_.size());
    // `T a[] = {…}`: the bound from the list ([dcl.array]/8), parsed here and used below.
    std::unique_ptr<sema::Braced> early;
    if (d.unknown_bound && ps_.is(Tk::equal) && ps_.kind(1) == Tk::string_lit) {
      // `char s[] = "…"`: the bound from the literal, null included ([dcl.init.string]/1); the
      // initialization below checks the element type.
      d.type = types.array(d.type, ps_.literal_at(1).units.size() + 1);
    } else if (d.unknown_bound && !(ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
      // `extern T a[];`: an incomplete type ([dcl.array]/6); a definition needs a bound.
      d.type = types.array_unknown(d.type);
      if (!spec.is_extern && !single_linkage_spec_) {
        s_.diag(d.name_at, Diag::incomplete_type, d.type);
        return false;
      }
    } else if (d.unknown_bound) {
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
    // [basic.link]/3–4: static, certain const declarations, and names in unnamed namespaces.
    const auto has_internal_linkage = [&] {
      return spec.is_static || Program::unnamed(p_->ns_of(k_)) ||
             (!fn && sema::Types::is_const(d.type) && !spec.is_extern && !spec.is_inline && !template_ && !explicit_);
    };
    const auto declaration_entity = [&](sema::EntityKind kind, QualType type, ast::Linkage linkage, sema::Fp128 head) {
      sema::EntityRec record{kind, d.name, type, lang, linkage,
          {p_->id(k_), ast::make_decl_ord(ast::DeclClass::syntactic, ord)},
          lang == LangLinkage::c ? EntityId::none : ns_entity()};
      record.head = head;
      record.constraints = constraints_fp_;
      return record;
    };
    // The first declaration-owned closure is the initializer of an ordinary internal variable.
    // Its identity is the final variable's. Its EntityRec is published after deduction.
    std::optional<Ex> closure_init;
    EntityId closure_owner = EntityId::none;
    if (!fn && types.kind(d.type) == sema::TypeKind::placeholder && !template_ && !var_ && !explicit_ &&
        !d.qualified && d.name != s_.no_name() && !spec.is_inline && !spec.is_extern &&
        lang == LangLinkage::cxx && has_internal_linkage() && (ps_.is(Tk::equal) || ps_.is(Tk::l_brace))) {
      u32 start = 1;
      while (ps_.kind(start) == Tk::l_paren) ++start;
      if (ps_.kind(start) == Tk::l_square && ps_.kind(start + 1) != Tk::l_square) {
        const auto ns = ns_entity();
        const auto record = declaration_entity(sema::EntityKind::variable, d.type, ast::Linkage::internal, head_fp({}));
        closure_owner = s_.world().entities.identity(record);
        s_.namespace_lambda = sema::Sema::NamespaceLambda{closure_owner, ns, d.name, ps_.pos() + start};
      }
    }
    // A placeholder type, deduced from the initializer ([dcl.type.auto.deduct]): `= e` or `{e}`.
    std::optional<Ex> early_expr;
    if (!fn && types.has_placeholder(d.type)) {
      bool deduced = deduce_variable(d, early, early_expr, template_ && !var_);
      if (s_.namespace_lambda && deduced) {
        Ex e = early_expr ? *early_expr : early->clauses.front().expr;
        closure_init = e;
        while (auto p = s_.as<ast::Paren>(e))
          e = s_.extended({p->operand.id, e.begin + p->operand.at, e.end});
        if (!s_.as<ast::Lambda>(e)) {
          s_.diag(e.begin, Diag::namespace_lambda_init);
          deduced = false;
        }
      }
      s_.namespace_lambda.reset();
      if (!deduced) return false;
    }
    ast::DeclSema sem{};
    sem.kind = fn ? ast::DeclKind::function : ast::DeclKind::variable;
    sem.type = d.type;
    sem.storage = spec.is_static ? ast::Storage::static_ : spec.is_extern ? ast::Storage::extern_ : ast::Storage::none;
    sem.linkage = has_internal_linkage() ? ast::Linkage::internal : ast::Linkage::external;
    sem.access = ast::Access::none;
    sem.constraint = NodeId::none;
    sem.constraints = ast::ConstId::none;
    sem.contracts = NodeId::none;
    sem.attributes = b_.seq(std::span<const NodeId>{});
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>{});
    u16 flags = ordinary_friend ? sema::decl_flag::friend_ : 0;
    if (spec.is_inline || (ordinary_friend && is_function_def)) flags |= sema::decl_flag::inline_;
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

    // A deleted definition ([dcl.fct.def.delete]): `= delete`, which the splitter takes for the
    // function's body.
    bool deleted = fn && deleted_body();
    if (deleted) is_function_def = true;
    if (deleted) flags |= sema::decl_flag::deleted;
    bool has_init = !deleted && (early || early_expr || ps_.is(Tk::equal) || ps_.is(Tk::l_brace) || (!fn && ps_.is(Tk::l_paren)));
    bool definition = fn ? is_function_def || deleted : !((spec.is_extern || single_linkage_spec_) && !has_init);
    if (!fn && definition && !s_.require_supported_virtual_layout(d.type, d.name_at)) return false;
    if (definition) flags |= sema::decl_flag::definition;
    const bool reference = types.is_reference(d.type);
    const bool ordinary_reference = reference && !types.is_function(types.referee(d.type)) &&
                                    !(d.qualified && d.scope.type) && !template_ && !var_ && !explicit_;
    if (!fn && types.is_void(d.type)) {
      s_.diag(d.name_at, Diag::void_variable, d.name);
      return false;
    }
    if (!fn && p_->world().ins.names.spelling(d.name) == "main") {
      s_.diag(d.name_at, Diag::main_not_function);  // [basic.start.main]/3
      return false;
    }

    if (var_) {
      // A variable template specialization's instantiation: its own entity, whose template the
      // definition declared and checked ([temp.inst]/7).
      sem.entity = var_->spec;
      sem.flags = flags;
      sem.params = b_.seq(std::span<const ast::ParamSema>{});
    } else if (explicit_) {
      if (!explicit_declaration(spec, d, sem, flags, fn, definition, has_init)) return false;
    } else if (template_ && d.template_args) {
      if (!partial_variable(d, sem, flags, fn)) return false;
    } else {
      // [temp.pre]/7: a class template's name names nothing else in its scope.
      if (!d.qualified)
        for (DeclId p : q_.visible_decls(d.name))
          if (auto ds = p_->decl_sema(p); ds && ds->kind == ast::DeclKind::class_ && (ds->flags & sema::decl_flag::template_) &&
                                          same_scope(*ds)) {
            s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
            return false;
          }
      // Earlier declarations of the name: in this item, then before it ([basic.link], [dcl.fct]). A
      // qualified name redeclares a member of its class instead.
      std::vector<ast::DeclSema> prior, interned_prior;  // the latter found by lookup, not this item's own
      std::vector<ast::ParamSema> params;
      // A class and a variable or function may share a name; the class is hidden
      // ([basic.scope.scope]/4, [basic.scope.hiding]/2), so it is not an earlier declaration.
      if (!d.qualified) {
        auto candidates = ordinary_friend ? p_->declared_in(name, p_->ns_of(k_), k_, false, /*inhabit=*/true)
                                          : q_.visible_decls(d.name);
        if (ordinary_friend || p_->in_global_scope(k_)) {
          auto hidden = p_->hidden_friends_before(name, k_);
          if (!hidden.empty()) {
            candidates.insert(candidates.end(), hidden.begin(), hidden.end());
            std::ranges::sort(candidates, {}, [&](DeclId decl) {
              return std::pair{p_->index(decl.item), ast::number_of(decl.ord)};
            });
          }
        }
        for (DeclId p : candidates) {
          auto ds = p_->decl_sema(p);
          if (!ds || ds->entity == EntityId::none || ds->kind == ast::DeclKind::class_ ||
              ds->kind == ast::DeclKind::namespace_ || ds->kind == ast::DeclKind::enum_)
            continue;
          const bool ordinary_function = fn && !template_ && ds->kind == ast::DeclKind::function;
          const bool prior_c = ds->flags & sema::decl_flag::lang_c;
          // C-linkage function identities are shared across namespaces ([dcl.link]). For a
          // redeclaration in the same target scope, use the declaration's namespace ([basic.link]).
          const bool same_target = ordinary_function && prior_c
              ? p_->ns_of(p_->index(p.item)) == p_->ns_of(k_)
              : same_scope(*ds);
          if (!same_target && (lang != LangLinkage::c || (ordinary_function && !prior_c))) continue;
          prior.push_back(*ds);
          interned_prior.push_back(*ds);
        }
        for (const auto& o : s_.own)
          if (o.name == d.name && o.sema.kind != ast::DeclKind::class_) prior.push_back(o.sema);
        if (clashes_with_using(d.name, d.name_at, fn ? ast::DeclKind::function : ast::DeclKind::variable)) return false;
      }
      if (fn && !d.qualified && !operator_function_ok(d)) return false;
      if (fn) {
        if (!explicit_lang && !template_ && std::ranges::any_of(prior, [&](const ast::DeclSema& p) {
              return p.kind == ast::DeclKind::function && (p.flags & sema::decl_flag::lang_c) &&
                     !(p.flags & sema::decl_flag::template_) && same_function_parameters(p.type, d.type);
            }))
          lang = LangLinkage::c;  // [dcl.link]/6: inherited from a corresponding declaration
        if (ordinary_friend && is_function_def && lang == LangLinkage::c) {
          s_.diag(d.name_at, Diag::friend_c_linkage);
          return false;
        }
        const sema::TypeRec& r = types.rec(d.type);
        if (r.fn_cv != sema::Cv::none && !d.qualified) {
          s_.diag(d.name_at, Diag::cv_on_non_member);  // [dcl.fct]/6
          return false;
        }
        std::vector<QualType> ps = r.params;
        sem.type = types.function(r.inner, ps, r.variadic, lang, r.fn_cv, r.fn_ref, r.fn_noexcept);
        if (lang == LangLinkage::c) flags |= sema::decl_flag::lang_c;
        if (r.variadic) flags |= sema::decl_flag::variadic;
        for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
        sem.params = b_.seq(std::span<const ast::ParamSema>(params));
      } else {
        sem.params = b_.seq(std::span<const ast::ParamSema>{});
      }
      // `template<class T> R X<T>::f(…) {…}`: a member of the class template X defined outside it
      // ([temp.mem.func]), not a template of its own; the head is its body's. The qualifier names
      // the current instantiation: X's own parameters, in order, as its arguments ([temp.dep.type]).
      bool class_member = false;
      // [temp.mem]/1: two heads only before a member template of a class template.
      if (two_heads_ && !(fn && d.qualified && d.scope.type && s_.types().is_class(*d.scope.type) &&
                          s_.world().entities.rec(s_.types().class_of(*d.scope.type)).templ != EntityId::none)) {
        s_.diag(d.name_at, Diag::extraneous_template_head);
        return false;
      }
      if (template_ && d.qualified && d.scope.type && s_.types().is_class(*d.scope.type)) {
        const sema::EntityRec& c = s_.world().entities.rec(s_.types().class_of(*d.scope.type));
        if (c.templ == EntityId::none) {
          // `template<class T> R S::f(T)`: a member template of a class defined outside it
          // ([temp.mem]); a static data member template is not supported.
          if (!fn) {
            s_.diag(d.qualifier_at, Diag::not_a_template, c.name);
            return false;
          }
          member_template_ = true;
        } else {
          const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.front();
          bool current = c.args == s_.templates().own_args(head);
          // Or a partial specialization's pattern, whose members are its own ([temp.spec.partial.member]).
          if (!current && !p_->partial_pattern(s_.types().class_of(*d.scope.type), k_)) {
            s_.diag(d.qualifier_at, Diag::template_args_mismatch, c.name);
            return false;
          }
          if (!fn) {  // its instantiation is #256
            s_.diag(d.qualifier_at, Diag::static_member_definition_not_supported);
            return false;
          }
        }
        class_member = true;
      }
      if (member_template_ && class_member) {
        // A member template's definition: its own head, the innermost ([temp.mem]/1).
        sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
        flags |= sema::decl_flag::template_;
        class_member = true;
      } else if (class_member) {
        sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.front()));
      } else if (template_) {
        // [temp.local]/6: neither the template nor its parameters may take a parameter's name.
        if (s_.template_param(d.name)) {
          s_.diag(d.name_at, Diag::redeclared_local, d.name);
          return false;
        }
        for (const parse::Param& prm : d.params)
          if (prm.name != s_.no_name() && s_.template_param(prm.name)) {
            s_.diag(prm.at, Diag::redeclared_local, prm.name);
            return false;
          }
        sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.front()));
        flags |= sema::decl_flag::template_;
        if (fn) {  // its parameters are in scope ([basic.scope.param])
          s_.push_scope();
          for (const parse::Param& prm : d.params)
            if (prm.name != s_.no_name()) s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, prm.at, false);
          trailing_requires_clause();
          s_.pop_scope();
        }
        sem.constraints = associated_constraints();
        // [temp.param]: a variable template's pack is last; a function template's is followed only
        // by parameters with a default or deduced from its parameter-type-list.
        const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.front();
        if (!fn && !pack_last(head)) {
          s_.diag(0, Diag::pack_not_last);
          return false;
        }
        // A function parameter pack that is not last is not supported yet (#88).
        if (fn) {
          const std::vector<QualType>& ps = types.rec(sem.type).params;
          for (std::size_t i = 0; i + 1 < ps.size(); ++i)
            if (types.kind(ps[i]) == sema::TypeKind::pack_expansion) {
              s_.diag(d.name_at, Diag::not_supported, Tk::ellipsis);
              return false;
            }
        }
        if (fn) {
          std::vector<std::pair<u16, u16>> deduced;
          for (QualType p : types.rec(sem.type).params) s_.templates().params_in(p, deduced);
          bool after_pack = false;
          for (const ast::TemplateParamSema& p : head) {
            if (after_pack && !p.has_default && !std::ranges::contains(deduced, std::pair(p.depth, p.index))) {
              s_.diag(0, Diag::pack_not_last);
              return false;
            }
            after_pack = after_pack || p.pack;
          }
        }
      }
      sema::Fp128 head = class_member ? sema::Fp128{} : head_fp(p_->world().ins.seqs.elements(sem.template_params));
      sem.flags = flags;
      // [over.load]: functions whose parameter lists differ are overloads, not redeclarations (C
      // language linkage aside, [dcl.link]).
      if (fn)
        std::erase_if(prior, [&](const ast::DeclSema& p) {
          if (p.kind != ast::DeclKind::function) return false;
          const bool prior_c = p.flags & sema::decl_flag::lang_c;
          if (prior_c && lang == LangLinkage::c) return false;  // [dcl.link]/7
          // A template and a function, or templates whose heads differ, are overloads ([temp.over.link]).
          if ((p.flags & sema::decl_flag::template_) != (flags & sema::decl_flag::template_) ||
              head_fp(p_->world().ins.seqs.elements(p.template_params)) != head ||
              s_.world().entities.rec(p.entity).constraints != constraints_fp_)
            return true;
          if (prior_c || lang == LangLinkage::c) return !same_function_parameters(p.type, sem.type);
          sema::TypeRec a = types.rec(p.type), c = types.rec(sem.type);
          // A function template's return type is part of its signature ([defns.signature.templ]).
          if ((flags & sema::decl_flag::template_) && types.canonical(a.inner) != types.canonical(c.inner)) return true;
          auto canon = [&](std::vector<QualType> v) {
            for (QualType& q : v) q = types.canonical(q);
            return v;
          };
          return canon(a.params) != canon(c.params) || a.variadic != c.variadic || a.fn_cv != c.fn_cv;
        });

      if (d.qualified) {
        if (d.scope.ns) {
          if (!define_namespace_member(d, sem, fn, definition, ord, explicit_lang)) return false;
          // `R N::f(…)`: the namespace's earlier declarations' default arguments ([dcl.fct.default]/4).
          if (fn) {
            if (!merge_defaults(params, q_.namespace_members(*d.scope.ns, d.name), sem.entity, d)) return false;
            sem.params = b_.seq(std::span<const ast::ParamSema>(params));
          }
        } else if (!define_member(spec, d, sem, fn, definition, has_init)) {
          return false;
        }
        // The rest of the declarator, its initializer included, is in the class's scope
        // ([basic.scope.class]).
        s_.current_class = s_.world().entities.rec(sem.entity).scope;
        s_.settle_type_access(sem.entity);
      } else if (!prior.empty()) {
        const ast::DeclSema& p = prior.back();
        if ((p.kind == ast::DeclKind::function) != fn || p.kind == ast::DeclKind::type_alias) {
          s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
          return false;
        }
        const bool canonical_match = fn && (ordinary_friend || (p.flags & (sema::decl_flag::friend_ | sema::decl_flag::lang_c)) ||
                                            lang == LangLinkage::c);
        const QualType prior_type = canonical_match ? types.canonical(p.type) : p.type;
        const QualType current_type = canonical_match ? types.canonical(sem.type) : sem.type;
        if (fn && prior_type != current_type) {
          const sema::TypeRec& a = types.rec(prior_type);
          const sema::TypeRec& c = types.rec(current_type);
          if (a.params == c.params && a.variadic == c.variadic && a.inner == c.inner)
            s_.diag(d.name_at, Diag::linkage_conflict, d.name);
          else if (a.params == c.params && a.variadic == c.variadic)
            s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
          else
            s_.diag(d.name_at, Diag::overload_not_supported, d.name);
          return false;
        }
        // [basic.link]/11: the same type, cv included, except that an array's bound may be
        // given by one declaration and not another.
        auto bound_only = [&](QualType a, QualType b) {
          return types.is_array(a) && types.is_array(b) && types.rec(a).inner == types.rec(b).inner &&
                 (types.rec(a).bound_value == ConstId::none || types.rec(b).bound_value == ConstId::none);
        };
        if (!fn && p.type != sem.type && bound_only(p.type, sem.type)) {
          if (types.rec(sem.type).bound_value == ConstId::none) sem.type = p.type;  // the bound known before
        } else if (!fn && p.type != sem.type) {
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
        if (fn && !merge_defaults(params, interned_prior, sem.entity, d)) return false;
        if (fn) sem.params = b_.seq(std::span<const ast::ParamSema>(params));
      } else {
        if (fn && !defaults_trail(params, d.params, d.name_at)) return false;
        // A function with C language linkage is one function in whichever namespace ([dcl.link]).
        const auto kind = template_ ? (fn ? sema::EntityKind::function_template : sema::EntityKind::variable_template)
                                    : (fn ? sema::EntityKind::function : sema::EntityKind::variable);
        sem.entity = s_.world().entities.entity(declaration_entity(kind, sem.type, sem.linkage, head));
      }
      s_.settle_type_access(sem.entity);
      if (fn) {  // for a later declarator of this declaration (`merge_defaults`)
        std::vector<NodeId>& own = own_defaults_[std::to_underlying(sem.entity)];
        own.clear();
        for (const ast::ParamSema& p : params) own.push_back(p.default_arg);
      }

    }
    // [dcl.link]/7: a function with C language linkage can have declarations in distinct
    // namespaces. Apply [basic.def.odr] to their shared entity before publishing the definition.
    if (fn && definition && sem.linkage == ast::Linkage::external &&
        (sem.flags & (sema::decl_flag::lang_c | sema::decl_flag::member | sema::decl_flag::template_)) ==
            sema::decl_flag::lang_c && p_->function_defined_before(sem.entity, k_)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    if (fn && p_->world().ins.names.spelling(d.name) == "main") check_main(spec, sem, d, lang);

    // The initializer ([dcl.init]); the name is in scope from the end of its declarator.
    // An explicit or partial specialization, or an instantiation, declares no name: the
    // template's name still names the template in its initializer.
    if (!var_ && !(sem.flags & (sema::decl_flag::specialization | sema::decl_flag::partial | sema::decl_flag::friend_)))
      s_.own.push_back({d.name, sem});
    p_->declared(sem, k_);
    // A variable template's initializer is a pattern ([temp.res]): its specializations' are evaluated.
    const bool pattern = template_ && !var_ && !fn;
    Ex init;
    sema::ConstId value = sema::ConstId::none;
    // An initializer that is not a constant expression initializes the variable dynamically
    // ([basic.start.dynamic]), by the unit's initialization function, unless it must be constant
    // ([dcl.constexpr], [dcl.constinit]) or is not supported yet: an inline variable's or a
    // variable template's (#368), and a scalar's the front end has no code for.
    auto dynamic_ok = [&](u32 at, std::uint64_t reason) {
      if (spec.is_constexpr || spec.is_constinit) {
        s_.diag(at, Diag::not_constant, reason);
        return false;
      }
      bool object = types.is_class(element_type(d.type)) || types.is_array(d.type);  // evaluated in parts, if at all
      if ((reference && !ordinary_reference) || spec.is_inline || var_ || template_ ||
          (!object && !sema::has_runtime_code(reason))) {
        s_.diag(at, Diag::nonconstant_initializer, reason);
        return false;
      }
      return true;
    };
    auto evaluate_reference = [&](u32 at) {
      sema::Evaluated v = sema::Evaluator(s_, q_).evaluate_reference_initializer(init);
      if (v.value) {
        value = *v.value;
        p_->set_constant(sem.entity, value, k_);  // a reference is potentially constant ([expr.const.init])
      } else if (!v.dependent) {
        dynamic_ok(at, v.reason);
      }
    };
    if (fn && has_init && !is_function_def) {
      s_.diag(ps_.pos(), Diag::function_initialized, d.name);
      ps_.skip_to_semi();
    } else if (closure_init) {
      contract_assert(sem.entity == closure_owner);
      init = s_.convert(*closure_init, d.type, closure_init->begin);
      if (init.ok()) {
        // The captureless prvalue has no fields to initialize. Keep a constant of the complete
        // closure type so the ordinary variable path allocates its addressable storage.
        value = p_->world().values.aggregate(d.type, {});
        if (spec.is_constexpr) p_->set_constant(sem.entity, value, k_);
      }
    } else if (!fn && (early || ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
      // List-initialization; constant initialization when every element is a constant
      // ([basic.start.static]).
      ps_.accept(Tk::equal);
      auto b = early ? std::move(early) : ps_.braced_init_list();
      init = s_.list_init(d.type, *b);
      if (init.ok() && !pattern) {
        if (ordinary_reference) {
          evaluate_reference(b->begin);
        } else if (auto v = constant_initializer(init)) {
          value = *v;
          if (spec.is_constexpr) p_->set_constant(sem.entity, *v, k_);
        } else {
          dynamic_ok(b->begin, sema::Evaluator(s_, q_).evaluate(init).reason);
        }
      }
    } else if (!fn && (early_expr || ps_.accept(Tk::equal))) {
      {
        Ex e = early_expr ? *early_expr : ps_.assignment_expression();
        init = s_.convert(e, d.type, e.begin);
        if (init.ok() && !pattern && ordinary_reference) {
          evaluate_reference(e.begin);
        } else if (init.ok() && !pattern) {
          // [basic.start.static]: constant or dynamic initialization.
          // A constexpr or constinit variable must be constant-initialized ([dcl.constexpr],
          // [dcl.constinit]); a constexpr one, and a const integral one that is, are usable in
          // constant expressions ([expr.const]).
          // An enum literal already carries the underlying integer constant.
          auto lit = types.is_enum(d.type) ? s_.as<ast::Literal>(init) : std::nullopt;
          // A character array from a string literal is its elements' list ([dcl.init.string]).
          auto list = s_.as<ast::InitList>(init) ? constant_initializer(init) : std::nullopt;
          sema::Evaluated v = lit    ? sema::Evaluated{lit->value, 0, true}
                              : list ? sema::Evaluated{*list, 0, true}
                                     : sema::Evaluator(s_, q_).evaluate(init);
          if (v.value) {
            value = *v.value;
            if (spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                      types.is_integral(d.type)))
              p_->set_constant(sem.entity, *v.value, k_);
          } else {
            dynamic_ok(e.begin, v.reason);
          }
        }
      }
    } else if (!fn && ps_.is(Tk::l_paren)) {
      // Direct-initialization ([dcl.init.general]): `T x(args)`, constant or dynamic; a pattern's
      // is evaluated in its specializations.
      u32 open = ps_.pos();
      ps_.bump();
      std::vector<Ex> args;
      if (!ps_.is(Tk::r_paren))
        do args.push_back(ps_.assignment_expression());
        while (ps_.accept(Tk::comma));
      if (!ps_.expect(Tk::r_paren)) ps_.skip_to_semi();
      else if (pattern) {}
      else if (types.is_class(d.type)) init = s_.construct(d.type, std::move(args), open, ps_.pos());
      else if (args.size() == 1 && !types.is_array(d.type) && (!reference || ordinary_reference))
        init = s_.convert(args[0], d.type, open);
      else s_.diag(open, Diag::not_supported, Tk::l_paren);  // a list for a scalar or an array
      if (init.ok() && ordinary_reference) {
        evaluate_reference(open);
      } else if (init.ok() && !types.is_class(d.type)) {
        sema::Evaluated v = sema::Evaluator(s_, q_).evaluate(init);
        if (v.value) {
          value = *v.value;
          if (spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) && types.is_integral(d.type)))
            p_->set_constant(sem.entity, *v.value, k_);
        } else {
          dynamic_ok(open, v.reason);
        }
      } else if (init.ok()) {
        dynamic_ok(open, sema::reason_unsupported);
      }
    } else if (pattern) {
      // Its specializations' types are complete, or not ([temp.inst]/7).
    } else if (ordinary_reference && definition) {
      s_.diag(d.name_at, Diag::reference_uninitialized);  // [dcl.ref]/5
    } else if (!fn && definition && !s_.require_complete(d.type, d.name_at)) {
      // A definition needs a complete object type ([basic.def]).
    } else if (!fn && definition && !types.is_dependent(d.type) && s_.has_user_ctors(element_type(d.type))) {
      // Default-initialization by a constructor, of an object or each element: dynamic
      // ([basic.start.dynamic]).
      init = s_.default_init(d.type, d.name_at, ps_.pos());
      if (init.ok()) dynamic_ok(d.name_at, sema::reason_unsupported);
    } else if (!fn && definition && s_.has_member_inits(d.type)) {
      // Zero-initialization, then the default member initializers ([basic.start.static]), or, if
      // they are not constant, dynamic initialization ([basic.start.dynamic]).
      Ex di = s_.default_init(d.type, d.name_at, ps_.pos());
      if (auto v = di.ok() ? constant_initializer(di) : std::nullopt) value = *v;
      else if (di.ok() && dynamic_ok(d.name_at, sema::reason_unsupported)) init = di;
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
    if (!d.asm_label.empty()) {
      // Every labelled declaration of an entity gives it one label (docs/EXTENSIONS.md).
      std::string earlier = p_->asm_label(sem.entity);
      if (!earlier.empty() && earlier != d.asm_label) {
        s_.diag(d.name_at, Diag::asm_label_conflict, d.name);
        return false;
      }
      p_->asm_labels_[std::to_underlying(sem.entity)] = d.asm_label;
    }
    return true;
  }

  // Variable `d` declared with a placeholder type: its initializer parsed (`= e` into `expr`,
  // `{e}` into `list`) and its type deduced from it ([dcl.type.auto.deduct]); several declarators
  // deduce one type for the placeholder (/2). False after a diagnosed error.
  // A variable template's definition (`pattern`) keeps the placeholder while its initializer is
  // dependent: each specialization deduces its own type (`Program::specialization_type`).
  bool deduce_variable(parse::Declarator& d, std::unique_ptr<sema::Braced>& list, std::optional<Ex>& expr, bool pattern = false) {
    Ex e;
    if (ps_.is(Tk::equal) && ps_.kind(1) != Tk::l_brace) {
      ps_.bump();
      e = ps_.assignment_expression();
      expr = e;
    } else if (ps_.is(Tk::l_brace)) {
      list = ps_.braced_init_list();
      if (list->clauses.size() != 1 || list->clauses[0].list || list->clauses[0].designated) {
        s_.diag(list->begin, Diag::auto_list, sema::u64(list->clauses.size()));
        return false;
      }
      e = list->clauses[0].expr;
    } else if (ps_.is(Tk::equal) || ps_.is(Tk::l_paren)) {
      // `= {…}` deduces std::initializer_list; `(e)` is not supported at namespace scope yet.
      s_.diag(ps_.pos() + (ps_.is(Tk::equal) ? 1 : 0), Diag::not_supported, ps_.is(Tk::equal) ? Tk::l_brace : Tk::l_paren);
      return false;
    } else {
      s_.diag(d.name_at, Diag::auto_uninitialized, d.name);
      return false;
    }
    if (!e.ok()) return false;
    QualType r{};
    auto t = s_.deduce_placeholder(d.type, e, e.begin, &r);
    if (!t) return false;
    sema::Types& types = s_.types();
    if (auto_replaced_ && !types.is_dependent(r) && !types.is_dependent(*auto_replaced_) &&
        types.canonical(*auto_replaced_) != types.canonical(r)) {
      s_.diag(d.name_at, Diag::auto_inconsistent, d.type, r, *auto_replaced_);
      return false;
    }
    auto_replaced_ = r;
    if (types.is_void(*t)) {
      s_.diag(d.name_at, Diag::void_variable, d.name);
      return false;
    }
    if (!(pattern && types.is_dependent(*t))) d.type = *t;
    return true;
  }
  // The declarator of an explicit specialization ([temp.expl.spec]): `sem` becomes the declaration
  // of the specialization of the function or variable template it names. A function template's
  // arguments are deduced from the declared type ([temp.deduct.decl]), after any given; of several
  // templates whose specialization it can be, the most specialized ([temp.func.order]).
  bool explicit_declaration(const DeclSpec& spec, const parse::Declarator& d, ast::DeclSema& sem, u16 flags, bool fn,
                            bool definition, bool has_init) {
    sem.flags = u16(flags | sema::decl_flag::specialization);
    std::vector<ast::TemplateArg> given = d.template_args.value_or(std::vector<ast::TemplateArg>{});
    if (!defaults_allowed(d.params, false, d.name)) return false;  // [dcl.fct.default]/4: no explicit specialization's
    std::vector<ast::ParamSema> params;
    for (const parse::Param& p : d.params) params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
    sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    // `template<> R X<args>::f(…)`: a member of a class template specialization, defined here
    // instead of instantiated ([temp.expl.spec]/1). Members of the template itself are not
    // supported yet.
    if (d.qualified) {
      const bool of_specialization = d.scope.type && s_.types().is_class(*d.scope.type) && !d.template_args &&
                                     s_.world().entities.rec(s_.types().class_of(*d.scope.type)).templ != EntityId::none;
      if (!of_specialization) {
        s_.diag(d.qualifier_at, Diag::not_supported, Tk::kw_template);
        return false;
      }
      // An explicitly specialized class's members are its own, defined without `template<>`
      // ([temp.expl.spec]).
      if (p_->explicitly_specialized(s_.types().class_of(*d.scope.type), k_)) {
        s_.diag(d.qualifier_at, Diag::member_of_explicit_specialization,
                s_.world().entities.rec(s_.types().class_of(*d.scope.type)).name);
        return false;
      }
      if (!define_member(spec, d, sem, fn, definition, has_init)) return false;
      s_.current_class = s_.world().entities.rec(sem.entity).scope;
      return true;
    }
    if (fn) {
      sema::Templates t = s_.templates();
      std::vector<std::pair<ast::DeclSema, std::vector<ast::TemplateArg>>> found;
      for (DeclId p : q_.visible_decls(d.name))
        if (auto ds = p_->decl_sema(p); ds && ds->kind == ast::DeclKind::function && (ds->flags & sema::decl_flag::template_) &&
                                        std::ranges::none_of(found, [&](const auto& f) { return f.first.entity == ds->entity; }))
          if (auto a = t.deduce_declaration(*ds, sem.type, given)) found.push_back({*ds, std::move(*a)});
      if (found.empty()) {
        s_.diag(d.name_at, Diag::no_template_matches, d.name);
        return false;
      }
      std::size_t n = s_.types().rec(sem.type).params.size();
      auto best = std::ranges::find_if(found, [&](const auto& f) {
        return std::ranges::all_of(found, [&](const auto& g) { return &f == &g || t.more_specialized(f.first, g.first, n); });
      });
      if (best == found.end()) {
        s_.diag(d.name_at, Diag::ambiguous_specialization, d.name);
        return false;
      }
      sem.entity = s_.specialization(best->first, best->second, sem.type);
    } else {
      auto tmpl = s_.variable_template(d.name);
      if (!tmpl || !d.template_args) {
        s_.diag(d.name_at, Diag::not_a_template, d.name);
        return false;
      }
      auto full = s_.template_arguments(*tmpl, given, d.template_args_at);
      if (!full) return false;
      sem.entity = s_.variable_specialization(tmpl->entity, *full);
    }
    // An earlier explicit specialization's definition; an instantiation's is diagnosed at the end.
    if (auto prior = p_->declaration(sem.entity, k_);
        prior && definition && (prior->flags & sema::decl_flag::definition) && (prior->flags & sema::decl_flag::specialization)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    p_->specialization_named_at(sem.entity, d.name_at);
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

  std::optional<ConstId> constant_initializer(Ex e) {
    return sema::Evaluator(s_, q_).evaluate_initializer(e).value;
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
  std::map<u32, std::vector<NodeId>> own_defaults_;  // by function: its default arguments in this item
  std::vector<Ex> declarators_;
  NodeId tree_ = NodeId::none;  // the root when it is not a DeclGroup (a static_assert)
  std::optional<Program::ClassInstantiation> inst_;
  std::optional<Program::VarInstantiation> var_;
  // Analysing a member item again for one static data member's value (`Program::member_value`):
  // the ordinal of the declarator whose initializer is instantiated.
  std::optional<u32> member_value_;
  // The member declaration being analysed is a member template's ([temp.mem]).
  bool member_template_ = false;
  // The declaration has two template-heads: a member template of a class template's definition.
  bool two_heads_ = false;
  bool single_linkage_spec_ = false;  // `extern "C" declaration` without braces
  std::optional<QualType> auto_replaced_;  // what the declarators' placeholder deduced
  bool template_ = false;             // the declaration is a template-declaration ([temp.pre])
  bool explicit_ = false;             // an explicit specialization ([temp.expl.spec])
  ConstId head_requires_ = ConstId::none;      // the template-head's requires-clause, in normal form
  ConstId trailing_requires_ = ConstId::none;  // the declarator's
  sema::Fp128 constraints_fp_{};               // their tokens' fingerprint, for identity
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

// The members of the class item `item` from its member items' records, read by `sema_of`: the
// class's own, or a class template specialization's (#88).
Program::ClassContent Program::class_content(u32 item, const std::function<const ast::ItemSema&(u32)>& sema_of) {
  ClassContent out;
  std::vector<ast::MemberRec>& members = out.members;
  const items::Item& it = items_->items[item];
  std::vector<sema::FieldRec>& fields = out.fields;
  std::vector<sema::MemberDeclRec>& statics = out.statics;
  std::vector<sema::NestedType>& nested = out.nested;
  // [class.access.spec]: members of a class defined with `class` are private by default. The
  // class-key follows any template heads: past each `template <…>`.
  u32 key = it.begin;
  while (key < it.brace_begin && r_->tokens[key].kind == lex::Tk::kw_template) {
    int depth = 0;
    for (++key; key < it.brace_begin; ++key) {
      lex::Tk k = r_->tokens[key].kind;
      if (k == lex::Tk::less) ++depth;
      else if (k == lex::Tk::greater && --depth == 0) break;
      else if (k == lex::Tk::greatergreater && (depth -= 2) <= 0) break;
    }
    ++key;
  }
  ast::Access access = r_->tokens[key].kind == lex::Tk::kw_class ? ast::Access::private_ : ast::Access::public_;
  for (u32 m : it.members) {
    const items::Item& mi = items_->items[m];
    if (mi.kind == ItemKind::access_spec) {
      lex::Tk k = r_->tokens[mi.begin].kind;
      access = k == lex::Tk::kw_public ? ast::Access::public_ : k == lex::Tk::kw_protected ? ast::Access::protected_
                                                                                          : ast::Access::private_;
      continue;
    }
    const ast::ItemSema& ms = sema_of(m);
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
      // A friend declaration names a function or class that is not a member ([class.friend]/1).
      if (d.flags & sema::decl_flag::friend_) {
        if (std::ranges::find(out.friends, d.entity) == out.friends.end()) out.friends.push_back(d.entity);
        if (ordinary_friend_items_.contains(m))
          out.adl_friends.push_back({{id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.entity, ast::Access::none});
        continue;
      }
      members.push_back({{id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.entity, access});
      if (d.kind == ast::DeclKind::data_member)
        fields.push_back({w_->entities.rec(d.entity).name, d.type, d.entity, inits.contains(i) ? inits[i] : ConstId::none,
                        dmis_.contains(std::to_underlying(d.entity)), d.bit_width});
      else if (d.kind == ast::DeclKind::enum_ || d.kind == ast::DeclKind::type_alias)
        nested.push_back({w_->entities.rec(d.entity).name, d.type, d.entity});
      else if (d.kind == ast::DeclKind::function || d.kind == ast::DeclKind::variable) {
        member_decls_[std::to_underlying(d.entity)] = {id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)};
        std::vector<NodeId> defaults;
        for (const ast::ParamSema& p : w_->ins.seqs.elements(d.params)) defaults.push_back(p.default_arg);
        if (std::ranges::all_of(defaults, [](NodeId n) { return n == NodeId::none; })) defaults.clear();
        statics.push_back({w_->entities.rec(d.entity).name, d.entity, d.type, d.kind == ast::DeclKind::function,
                           (d.flags & sema::decl_flag::static_) != 0, (d.flags & sema::decl_flag::constructor) != 0,
                           (d.flags & sema::decl_flag::destructor) != 0, (d.flags & sema::decl_flag::deleted) != 0,
                           (d.flags & sema::decl_flag::template_) != 0, {}, std::move(defaults)});
        statics.back().is_virtual = d.is_virtual;
        if (w_->ins.names.spelling(statics.back().name) == "operator=" && defaulted_member_definition(m, d))
          statics.back().explicit_defaulted_assignment = true;
      }
    }
  }
  return out;
}

const Result<ast::ClassDef>& Program::class_def(u32 item) {
  if (class_cells_[item]) return *class_cells_[item];
  u32 key = class_def_key(item);  // apart from item_sema(item), which may be running and ask for it
  contract_assert(std::ranges::find(running_, key) == running_.end());
  running_.push_back(key);
  Result<ast::ClassDef> out;
  ast::ClassDef def{};
  def.injected_class_name = EntityId::none;
  std::optional<EntityId> cls = class_entity(item);
  // Its head first: the base-clause (`set_bases`).
  if (std::ranges::find(running_, item) == running_.end()) item_sema(item);
  ClassContent c = class_content(item, [&](u32 m) -> const ast::ItemSema& { return item_sema(m).value; });
  std::vector<ast::MemberRec>& members = c.members;
  std::vector<sema::FieldRec>& fields = c.fields;
  std::vector<sema::MemberDeclRec>& statics = c.statics;
  std::vector<sema::NestedType>& nested = c.nested;
  if (cls && *cls != EntityId::none) {
    // Every unit that defines the class builds its own record (#115), and synthesizes the special
    // members its own object needs.
    sema::ClassRec rec;
    rec.union_ = class_rec_is_union(item);
    rec.fields = std::move(fields);
    rec.members = std::move(statics);
    rec.types = std::move(nested);
    if (auto b = class_bases_.find(std::to_underlying(*cls)); b != class_bases_.end()) rec.bases = b->second;
    for (const ast::MemberRec& m : members) rec.access.push_back({m.entity, m.access});
    rec.friends = std::move(c.friends);
    rec.adl_friends = c.adl_friends;
    special_members(item, *cls, rec);
    rec.defined = true;
    // Default arguments are checked with the full member set, before function bodies are read.
    classes_.publish(*cls, std::move(rec), [&](sema::ClassRec& fresh) {
      bind_member_defaults(*cls, fresh, out);
    });
  }
  ast::Builder b(w_->ins, &arena_);
  def.bases = b.seq(std::span<const ast::BaseSpec>{});
  def.members = b.seq(std::span<const ast::MemberRec>(members));
  def.friends = b.seq(std::span<const ast::MemberRec>(c.adl_friends));
  def.synthesized = b.seq(std::span<const ast::DeclSema>{});
  def.nested = b.seq(std::span<const EntityId>{});
  out.value = b.commit(def);
  running_.pop_back();
  class_cells_[item] = std::move(out);
  return *class_cells_[item];
}

void Program::bind_member_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out) {
  ast::Builder b(w_->ins, &arena_);
  for (sema::MemberDeclRec& member : rec.members) {
    auto tokens = member_default_tokens_.extract(member.entity);
    if (tokens.empty()) continue;
    DeclId decl = member_decls_.at(std::to_underlying(member.entity));
    u32 item = index(decl.item);
    u32 ordinal = ast::number_of(decl.ord);
    const auto& it = items_->items[item];
    const ast::ItemSema& cell = item_cells_[item]->value;
    auto params = w_->ins.seqs.elements(w_->ins.seqs.at(cell.decls, ordinal).params);
    AtItem q(*this, item, false);
    q.completing_class = cls;
    member.defaults.resize(params.size(), NodeId::none);
    for (std::size_t i = 0; i < params.size(); ++i) {
      if (!tokens.mapped()[i]) continue;
      auto [begin, end] = *tokens.mapped()[i];
      sema::Sema s(*w_, b, q, out.diags, decl.item, 0);
      s.current_class = cls;
      s.push_scope();
      for (std::size_t j = 0; j <= i; ++j)
        s.declare_local(params[j].name, params[j].type,
                        sema::local_flag::parameter | sema::local_flag::default_argument_parameter, 0, false);
      Parser p(*r_, it.begin, end, s);
      p.set_questions(q);
      p.seek(begin);
      auto previous_diags = out.diags.size();
      NodeId arg;
      try {
        arg = p.default_argument(params[i].type);
      } catch (const TooDeep&) {
        continue;  // The parser already diagnosed excessive nesting.
      }
      if (arg == NodeId::none || !p.at_end()) {
        if (out.diags.size() == previous_diags) {
          if (p.at_end()) s.diag(p.pos(), Diag::expected_expression);
          else s.diag(p.pos(), Diag::unexpected_token, p.kind());
        }
        continue;
      }
      member.defaults[i] = b.commit(ast::Child{arg, 0}).id;
      out.demands.insert(out.demands.end(), s.demands.begin(), s.demands.end());
    }
  }
}

// ---------------------------------------------------------------- constraint satisfaction

// An atomic constraint's expression analysed for its satisfaction ([temp.constr.atomic]/3): its
// tokens read again in the item that contains them, with the atom's parameter mapping bound, its
// diagnostics in a sink of its own. Fable's decision on #88 step 6: an error there means the
// substitution failed, so the atom is not satisfied; a compiler limitation is a hard error, never
// an unsatisfied constraint; a well-formed result that is not a constant of type bool is a hard
// error. What it instantiates keeps its own diagnostics in its own cell.
class AtomAnalysis {
public:
  AtomAnalysis(Program& p, const ast::VAtom& a, u32 point)
      : p_(&p), k_(p.index(a.item)), it_(&p.items().items[k_]), b_(p.world().ins, &p.arena_), q_(p, k_, false),
        s_(p.world(), b_, q_, sink_, a.item, a.begin), ps_(p.region(), it_->begin + a.begin, it_->begin + a.end, s_), atom_(a) {
    ps_.set_questions(q_);
    q_.at_point = point;
    q_.instantiation_calls = p.world().ins.seqs.elements(a.calls);
  }

  // Satisfied or not; hard errors go to `hard`.
  bool run(std::vector<engine::Diagnostic>& hard) {
    auto& ins = p_->world().ins;
    for (auto& head : p_->enclosing_heads(k_)) s_.template_scopes.push_back(std::move(head));
    const ast::ItemSema& is = p_->item_sema(k_).value;
    if (ins.seqs.size(is.decls) > 0) {
      ast::DeclSema d = ins.seqs.at(is.decls, 0);
      if (ins.seqs.size(d.template_params) > 0) s_.template_scopes.push_back(ins.seqs.elements(d.template_params));
    }
    sema::Templates t = s_.templates();
    s_.template_args.levels.resize(atom_.depth + 1);
    for (ConstId e : ins.seqs.elements(atom_.mapping))
      s_.template_args.levels[atom_.depth].push_back(e == ConstId::none ? std::nullopt : std::optional(t.mapping_arg(e)));
    // The function's parameters, of their substituted types, for an atom that names them.
    if (atom_.fn_params && ins.seqs.size(is.decls) > 0) s_.push_scope();
    if (atom_.fn_params && ins.seqs.size(is.decls) > 0)
      for (const ast::ParamSema& prm : ins.seqs.elements(ins.seqs.at(is.decls, 0).params)) {
        auto ty = t.substitute(prm.type, s_.template_args);
        s_.declare_local(prm.name, ty ? *ty : prm.type, sema::local_flag::parameter, 0, false);
      }
    // A value argument without a type of its own has its parameter's, substituted
    // ([temp.arg.nontype]).
    for (const auto& head : s_.template_scopes)
      if (!head.empty() && head[0].depth == atom_.depth)
        for (std::size_t i = 0; i < head.size() && i < s_.template_args.levels[atom_.depth].size(); ++i)
          if (auto& a = s_.template_args.levels[atom_.depth][i]; a && a->kind == ast::TemplateArgKind::value && a->type == QualType{})
            if (auto ty = t.substitute(head[i].type, s_.template_args); ty && !s_.types().is_dependent(*ty)) a->type = *ty;
    Ex e = ps_.expression();
    bool failed = !e.ok() || !ps_.at_end();
    for (const engine::Diagnostic& d : sink_) {
      if (sema::survives_substitution(d.id)) hard.push_back(d);
      failed = true;
    }
    if (failed) return false;
    QualType ty = sema::Types::unqualified(s_.types().canonical(s_.rvalue(e).type()));
    if (!s_.types().is(ty, sema::Builtin::bool_)) {
      s_.diag(0, Diag::constraint_not_bool, ty);
      hard.insert(hard.end(), sink_.begin(), sink_.end());
      return false;
    }
    sema::Evaluated v = sema::Evaluator(s_, q_).evaluate(s_.rvalue(e));
    if (!v.value) {
      s_.diag(0, Diag::constraint_not_constant, v.reason);
      hard.push_back(sink_.back());
      return false;
    }
    return p_->world().values.as_signed(*v.value) != 0;
  }

private:
  Program* p_;
  u32 k_;
  const items::Item* it_;
  std::vector<engine::Diagnostic> sink_;
  ast::Builder b_;
  AtItem q_;
  sema::Sema s_;
  Parser ps_;
  ast::VAtom atom_;
};

std::optional<bool> Program::atom_satisfied(ConstId c, u32 point) {
  const u32 key = std::to_underlying(c);
  if (auto it = atoms_.find(key); it != atoms_.end()) return it->second;
  sema::Templates t = templates();
  if (t.constraint_dependent(c)) return std::nullopt;
  // An atom of the item being analysed is checked once its analysis is done: its head is not
  // known before ([temp.constr.decl] constraints are checked where they are used).
  if (std::ranges::contains(running_, index(w_->ins.values.get<ast::VAtom>(c).item))) return std::nullopt;
  atoms_[key] = true;  // its own evaluation asks for itself only through a cycle, which [temp.constr.atomic] does not satisfy
  std::vector<engine::Diagnostic> hard;
  bool r = AtomAnalysis(*this, w_->ins.values.get<ast::VAtom>(c), point).run(hard);
  if (!hard.empty()) atom_errors_.insert(key);
  // A limit met in the atom's tokens may have been met already where they were first analysed.
  const std::vector<engine::Diagnostic>& seen = item_sema(index(w_->ins.values.get<ast::VAtom>(c).item)).diags;
  for (engine::Diagnostic& d : hard)
    if (!std::ranges::contains(end_diags_, d) && !std::ranges::contains(seen, d)) end_diags_.push_back(std::move(d));
  atoms_[key] = r;
  return r;
}

void Program::member_value(EntityId e) {
  auto it = lazy_members_.find(std::to_underlying(e));
  if (it == lazy_members_.end() || it->second.state != LazyMember::State::pending) return;
  it->second.state = LazyMember::State::running;  // a use in its own initializer finds no value
  LazyMember m = it->second;
  // [temp.inst]/16: initializers instantiated through each other are limited as classes are; past
  // the limit, reported once, nothing more is instantiated.
  if (depth_reported_ || members_running_ >= instantiation_limit) {
    if (!depth_reported_)
      end_diags_.push_back({engine::Loc{std::to_underlying(id(m.item)), 0}, sema::diag_id(sema::Diag::instantiation_depth),
                            {engine::diag_arg(instantiation_limit)}});
    depth_reported_ = true;
    lazy_members_.at(std::to_underlying(e)).state = LazyMember::State::done;
    return;
  }
  ++members_running_;
  Result<ast::ItemSema> r = ItemAnalysis(*this, m.item, m.inst, m.ordinal).run();
  --members_running_;
  // What the template's definition diagnosed it says once, there.
  if (item_sema(m.item).diags.empty())
    for (engine::Diagnostic& d : r.diags)
      if (!std::ranges::contains(end_diags_, d)) end_diags_.push_back(std::move(d));
  lazy_members_.at(std::to_underlying(e)).state = LazyMember::State::done;
}

// ---------------------------------------------------------------- analyze_body

class BodyAnalysis {
public:
  // `inst` analyses the body as an instantiation of the function template the item defines
  // (ast.md §6.4 as amended by #88): with the specialization's declaration, and the template
  // arguments bound to the template's parameters.
  // `generic` analyses only a generic lambda's body in the item's, as a specialization of its
  // function call operator (docs/design/closures.md, decision 11).
  BodyAnalysis(Program& p, u32 item, std::optional<Program::Instantiation> inst = std::nullopt,
               std::optional<Program::GenericInst> generic = std::nullopt)
      : generic_(std::move(generic)), p_(&p), k_(item), it_(&p.items().items[item]), b_(p.world().ins, &p.arena_),
        q_(inst ? std::unique_ptr<sema::Questions>(std::make_unique<AtInstantiation>(p, item))
                : std::unique_ptr<sema::Questions>(std::make_unique<AtItem>(p, item, true))),
        s_(p.world(), b_, *q_, out_.diags, p.id(item), it_->body_begin - it_->begin),
        ps_(p.region(), it_->body_begin, it_->body_end, s_), inst_(std::move(inst)) {
    ps_.set_questions(*q_);
    ps_.body = true;
    at_ = inst_ ? &static_cast<AtInstantiation&>(*q_).pattern_context() : &static_cast<AtItem&>(*q_);
  }

  Result<ast::BodySema> run() {
    try {
      return analyse();
    } catch (const TooDeep&) {
      // Diagnosed: the body fails, its scopes unwound with the parse; it holds nothing.
      std::vector<sema::Expansion> none;
      out_.value = b_.commit(ast::BodySema{NodeId::none, b_.seq(std::span<const ast::LocalRec>{}), expansion_list(b_, none),
                                           b_.seq(std::span<const u32>{}), QualType{}, 0,
                                           b_.seq(std::span<const ast::ClosureDef>{})});
      out_.demands.clear();
      return std::move(out_);
    }
  }

  Result<ast::BodySema> analyse() {
    const ast::ItemSema& is = p_->item_sema(k_).value;
    std::optional<ast::DeclSema> f;
    if (p_->world().ins.seqs.size(is.decls) > 0) f = p_->world().ins.seqs.at(is.decls, 0);
    std::optional<ast::DeclSema> pattern = f;
    if (inst_) f = inst_->decl;
    Ex root;
    if (f && f->entity != EntityId::none && f->kind == ast::DeclKind::function &&
        !(f->flags & sema::decl_flag::deleted) && !p_->defaulted_member_definition(k_, *f) &&
        s_.require_supported_virtual_signature(s_.types().rec(f->type), 0)) {
      const auto signature = s_.types().rec(f->type);
      s_.set_return_type(signature.inner);
      // A consteval function's body is an immediate function context ([expr.const]).
      if (f->flags & sema::decl_flag::consteval_) s_.immediate_context = 1;
      // A template's body is in the scope of its template-head ([basic.scope.temp]); an
      // instantiation's, with the arguments bound.
      for (auto& head : p_->enclosing_heads(k_)) s_.template_scopes.push_back(std::move(head));
      if (pattern && p_->world().ins.seqs.size(pattern->template_params) > 0)
        s_.template_scopes.push_back(p_->world().ins.seqs.elements(pattern->template_params));
      if (inst_) s_.template_args = inst_->args;
      // A member function's body sees its class's members ([basic.scope.class]) and `this`; a
      // body is looked up in the namespace of what it defines ([basic.lookup.unqual]/9).
      EntityId scope = p_->world().entities.rec(f->entity).scope;
      s_.current_function = f->entity;
      s_.checking_virtual_body = f->is_virtual;
      s_.variadic_function_body = signature.variadic;
      // Its closures' members are defined wherever it is: an inline function, a member defined in
      // its class, or a specialization ([dcl.inline], [temp.inst]).
      s_.function_inline = (f->flags & sema::decl_flag::inline_) || inst_ ||
                           (it_->parent != items::none && p_->items().items[it_->parent].kind == ItemKind::class_def);
      if (f->flags & sema::decl_flag::member) {
        s_.current_class = scope;
        s_.this_type = p_->this_type(*f);
        scope = p_->world().entities.rec(scope).scope;
      } else if (f->flags & sema::decl_flag::friend_) {
        // [class.mem.general]: the body is a complete-class context even though the
        // function belongs to the namespace and has no implicit object parameter.
        s_.current_class = p_->class_entity(it_->parent);
        p_->is_complete(s_.current_class, k_, true);
        at_->completing_class = s_.current_class;
      }
      if ((f->flags & sema::decl_flag::lang_c) && !(f->flags & sema::decl_flag::member)) {
        if (auto target = p_->qualified_c_functions_.find({k_, 0}); target != p_->qualified_c_functions_.end())
          at_->from = target->second;
      } else if (p_->is_namespace(scope)) {
        at_->from = p_->namespace_path(scope);
      }
      if (generic_) return generic_body();
      p_->running_bodies_.push_back({f->entity, k_, inst_});
      struct Pop {
        Program* p;
        ~Pop() { p->running_bodies_.pop_back(); }
      } pop{p_};
      // The parameters are the first locals, in their own scope ([basic.scope.param]).
      s_.push_scope();
      s_.guard_scope();
      // In an instantiation, a function parameter pack of the definition is its elements' names
      // ([temp.variadic]/8), however many there are.
      if (inst_)
        for (const ast::ParamSema& prm : p_->world().ins.seqs.elements(pattern->params))
          if (s_.types().kind(prm.type) == sema::TypeKind::pack_expansion) s_.declare_param_pack(prm.name);
      for (const ast::ParamSema& prm : p_->world().ins.seqs.elements(f->params)) {
        if (prm.flags & sema::param_flag::pack_element) s_.declare_pack_element(prm.name, prm.type);
        else s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, 0, false);
      }
      bool ctor = f->flags & sema::decl_flag::constructor;
      std::map<u32, Ex> inits;
      if (ctor && ps_.is(lex::Tk::colon)) inits = mem_initializers();
      if (!ps_.is(lex::Tk::l_brace)) {
        // `= default`, `= delete` or a function-try-block.
        s_.diag(ps_.pos(), sema::Diag::not_supported, ps_.kind());
      } else {
        root = ps_.compound_statement();
        if (!ps_.at_end()) s_.diag(ps_.pos(), sema::Diag::unexpected_token, ps_.kind());
        s_.check_labels();  // labels have function scope ([stmt.label]/2)
        // No return statement deduces the placeholder as `return;` would ([dcl.type.auto.deduct]/2).
        if (root.ok() && s_.deduces_return() && !s_.return_deduced() && !s_.return_seen() && !templated_pattern())
          if (!s_.deduce_return(nullptr, ps_.pos() > 0 ? ps_.pos() - 1 : 0)) root = {};
      }
      s_.pop_scope();
      if (root.ok() && ctor) root = constructor_body(std::move(inits), root);
      if (root.ok() && (f->flags & sema::decl_flag::destructor)) root = destructor_body(s_, s_.current_class, root);
    }
    std::vector<ast::LocalRec> locals(s_.locals().begin(), s_.locals().end());
    // A template's definition demands nothing; its instantiations do. What it keeps for them is
    // which calls were dependent ([temp.dep.candidate]) and its pack expansions (ast.md §6.4).
    bool templated = (pattern && p_->world().ins.seqs.size(pattern->template_params) > 0) || !p_->enclosing_heads(k_).empty();
    std::vector<sema::Expansion> none;
    std::vector<u32> no_calls;
    const bool keeps = templated && !inst_;
    // A body with generic lambdas keeps its expansions too: their specializations expand them.
    const bool keep_expansions = keeps || s_.has_generic_lambdas();
    const bool deduced = f && s_.return_deduced() && !keeps;
    ast::BodySema v{root.id, b_.seq(std::span<const ast::LocalRec>(locals)), expansion_list(b_, keep_expansions ? s_.expansions : none),
                    b_.seq(std::span<const u32>(keeps ? s_.dependent_calls : no_calls)),
                    deduced ? s_.with_return(f->type, s_.return_type()) : s_.builtin(sema::Builtin::void_), u8(deduced),
                    b_.seq(std::span<const ast::ClosureDef>(s_.closures))};
    out_.value = b_.commit(v);
    s_.check_immediate_addresses(out_.value.tree, root.begin);
    if (!keeps) out_.demands = std::move(s_.demands);
    return std::move(out_);
  }

  // A generic lambda's body as its operator's specialization: the enclosing function's constants
  // it names are this context's locals; the closure's frame, the head's arguments and the
  // parameters come from `open_generic`. The tree's root is at the lambda's `{`.
  Result<ast::BodySema> generic_body() {
    const Program::GenericInst& gi = *generic_;
    p_->running_bodies_.push_back({gi.spec, k_, inst_});
    struct Pop {
      Program* p;
      ~Pop() { p->running_bodies_.pop_back(); }
    } pop{p_};
    s_.push_scope();
    for (const auto& [n, t] : gi.g.aliases) s_.local_type_alias(n, t, 0);
    // Innermost first: the first of a name is the one its definition saw.
    for (std::size_t i = std::min(gi.g.consts.size(), gi.g.const_values.size()); i-- > 0;) {
      u32 l = s_.declare_local(gi.g.consts[i].name, gi.g.consts[i].type, 0, 0, false);
      if (gi.g.const_values[i] != ast::ConstId::none) s_.set_local_value(l, gi.g.const_values[i]);
    }
    if (!gi.g.expansions.empty()) s_.expansions_override = &gi.g.expansions;
    s_.open_generic(gi.g, gi.spec, gi.spec_type, gi.args);
    ps_.seek(it_->body_begin + gi.g.body_begin);
    Ex body = ps_.compound_statement();
    auto r = s_.close_generic(body, ps_.pos());
    s_.pop_scope();
    Ex root;
    if (r && body.ok()) root = s_.node(ast::Compound{b_.list(std::vector<ast::Child>{sema::Sema::at(body, 0)})}, 0, body.end);
    std::vector<ast::LocalRec> locals = r ? r->locals : std::vector<ast::LocalRec>{};
    std::vector<sema::Expansion> none;
    out_.value = b_.commit(ast::BodySema{root.id, b_.seq(std::span<const ast::LocalRec>(locals)), expansion_list(b_, none),
                                         b_.seq(std::span<const u32>{}), r ? r->type : s_.builtin(sema::Builtin::void_), u8(r.has_value()),
                                         b_.seq(std::span<const ast::ClosureDef>(s_.closures))});
    s_.check_immediate_addresses(out_.value.tree, root.begin);
    out_.demands = std::move(s_.demands);
    return std::move(out_);
  }

  // Whether the body is a template's definition, not an instantiation of it.
  bool templated_pattern() {
    if (inst_) return false;
    const ast::ItemSema& is = p_->item_sema(k_).value;
    bool own = p_->world().ins.seqs.size(is.decls) > 0 && p_->world().ins.seqs.size(p_->world().ins.seqs.at(is.decls, 0).template_params) > 0;
    return own || !p_->enclosing_heads(k_).empty();
  }

  // A destructor's body ([class.dtor]/13): `body`, then its members destroyed, the last first,
  // then its direct bases, the last first (a `DestroyMember` of field `~0u`).
  static Ex destructor_body(sema::Sema& s, EntityId cls, Ex body) {
    const sema::ClassRec& rec = s.classes().rec(cls);
    std::vector<ast::Child> members;
    for (u32 i = u32(rec.fields.size()); i-- > 0;) {
      const sema::FieldRec& f = rec.fields[i];
      EntityId d = s.classes().destructor_of(f.type);
      if (d != EntityId::none) members.push_back({s.node(ast::DestroyMember{i, f.type, d}, body.end, body.end).id, 0});
    }
    for (std::size_t i = rec.bases.size(); i-- > 0;) {
      QualType bt = s.types().class_type(rec.bases[i].cls);
      EntityId d = s.classes().destructor_of(bt);
      if (d != EntityId::none) members.push_back({s.node(ast::DestroyMember{~0u, bt, d}, body.end, body.end).id, 0});
    }
    return s.node(ast::Destructor{sema::Sema::at(body, body.begin), s.builder().list(members)}, body.begin, body.end);
  }

private:
  // mem-initializer-list ([class.base.init]): `: m(args), n{…}`, each member's initializer by
  // its position among the class's members.
  std::map<u32, Ex> mem_initializers() {
    std::map<u32, Ex> r;
    const sema::ClassRec& rec = p_->classes_.rec(s_.current_class);
    QualType ct = s_.types().class_type(s_.current_class);
    ps_.bump();
    do {
      if (!ps_.is(Tk::identifier)) {
        s_.diag(ps_.pos(), Diag::expected_unqualified_id);
        return r;
      }
      u32 at = ps_.pos();
      NameId n = ps_.name_of(ps_.tok());
      ps_.bump();
      u32 field = 0;
      while (field < rec.fields.size() && rec.fields[field].name != n) ++field;
      // [class.base.init]/2: else a direct base, named by a type-name.
      if (field == rec.fields.size()) {
        auto bt = s_.class_name(n, false);
        auto b = bt && s_.types().is_class(*bt)
                     ? std::ranges::find(rec.bases, s_.types().class_of(*bt), &sema::BaseRec::cls)
                     : rec.bases.end();
        if (b != rec.bases.end()) field = u32(rec.fields.size() + (b - rec.bases.begin()));
      }
      if (field == rec.fields.size() + rec.bases.size() || (field >= rec.fields.size() && rec.bases.empty())) {
        s_.diag(at, Diag::no_member, n, ct);  // delegating constructors are not supported yet
        ps_.skip_param();
        continue;
      }
      QualType ft = field < rec.fields.size() ? rec.fields[field].type : s_.types().class_type(rec.bases[field - rec.fields.size()].cls);
      Ex init;
      if (ps_.is(Tk::l_brace)) {
        init = s_.list_init(ft, *ps_.braced_init_list());
      } else {
        u32 open = ps_.pos();
        if (!ps_.expect(Tk::l_paren)) return r;
        std::vector<Ex> args;
        if (!ps_.is(Tk::r_paren))
          do args.push_back(ps_.assignment_expression());
          while (ps_.accept(Tk::comma));
        ps_.expect(Tk::r_paren);
        if (s_.types().is_class(ft)) {
          init = s_.has_user_ctors(ft) || args.size() != 1 ? s_.construct(ft, std::move(args), open, ps_.pos())
                                                            : s_.convert(args[0], ft, open);
        } else if (args.empty()) {
          init = s_.value_init(ft, open, ps_.pos());
        } else if (args.size() > 1) {
          s_.diag(open, Diag::too_many_initializers, ft);
        } else {
          init = s_.convert(args[0], ft, open);  // a reference binds ([dcl.init.ref])
        }
      }
      if (r.contains(field)) s_.diag(at, Diag::redeclared_member, n);  // [class.base.init]/3: one initializer each
      r[field] = init;
    } while (ps_.accept(Tk::comma));
    return r;
  }

  // A constructor's body ([class.base.init]/13): its direct bases initialized in declaration
  // order, from their mem-initializers (keyed after the members) or default-initialized (a
  // `MemberInit` of field `~0u`); then every member in declaration order, from its
  // mem-initializer, its default member initializer, or default-initialized; then the compound
  // statement.
  Ex constructor_body(std::map<u32, Ex> inits, Ex body) {
    const sema::ClassRec& rec = p_->classes_.rec(s_.current_class);
    std::vector<ast::Child> stmts;
    for (std::size_t i = 0; i < rec.bases.size(); ++i) {
      QualType bt = s_.types().class_type(rec.bases[i].cls);
      Ex init;
      if (auto it = inits.find(u32(rec.fields.size() + i)); it != inits.end()) init = it->second;
      else init = s_.default_init(bt, 0, 0);
      if (!init.ok()) continue;
      stmts.push_back({s_.node(ast::MemberInit{~0u, bt, {init.id, 0}}, 0, 0).id, 0});
    }
    for (u32 i = 0; i < rec.fields.size(); ++i) {
      const sema::FieldRec& f = rec.fields[i];
      Ex init;
      if (auto it = inits.find(i); it != inits.end()) init = it->second;
      else if (f.init != ConstId::none) init = s_.literal(sema::Types::unqualified(f.type), f.init, ast::ValueCategory::prvalue, 0, 0);
      else if (auto d = p_->dmi(f.entity)) init = member_initializer(f, *d);
      else if (s_.types().is_reference(f.type)) s_.diag(0, Diag::reference_uninitialized);  // [class.base.init]/10
      else init = s_.default_init(f.type, 0, 0);
      if (!init.ok()) continue;
      stmts.push_back({s_.node(ast::MemberInit{i, f.type, {init.id, 0}}, 0, 0).id, 0});
    }
    stmts.push_back(sema::Sema::at(body, 0));
    return s_.node(ast::Compound{b_.list(stmts)}, 0, body.end);
  }

  // A default member initializer, analysed in this constructor (a complete-class context with
  // `this`, [class.mem.general]); the function's parameters are not in its scope.
  Ex member_initializer(const sema::FieldRec& f, const Program::Dmi& d) {
    auto saved = s_.take_scopes();
    auto evaluate = s_.constant_of;
    Parser sub(p_->region(), d.begin, d.end, s_);
    sub.set_questions(*q_);
    Ex init = d.braced ? s_.list_init(f.type, *sub.braced_init_list()) : s_.convert(sub.assignment_expression(), f.type, 0);
    s_.constant_of = evaluate;
    s_.put_scopes(std::move(saved));
    return init;
  }

  std::optional<Program::GenericInst> generic_;
  Program* p_;
  u32 k_;
  const items::Item* it_;
  Result<ast::BodySema> out_;
  ast::Builder b_;
  std::unique_ptr<sema::Questions> q_;
  sema::Sema s_;
  Parser ps_;
  std::optional<Program::Instantiation> inst_;
  AtItem* at_ = nullptr;  // where unqualified lookup starts: the item's own context

  friend class Program;
};

// ---------------------------------------------------------------- special members ([special])

// [class.copy.ctor]: the supported defaulted copy forms initialize each member from the
// corresponding glvalue. Classify those operations before permitting a trivial representation
// copy, including when the defaulted copy is selected by another class's implicit constructor.
void Program::defaulted_copy(EntityId cls, sema::ClassRec& rec, std::span<const QualType> subobjects, bool implicit) {
  using enum sema::CopyState;
  auto& types = w_->types;
  for (const auto& m : rec.members)
    if (m.is_ctor)
      if (const auto d = member_decl(m.entity); d && defaulted_body(index(d->item)))
        rec.defaulted_copy_declarations.push_back(m.entity);
  const bool written = !rec.defaulted_copy_declarations.empty();
  const auto declared = written
      ? std::ranges::find(rec.members, rec.defaulted_copy_declarations.front(), &sema::MemberDeclRec::entity)
      : rec.members.end();
  auto element = [&](QualType t) {
    t = types.canonical(t);
    while (types.is_array(t)) t = types.add_cv(types.rec(t).inner, sema::Types::cv(t));
    return t;
  };
  auto depends_on_defaulting = [&](QualType t) { return classes_.defaulted_copy_of(t) != ordinary; };
  if (!written && (!implicit || std::ranges::none_of(subobjects, depends_on_defaulting))) return;

  auto copy_parameter = [&](const sema::MemberDeclRec& m, EntityId owner) -> std::optional<QualType> {
    if (!m.is_ctor || m.is_template) return std::nullopt;
    const auto f = types.rec(m.type);
    if (f.params.empty()) return std::nullopt;
    const auto r = types.rec(f.params.front());
    if (r.kind != sema::TypeKind::reference || r.rvalue_ref ||
        sema::Types::unqualified(types.canonical(r.inner)) != types.class_type(owner))
      return std::nullopt;
    return types.canonical(r.inner);
  };
  const bool written_const = written && sema::Types::is_const(*copy_parameter(*declared, cls));
  const auto outside_scope = [&](EntityId type, const sema::ClassRec& shape) {
    const auto& entity = w_->entities.rec(type);
    return shape.union_ || shape.closure || shape.layout_requires_virtual_support || !shape.bases.empty() ||
           entity.templ != EntityId::none || entity.kind != sema::EntityKind::class_ || !is_namespace(entity.scope);
  };
  auto finalize = [&](sema::CopyState status, bool source_const) {
    rec.defaulted_copy = status;
    rec.defaulted_copy_const = source_const;
    rec.copy_deleted = status == deleted;
    if (written) {
      if (rec.copy_ctor == declared->entity) rec.copy_ctor = EntityId::none;
      if (status == deleted) declared->is_deleted = true;
    }
  };
  const auto constructor_template = [](const sema::MemberDeclRec& m) { return m.is_ctor && m.is_template; };
  if (outside_scope(cls, rec) || std::ranges::any_of(rec.members, constructor_template) ||
      (written && std::ranges::count_if(rec.members, [&](const auto& m) { return copy_parameter(m, cls).has_value(); }) != 1)) {
    finalize(unsupported, !written || written_const);
    return;
  }

  struct Subobject {
    sema::CopyState from_mutable;
    sema::CopyState from_const;
    bool takes_const = true;
  };
  const auto uniform = [](sema::CopyState result) { return Subobject{result, result}; };
  const auto combine = [](Subobject& result, Subobject member) {
    result.from_mutable = std::max(result.from_mutable, member.from_mutable);
    result.from_const = std::max(result.from_const, member.from_const);
    result.takes_const &= member.takes_const;
  };
  // Each enclosing class examined here is an ordinary namespace-scope class without bases.
  // A non-public subobject operation is accessible only through friendship in this subset.
  const auto inaccessible = [](const sema::ClassRec& leaf, EntityId e, EntityId owner, EntityId ctor) {
    const ast::Access access = leaf.access_of(e);
    return access != ast::Access::public_ && access != ast::Access::none &&
           !std::ranges::contains(leaf.friends, owner) &&
           (ctor == EntityId::none || !std::ranges::contains(leaf.friends, ctor));
  };
  std::map<EntityId, Subobject> implicit_fields;
  const auto inspect = [&](this const auto& self, QualType type, EntityId owner, EntityId ctor) -> Subobject {
    QualType t = element(type);
    const auto r = types.rec(t);
    if (r.kind == sema::TypeKind::reference) return uniform(r.rvalue_ref ? deleted : trivial);
    if (sema::Types::is_volatile(t)) return uniform(unsupported);
    switch (r.kind) {
      case sema::TypeKind::builtin: return uniform(w_->target[r.builtin].size ? trivial : unsupported);
      case sema::TypeKind::enum_: case sema::TypeKind::pointer: return uniform(trivial);
      case sema::TypeKind::class_: break;
      default: return uniform(unsupported);
    }
    const auto* leaf = classes_.find(r.entity);
    if (!leaf || !leaf->defined || outside_scope(r.entity, *leaf)) return uniform(unsupported);

    const sema::MemberDeclRec* copy = nullptr;
    bool takes_const = leaf->defaulted_copy_const;
    bool unusable_destructor = false;
    for (const auto& m : leaf->members) {
      if (constructor_template(m)) return uniform(unsupported);
      if (auto ref = copy_parameter(m, r.entity)) {
        // Only a single ordinary copy taking one class reference can be classified here.
        const auto f = types.rec(m.type);
        if (copy || f.params.size() != 1 || f.variadic || sema::Types::is_volatile(*ref)) return uniform(unsupported);
        copy = &m;
        takes_const = sema::Types::is_const(*ref);
      }
      if (m.is_dtor) unusable_destructor |= m.is_deleted || inaccessible(*leaf, m.entity, owner, ctor);
    }

    sema::CopyState operation;
    if (copy) {
      operation = nontrivial;
      if (std::ranges::contains(leaf->defaulted_copy_declarations, copy->entity)) operation = leaf->defaulted_copy;
      if (copy->is_deleted || inaccessible(*leaf, copy->entity, owner, ctor)) operation = deleted;
    } else if (leaf->defaulted_copy != ordinary) {
      operation = leaf->defaulted_copy;
    } else {
      // Legacy implicit-copy records do not prove triviality. Classify the implicit copy in
      // this leaf's context, including its own fields' access, volatile types and references.
      // The outcomes within the leaf are independent of who is copying the leaf. Start
      // unresolved so a malformed recursive record cannot be mistaken for a trivial copy.
      auto [known, first] = implicit_fields.try_emplace(r.entity, uniform(unsupported));
      if (first) {
        auto fields = uniform(trivial);
        for (const auto& field : leaf->fields)
          combine(fields, self(field.type, r.entity, EntityId::none));
        known->second = fields;
      }
      const auto& fields = known->second;
      takes_const = fields.takes_const;
      operation = takes_const ? fields.from_const : fields.from_mutable;
      if (leaf->copy_deleted) operation = deleted;
    }
    if (unusable_destructor) operation = deleted;

    const auto const_result = takes_const ? operation : deleted;
    return {sema::Types::is_const(t) ? const_result : operation, const_result, takes_const};
  };

  const EntityId ctor = written ? declared->entity : EntityId::none;
  auto members = uniform(trivial);
  for (QualType t : subobjects) combine(members, inspect(t, cls, ctor));
  const bool const_source = written ? written_const : members.takes_const;
  const auto result = const_source
      ? (members.takes_const ? members.from_const : deleted)
      : members.from_mutable;
  finalize(result, const_source);
}

std::optional<QualType> Program::assignment_parameter(const sema::MemberDeclRec& member, QualType owner) const {
  if (!member.is_function || member.is_static || member.is_template ||
      w_->ins.names.spelling(member.name) != "operator=")
    return std::nullopt;
  const auto& types = w_->types;
  const sema::TypeRec function = types.rec(member.type);
  if (function.params.size() != 1 || function.variadic) return std::nullopt;
  const QualType parameter = function.params.front();
  const QualType type = types.is_reference(parameter) ? types.referee(parameter) : parameter;
  if (sema::Types::unqualified(types.canonical(type)) != owner) return std::nullopt;
  return parameter;
}

void Program::special_members(u32 item, EntityId cls, sema::ClassRec& rec) {
  sema::Types& types = w_->types;
  QualType ct = types.class_type(cls);
  bool user_copy = false;
  bool user_move = false;
  bool user_copy_assignment = false;
  bool user_move_assignment = false;
  bool defaulted_assignment = false;
  EntityId user_dtor = EntityId::none;
  for (const sema::MemberDeclRec& m : rec.members) {
    defaulted_assignment |= m.explicit_defaulted_assignment;
    if (const auto parameter = assignment_parameter(m, ct)) {
      if (types.rec(*parameter).rvalue_ref) user_move_assignment = true;
      else user_copy_assignment = true;
    }
    if (m.is_virtual) rec.polymorphic = true;
    if (m.is_dtor) {
      user_dtor = m.entity;
      rec.virtual_destructor = m.is_virtual;
    }
    if (!m.is_ctor) continue;
    rec.user_ctors = true;
    if (m.is_template) continue;
    sema::TypeRec fr = types.rec(m.type);
    // [class.copy.ctor]/1–3: the first parameter a reference to the class, any others defaulted.
    std::size_t required = fr.params.size();
    if (auto deferred = member_default_tokens_.find(m.entity); deferred != member_default_tokens_.end()) {
      while (required > 1 && deferred->second[required - 1]) --required;
    } else {
      while (required > 1 && required - 1 < m.defaults.size() && m.defaults[required - 1] != NodeId::none) --required;
    }
    if (!fr.params.empty() && required == 1 && types.is_reference(fr.params[0]) &&
        types.canonical(sema::Types::unqualified(types.referee(fr.params[0]))) == types.canonical(ct)) {
      if (types.rec(fr.params[0]).rvalue_ref) {
        user_move = true;  // a move constructor, user-provided unless deleted (no `= default` yet)
        rec.move_usable = !m.is_deleted;
        rec.move_nontrivial = !m.is_deleted;
        continue;
      }
      user_copy = true;
      rec.copy_ctor = m.is_deleted ? EntityId::none : m.entity;
      rec.copy_deleted = m.is_deleted;
    }
  }
  ast::Builder b(w_->ins, &arena_);
  auto synthesize = [&](bool copy) {
    std::string cname(w_->ins.names.spelling(w_->entities.rec(cls).name));
    QualType void_ = types.builtin(sema::Builtin::void_);
    std::vector<QualType> ps;
    std::vector<ast::ParamSema> params;
    if (copy) {
      ps.push_back(types.lvalue_ref(types.add_cv(ct, sema::Cv::const_)));
      params.push_back({EntityId::none, ps[0], w_->ins.names.intern("other"), NodeId::none, 0});
    }
    QualType type = types.function(void_, ps, false, LangLinkage::cxx);
    ast::DeclSema d{};
    d.kind = ast::DeclKind::function;
    d.type = type;
    d.linkage = ast::Linkage::external;
    d.storage = ast::Storage::none;
    d.access = ast::Access::public_;
    d.flags = sema::decl_flag::member | sema::decl_flag::implicit | sema::decl_flag::definition | sema::decl_flag::inline_ |
              (copy ? sema::decl_flag::constructor : sema::decl_flag::destructor);
    d.attributes = b.seq(std::span<const NodeId>{});
    d.constraint = NodeId::none;
    d.constraints = ast::ConstId::none;
    d.contracts = NodeId::none;
    d.params = b.seq(std::span<const ast::ParamSema>(params));
    d.template_params = b.seq(std::span<const ast::TemplateParamSema>{});
    d.entity = w_->entities.entity({copy ? sema::EntityKind::constructor : sema::EntityKind::destructor,
                                    w_->ins.names.intern(copy ? cname : "~" + cname), type, LangLinkage::cxx,
                                    ast::Linkage::external, {}, cls});
    synth_.push_back({item, b.commit(d), copy ? SpecialMember::copy_constructor : SpecialMember::destructor});
    return synth_.back().decl.entity;
  };
  // The types of its subobjects: its direct bases', then its members' ([class.base.init]/13).
  std::vector<QualType> subobjects;
  for (const sema::BaseRec& b : rec.bases) subobjects.push_back(types.class_type(b.cls));
  for (const sema::FieldRec& f : rec.fields) subobjects.push_back(f.type);
  rec.layout_requires_virtual_support = rec.polymorphic || std::ranges::any_of(subobjects, [&](QualType t) {
    return classes_.layout_requires_virtual_support(t);
  });
  // [class.copy.ctor]/6: either user-declared move operation deletes the otherwise implicit copy.
  const bool deletes_implicit_copy = user_move || user_move_assignment;
  const bool implicit_copy = !user_copy && !deletes_implicit_copy;
  defaulted_copy(cls, rec, subobjects, implicit_copy);
  // [class.dtor]/8: the implicit destructor is not trivial when a base's or member's is not.
  if (user_dtor != EntityId::none) rec.destructor = user_dtor;
  else if (std::ranges::any_of(subobjects, [&](QualType t) { return classes_.destructor_of(t) != EntityId::none; }))
    rec.destructor = synthesize(false);
  // [class.copy.ctor]/10–11: the implicit copy constructor is deleted with a member's, and not
  // trivial when a member's is not.
  if (!user_copy && deletes_implicit_copy) rec.copy_deleted = true;
  if (implicit_copy && rec.defaulted_copy == sema::CopyState::ordinary) {
    bool nontrivial = false;
    for (QualType t : subobjects) {
      if (types.is_reference(t)) continue;
      auto [n, del] = classes_.copy_of(t);
      rec.copy_deleted = rec.copy_deleted || del;
      nontrivial = nontrivial || n;
    }
    if (!rec.copy_deleted && nontrivial && !rec.layout_requires_virtual_support) rec.copy_ctor = synthesize(true);
  }
  // [class.copy.ctor]/8, 10–11: implicit move requires no user-declared copy, move or destructor.
  if (implicit_copy && !user_copy_assignment && user_dtor == EntityId::none) {
    rec.move_usable = !rec.copy_deleted || std::ranges::none_of(subobjects, [&](QualType t) {
                        return !types.is_reference(t) && classes_.unmovable(t);
                      });
    rec.move_nontrivial = rec.polymorphic || std::ranges::any_of(subobjects, [&](QualType t) {
      return !types.is_reference(t) && classes_.move_nontrivial(t);
    });
  }
  // POD for layout, as Clang has it (`ast::record_layout`): a trivial class ([class.prop]/2: its
  // default constructor, copy and move operations and destructor trivial), and standard-layout
  // (/3: one access for all its data members, no reference members, and its data members all
  // in one class of the hierarchy), whose members and bases are too.
  auto pod_type = [&](QualType t) {
    while (types.is_array(t)) t = types.rec(t).inner;
    if (types.is_reference(t)) return false;
    const sema::ClassRec* m = types.is_class(t) ? classes_.find(types.class_of(t)) : nullptr;
    return !m || m->pod;
  };
  // [dcl.fct.def.default]/5: a first-declaration defaulted or deleted assignment is not
  // user-provided. Its fields still have to satisfy the layout predicate below.
  const bool user_provided_assignment = std::ranges::any_of(rec.members, [&](const sema::MemberDeclRec& m) {
    return m.is_function && !m.is_static && !m.explicit_defaulted_assignment && !m.is_deleted &&
           w_->ins.names.spelling(m.name) == "operator=";
  });
  bool one_access = std::ranges::adjacent_find(rec.fields, [&](const sema::FieldRec& a, const sema::FieldRec& b) {
                      return rec.access_of(a.entity) != rec.access_of(b.entity);
                    }) == rec.fields.end();
  bool data_in_bases = std::ranges::any_of(rec.bases, [&](const sema::BaseRec& b) {
    const sema::ClassRec* r = classes_.find(b.cls);
    return r && (!r->fields.empty() || !r->pod);
  });
  rec.pod = !rec.layout_requires_virtual_support && !rec.user_ctors && user_dtor == EntityId::none &&
            rec.destructor == EntityId::none &&
            !rec.copy_nontrivial() && !rec.copy_deleted && !user_move && !user_provided_assignment &&
            std::ranges::none_of(rec.fields, &sema::FieldRec::dmi) && one_access &&
            std::ranges::all_of(rec.fields, [&](const sema::FieldRec& f) { return pod_type(f.type); }) &&
            std::ranges::all_of(rec.bases, [&](const sema::BaseRec& b) { return pod_type(types.class_type(b.cls)); }) &&
            (rec.fields.empty() || !data_in_bases);
  if (!user_copy_assignment || defaulted_assignment)
    declare_copy_assignment(item, cls, rec, !user_copy_assignment, deletes_implicit_copy);
}

void Program::declare_copy_assignment(u32 item, EntityId cls, sema::ClassRec& rec, bool implicit, bool user_move) {
  using sema::CopyAssignment;
  auto& types = w_->types;
  const QualType ct = types.class_type(cls);
  if (types.is_dependent(ct)) return;
  const NameId name = w_->ins.names.intern("operator=");

  bool const_parameter = true;
  bool trivial = !rec.layout_requires_virtual_support;
  bool memberwise = !rec.layout_requires_virtual_support && rec.bases.empty() && !rec.union_ && !rec.closure &&
                    w_->entities.rec(cls).templ == EntityId::none && items_->items[item].template_depth == 0;
  bool deleted = rec.closure && rec.default_deleted;
  bool const_member = false;
  const auto inspect = [&](QualType t, bool base) {
    if (types.is_reference(t)) {
      deleted = true;
      return;
    }
    if (types.is_array(t)) memberwise = false;
    while (types.is_array(t)) t = types.add_cv(types.rec(t).inner, sema::Types::cv(t));
    t = types.canonical(t);
    if (sema::Types::is_volatile(t)) {
      trivial = false;
      memberwise = false;
    }
    if (!types.is_class(t)) {
      const_member |= sema::Types::is_const(t);
      trivial &= types.is_object(t) && !types.is_member_pointer(t);
      memberwise &= types.is_object(t) && !types.is_member_pointer(t);
      return;
    }
    const auto* sub = classes_.find(types.class_of(t));
    if (!sub) {
      trivial = false;
      memberwise = false;
      return;
    }
    const QualType owner = sema::Types::unqualified(t);
    const_parameter &= std::ranges::any_of(sub->members, [&](const sema::MemberDeclRec& m) {
      const auto p = assignment_parameter(m, owner);
      return p && !types.rec(*p).rvalue_ref &&
             (!types.is_reference(*p) || sema::Types::is_const(types.canonical(types.referee(*p))));
    });
    const auto* copy = sub->implicit_copy_assignment();
    const bool sole = copy && std::ranges::count(sub->members, name, &sema::MemberDeclRec::name) == 1;
    trivial &= sole && copy->copy_assignment == CopyAssignment::trivial && !sema::Types::is_const(t);
    // Preserve a proved deleted base copy. Other nontrivial base operations are deferred.
    if (base && sole) {
      const_member |= copy->copy_assignment == CopyAssignment::const_member;
      deleted |= copy->copy_assignment == CopyAssignment::deleted;
    }
  };
  for (const auto& b : rec.bases) inspect(types.class_type(b.cls), true);
  for (const auto& f : rec.fields) inspect(f.type, false);

  const auto record_body = [&](u32 definition_item, ast::DeclSema d, QualType source, CopyAssignment state) {
    if (state != CopyAssignment::trivial && state != CopyAssignment::memberwise) return;
    ast::ParamSema param{EntityId::none, source, w_->ins.names.intern("other"), NodeId::none, 0};
    d.params = w_->ins.intern_seq(std::span<const ast::ParamSema>(&param, 1));
    copy_assignments_.try_emplace(d.entity,
        PendingCopyAssignment{{definition_item, d, SpecialMember::copy_assignment}, state});
  };
  if (!implicit) {
    for (auto& copy : rec.members) {
      if (!copy.explicit_defaulted_assignment) continue;
      const QualType source = types.rec(copy.type).params.front();
      const bool const_mismatch = !const_parameter && sema::Types::is_const(types.canonical(types.referee(source)));
      copy.copy_assignment = const_member ? CopyAssignment::const_member
                           : deleted || const_mismatch ? CopyAssignment::deleted
                           : memberwise ? CopyAssignment::memberwise : CopyAssignment::unsupported;
      const DeclId decl = member_decls_.at(std::to_underlying(copy.entity));
      record_body(index(decl.item), *decl_sema(decl), source, copy.copy_assignment);
    }
    return;
  }

  const CopyAssignment state = const_member ? CopyAssignment::const_member
                               : deleted || user_move ? CopyAssignment::deleted
                               : trivial ? CopyAssignment::trivial
                               : memberwise ? CopyAssignment::memberwise : CopyAssignment::unsupported;
  const QualType source = types.lvalue_ref(const_parameter ? types.add_cv(ct, sema::Cv::const_) : ct);
  const QualType result = types.lvalue_ref(ct);
  const QualType type = types.function(result, std::span<const QualType>(&source, 1), false, LangLinkage::cxx);
  const EntityId entity = w_->entities.entity({sema::EntityKind::function, name, type, LangLinkage::cxx,
                                                ast::Linkage::external, {}, cls});
  sema::MemberDeclRec candidate{};
  candidate.name = name;
  candidate.entity = entity;
  candidate.type = type;
  candidate.is_function = true;
  candidate.copy_assignment = state;
  rec.members.push_back(std::move(candidate));
  if (state != CopyAssignment::trivial && state != CopyAssignment::memberwise) return;

  ast::DeclSema d = invalid_decl();
  d.kind = ast::DeclKind::function;
  d.entity = entity;
  d.type = type;
  d.linkage = ast::Linkage::external;
  d.storage = ast::Storage::none;
  d.access = ast::Access::public_;
  d.flags = sema::decl_flag::member | sema::decl_flag::implicit | sema::decl_flag::definition |
            sema::decl_flag::inline_ | sema::decl_flag::constexpr_;
  d.attributes = w_->ins.intern_seq(std::span<const NodeId>{});
  d.template_params = w_->ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  record_body(item, d, source, state);
}

sema::CopyAssignment Program::require_copy_assignment(EntityId f) {
  using sema::CopyAssignment;
  auto it = copy_assignments_.find(f);
  if (it == copy_assignments_.end() || it->second.checking) return CopyAssignment::unsupported;
  auto& copy = it->second;
  if (copy.body || (copy.state != CopyAssignment::trivial && copy.state != CopyAssignment::memberwise)) return copy.state;
  copy.checking = true;
  const Synthesized& s = copy.definition;
  const EntityId cls = w_->entities.rec(f).scope;
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;
  AtItem q(*this, s.item, true);
  q.own_class_complete = true;
  sema::Sema sm(*w_, b, q, diags, id(s.item), 0);
  sm.current_class = cls;
  sm.current_function = f;
  sm.this_type = this_type(s.decl);
  const auto param = w_->ins.seqs.at(s.decl.params, 0);
  sm.push_scope();
  sm.declare_local(param.name, param.type, sema::local_flag::parameter, 0, false);
  const Ex self = sm.this_expr(0, 0);
  const Ex destination = sm.unary(ast::UnaryOp::deref, self, 0, 0, 0);
  const Ex source = sm.name_expr(param.name, 0, 0);
  std::vector<ast::Child> statements;
  const auto append = [&](Ex expr) {
    if (!expr.ok()) return false;
    if (w_->types.is_class(expr.type()) && !expr.glvalue())
      expr = sm.materialize(expr, sema::Types::unqualified(expr.type()), false);
    statements.push_back({sm.node(ast::ExprStmt{{expr.id, 0}}, 0, 0).id, 0});
    return true;
  };

  bool ok = true;
  if (copy.state == CopyAssignment::trivial) {
    const auto info = destination.info;
    ok = append(sm.node(ast::Assign{info, ast::AssignOp::trivial_class_copy,
                                   {destination.id, 0}, {source.id, 0}, info.type}, 0, 0, info));
  } else {
    for (const auto& field : classes_.rec(cls).fields) {
      Ex from = sm.member(source, false, field.name, 0, 0, 0);
      Ex to = sm.member(self, true, field.name, 0, 0, 0);
      if (!append(sm.assign(sema::Sema::AsgOp::assign, to, from, 0))) {
        ok = false;
        break;
      }
    }
  }
  copy.checking = false;
  if (!ok || !diags.empty()) {
    const bool unsupported = diags.empty() || std::ranges::any_of(diags, [](const engine::Diagnostic& d) {
      return d.id == sema::diag_id(Diag::unsupported_copy_assignment) || d.id == sema::diag_id(Diag::not_supported);
    });
    copy.state = unsupported ? CopyAssignment::unsupported : CopyAssignment::deleted;
    return copy.state;
  }
  statements.push_back({sm.node(ast::Return{{destination.id, 0}}, 0, 0).id, 0});
  const Ex root = sm.node(ast::Compound{b.list(statements)}, 0, 0);
  copy.body = b.commit(ast::BodySema{root.id, b.seq(sm.locals()), b.seq(std::span<const ast::ExpansionRec>{}),
                                    b.seq(std::span<const u32>{}), QualType{}, 0,
                                    b.seq(std::span<const ast::ClosureDef>{})});
  sm.pop_scope();
  return copy.state;
}

// The copy of the object `src` designates as an initializer of type `t` ([class.copy.ctor]/14):
// a class through its copy constructor; an array whose elements' copy is not trivial element by
// element, else as a whole.
Ex Program::copy_from(sema::Sema& sm, Ex src, QualType t) {
  if (!w_->types.is_array(t)) return sm.convert(src, t, 0);
  if (!classes_.copy_of(t).first) return sm.convert_node(src, ast::ConversionKind::lvalue_to_rvalue, sema::Types::unqualified(t));
  sema::TypeRec r = w_->types.rec(t);
  std::vector<Ex> elems;
  QualType index = w_->types.builtin(sema::Builtin::int_);
  for (sema::u64 i = 0; i < r.bound; ++i) {
    Ex at = sm.literal(index, w_->values.integer(index, i), ast::ValueCategory::prvalue, 0, 0);
    elems.push_back(copy_from(sm, sm.subscript(src, at, 0, 0), r.inner));
  }
  return sm.list_of(t, std::move(elems), 0, 0);
}

const ast::BodySema& Program::synthesized_body(std::size_t i) {
  if (auto it = synth_bodies_.find(i); it != synth_bodies_.end()) return it->second;
  const Synthesized& s = synth_[i];
  if (s.kind == SpecialMember::copy_assignment) return *copy_assignments_.at(s.decl.entity).body;
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;  // none: the class was analysed
  AtItem q(*this, s.item, true);
  q.own_class_complete = true;
  sema::Sema sm(*w_, b, q, diags, id(s.item), 0);
  EntityId cls = w_->entities.rec(s.decl.entity).scope;
  sm.current_class = cls;
  sm.this_type = this_type(s.decl);
  const sema::ClassRec& rec = classes_.rec(cls);
  Ex root;
  if (s.kind == SpecialMember::copy_constructor) {
    // [class.copy.ctor]/14: each member copied from the other object's, in declaration order.
    NameId other = w_->ins.names.intern("other");
    sm.push_scope();
    sm.declare_local(other, w_->ins.seqs.at(s.decl.params, 0).type, sema::local_flag::parameter, 0, false);
    std::vector<ast::Child> stmts;
    // Each direct base from the other object's base class subobject.
    for (const sema::BaseRec& base : rec.bases) {
      QualType bt = w_->types.class_type(base.cls);
      Ex init = copy_from(sm, sm.name_expr(other, 0, 0), bt);
      stmts.push_back({sm.node(ast::MemberInit{~0u, bt, {init.id, 0}}, 0, 0).id, 0});
    }
    for (u32 i = 0; i < rec.fields.size(); ++i) {
      const sema::FieldRec& f = rec.fields[i];
      Ex src = sm.member(sm.name_expr(other, 0, 0), false, f.name, 0, 0, 0);
      Ex init = copy_from(sm, src, f.type);
      stmts.push_back({sm.node(ast::MemberInit{i, f.type, {init.id, 0}}, 0, 0).id, 0});
    }
    Ex body = sm.node(ast::Compound{b.list({})}, 0, 0);
    stmts.push_back({body.id, 0});
    root = sm.node(ast::Compound{b.list(stmts)}, 0, 0);
    sm.pop_scope();
  } else {
    Ex body = sm.node(ast::Compound{b.list({})}, 0, 0);
    root = BodyAnalysis::destructor_body(sm, cls, body);
  }
  std::vector<ast::LocalRec> locals(sm.locals().begin(), sm.locals().end());
  return synth_bodies_[i] = b.commit(ast::BodySema{root.id, b.seq(std::span<const ast::LocalRec>(locals)),
                                                   b.seq(std::span<const ast::ExpansionRec>{}), b.seq(std::span<const u32>{}),
                                                   QualType{}, 0, b.seq(std::span<const ast::ClosureDef>{})});
}

const Result<ast::BodySema>& Program::analyze_body(u32 item) {
  if (!body_cells_[item]) body_cells_[item] = BodyAnalysis(*this, item).run();
  return *body_cells_[item];
}

// ---------------------------------------------------------------- instantiation (#88)

std::optional<std::vector<ast::TemplateArg>> Program::substitute_partial_args(
    u32 item, u32 point, std::span<const ast::TemplateParamSema> params, std::span<const ast::TemplateArg> pattern,
    sema::Bindings args) {
  std::vector<engine::Diagnostic> sink;
  ast::Builder b(w_->ins, &arena_);
  AtItem q(*this, item, false);
  q.at_point = point;
  sema::Sema s(*w_, b, q, sink, id(item), 0);
  auto replay = partial_replays_.find(item);
  if (replay == partial_replays_.end()) return s.templates().substitute_args(pattern, args);

  // The canonical type can lose a dependent decltype operand or alias argument. A dependent
  // qualified type also needs access checking. Re-analyse the written arguments with the deduced
  // bindings so these checks run at substitution ([temp.deduct.general]).
  const PartialReplay& source = replay->second;
  const items::Item& it = items_->items[item];
  Parser ps(*r_, it.begin, it.begin + it.decl_length(), s);
  ps.set_questions(q);
  ps.substituting_partial_arguments = true;
  ps.seek(it.begin + source.begin);
  q.instantiation_calls = source.dependent_calls;
  s.template_scopes.emplace_back(params.begin(), params.end());
  s.template_args = std::move(args);
  std::optional<std::vector<ast::TemplateArg>> back;
  try {
    if (auto written = ps.template_argument_list()) back = s.template_arguments(source.primary, *written, source.begin);
  } catch (const TooDeep&) {
    // The parser diagnosed the stack limit in this operand.
  }
  if (s.has_error_since(0)) back.reset();
  for (const engine::Diagnostic& d : sink) {
    // Only immediate-context errors make a candidate fail silently. Instantiations triggered
    // during this check record their own diagnostics in their own cells.
    if ((sema::survives_substitution(d.id) || d.id == sema::diag_id(Diag::nested_too_deeply)) &&
        !std::ranges::contains(end_diags_, d)) end_diags_.push_back(d);
  }
  return back;
}

const sema::ClassRec* Program::instantiate_class(EntityId spec, u32 point) {
  const u32 key = std::to_underlying(spec);
  // This unit's instantiation: its member records, member patterns and diagnostics are the
  // unit's, even when another unit published the class's record first (#156).
  // Instantiated already: complete at points after its pattern's definition, wherever it was
  // first needed.
  if (classes_instantiated_.contains(key)) return point > last_member(class_patterns_.at(key)) ? classes_.find(spec) : nullptr;
  const sema::EntityRec rec = w_->entities.rec(spec);
  if (w_->types.is_dependent(w_->types.class_type(spec)) || classes_instantiating_.contains(key) || partial_matching_.contains(key))
    return nullptr;
  // Its pattern: the partial specialization that matches it, else the template's definition.
  std::optional<Chosen> partial = choose_partial(spec, point);
  if (partial && (partial->ambiguous || !partial->defined)) {
    if (partial->ambiguous) ambiguous_partial(spec, point);
    return nullptr;
  }
  auto t = class_templates_.find(std::to_underlying(rec.templ));
  if (!partial && t == class_templates_.end()) return nullptr;
  const u32 k = partial ? partial->item : t->second;
  // Defined before the point ([temp.inst]/2): not inside its own definition, which is not complete there.
  if (point <= last_member(k)) return nullptr;
  // Not inside its own template's definition, which is not complete there ([temp.inst]/2).
  if (std::ranges::find(running_, class_def_key(k)) != running_.end() || std::ranges::find(running_, k) != running_.end())
    return nullptr;
  // [temp.inst]/16: nested instantiations are limited, as functions' are.
  if (too_deep(classes_instantiating_.size(), k)) return nullptr;
  class_def(k);  // the pattern, analysed as the current instantiation's
  ast::DeclSema head = w_->ins.seqs.at(item_sema(k).value.decls, 0);
  std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(head.template_params);
  sema::Bindings args = partial ? partial->args : sema::Bindings::of(params.empty() ? 0 : params[0].depth, rec.args);
  class_args_.emplace(key, args);
  class_patterns_.emplace(key, k);
  classes_instantiating_.insert(key);
  class_points_.emplace(key, point);
  // Each member item's declaration part, again, with the arguments bound: the members of `spec`,
  // asked at the point of instantiation ([temp.point]).
  ClassContent c = class_content(k, [&](u32 m) -> const ast::ItemSema& {
    auto cell_key = std::pair(m, key);
    if (!class_member_cells_.contains(cell_key)) {
      Result<ast::ItemSema> cell = ItemAnalysis(*this, m, ClassInstantiation{spec, args, point}).run();
      // What the definition diagnosed it says once, there.
      if (!item_sema(m).diags.empty()) cell.diags.clear();
      class_member_cells_.emplace(cell_key, std::move(cell));
      class_member_order_.push_back(cell_key);
    }
    return class_member_cells_.at(cell_key).value;
  });
  classes_instantiating_.erase(key);
  classes_instantiated_.insert(key);
  for (const ast::MemberRec& m : c.members)
    if (w_->entities.rec(m.entity).kind != sema::EntityKind::data_member)
      member_patterns_.emplace(std::to_underlying(m.entity), MemberOf{index(m.decl.item), spec});
  // Built whole, then published once (#156); the first publication wins.
  sema::ClassRec built;
  built.union_ = class_rec_is_union(k);
  built.fields = std::move(c.fields);
  built.members = std::move(c.statics);
  built.types = std::move(c.nested);
  for (const ast::MemberRec& m : c.members) built.access.push_back({m.entity, m.access});
  built.friends = std::move(c.friends);
  special_members(k, spec, built);
  built.defined = true;
  return &classes_.publish(spec, std::move(built));
}

// The item that defines, outside its class, the member of a class template specialization that
// `spec` is: the definition of the current instantiation's member declared at the same place.
std::optional<u32> Program::out_of_class_definition(EntityId spec, MemberOf of) {
  const Result<ast::ItemSema>& cell = class_member_cells_.at(std::pair(of.item, std::to_underlying(of.spec)));
  std::vector<ast::DeclSema> inst = w_->ins.seqs.elements(cell.value.decls);
  std::vector<ast::DeclSema> pattern = w_->ins.seqs.elements(item_sema(of.item).value.decls);
  for (std::size_t i = 0; i < inst.size() && i < pattern.size(); ++i) {
    if (inst[i].entity != spec) continue;
    auto def = definitions_.find(std::to_underlying(pattern[i].entity));
    if (def != definitions_.end()) return def->second;
  }
  return std::nullopt;
}

const Program::VarInstance* Program::variable_instance(EntityId v, u32 point) {
  const u32 key = std::to_underlying(v);
  if (auto it = var_index_.find(key); it != var_index_.end())
    return var_instances_[it->second].item < point ? &var_instances_[it->second] : nullptr;
  const sema::EntityRec& rec = w_->entities.rec(v);
  if (rec.kind != sema::EntityKind::variable || rec.templ == EntityId::none || var_instantiating_.contains(key) || partial_matching_.contains(key) ||
      explicitly_specialized(v, point))
    return nullptr;
  // Its pattern, a partial specialization that matches it or the template, must be defined before
  // the point, as a variable must ([expr.const]).
  std::optional<Chosen> partial = choose_partial(v, point);
  if (partial && partial->ambiguous) {
    ambiguous_partial(v, point);
    return nullptr;
  }
  auto t = var_templates_.find(std::to_underlying(rec.templ));
  if (!partial && (t == var_templates_.end() || t->second >= point)) return nullptr;
  const u32 k = partial ? partial->item : t->second;
  // [temp.inst]/16: nested instantiations are limited, as functions' are.
  if (too_deep(var_instantiating_.size(), k)) return nullptr;
  ast::DeclSema tmpl = w_->ins.seqs.at(item_sema(k).value.decls, 0);
  std::vector<ast::TemplateParamSema> head = w_->ins.seqs.elements(tmpl.template_params);
  sema::Bindings args = partial ? partial->args : sema::Bindings::of(head.empty() ? 0 : head[0].depth, rec.args);
  var_instantiating_.insert(key);
  Result<ast::ItemSema> cell = ItemAnalysis(*this, k, VarInstantiation{v, std::move(args), point}).run();
  var_instantiating_.erase(key);
  // What the definition diagnosed it says once, there.
  if (!item_sema(k).diags.empty()) cell.diags.clear();
  var_index_.emplace(key, var_instances_.size());
  var_instances_.push_back({v, k, std::move(cell)});
  return &var_instances_.back();
}

const Program::Instance* Program::instance(EntityId spec, u32 point) {
  const u32 key = std::to_underlying(spec);
  if (auto it = instance_index_.find(key); it != instance_index_.end()) return &instances_[it->second];
  if (instantiating_.contains(key)) return nullptr;  // its own constant evaluation (#88 follow-up)
  if (explicitly_specialized(spec, point)) return nullptr;  // defined, or not, by its explicit specialization
  const sema::EntityRec& rec = w_->entities.rec(spec);
  // A template is no function: only its specializations are instantiated ([temp.inst]).
  if (rec.kind == sema::EntityKind::function_template) return nullptr;
  // A member function of a class template specialization ([temp.inst]/4): its pattern is its
  // member item, defined in the class.
  auto member = member_patterns_.find(key);
  if (member == member_patterns_.end() && rec.templ == EntityId::none) return nullptr;
  auto def = member != member_patterns_.end() ? definitions_.end() : definitions_.find(std::to_underlying(rec.templ));
  // A member template of a class template specialization defined outside the class: the
  // definition of the current instantiation's member template at the same place ([temp.mem]/1).
  std::optional<u32> outside;
  if (member == member_patterns_.end() && def == definitions_.end())
    if (auto mt = member_patterns_.find(std::to_underlying(rec.templ)); mt != member_patterns_.end())
      outside = out_of_class_definition(rec.templ, mt->second);
  if (member == member_patterns_.end() && def == definitions_.end() && !outside) return nullptr;  // declared only
  u32 decl_item = member != member_patterns_.end() ? member->second.item : 0;
  u32 body_item = decl_item;
  if (member != member_patterns_.end() && !items_->items[decl_item].has_body()) {
    // Defined outside the class ([temp.mem.func]): the definition of the current instantiation's
    // member at the same place in the class.
    auto out = out_of_class_definition(spec, member->second);
    if (!out) return nullptr;
    body_item = *out;
  }
  const u32 k = member != member_patterns_.end() ? body_item : outside ? *outside : def->second;
  // [temp.inst]/16: instantiations nested through constant evaluation are limited as the END
  // worklist's are; the limit is reported once, at the template.
  if (too_deep(instantiating_.size(), k)) return nullptr;
  if (member != member_patterns_.end()) {
    // Its declaration is the one the class's instantiation made; the class's arguments are bound.
    EntityId cls = member->second.spec;
    const Result<ast::ItemSema>& cell = class_member_cells_.at(std::pair(decl_item, std::to_underlying(cls)));
    std::optional<ast::DeclSema> decl;
    for (const ast::DeclSema& d : w_->ins.seqs.elements(cell.value.decls))
      if (d.entity == spec) decl = d;
    if (!decl) return nullptr;
    // Defined outside the class: its body names the definition's parameters, of the types the
    // class's instantiation gave them ([dcl.fct]/9).
    if (k != decl_item && w_->ins.seqs.size(item_sema(k).value.decls) > 0) {
      std::vector<ast::ParamSema> params = w_->ins.seqs.elements(decl->params);
      std::vector<ast::ParamSema> named = w_->ins.seqs.elements(w_->ins.seqs.at(item_sema(k).value.decls, 0).params);
      // A function parameter pack of the definition names all its elements ([temp.variadic]/8).
      std::size_t i = 0;
      for (const ast::ParamSema& n : named) {
        if (w_->types.kind(n.type) == sema::TypeKind::pack_expansion) {
          for (; i < params.size() && (params[i].flags & sema::param_flag::pack_element); ++i) params[i].name = n.name;
          continue;
        }
        if (i < params.size()) params[i++].name = n.name;
      }
      decl->params = w_->ins.intern_seq(std::span<const ast::ParamSema>(params));
    }
    sema::Bindings args = class_args_.at(std::to_underlying(cls));
    Instance in{spec, k, *decl, {}, false};
    in.body.value.tree = NodeId::none;  // until analysed: a default NodeId is a real node
    if (!cell.diags.empty() || !analyze_body(k).diags.empty()) {
      in.failed = true;
    } else {
      instantiating_.insert(key);
      in.body = BodyAnalysis(*this, k, Instantiation{*decl, std::move(args)}).run();
      instantiating_.erase(key);
      if (in.body.value.deduced) in.decl.type = in.body.value.deduced_type;
    }
    instance_index_.emplace(key, instances_.size());
    instances_.push_back(std::move(in));
    instance_order_.push_back(&instances_.back());
    return &instances_.back();
  }
  ast::DeclSema tmpl = w_->ins.seqs.at(item_sema(k).value.decls, 0);
  std::vector<ast::TemplateParamSema> head = w_->ins.seqs.elements(tmpl.template_params);
  const sema::u16 depth = head.empty() ? 0 : head[0].depth;
  sema::Bindings args = sema::Bindings::of(depth, rec.args);
  // A member template of a class template specialization ([temp.mem]): the class's arguments
  // bind the enclosing levels.
  if (auto outer = class_args_.find(std::to_underlying(w_->entities.rec(rec.templ).scope)); outer != class_args_.end()) {
    sema::Bindings all = outer->second;
    all.levels.resize(depth + 1);
    all.levels[depth] = args.levels[depth];
    args = std::move(all);
  }
  // The specialization's declaration: the template's, with the arguments substituted ([temp.inst]).
  // Every handle in it is interned already.
  ast::DeclSema decl = tmpl;
  decl.entity = spec;
  decl.type = rec.type;
  decl.flags = sema::u16(decl.flags & ~sema::decl_flag::template_);
  decl.template_params = w_->ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  sema::Templates t = templates();
  std::vector<ast::ParamSema> params;
  // A parameter's type is adjusted again after substitution ([dcl.fct]/5): an array or function
  // becomes a pointer; the body's object keeps its cv-qualifiers. A function parameter pack is a
  // parameter for each element ([temp.variadic]/8).
  sema::Types& ty = w_->types;
  auto adjusted = [&](ast::ParamSema p) {
    if (ty.is_array(p.type)) p.type = ty.add_cv(ty.pointer(ty.rec(p.type).inner), sema::Types::cv(p.type));
    else if (ty.is_function(p.type)) p.type = ty.pointer(p.type);
    return p;
  };
  for (ast::ParamSema p : w_->ins.seqs.elements(tmpl.params)) {
    if (ty.kind(p.type) == sema::TypeKind::pack_expansion) {
      for (const ast::TemplateArg& x : t.substitute_expansion(t.type_arg(p.type), args).value_or(std::vector<ast::TemplateArg>{})) {
        ast::ParamSema e = p;
        e.type = x.type;
        e.flags = u16(e.flags | sema::param_flag::pack_element);
        params.push_back(adjusted(e));
      }
      continue;
    }
    p.type = t.substitute(p.type, args).value_or(p.type);
    params.push_back(adjusted(p));
  }
  decl.params = w_->ins.intern_seq(std::span<const ast::ParamSema>(params));
  Instance in{spec, k, decl, {}, false};
  in.body.value.tree = NodeId::none;  // until analysed: a default NodeId is a real node
  // A definition with errors was diagnosed; its instantiations say nothing more.
  if (!analyze_body(k).diags.empty()) {
    in.failed = true;
  } else {
    instantiating_.insert(key);
    in.body = BodyAnalysis(*this, k, Instantiation{decl, std::move(args)}).run();
    instantiating_.erase(key);
    if (in.body.value.deduced) in.decl.type = in.body.value.deduced_type;
  }
  instance_index_.emplace(key, instances_.size());
  instances_.push_back(std::move(in));
  instance_order_.push_back(&instances_.back());
  return &instances_.back();
}

// The entities a tree names, in tree order.
void Program::references(NodeId n, std::vector<EntityId>& out) const {
  if (n == NodeId::none) return;
  ast::dispatch<ast::Nodes>(ast::kind_of(n), [&]<class T>(std::type_identity<T>) {
    T v = w_->ins.ast.get<T>(n);
    ast::for_each_ref(v, [&]<class H>(H& h) {
      if constexpr (std::is_same_v<H, EntityId>) {
        if (h != EntityId::none) out.push_back(h);
      } else if constexpr (std::is_same_v<H, NodeId>) {
        references(h, out);
      } else if constexpr (std::is_same_v<H, ast::ChildList>) {
        for (ast::Child c : w_->ins.ast.children(h)) references(c.id, out);
      }
    });
  });
}

void Program::instantiate_all() {
  // [temp.expl.spec]/7: a specialization is explicitly specialized before its first use that
  // would instantiate it: before any earlier cell demanded it, or instantiated the class.
  specializations_before(end());
  for (const auto& [e, k] : explicit_specs_) {
    // Instantiated in a constant evaluation before it: an instance exists, since none is made
    // once the explicit specialization precedes the point.
    auto c = class_points_.find(e);
    bool used = (c != class_points_.end() && c->second < k) || instance_index_.contains(e) || var_index_.contains(e);
    auto demanded = [&](const auto& cell) {
      return cell && std::ranges::any_of(cell->demands, [&](const sema::Demand& d) { return std::to_underlying(d.spec) == e; });
    };
    for (u32 j = 0; j < k && !used; ++j)
      used = demanded(item_cells_[j]) || demanded(class_cells_[j]) || demanded(body_cells_[j]);
    if (!used) continue;
    ast::NameId name = w_->entities.rec(EntityId(e)).name;
    auto at = spec_names_.find(e);
    end_diags_.push_back({engine::Loc{std::to_underlying(id(k)), at == spec_names_.end() ? 0 : at->second},
                          sema::diag_id(sema::Diag::specialized_after_use),
                          {{std::to_underlying(name), w_->ins.names.fp(name)}}});
  }
  // The demands of every cell, in item order, then those of each instance as it is made: the END
  // worklist of §B6.2, deterministic.
  std::vector<sema::Demand> work;
  // What a body requires, besides what its cell demanded: every member function of a class
  // template specialization it names, a constructor or destructor it runs included
  // ([temp.inst]/4), found by walking its tree.
  auto required_members = [&](NodeId root, ItemId at) {
    std::vector<EntityId> refs;
    references(root, refs);
    for (EntityId e : refs) {
      if (auto assign = copy_assignments_.find(e); assign != copy_assignments_.end() && assign->second.body &&
          !assign->second.emitted) {
        assign->second.emitted = true;
        synth_.push_back(assign->second.definition);
      }
      if (member_patterns_.contains(std::to_underlying(e))) work.push_back({e, ast::Loc{at, 0}});
    }
  };
  const auto required_part = [&](NodeId root, ast::SeqId<ast::ClosureDef> closures, ItemId at) {
    required_members(root, at);
    for (const auto& closure : w_->ins.seqs.elements(closures)) {
      if (!sema::Sema::closure_has_code(*w_, closure)) continue;
      required_members(closure.body, at);
      required_members(closure.invoker_body, at);
    }
  };
  for (u32 k = 0; k < items_->items.size(); ++k) {
    if (item_cells_[k]) {
      work.insert(work.end(), item_cells_[k]->demands.begin(), item_cells_[k]->demands.end());
      // A namespace-scope variable's initializer, and its destructor ([basic.start.term]), are code
      // of the unit's initialization function.
      if (!templated_body(k) && !in_class_[k]) {
        const auto& part = item_cells_[k]->value;
        required_part(part.tree, part.closures, id(k));
        for (const ast::DeclSema& d : w_->ins.seqs.elements(item_cells_[k]->value.decls))
          if (d.kind == ast::DeclKind::variable && d.entity != EntityId::none && (d.flags & sema::decl_flag::definition)) {
            QualType t = d.type;
            while (w_->types.is_array(t)) t = w_->types.rec(t).inner;
            EntityId dtor = w_->types.is_class(t) ? classes_.destructor_of(t) : EntityId::none;
            if (member_patterns_.contains(std::to_underlying(dtor))) work.push_back({dtor, ast::Loc{id(k), 0}});
          }
      }
    }
    if (class_cells_[k])
      work.insert(work.end(), class_cells_[k]->demands.begin(), class_cells_[k]->demands.end());
    if (body_cells_[k]) {
      work.insert(work.end(), body_cells_[k]->demands.begin(), body_cells_[k]->demands.end());
      const auto& part = body_cells_[k]->value;
      if (!templated_body(k)) required_part(part.tree, part.closures, id(k));
    }
  }
  // The synthesized special members' bodies, those of classes instantiated as the worklist runs
  // included.
  std::size_t walked = 0;
  auto walk_synthesized = [&] {
    for (; walked < synth_.size(); ++walked) {
      EntityId cls = w_->entities.rec(synth_[walked].decl.entity).scope;
      if (w_->types.is_dependent(w_->types.class_type(cls))) continue;
      required_members(synthesized_body(walked).tree, id(synth_[walked].item));
    }
  };
  walk_synthesized();
  std::vector<u32> depth(work.size(), 1);
  const u32 limit = instantiation_limit;
  std::set<u32> seen;
  for (std::size_t i = 0; i < work.size(); ++i) {
    if (!seen.insert(std::to_underlying(work[i].spec)).second) continue;
    required_.push_back(work[i].spec);
    if (depth[i] > limit) {
      if (!depth_reported_)
        end_diags_.push_back({engine::Loc{std::to_underlying(work[i].at.item), work[i].at.offset},
                              sema::diag_id(sema::Diag::instantiation_depth), {engine::diag_arg(limit)}});
      depth_reported_ = true;
      break;
    }
    // A variable template specialization's initializer, and what it demands ([temp.inst]/7).
    end_at_ = engine::Loc{std::to_underlying(work[i].at.item), work[i].at.offset};
    if (const VarInstance* v = variable_instance(work[i].spec, end())) {
      if (v->cell.diags.empty())
        for (const sema::Demand& d : v->cell.demands) {
          work.push_back(d);
          depth.push_back(depth[i] + 1);
        }
      continue;
    }
    const Instance* in = instance(work[i].spec, end());
    if (!in || in->failed || !in->body.diags.empty()) continue;  // what an erroneous instance demands is not code
    for (const sema::Demand& d : in->body.demands) {
      work.push_back(d);
      depth.push_back(depth[i] + 1);
    }
    required_part(in->body.value.tree, in->body.value.closures, id(in->item));
    walk_synthesized();
    depth.resize(work.size(), depth[i] + 1);
  }
}

interp::Callee Program::function_ir(EntityId f, u32 item) {
  if (sema::Sema::closure_member(*w_, f)) return closure_ir(f, item);
  interp::Callee c;
  c.is_constexpr = is_constexpr(f, item);
  if (auto copy = copy_assignments_.find(f); copy != copy_assignments_.end()) {
    if (!c.is_constexpr || !copy->second.body) return c;
    const u32 key = std::to_underlying(f);
    if (!instance_irs_.contains(key)) {
      const auto& s = copy->second.definition;
      irgen::FunctionInput input{f, s.decl, *copy->second.body, id(s.item), 0};
      input.this_type = this_type(s.decl);
      AtItem q(*this, s.item, true);
      q.own_class_complete = true;
      sema::EvalFacts facts(*w_, q);
      std::optional<irgen::FunctionIr> ir;
      if (auto r = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*r);
      instance_irs_.emplace(key, std::move(ir));
    }
    const auto& ir = instance_irs_.at(key);
    if (!ir) c.unsupported = true;
    else c.body = &ir->body;
    return c;
  }
  // A function template specialization needed for constant evaluation is instantiated where it
  // is needed ([temp.inst]/8).
  if (EntityId templ = w_->entities.rec(f).templ; templ != EntityId::none && !explicitly_specialized(f, item)) {
    // Its template must be defined before the point, as a function must ([expr.const]).
    auto def = definitions_.find(std::to_underlying(templ));
    if (def == definitions_.end() || def->second >= item) return c;
    const Instance* in = instance(f, item);
    const u32 key = std::to_underlying(f);
    if (!in || in->failed || !in->body.diags.empty() || in->body.value.tree == NodeId::none) {
      c.unsupported = in != nullptr;  // erroneous: its body cannot be evaluated
      return c;
    }
    if (!instance_irs_.contains(key)) {
      const items::Item& it = items_->items[in->item];
      irgen::FunctionInput input{f, in->decl, in->body.value, id(in->item), it.body_begin - it.begin};
      AtInstantiation q(*this, in->item);
      sema::EvalFacts facts(*w_, q);
      std::optional<irgen::FunctionIr> ir;
      if (auto r = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*r);
      instance_irs_.emplace(key, std::move(ir));
    }
    const auto& ir = instance_irs_.at(key);
    if (!ir) c.unsupported = true;
    else c.body = &ir->body;
    return c;
  }
  auto def = definitions_.find(std::to_underlying(f));
  if (def == definitions_.end() || def->second >= item || !friend_body_ready(def->second)) return c;
  u32 k = def->second;
  if (!irs_.contains(k)) {
    // A body with errors has no L1: the evaluation stops at the call.
    const Result<ast::BodySema>& body = analyze_body(k);
    std::optional<irgen::FunctionIr> ir;
    const ast::ItemSema& is = item_sema(k).value;
    if (body.diags.empty() && body.value.tree != NodeId::none && w_->ins.seqs.size(is.decls) > 0) {
      ast::DeclSema d = w_->ins.seqs.at(is.decls, 0);
      if (body.value.deduced) d.type = body.value.deduced_type;
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

// A closure member's L1 ([expr.prim.lambda.closure]/5: constexpr when it can be), from the body
// that has its lambda: a function defined before `item`, or a specialization instantiated here.
interp::Callee Program::closure_ir(EntityId f, u32 item) {
  interp::Callee r;
  r.is_constexpr = true;
  const u32 key = std::to_underlying(f);
  if (auto it = closure_irs_.find(key); it != closure_irs_.end()) {
    if (!it->second) r.unsupported = true;
    else r.body = &it->second->body;
    return r;
  }
  // A generic lambda's operator specialized: its instance's body.
  if (auto it = instance_index_.find(key); it != instance_index_.end()) {
    const Instance& in = instances_[it->second];
    if (in.failed) {
      r.unsupported = true;
      return r;
    }
    const items::Item& itm = items_->items[in.item];
    irgen::FunctionInput input{f, in.decl, in.body.value, id(in.item), itm.body_begin - itm.begin, false, {}, this_type(in.decl)};
    AtItem q(*this, in.item, true);
    sema::EvalFacts facts(*w_, q);
    std::optional<irgen::FunctionIr> ir;
    if (auto x = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*x);
    auto& slot = closure_irs_[key] = std::move(ir);
    if (!slot) r.unsupported = true;
    else r.body = &slot->body;
    return r;
  }
  // The function whose body has the lambda: past the operators of enclosing lambdas.
  EntityId fn = w_->entities.rec(w_->entities.rec(f).scope).scope;
  while (sema::Sema::closure_member(*w_, fn)) fn = w_->entities.rec(w_->entities.rec(fn).scope).scope;
  const ast::BodySema* body = nullptr;
  u32 k = 0;
  if (const sema::EntityRec& fr = w_->entities.rec(fn); (fr.templ != EntityId::none && !explicitly_specialized(fn, item)) ||
                                                         member_patterns_.contains(std::to_underlying(fn))) {
    const Instance* in = instance(fn, item);
    if (!in || in->failed) return r;
    body = &in->body.value;
    k = in->item;
  } else {
    auto def = definitions_.find(std::to_underlying(fn));
    if (def == definitions_.end() || def->second >= item) return r;
    k = def->second;
    body = &analyze_body(k).value;
  }
  for (const ast::ClosureDef& c : w_->ins.seqs.elements(body->closures)) {
    if (c.call != f && c.invoker != f) continue;
    const items::Item& it = items_->items[k];
    sema::Sema::ClosureMember m =
        sema::Sema::closure_member_def(*w_, c, f, c.call == f ? c.body : c.invoker_body, w_->ins.seqs.elements(c.locals));
    irgen::FunctionInput input{f, m.decl, m.body, id(k), it.body_begin - it.begin + c.body_begin, false, {}, m.this_type};
    AtItem q(*this, k, true);
    sema::EvalFacts facts(*w_, q);
    std::optional<irgen::FunctionIr> ir;
    if (auto x = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*x);
    auto& slot = closure_irs_[key] = std::move(ir);
    if (!slot) r.unsupported = true;
    else r.body = &slot->body;
    return r;
  }
  r.unsupported = true;  // its lambda is not in that body: an evaluation cannot go on
  return r;
}

sema::GenericResult Program::generic_instance(EntityId spec, const sema::GenericClosure* known, u32 point) {
  const u32 key = std::to_underlying(spec);
  const sema::EntityRec sr = w_->entities.rec(spec);
  if (auto it = instance_index_.find(key); it != instance_index_.end()) {
    const Instance& in = instances_[it->second];
    if (in.failed || !in.body.value.deduced) return {std::nullopt, true};
    return {in.body.value.deduced_type};
  }
  // Called in its own body: known if its return type is declared, not deduced.
  if (instantiating_.contains(key))
    return {w_->types.has_placeholder(w_->types.rec(sr.type).inner) ? std::nullopt : std::optional(sr.type)};
  EntityId closure = w_->entities.rec(sr.templ).scope;
  EntityId fn = w_->entities.rec(closure).scope;
  // The body that defines the lambda: one being analysed, else a definition before the point or
  // a specialization's instance.
  std::optional<u32> k;
  std::optional<Instantiation> enclosing;
  std::optional<sema::GenericClosure> g;
  if (!known)
    if (auto d = generic_defs_.find(std::to_underlying(sr.templ)); d != generic_defs_.end()) known = &d->second;
  if (known) {
    g = *known;
    // The body being analysed that has it: its function's, or an enclosing lambda's
    // specialization's, or, past enclosing lambdas' operators, the function's.
    for (EntityId e = fn; !k;) {
      for (auto r = running_bodies_.rbegin(); r != running_bodies_.rend() && !k; ++r)
        if (r->function == e) {
          k = r->item;
          enclosing = r->inst;
        }
      if (k || !sema::Sema::closure_member(*w_, e)) break;
      e = w_->entities.rec(w_->entities.rec(e).scope).scope;
    }
  }
  if (!k) {
    const ast::BodySema* body = nullptr;
    // In a lambda: in the instance of an enclosing generic lambda's operator, or in the body of
    // the function the enclosing lambdas are in.
    if (sema::Sema::closure_member(*w_, fn)) {
      if (auto it = instance_index_.find(std::to_underlying(fn)); it != instance_index_.end()) {
        const Instance& oi = instances_[it->second];
        body = &oi.body.value;
        k = oi.item;
        enclosing = oi.enclosing;
      } else {
        while (sema::Sema::closure_member(*w_, fn)) fn = w_->entities.rec(w_->entities.rec(fn).scope).scope;
      }
    }
    if (body) {
    } else if (const sema::EntityRec& fr = w_->entities.rec(fn); fr.templ != EntityId::none || member_patterns_.contains(std::to_underlying(fn))) {
      const Instance* in = instance(fn, point);
      if (!in || in->failed) return {std::nullopt, in != nullptr};
      k = in->item;
      body = &in->body.value;
      if (fr.templ != EntityId::none) {
        ast::DeclSema t = w_->ins.seqs.at(item_sema(*k).value.decls, 0);
        std::vector<ast::TemplateParamSema> head = w_->ins.seqs.elements(t.template_params);
        enclosing = Instantiation{in->decl, sema::Bindings::of(head.empty() ? 0 : head[0].depth, fr.args)};
      } else {
        enclosing = Instantiation{in->decl, class_args_.at(std::to_underlying(member_patterns_.at(std::to_underlying(fn)).spec))};
      }
    } else {
      auto def = definitions_.find(std::to_underlying(fn));
      if (def == definitions_.end() || def->second >= point) return {};
      k = def->second;
      body = &analyze_body(*k).value;
    }
    for (const ast::ClosureDef& c : w_->ins.seqs.elements(body->closures)) {
      if (c.call != sr.templ) continue;
      sema::GenericClosure x;
      x.closure = c.closure;
      x.call = c.call;
      x.call_type = c.call_type;
      x.head = w_->ins.seqs.elements(c.template_params);
      std::vector<ast::LocalRec> locals = w_->ins.seqs.elements(c.locals);
      for (std::size_t i = 0; i < w_->types.rec(c.call_type).params.size() && i < locals.size(); ++i) x.params.push_back(locals[i].name);
      x.body_begin = c.body_begin;
      x.flags = c.flags;
      x.consts = w_->ins.seqs.elements(c.consts);
      x.captures = w_->ins.seqs.elements(c.captures);
      for (const ast::ExpansionRec& e : w_->ins.seqs.elements(body->expansions)) {
        if (e.begin < c.body_begin || e.begin >= c.end) continue;
        sema::Expansion xe{e.begin, e.end, {}};
        for (const ast::PackRefRec& pr : w_->ins.seqs.elements(e.packs)) xe.packs.push_back({pr.depth, pr.index, pr.fn});
        x.expansions.push_back(std::move(xe));
      }
      x.const_values = w_->ins.seqs.elements(c.const_values);
      g = std::move(x);
    }
  }
  if (!k || !g) return {};
  // Reported at the lambda's body, which nests ([temp.inst]/16).
  if (too_deep(instantiating_.size(), *k, items_->items[*k].body_begin - items_->items[*k].begin + g->body_begin))
    return {std::nullopt, true};
  instantiating_.insert(key);
  Instance in{spec, *k, {}, {}, false, enclosing};
  in.body = BodyAnalysis(*this, *k, enclosing, GenericInst{*g, spec, sr.type, sr.args}).run();
  instantiating_.erase(key);
  in.failed = !in.body.diags.empty() || !in.body.value.deduced;
  // Its declaration, as the closure's other members': a member, inline and constexpr when it can be.
  ast::DeclSema d{};
  d.entity = spec;
  d.type = in.body.value.deduced ? in.body.value.deduced_type : sr.type;
  d.kind = ast::DeclKind::function;
  d.linkage = sr.linkage;
  d.flags = sema::u16(sema::decl_flag::member | sema::decl_flag::definition | sema::decl_flag::inline_);
  d.attributes = w_->ins.intern_seq(std::span<const NodeId>{});
  d.constraint = NodeId::none;
  d.constraints = ast::ConstId::none;
  d.contracts = NodeId::none;
  std::vector<ast::ParamSema> params;
  std::vector<ast::LocalRec> locals = w_->ins.seqs.elements(in.body.value.locals);
  for (std::size_t i = 0; i < w_->types.rec(d.type).params.size() && i < locals.size(); ++i)
    params.push_back({EntityId::none, locals[i].type, locals[i].name, NodeId::none, 0});
  d.params = w_->ins.intern_seq(std::span<const ast::ParamSema>(params));
  d.template_params = w_->ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  in.decl = d;
  instance_index_.emplace(key, instances_.size());
  instances_.push_back(std::move(in));
  instance_order_.push_back(&instances_.back());
  const Instance& r = instances_.back();
  if (r.failed) return {std::nullopt, !r.body.diags.empty()};
  return {r.body.value.deduced_type};
}

std::optional<QualType> Program::deduced_type(EntityId f, u32 item) {
  // A function template specialization's is its instantiation's, instantiated where it is needed
  // ([temp.inst]/8); its template must be defined before the point.
  if (EntityId templ = w_->entities.rec(f).templ; templ != EntityId::none && !explicitly_specialized(f, item)) {
    auto def = definitions_.find(std::to_underlying(templ));
    if (def == definitions_.end() || def->second >= item) return std::nullopt;
    const Instance* in = instance(f, item);
    if (!in || in->failed || !in->body.value.deduced) return std::nullopt;
    return in->body.value.deduced_type;
  }
  // A member function of a class template specialization, instantiated likewise.
  if (member_patterns_.contains(std::to_underlying(f))) {
    const Instance* in = instance(f, item);
    if (!in || in->failed || !in->body.value.deduced) return std::nullopt;
    return in->body.value.deduced_type;
  }
  auto def = definitions_.find(std::to_underlying(f));
  if (def == definitions_.end() || def->second >= item || !friend_body_ready(def->second)) return std::nullopt;
  const Result<ast::BodySema>& body = analyze_body(def->second);
  if (!body.value.deduced) return std::nullopt;
  return body.value.deduced_type;
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
    else {
      pp::Session& ps = pp::session();
      std::lock_guard lock(ps.mutex);
      pp_messages.push_back(pp::message(ps, ps.files.id(pre.region.files[d.file]), pre.region.file_names[d.file], d));
    }
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
