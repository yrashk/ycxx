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

// The parser skips attributes whose layout semantics we do not implement. Find those
// that could change a record or field even though its computed offsets look natural.
enum class LayoutAttributes { affecting_layout, leading_affecting_layout, trailing_definition, any };

bool layout_attribute(const pp::Region& region, u32 begin, u32 end, LayoutAttributes mode = LayoutAttributes::affecting_layout,
                      std::span<const std::pair<u32, u32>> applied = {},
                      std::string_view standard_attribute = {}) {
  using enum lex::Tk;
  for (std::uint32_t i = begin; i < end; ++i) {
    while (!applied.empty() && applied.front().second <= i) applied = applied.subspan(1);
    if (!applied.empty() && applied.front().first <= i) {
      i = applied.front().second - 1;
      continue;
    }
    auto tok = region.tokens[i];
    if (tok.kind == kw_alignas && standard_attribute.empty()) return true;
    if (i + 1 == end) break;
    const auto next = region.tokens[i + 1].kind;
    bool bracket = tok.kind == l_square && next == l_square;
    bool gnu = tok.kind == identifier && next == l_paren && region.name(tok) == "__attribute__";
    if (!bracket && !gnu) {
      if (mode == LayoutAttributes::leading_affecting_layout) return false;
      continue;
    }
    // An annotation payload does not participate in layout, even if it names packed or aligned.
    const bool annotation = bracket && i + 2 < end && region.tokens[i + 2].kind == equal;
    if (mode == LayoutAttributes::any && !annotation) return true;
    const bool unscoped = !standard_attribute.empty() && bracket && i + 2 < end &&
                          region.tokens[i + 2].kind != kw_using;
    if (gnu) ++i;
    const auto open = bracket ? l_square : l_paren;
    const auto close = bracket ? r_square : r_paren;
    std::uint32_t depth = 1;
    std::uint32_t parentheses = 0;
    while (++i < end && depth) {
      auto part = region.tokens[i];
      if (part.kind == open) ++depth;
      else if (part.kind == close && --depth == 0) break;
      else if (unscoped && part.kind == l_paren) ++parentheses;
      else if (unscoped && part.kind == r_paren && parentheses) --parentheses;
      else if (part.kind == identifier && !annotation) {
        auto name = region.name(part);
        if (!standard_attribute.empty()) {
          const auto previous = region.tokens[i - 1].kind;
          const auto following = i + 1 < end ? region.tokens[i + 1].kind : eof;
          if (unscoped && depth == 2 && parentheses == 0 &&
              (previous == l_square || previous == comma) &&
              (following == r_square || following == comma) && name == standard_attribute) return true;
          continue;
        }
        if (name.size() > 4 && name.starts_with("__") && name.ends_with("__"))
          name = name.substr(2, name.size() - 4);
        if (mode == LayoutAttributes::trailing_definition && gnu && depth == 2 &&
            (region.tokens[i - 1].kind == l_paren || region.tokens[i - 1].kind == comma) &&
            (name == "copy" || name == "trivial_abi")) return true;
        if (name == "packed" || name == "aligned" || name == "mode" || name == "vector_size" ||
            name == "no_unique_address" || name == "ms_struct" || name == "gcc_struct") return true;
      }
    }
  }
  return false;
}

bool layout_may_be_packed(const pp::Region& region, u32 begin, u32 end) {
  const auto& ranges = region.possibly_packed;
  auto pack = std::ranges::upper_bound(ranges, begin, {}, [](const auto& range) { return range.second; });
  return pack != ranges.end() && pack->first < end;
}

bool record_layout_attribute(const pp::Region& region, const items::Item& item, std::span<const std::pair<u32, u32>> applied) {
  using enum LayoutAttributes;
  // An enum-head's attributes and those right after its closing brace apply to the enumeration.
  if (item.kind != ItemKind::class_def && item.kind != ItemKind::enum_def)
    return layout_attribute(region, item.begin, item.end, affecting_layout, applied);
  return layout_attribute(region, item.begin, item.brace_begin, affecting_layout, applied) ||
         layout_attribute(region, item.brace_end, item.end, leading_affecting_layout);
}

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

// A unit's items as a Program reads them (#947): every item it reads is recorded in `reads`, for
// the trace of what a group's analysis depended on.
class ItemsView {
 public:
  explicit ItemsView(const items::Items& its) : its_(&its) {}
  ItemsView(const ItemsView&) = delete;
  struct List {
    const ItemsView* view;
    const items::Item& operator[](std::size_t k) const {
      if (view->reads) (*view->reads)[k] = 1;
      return view->its_->items[k];
    }
    std::size_t size() const { return view->its_->items.size(); }
  };
  const List items{this};
  const items::Items& unlogged() const { return *its_; }
  std::vector<std::uint8_t>* reads = nullptr;  // by item: read

 private:
  const items::Items* its_;
};

class Program {
public:
  struct Index;
  // The source configuration supplies the instantiation limit and language modes.
  // `index`, when given, is `make_index` of the same world, region and items, shared (#947).
  Program(sema::World& w, const pp::Region& r, const items::Items& its, const pp::Config& config = {},
          std::shared_ptr<const Index> index = nullptr)
      : instantiation_limit(config.template_depth), exceptions_enabled_(config.exceptions_enabled),
        rtti_enabled_(config.rtti_enabled), compatibility_(config.compatibility), w_(&w), r_(&r),
        view_(its), items_(&view_), ix_(index ? std::move(index) : make_index(w, r, its)) {
    w.install_builtin_classes(classes_);
    may_have_virtual_layouts_ = classes_.has_virtual_layouts() || ix_->says_virtual;
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
  // The scope of the outermost class enclosing class `cls` ([class.nest]), through ordinary
  // classes only: a namespace for a class at namespace scope or nested in one (#306).
  EntityId outermost_class_scope(EntityId cls) const {
    EntityId scope = w_->entities.rec(cls).scope;
    while (scope != EntityId::none && w_->entities.rec(scope).kind == sema::EntityKind::class_ &&
           w_->entities.rec(scope).templ == EntityId::none)
      scope = w_->entities.rec(scope).scope;
    return scope;
  }
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
  struct InlineNamespace { std::string path; u32 item; };
  // What every Program over one unit's items shares (#947): facts of the items and tokens alone,
  // made once, so a Program that analyses one group of items costs what the group does.
  struct Index {
    std::vector<ItemId> ids;
    std::unordered_map<u32, u32> index;  // by ItemId: its item
    // By declared name: (item, index in its names), in order; a lookup visits only its own name's.
    std::unordered_map<std::string_view, std::vector<std::pair<u32, u32>>> by_name;
    std::vector<std::string> ns_of, eff_ns;  // by item
    std::vector<ast::GnuVisibility> namespace_visibility;  // by item: its enclosing namespaces' attribute
    // A namespace definition's components' namespaces, as declared and for lookup, by name
    // (`namespace a::b` declares `a` in the enclosing namespace and `b` in `a`).
    std::map<u32, std::vector<std::pair<std::string, std::string>>> name_scopes;
    std::vector<std::string> inner_of;  // by namespace definition item: the namespace
    std::vector<bool> in_class;         // by item
    std::map<std::string, std::vector<InlineNamespace>> inline_namespaces;  // by enclosing namespace
    std::map<std::string, std::vector<InlineNamespace>> unnamed_namespaces;  // implicit using-directives
    std::vector<Directive> directives;  // their targets unresolved
    bool says_virtual = false;          // the unit's tokens hold `virtual`
    std::vector<bool> qualified;        // by item: its first declarator's name is qualified
    std::vector<bool> name_scoped;      // by item: it has `name_scopes`
    std::vector<u32> specializations;   // the items outside classes that declare specializations
    // `by_name` by (name, namespace): as declared (`name_ns`), for lookup (`name_eff_ns`), and the
    // item's (`ns_of`); and the qualified declarations, by name. For answering questions (`answer`).
    using Scoped = std::map<std::pair<std::string_view, std::string_view>, std::vector<std::pair<u32, u32>>>;
    Scoped by_scope, by_lookup_scope, by_item_scope;
    std::map<std::string_view, std::vector<std::pair<u32, u32>>> qualified_by_name;
  };
  // Each item's namespace, from the namespace definitions that enclose it; and the using-
  // directives and aliases at namespace scope, whose targets are resolved when their items are
  // analysed.
  static std::shared_ptr<const Index> make_index(sema::World& w, const pp::Region& r, const items::Items& items) {
    auto out = std::make_shared<Index>();
    Index& x = *out;
    for (const items::Item& it : items.items) {
      ItemId id = w.ins.items.add<ItemId>(it.id);
      x.ids.push_back(id);
      x.index.emplace(std::to_underlying(id), static_cast<u32>(x.ids.size() - 1));
    }
    for (u32 j = 0; j < items.items.size(); ++j)
      for (u32 k = 0; k < items.items[j].names.size(); ++k)
        if (!items.items[j].names[k].name.empty()) x.by_name[items.items[j].names[k].name].push_back({j, k});
    x.says_virtual = std::ranges::contains(r.tokens, lex::Tk::kw_virtual, &lex::Tok::kind);
    for (const items::Item& it : items.items) x.qualified.push_back(it.qualifier_begin < it.qualifier_end);
    const auto& its = items.items;
    x.ns_of.assign(its.size(), "");
    x.eff_ns.assign(its.size(), "");
    x.in_class.assign(its.size(), false);
    x.inner_of.assign(its.size(), "");
    x.namespace_visibility.assign(its.size(), ast::GnuVisibility::none);
    std::vector<std::string> inner_eff(its.size());
    for (u32 k = 0; k < its.size(); ++k) {  // preorder: a parent comes first
      u32 p = its[k].parent;
      if (p != items::none) {
        x.in_class[k] = x.in_class[p] || its[p].kind == ItemKind::class_def || its[p].kind == ItemKind::enum_def;
        bool ns = its[p].kind == ItemKind::namespace_def;
        x.ns_of[k] = ns ? x.inner_of[p] : x.ns_of[p];
        x.eff_ns[k] = ns ? inner_eff[p] : x.eff_ns[p];
        x.namespace_visibility[k] = x.namespace_visibility[p];
      }
      if (its[k].kind != ItemKind::namespace_def) continue;
      // The identifiers of `namespace a::inline b`, or none for an unnamed one; `inline` before
      // a component makes it transparent to lookup ([namespace.def]/7). Component i is declared
      // in the namespace the ones before it name.
      std::vector<std::pair<std::string, bool>> names;
      bool is_inline = false;
      bool after_namespace = false;
      DeclAttributes attributes;
      for (u32 t = its[k].begin; t < its[k].brace_begin; ++t) {
        // Past attributes, `[[…]]` and `__attribute__((…))`.
        if (r.tokens[t].kind == lex::Tk::l_square || r.tokens[t].kind == lex::Tk::l_paren) {
          const u32 begin = t;
          const bool scoped = r.tokens[t].kind == lex::Tk::l_square;
          int depth = 0;
          for (; t < its[k].brace_begin; ++t) {
            lex::Tk x = r.tokens[t].kind;
            depth += (x == lex::Tk::l_square || x == lex::Tk::l_paren) - (x == lex::Tk::r_square || x == lex::Tk::r_paren);
            if (depth == 0) break;
          }
          if (after_namespace && depth == 0 &&
              (scoped || (begin > its[k].begin && r.tokens[begin - 1].kind == lex::Tk::identifier &&
                          r.name(r.tokens[begin - 1]) == "__attribute__")))
            Parser::read_decl_attributes(attributes, r, begin, t + 1, scoped);
          continue;
        }
        if (r.tokens[t].kind == lex::Tk::identifier && t + 1 < its[k].brace_begin && r.tokens[t + 1].kind == lex::Tk::l_paren)
          continue;  // `__attribute__`, whose parentheses come next
        if (r.tokens[t].kind == lex::Tk::kw_namespace) after_namespace = true;
        if (r.tokens[t].kind == lex::Tk::kw_inline) is_inline = true;
        if (r.tokens[t].kind == lex::Tk::identifier) {
          names.push_back({std::string(r.name(r.tokens[t])), is_inline});
          is_inline = false;
        }
      }
      if (attributes.visibility != ast::GnuVisibility::none) x.namespace_visibility[k] = attributes.visibility;
      bool anonymous = names.empty();
      if (anonymous) names.push_back({"(anonymous)", true});
      x.inner_of[k] = x.ns_of[k];
      inner_eff[k] = x.eff_ns[k];
      for (const auto& [n, inl] : names) {
        const std::string parent = x.inner_of[k];
        if (!anonymous) x.name_scopes[k].push_back({parent, inner_eff[k]});
        x.inner_of[k] += (x.inner_of[k].empty() ? "" : "::") + n;
        // An unnamed namespace is transparent to ordinary lookup even without `inline`.
        // Only explicitly inline namespaces form the set used by ADL.
        if (anonymous ? is_inline : inl) {
          auto& children = x.inline_namespaces[parent];
          if (!std::ranges::contains(children, x.inner_of[k], &InlineNamespace::path))
            children.push_back({x.inner_of[k], k});
        } else if (anonymous) {
          auto& children = x.unnamed_namespaces[parent];
          if (!std::ranges::contains(children, x.inner_of[k], &InlineNamespace::path))
            children.push_back({x.inner_of[k], k});
        }
        if (!inl) inner_eff[k] += (inner_eff[k].empty() ? "" : "::") + n;
      }
    }
    for (u32 k = 0; k < its.size(); ++k) {
      if (x.in_class[k] || its[k].kind != ItemKind::declaration) continue;
      lex::Tk a = r.tokens[its[k].begin].kind;
      lex::Tk b = its[k].begin + 1 < its[k].end ? r.tokens[its[k].begin + 1].kind : lex::Tk::eof;
      if (a == lex::Tk::kw_using && b == lex::Tk::kw_namespace) x.directives.push_back({k, x.ns_of[k], std::nullopt, std::nullopt});
      if (a == lex::Tk::kw_namespace && b == lex::Tk::identifier)
        x.directives.push_back({k, x.ns_of[k], w.ins.names.intern(r.name(r.tokens[its[k].begin + 1])), std::nullopt});
    }
    x.name_scoped.assign(items.items.size(), false);
    for (const auto& [k, scopes] : x.name_scopes) x.name_scoped[k] = true;
    for (const auto& [name, entries] : x.by_name)
      for (auto [j, k] : entries) {
        x.by_scope[{name, name_ns(x, j, k)}].push_back({j, k});
        x.by_lookup_scope[{name, name_eff_ns(x, j, k)}].push_back({j, k});
        x.by_item_scope[{name, x.ns_of[j]}].push_back({j, k});
        if (x.qualified[j]) x.qualified_by_name[name].push_back({j, k});
      }
    for (u32 k = 0; k < items.items.size(); ++k) {
      const items::Item& it = items.items[k];
      if (!x.in_class[k] && (it.specialization || std::ranges::any_of(it.names, [](const items::DeclName& n) {
                               return (n.flags & items::name_flag::specialization) != 0;
                             })))
        x.specializations.push_back(k);
    }
    return out;
  }
  void set_directive_target(u32 item, std::string target) {
    for (Directive& d : directives_)
      if (d.item == item) d.target = std::move(target);
  }

  sema::World& world() { return *w_; }
  const pp::Region& region() const { return *r_; }
  bool exceptions_enabled() const { return exceptions_enabled_; }
  bool rtti_enabled() const { return rtti_enabled_; }
  ast::Compatibility compatibility() const { return compatibility_; }
  const ItemsView& items() const { return *items_; }
  // Records the items this Program reads into `reads` (#947).
  void record_reads(std::vector<std::uint8_t>* reads) {
    if (reads) reads->assign(items_->items.size(), 0);
    view_.reads = reads;
  }
  ItemId id(u32 item) const { return ids_[item]; }
  u32 index(ItemId id) const { return index_.at(std::to_underlying(id)); }

  const Result<ast::ItemSema>& item_sema(u32 item);
  std::optional<ast::DeclSema> decl_sema(DeclId d);
  std::optional<ast::DeclSema> friend_decl(const ast::MemberRec& f);
  std::optional<ast::DeclSema> class_friend(EntityId f);
  bool unresolved_special_member_exception(EntityId entity) const {
    const auto resolved = special_member_exception_types_.find(entity);
    return (resolved != special_member_exception_types_.end() && !resolved->second) ||
           special_member_exception_tokens_.contains(entity);
  }
  ast::DeclSema completed_declaration(ast::DeclSema decl) const {
    if (auto found = special_member_exception_types_.find(decl.entity);
        found != special_member_exception_types_.end() && found->second)
      decl.type = *found->second;
    if (auto found = defaulted_comparisons_.find(decl.entity); found != defaulted_comparisons_.end()) {
      decl.type = found->second.type;
      if (found->second.deleted) decl.flags |= sema::decl_flag::deleted;
    }
    return decl;
  }
  const Result<ast::BodySema>& analyze_body(u32 item);

  bool defaulted_body(u32 item) const {
    const auto& it = items_->items[item];
    return it.body_end - it.body_begin == 3 &&
           r_->tokens[it.body_begin].kind == lex::Tk::equal &&
           r_->tokens[it.body_begin + 1].kind == lex::Tk::kw_default &&
           r_->tokens[it.body_begin + 2].kind == lex::Tk::semi;
  }
  // A defaulted comparison operator function ([class.compare.default]) of a class completed here.
  bool defaulted_comparison(EntityId f) const { return defaulted_comparisons_.contains(f); }
  // [class.eq]: the result of defaulted comparison `f`'s implicit definition, whose parameters are
  // the locals `params` (the class's members compared with the object parameter's first).
  std::optional<Ex> defaulted_comparison_result(sema::Sema& sm, const ast::DeclSema& f, std::span<const u32> params);
  // An accepted in-class defaulted special member has a definition and no token body to analyse.
  bool defaulted_member_definition(u32 item, const ast::DeclSema& d) const {
    const auto& it = items_->items[item];
    if (d.entity == EntityId::none || !(d.flags & sema::decl_flag::member) ||
        it.parent == items::none || items_->items[it.parent].kind != ItemKind::class_def || !defaulted_body(item))
      return false;
    return (d.flags & (sema::decl_flag::constructor | sema::decl_flag::destructor)) ||
           (d.kind == ast::DeclKind::function && w_->ins.names.spelling(w_->entities.rec(d.entity).name) == "operator=");
  }
  // `class_def(C)` (ast.md §3.6) of the class item `item`: the records of its members, each its
  // own item, analysed at the class's prefix (§4.2). It also fills this unit's class table
  // (`classes`), which member lookup and the layout read until #33 has them.
  const Result<ast::ClassDef>& class_def(u32 item);
  // The records of the classes and enumerations this unit has analysed.
  const sema::Classes& classes() const { return classes_; }
  // Class and enumeration layouts, and optionally constructor and destructor signatures,
  // affected by annotations erased from their types.
  std::set<EntityId> uncertain_object_layouts(bool include_lifecycle_signatures = false) const;
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
  // ABI attributes on declarations outside the entity's ordinary declaration records.
  bool has_unapplied_declaration_attributes(EntityId e) const { return unapplied_declaration_attributes_.contains(e); }
  bool class_rtti_declaration_has_unapplied_abi(EntityId cls, u32 item, bool friend_declaration) const;
  bool peer_root_vtable_visible(EntityId cls) const;
  bool plain_class_template_type(EntityId cls) const;
  bool plain_class_template_function(EntityId pattern, u32 definition) const;
  // Empty on unsupported declarations, or EntityId::none when a peer owns the derived table.
  std::optional<EntityId> automatic_derived_vtable_key(EntityId cls) const;
  bool peer_pure_dispatch(EntityId cls) const;
  bool has_typed_dynamic_cast() const { return has_typed_dynamic_cast_; }
  enum class TypedCastRtti { unsupported, imported, keyless_base };
  TypedCastRtti peer_typed_dynamic_cast(EntityId source, EntityId target) const;
  // A fieldless virtual root whose destructor may be emitted against a peer's table (#136).
  bool peer_root_destructor_producer(EntityId cls) const;
  bool peer_root_destructor_observer(EntityId function, EntityId cls) const;
  struct DerivedVirtualDestructor {
    EntityId base;
    EntityId destructor;
    EntityId base_destructor;
    u64 size;
    bool owns_derived_key = false;
  };
  std::optional<DerivedVirtualDestructor> derived_virtual_destructor_ownership(EntityId cls) const;
  // Whether an ordinary written definition can also supply its ELF base-object entry point (#136).
  bool supports_base_object_variant(EntityId function, const std::set<EntityId>& uncertain_layouts,
                                    std::vector<EntityId>* layout_inputs = nullptr) const;

  // The parsed class-key of a defining item ([class.union]).
  bool class_rec_is_union(u32 item) const {
    auto head = class_heads_.find(item);
    return head != class_heads_.end() && classes_.is_union_class(head->second.entity) == true;
  }
  // An entity's GNU asm label: its symbol as written (docs/EXTENSIONS.md); empty if it has none.
  std::string asm_label(EntityId e) const {
    auto it = asm_labels_.find(std::to_underlying(e));
    return it == asm_labels_.end() ? std::string() : it->second.spelling;
  }
  std::optional<ast::Loc> asm_label_site(EntityId e) const {
    auto it = asm_labels_.find(std::to_underlying(e));
    if (it == asm_labels_.end()) return std::nullopt;
    return ast::Loc{id(it->second.item), it->second.offset};
  }
  // Whether class `c` is complete at `item` ([class.mem.general]): its definition is an earlier
  // item that does not enclose `item`.
  // A function body inside the class is a complete-class context ([class.mem.general]); past
  // the closing brace of the class an item defines (`own`), that class is complete too.
  bool is_complete(EntityId c, u32 item, bool body = false, bool own = false) {
    // A closure is complete once its class record is published. It has no source declaration
    // to look up, and the variable owning a namespace lambda is still being deduced here.
    if (const auto* entity = w_->entities.find(c);
        entity && entity->kind == sema::EntityKind::class_ && !entity->unnamed_type_ordinal &&
        w_->ins.names.spelling(entity->name).empty())
      return classes_.is_defined(c);
    EntityId templ = w_->entities.rec(c).templ;
    if (templ == EntityId::none && class_patterns_.contains(std::to_underlying(c))) {
      const sema::ClassRec* r = instantiate_member_class(c, item);
      return r && r->defined;
    }
    ensure_declared(c, item);  // its definition, if an earlier item has it
    if (templ != EntityId::none) specializations_before(item);
    auto it = class_items_.find(std::to_underlying(c));
    // A class template specialization is instantiated where it must be complete ([temp.inst]/2),
    // after its template's definition, unless it is explicitly specialized ([temp.expl.spec]).
    if (it == class_items_.end()) {
      // Built-in classes are complete from the start.
      if (templ == EntityId::none && classes_.is_defined(c)) return true;
      if (templ == EntityId::none || explicitly_specialized(c, item)) return false;
      const sema::ClassRec* r = instantiate_class(c, item);
      return r && r->defined;
    }
    u32 d = it->second;
    bool inside = item > d && item <= last_member(d);
    // A class nested in the one being completed was defined before its closing brace; a member
    // body is a complete-class context of every enclosing class ([class.mem.general]/8).
    const bool nested_before_close = item < d && ((own && d <= last_member(item)) || (body && within_outer_class(d, item)));
    if ((item < d && !nested_before_close) || (item == d && !own) || (inside && !body)) return false;
    if (classes_.is_defined(c)) return true;
    // Not while it is being defined: classifying a defaulted special member of a class instantiated
    // there can ask.
    if (std::ranges::find(running_, class_def_key(d)) != running_.end()) return false;
    class_def(d);
    return classes_.is_defined(c);
  }
  // The outermost class that encloses item `item`, or none ([class.nest]).
  u32 outer_class(u32 item) const {
    u32 outer = items::none;
    for (u32 p = items_->items[item].parent; p != items::none && items_->items[p].kind == ItemKind::class_def;
         p = items_->items[p].parent)
      outer = p;
    return outer;
  }
  // Whether item `d` is nested in the outermost class that encloses item `item`.
  bool within_outer_class(u32 d, u32 item) const {
    const u32 outer = outer_class(item);
    return outer != items::none && d > outer && d <= last_member(outer);
  }
  // Whether the outermost class that encloses item `item` is being defined: its definition or one
  // of its members' declarations is being analysed.
  bool defining_outer_class(u32 item) const {
    const u32 outer = outer_class(item);
    return outer != items::none && std::ranges::any_of(running_, [&](u32 key) {
             const u32 k = key & ~class_def_key(0);
             return k >= outer && k <= last_member(outer);
           });
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
  // A friend class-template declaration stops at the starting namespace ([dcl.type.elab]).
  enum class LookupNames { all, types, qualifiers, using_declaration, immediate_call };

  void filter_lookup(std::vector<DeclId>& declarations, LookupNames filter) {
    if (filter == LookupNames::all || filter == LookupNames::using_declaration ||
        filter == LookupNames::immediate_call) return;
    std::erase_if(declarations, [&](DeclId id) {
      auto d = decl_sema(id);
      return !d || d->entity == EntityId::none ||
             (!sema::Sema::is_type(d->kind) && !(filter == LookupNames::qualifiers && d->kind == ast::DeclKind::namespace_));
    });
  }

  std::vector<DeclId> visible_decls(std::string_view name, u32 item, bool self, const std::string& from,
                                    std::span<const std::string> block = {}, LookupNames filter = LookupNames::all,
                                    bool innermost_only = false) {
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
    auto declarations = [&](const std::string& path) {
      return declared_in(name, path, item, self, /*inhabit=*/false, /*bound_functions=*/false,
                         /*hide_function_tags=*/filter == LookupNames::immediate_call);
    };
    for (std::string level = from;; level = parent_of(level)) {
      std::vector<DeclId> r = declarations(level);
      for (const auto& [t, at] : nominated)
        if (at == level) {
          std::vector<DeclId> more = declarations(t);
          r.insert(r.end(), more.begin(), more.end());
        }
      filter_lookup(r, filter);
      if (!r.empty() || level.empty() || innermost_only) return r;
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
  // A by-name question of the unit's items (#947): the items that declare `name` and pass
  // `kind`'s filter for `path`, which reads only the index. A group's trace keeps the questions
  // its analysis asked; it reads the items that pass through the items view.
  enum class Probe : std::uint8_t { declared_in, declared_in_inhabit, namespace_members, class_members, same_namespace, declares };
  struct NameProbe {
    Probe kind;
    std::string name;
    std::string path;  // "*": any namespace (`declares` a C function)
    auto operator<=>(const NameProbe&) const = default;
  };
  static const std::string& name_ns(const Index& x, u32 item, u32 name) {
    if (!x.name_scoped[item]) return x.ns_of[item];
    auto it = x.name_scopes.find(item);
    return it != x.name_scopes.end() && name < it->second.size() ? it->second[name].first : x.ns_of[item];
  }
  static const std::string& name_eff_ns(const Index& x, u32 item, u32 name) {
    if (!x.name_scoped[item]) return x.eff_ns[item];
    auto it = x.name_scopes.find(item);
    return it != x.name_scopes.end() && name < it->second.size() ? it->second[name].second : x.eff_ns[item];
  }
  static bool passes(const Index& x, Probe kind, const std::string& path, u32 j, u32 k) {
    switch (kind) {
      case Probe::declared_in: return name_ns(x, j, k) == path || name_eff_ns(x, j, k) == path;
      case Probe::declared_in_inhabit: return name_ns(x, j, k) == path;
      case Probe::namespace_members: return !x.in_class[j] && x.ns_of[j] == path;
      case Probe::class_members: return x.in_class[j] && x.ns_of[j] == path;
      case Probe::same_namespace: return x.ns_of[j] == path;
      case Probe::declares:
        return path == "*" || name_ns(x, j, k) == path || name_eff_ns(x, j, k) == path ||
               (x.qualified[j] && encloses(name_ns(x, j, k), path));
    }
    return true;
  }
  // The answer to a question: the items that pass, as (item, index in its names), in item order.
  // From the index's scoped lists, the entries `passes` keeps, without visiting the others.
  static std::vector<std::pair<u32, u32>> answer(const Index& x, const NameProbe& q) {
    std::vector<std::pair<u32, u32>> out;
    auto add = [&](const Index::Scoped& scoped) {
      if (auto f = scoped.find({q.name, q.path}); f != scoped.end()) out.insert(out.end(), f->second.begin(), f->second.end());
    };
    auto add_qualified = [&] {
      if (auto f = x.qualified_by_name.find(q.name); f != x.qualified_by_name.end()) out.insert(out.end(), f->second.begin(), f->second.end());
    };
    if (q.kind == Probe::declares && q.path == "*") {
      if (auto f = x.by_name.find(q.name); f != x.by_name.end()) out = f->second;
      return out;
    }
    switch (q.kind) {
      case Probe::declared_in: add(x.by_scope); add(x.by_lookup_scope); break;
      case Probe::declared_in_inhabit: add(x.by_scope); break;
      case Probe::namespace_members: case Probe::class_members: case Probe::same_namespace: add(x.by_item_scope); break;
      case Probe::declares: add(x.by_scope); add(x.by_lookup_scope); add_qualified(); break;
    }
    // In item order, once each, as `passes` keeps them.
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    std::erase_if(out, [&](std::pair<u32, u32> e) { return !passes(x, q.kind, q.path, e.first, e.second); });
    return out;
  }
  // Records the questions this Program asks into `probes` (#947).
  void record_probes(std::set<NameProbe>* probes) { probes_ = probes; }
  // Records a question before the caller walks `named(name)`, skipping each entry that fails
  // `passes` before it reads the item. Recorded whether or not any item declares the name: an
  // item that declares it later changes the answer.
  void ask(Probe kind, std::string_view name, const std::string& path) {
    if (probes_ && !(last_probe_.kind == kind && last_probe_.name == name && last_probe_.path == path)) {
      last_probe_ = {kind, std::string(name), path};
      probes_->insert(last_probe_);
    }
  }
  // Qualified lookup in namespace `ns` ([namespace.qual]/2): its own declarations, or, when it has
  // none, those of the namespaces its using-directives nominate.
  std::vector<DeclId> qualified_decls(std::string_view name, const std::string& ns, u32 item, bool self,
                                     LookupNames filter = LookupNames::all, bool direct_only = false) {
    const bool direct_first = filter == LookupNames::immediate_call || filter == LookupNames::qualifiers ||
                              filter == LookupNames::using_declaration;
    const u32 limit = item + (self ? 1 : 0);
    auto append_inline_children = [&](std::vector<std::string>& scopes, const std::string& scope) {
      if (auto children = inline_namespaces_.find(scope); children != inline_namespaces_.end())
        for (const InlineNamespace& child : children->second)
          if (child.item < limit) scopes.push_back(child.path);
    };
    auto candidates = [&](const std::string& path) {
      if (!direct_first) {
        auto declarations = declared_in(name, path, item, self);
        filter_lookup(declarations, filter);
        return declarations;
      }
      std::vector<std::string> scopes{path};
      std::vector<DeclId> declarations;
      // [namespace.qual]: only direct members and the inline namespace set precede using-directives.
      for (std::size_t i = 0; i < scopes.size(); ++i) {
        const std::string scope = scopes[i];
        auto own = declared_in(name, scope, item, self, /*inhabit=*/true, /*bound_functions=*/false,
                               /*hide_function_tags=*/filter == LookupNames::immediate_call);
        filter_lookup(own, filter);
        declarations.insert(declarations.end(), own.begin(), own.end());
        append_inline_children(scopes, scope);
      }
      return declarations;
    };
    std::vector<DeclId> r = candidates(ns);
    if (!r.empty() || direct_only) return r;
    std::vector<std::string> visited{ns};
    auto follow = [&](auto&& recurse, const std::string& path) -> void {
      auto nominate = [&](const std::string& target) {
        if (std::ranges::contains(visited, target)) return;
        visited.push_back(target);
        auto more = candidates(target);
        if (more.empty()) recurse(recurse, target);
        else r.insert(r.end(), more.begin(), more.end());
      };
      std::vector<std::string> scopes{path};
      for (std::size_t i = 0; i < scopes.size(); ++i) {
        const std::string scope = scopes[i];
        for (const Directive& d : directives_) {
          if (d.alias || d.item >= item || d.at != scope) continue;
          if (!d.target) item_sema(d.item);
          if (d.target) nominate(*d.target);
        }
        if (!direct_first) continue;
        if (auto children = ix_->unnamed_namespaces.find(scope); children != ix_->unnamed_namespaces.end())
          for (const InlineNamespace& child : children->second)
            if (child.item < limit) nominate(child.path);
        append_inline_children(scopes, scope);
      }
    };
    follow(follow, ns);
    return r;
  }
  // What the using-declarations that inhabit namespace `path` before `item` introduce under
  // `name`; an inline or unnamed member namespace's are not among them ([basic.scope.scope]/4).
  std::vector<DeclId> used_in(std::string_view name, const std::string& path, u32 item) {
    std::vector<DeclId> r;
    ask(Probe::namespace_members, name, path);
    for (auto [j, k] : named(name)) {
      if (j >= item) break;
      if (!Program::passes(*ix_, Probe::namespace_members, path, j, k)) continue;
      const items::DeclName& n = items_->items[j].names[k];
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
    ask(Probe::same_namespace, name, ns_of_[item]);
    for (auto [j, k] : named(name)) {
      if (j >= item) break;
      if (!Program::passes(*ix_, Probe::same_namespace, ns_of_[item], j, k)) continue;
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
  // A friend in a class template, or a partial specialization, of a namespace ([temp.friend]).
  bool namespace_class_template_friend(u32 item) const {
    u32 parent = items_->items[item].parent;
    return parent != items::none && items_->items[parent].kind == ItemKind::class_def &&
           in_global_scope(parent) && items_->items[parent].template_depth == 1;
  }
  // The declarations of `name` that are members of namespace `path` ([namespace.memdef]) before
  // the point: of its own definitions, and, unless `inhabit`, of its inline and unnamed members.
  // A using-declaration's name stands for what it introduces (query-engine.md, "Using-declarations").
  // The ADL search uses `bound_functions` to check where direct declarations bind their names.
  std::vector<DeclId> declared_in(std::string_view name, const std::string& path, u32 item, bool self,
                                  bool inhabit = false, bool bound_functions = false,
                                  bool hide_function_tags = false) {
    std::vector<DeclId> r;
    std::vector<std::string> binding_scopes;
    auto add = [&](DeclId declaration, const std::string& ns) {
      r.push_back(declaration);
      if (hide_function_tags) binding_scopes.push_back(ns);
    };
    ask(inhabit ? Probe::declared_in_inhabit : Probe::declared_in, name, path);
    for (auto [j, k] : named(name)) {
      if (j >= item + (self ? 1 : 0)) break;
      if (!Program::passes(*ix_, inhabit ? Probe::declared_in_inhabit : Probe::declared_in, path, j, k)) continue;
      const items::Item& it = items_->items[j];
      const items::DeclName& n = it.names[k];
      const bool nested = it.kind == ItemKind::namespace_def;  // only these have per-name scopes
      const std::string& ns = nested ? name_ns(j, k) : ns_of_[j];
      if ((in_class_[j] && !n.member_elaborated_candidate) ||
          (ns != path && (inhabit || (nested ? name_eff_ns(j, k) : eff_ns_[j]) != path))) continue;
      if (n.flags & items::name_flag::unknown) {
        if (std::ranges::find(running_, j) != running_.end()) continue;
        item_sema(j);  // resolves its targets
        if (auto t = using_targets_.find({j, n.ordinal}); t != using_targets_.end())
          for (DeclId target : t->second) add(target, ns);
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
      // A qualified class-head-name defines a member class ([class.pre]/3): it binds no name here.
      if (qualified && it.kind == ItemKind::class_def && n.ordinal == 0 && !it.template_depth && !it.specialization)
        continue;
      auto d = bound_functions || qualified || n.member_elaborated_candidate ? decl_sema(id) : std::nullopt;
      if (n.member_elaborated_candidate && (!d || d->entity == EntityId::none)) continue;
      if (qualified) {
        if (auto target = qualified_c_functions_.find({j, n.ordinal});
            target != qualified_c_functions_.end() && target->second != ns)
          continue;
        // [dcl.meaning.general]: a qualified C++ definition binds in its target scope.
        if (d && d->entity != EntityId::none && d->kind == ast::DeclKind::function &&
            !(d->flags & sema::decl_flag::lang_c) &&
            ((d->flags & sema::decl_flag::member) ||
             namespace_path(w_->entities.rec(d->entity).scope) != ns))
          continue;
      }
      if (bound_functions) {
        // A qualified definition can inhabit this namespace while binding its name elsewhere
        // ([dcl.meaning.general]). Targets of using-declarators were collected above.
        if (!d || d->entity == EntityId::none || d->kind != ast::DeclKind::function ||
            (d->flags & sema::decl_flag::member))
          continue;
        if (!(d->flags & sema::decl_flag::lang_c) && namespace_path(w_->entities.rec(d->entity).scope) != path)
          continue;
      }
      add(id, ns);
    }
    for (const Directive& d : directives_)
      if (d.alias && d.item < item + (self ? 1 : 0) && d.at == path && w_->ins.names.spelling(*d.alias) == name)
        add({ids_[d.item], ast::make_decl_ord(ast::DeclClass::syntactic, 0)}, path);
    if (!hide_function_tags) return r;

    // Inline namespace members can appear in the same lookup set without sharing a binding scope.
    std::set<std::string_view> function_scopes;
    for (std::size_t i = 0; i < r.size(); ++i)
      if (auto d = decl_sema(r[i]); d && d->kind == ast::DeclKind::function)
        function_scopes.insert(binding_scopes[i]);
    if (function_scopes.empty()) return r;

    std::vector<DeclId> visible;
    for (std::size_t i = 0; i < r.size(); ++i) {
      const auto d = decl_sema(r[i]);
      if (d && (d->kind == ast::DeclKind::class_ || d->kind == ast::DeclKind::enum_) &&
          function_scopes.contains(binding_scopes[i]))
        continue;
      visible.push_back(r[i]);
    }
    return visible;
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
  void declared(const ast::DeclSema& d, u32 item, bool replayed_class = false) {
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
      if (replayed_class) {
        class_patterns_.emplace(std::to_underlying(d.entity), item);
        return;
      }
      class_items_.emplace(std::to_underlying(d.entity), item);
      class_entities_.emplace(item, d.entity);
      class_heads_.emplace(item, d);
    }
  }
  const sema::EnumRec* defining_enum(EntityId e) const {
    for (const auto* active = defining_enum_; active; active = active->previous)
      if (active->entity == e) return &active->record;
    return nullptr;
  }
  // Template operations at the end of the unit, whose templates' declarations it knows.
  sema::Templates templates() {
    sema::Templates t(*w_, classes_);
    t.defining_enum_is_scoped = [this](EntityId e) -> std::optional<bool> {
      if (const auto* record = defining_enum(e)) return record->scoped;
      return std::nullopt;
    };
    t.plain_class_template_type = [this](EntityId c) { return plain_class_template_type(c); };
    t.template_decl = [this](EntityId e) { return declaration(e, end()); };
    t.variable_constant = [this](EntityId spec) { return constant_value(spec, end()); };
    t.variable_global = [this](EntityId spec) { return global(spec, end()); };
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
  // They are the index's list, so a group's Program reads only them (#947).
  void specializations_before(u32 item) {
    const std::vector<u32>& specs = ix_->specializations;
    for (; spec_scan_ < specs.size() && specs[spec_scan_] < item; ++spec_scan_)
      if (!std::ranges::contains(running_, specs[spec_scan_])) item_sema(specs[spec_scan_]);
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
      ConstId constraints;
    };
    std::vector<Match> matches;
    for (const Partial& p : candidates) {
      if (p.item >= point) continue;
      const ast::DeclSema decl = w_->ins.seqs.at(item_sema(p.item).value.decls, 0);
      std::vector<ast::TemplateParamSema> params = w_->ins.seqs.elements(decl.template_params);
      std::vector<ast::TemplateArg> pattern = w_->entities.rec(p.entity).args;
      auto a = t.deduce_partial(params, pattern, rec.args);
      if (!a) continue;
      auto back = substitute_partial_args(p.item, point, params, pattern, sema::Bindings::of(params[0].depth, *a));
      if (!back || !std::ranges::equal(*back, rec.args)) continue;
      // [temp.spec.partial.match]/2: the deduced arguments satisfy its associated constraints.
      if (decl.constraints != ConstId::none) {
        auto nf = t.substitute_constraint(decl.constraints, sema::Bindings::of(params[0].depth, *a));
        if (!nf || satisfied(*nf, point) != true) continue;
      }
      matches.push_back({&p, std::move(params), std::move(pattern), std::move(*a), decl.constraints});
    }
    if (matches.empty()) return std::nullopt;
    auto best = std::ranges::find_if(matches, [&](const Match& m) {
      return std::ranges::all_of(matches, [&](const Match& o) {
        return &m == &o || t.more_specialized_partial(m.params, m.pattern, o.params, o.pattern, m.constraints, o.constraints);
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
      ensure_declared(w_->entities.rec(v).templ, point);
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
    } else if (parent != items::none && items_->items[parent].kind == ItemKind::class_def) {
      return enclosing_heads(parent);
    }
    return r;
  }

  // The path of the namespace that is `scope` or encloses it, from the entities' records: a
  // namespace this Program has not met yet has no entry in `ns_paths_`.
  std::string enclosing_namespace_path(EntityId scope) const {
    while (scope != EntityId::none && w_->entities.rec(scope).kind != sema::EntityKind::namespace_)
      scope = w_->entities.rec(scope).scope;
    std::string path;
    for (; scope != EntityId::none; scope = w_->entities.rec(scope).scope) {
      std::string name(w_->ins.names.spelling(w_->entities.rec(scope).name));
      path = path.empty() ? name : name + "::" + path;
    }
    return path;
  }
  // Whether an earlier member of a class that contains item `j` is being analysed: `j`, a friend
  // declaration or a nested class's member, is read once that member is, with its class.
  bool after_running_member(u32 j) const {
    for (u32 p = items_->items[j].parent; p != items::none && items_->items[p].kind == ItemKind::class_def;
         p = items_->items[p].parent)
      if (std::ranges::any_of(running_, [&](u32 r) { return r < j && r > p && items_->items[r].parent == p && last_member(r) < j; }))
        return true;
    return false;
  }
  // Analyses the items up to `point` that may declare `e`, those that declare its name, so that
  // what `declared` recorded of it there does not depend on which items other cells analysed
  // first (§A1, #947). With every earlier item analysed (`analyze_all`) it analyses nothing.
  void ensure_declared(EntityId e, u32 point) {
    if (e == EntityId::none) return;
    u32& scanned = declared_scan_[std::to_underlying(e)];  // items before it are done
    if (scanned > point) return;
    const sema::EntityRec& r = w_->entities.rec(e);
    const std::string_view name = w_->ins.names.spelling(r.name);
    // The namespace it or its class belongs to: an item there declares it, or a qualified one in a
    // namespace enclosing it; a C function may be declared in any ([dcl.link]).
    const std::string path = r.lang == LangLinkage::c ? "*" : enclosing_namespace_path(r.scope);
    ask(Probe::declares, name, path);
    for (auto [j, k] : named(name)) {
      if (j > point) break;
      if (j < scanned || std::ranges::contains(running_, j)) continue;
      // An item is read only where it can declare `e`, so a group depends on no other (#947).
      if (Program::passes(*ix_, Probe::declares, path, j, k) && !after_running_member(j)) settle(j);
    }
    scanned = std::max(scanned, point + 1);
  }
  std::optional<ast::DeclSema> declaration(EntityId e, u32 item) {
    ensure_declared(e, item);
    auto it = decls_.find(std::to_underlying(e));
    if (it == decls_.end()) return std::nullopt;
    std::optional<ast::DeclSema> r;
    for (const auto& [k, d] : it->second)
      if (k <= item) r = completed_declaration(d);
    return r;
  }
  std::optional<ast::DeclSema> replay_function_decl(EntityId e) {
    ensure_declared(e, end());
    auto found = decls_.find(std::to_underlying(e));
    if (found == decls_.end()) return std::nullopt;
    // The entity index records declarations while their item is being built.
    // Replay must read the parameter and default-argument handles from the committed item.
    for (const auto& [item, indexed] : found->second | std::views::reverse) {
      if (!item_cells_[item]) continue;
      auto declarations = w_->ins.seqs.elements(item_cells_[item]->value.decls);
      for (const auto& decl : declarations | std::views::reverse)
        if (decl.entity == indexed.entity && decl.kind == ast::DeclKind::function)
          return completed_declaration(decl);
    }
    return std::nullopt;
  }
  bool inline_declared(EntityId entity, u32 item) {
    ensure_declared(entity, item);
    const auto declarations = decls_.find(std::to_underlying(entity));
    return declarations != decls_.end() && std::ranges::any_of(declarations->second, [&](const auto& entry) {
      const auto& [point, declaration] = entry;
      return point <= item && (declaration.flags & sema::decl_flag::inline_);
    });
  }
  // The annotations of an entity in this translation unit after analyze_all(), in source order.
  // Distinct declarations contribute separate occurrences even when their constants are equal.
  std::vector<ast::DeclarationAnnotation> declaration_annotations(EntityId entity) const {
    std::vector<ast::DeclarationAnnotation> result;
    const auto found = decls_.find(std::to_underlying(entity));
    if (found == decls_.end()) return result;
    for (const auto& [item, declaration] : found->second) {
      for (const auto& annotation : w_->ins.seqs.elements(declaration.annotations))
        result.push_back({annotation.type, annotation.value, id(item), annotation.offset});
    }
    std::ranges::stable_sort(result, {}, [&](const ast::DeclarationAnnotation& annotation) {
      return std::pair{index(annotation.item), annotation.offset};
    });
    return result;
  }
  // What `analyze_all` has done with item `k` before it goes on to later items: its declaration
  // part and, for a class at namespace scope, its definition (`class_def`). A later item that asks
  // for it lazily (#947) sees what it would have seen after `analyze_all` reached it.
  // The class is completed only when it and its members precede every item being analysed, as
  // `analyze_all` would have completed it before going on.
  const Result<ast::ItemSema>& settle(u32 k) {
    const Result<ast::ItemSema>& r = item_sema(k);
    if (items_->items[k].kind != ItemKind::class_def || !in_global_scope(k) || class_cells_[k]) return r;
    const u32 last = last_member(k);
    if (std::ranges::all_of(running_, [&](u32 running) { return (running & ~class_def_key(0)) > last; })) class_def(k);
    return r;
  }
  // A previous definition of this entity in this TU ([basic.def.odr]). C language linkage
  // can make a definition in another namespace correspond without ordinary lookup finding it.
  bool function_defined_before(EntityId e, u32 item) {
    if (auto known = definitions_.find(std::to_underlying(e)); known != definitions_.end() && known->second < item)
      return true;
    const sema::EntityRec& r = w_->entities.rec(e);
    const std::string_view name = w_->ins.names.spelling(r.name);
    const std::string path = r.lang == LangLinkage::c ? "*" : enclosing_namespace_path(r.scope);
    ask(Probe::declares, name, path);
    for (auto [j, k] : named(name)) {
      if (j >= item) break;
      if (!Program::passes(*ix_, Probe::declares, path, j, k)) continue;
      const items::Item& it = items_->items[j];
      if (!in_global_scope(j) || it.kind != ItemKind::function) continue;
      DeclId id{ids_[j], ast::make_decl_ord(ast::DeclClass::syntactic, it.names[k].ordinal)};
      if (auto d = decl_sema(id); d && d->entity == e && (d->flags & sema::decl_flag::definition)) return true;
    }
    return false;
  }
  struct MemberDemand {
    u32 item;
    bool body = true;
    bool own = true;
    EntityId completing = EntityId::none;
    auto operator<=>(const MemberDemand&) const = default;
  };
  std::optional<ConstId> constant_value(EntityId e, u32 item, std::optional<MemberDemand> member_demand = {}) {
    if (const auto* r = w_->entities.find(e); r && r->kind == sema::EntityKind::template_parameter_object)
      return r->args.front().value;
    ensure_declared(e, item);
    variable_instance(e, item);
    if (!member_value(e, member_demand.value_or(MemberDemand{item}))) return std::nullopt;
    auto it = constants_.find(std::to_underlying(e));
    if (it == constants_.end() || it->second.first > item) return std::nullopt;
    return it->second.second;
  }

  // `function_ir(F)` for a function defined in an item before `item` ([expr.const]: a function
  // defined later is not defined at the point of evaluation).
  interp::Callee function_ir(EntityId f, u32 item, bool* diagnosed_failure = nullptr);
  // A closure type's record, published by the analysis of `item`, its lambda's
  // (docs/design/closures.md), with its implicit special members ([expr.prim.lambda.closure]/14).
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r, u32 item, sema::Sema& context) {
    if (const sema::ClassRec* done = classes_.find(c)) return *done;
    special_members(item, c, r, nullptr, &context);
    return classes_.publish(c, std::move(r));
  }
  interp::Callee closure_ir(EntityId f, u32 item);
  // The closures of declaration parts (and of the generic lambdas instantiated in them) analysed
  // here, by member: their definition and the item and offset it is relative to (#338).
  struct DeclarationClosure {
    ast::ClosureDef def;
    u32 item;
    u32 begin;
    EntityId lazy_member = EntityId::none;
  };
  std::map<u32, DeclarationClosure> declaration_closures_;
  // A replayed class static initializer and the definitions it names.
  struct MemberInitializer {
    EntityId member;
    u32 item;
    ast::SeqId<ast::ClosureDef> closures;
    std::vector<sema::Demand> demands;
    bool type_only = false;
    bool native_closure_use = false;
    bool emits_closures() const { return !type_only || native_closure_use; }
  };
  std::deque<MemberInitializer> member_initializers_;
  const std::deque<MemberInitializer>& member_initializers() const { return member_initializers_; }
  void declaration_closures(ast::SeqId<ast::ClosureDef> closures, u32 item, u32 begin) {
    for (const ast::ClosureDef& c : w_->ins.seqs.elements(closures))
      for (EntityId f : {c.call, c.invoker})
        if (f != EntityId::none) declaration_closures_.insert_or_assign(std::to_underlying(f), DeclarationClosure{c, item, begin});
  }
  // A generic lambda's function call operator specialized ([temp.inst]): its body re-analysed
  // from the lambda's tokens with the arguments bound, an `Instance`; its type with the return
  // type deduced, none when it fails.
  struct GenericInst {
    sema::GenericClosure g;
    EntityId spec;
    QualType spec_type;
    std::vector<ast::TemplateArg> args;
    u32 point;
  };
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, u32 point);

  // The function type a definition before `item` deduced for `f` ([dcl.spec.auto.general]/11).
  std::optional<QualType> deduced_type(EntityId f, u32 item);
  // A namespace-scope variable as the interpreter sees it at `item`: its type and, if it is usable
  // in constant expressions, its value.
  std::optional<interp::GlobalVar> global(EntityId v, u32 item, std::optional<MemberDemand> member_demand = {}) {
    if (const auto* entity = w_->entities.find(v)) {
      if (entity->kind == sema::EntityKind::template_parameter_object)
        return interp::GlobalVar{entity->type, entity->args.front().value, true};
      if (entity->constant_object != ConstId::none)
        return interp::GlobalVar{entity->type, entity->constant_object, true};
    }
    // A static local: usable in constant expressions when const and constant-initialized
    // ([expr.const]); reading or writing another is not constant.
    if (auto s = static_locals_.find(std::to_underlying(v)); s != static_locals_.end())
      return interp::GlobalVar{s->second.first, s->second.second,
                               s->second.second.has_value() && sema::Types::is_const(s->second.first)};
    variable_instance(v, item);
    auto d = declaration(v, item);
    if (!d || d->kind != ast::DeclKind::variable) return std::nullopt;
    auto c = constant_value(v, item, member_demand);
    QualType type = d->type;
    if (w_->types.is_reference(type) && !w_->types.is_function(w_->types.referee(type))) {
      const sema::EntityRec& entity = w_->entities.rec(v);
      bool in_namespace = entity.scope == EntityId::none || w_->entities.rec(entity.scope).kind == sema::EntityKind::namespace_;
      if (in_namespace && entity.templ == EntityId::none) type = w_->types.pointer(w_->types.referee(type));
    }
    return interp::GlobalVar{type, c, c.has_value()};
  }
  // The point the declarations seen from `item` reach: a function body in a member list is a
  // complete-class context ([class.mem.general]), where the members declared after it, and those of
  // the enclosing classes, are declared too. Only once the member list is complete: whether a
  // callee is defined is decided where the evaluation happens ([expr.const]).
  u32 complete_class_point(u32 item) const {
    if (item >= items_->items.size() || !items_->items[item].has_body() || !member_body_ready(item)) return item;
    u32 outer = items::none;
    for (u32 p = items_->items[item].parent; p != items::none && items_->items[p].kind == ItemKind::class_def;
         p = items_->items[p].parent)
      outer = p;
    if (outer == items::none) return item;
    u32 last = outer;  // the items are in preorder: a class's members follow it
    while (!items_->items[last].members.empty()) last = items_->items[last].members.back();
    return std::max(item, last);
  }
  bool is_constexpr(EntityId f, u32 item) {
    item = complete_class_point(item);
    if (auto constructor = dynamic_synthesized_.find(f); constructor != dynamic_synthesized_.end()) {
      const auto& synthesized = synth_[constructor->second];
      const EntityId owner = w_->entities.rec(f).scope;
      return synthesized.item < item && synthesized.kind == SpecialMember::default_constructor &&
             classes_.constexpr_dynamic_default(owner) && classes_.rec(owner).implicit_default_ctor == f;
    }
    if (auto copy = copy_assignments_.find(f); copy != copy_assignments_.end())
      return copy->second.definition.item <= item;
    // [dcl.fct.def.default]: a defaulted constructor is constexpr when it is constexpr-suitable.
    if (auto constructor = defaulted_constructors_.find(f); constructor != defaulted_constructors_.end())
      return constructor->second.definition.item <= item;
    // A specialization is as its template is declared, unless explicitly specialized ([temp.expl.spec]/14).
    if (EntityId t = w_->entities.rec(f).templ; t != EntityId::none && !explicitly_specialized(f, item)) f = t;
    auto d = declaration(f, item);
    return d && (d->flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_));
  }

  bool is_immediate_function(EntityId f, u32 item) {
    if (EntityId t = w_->entities.rec(f).templ; t != EntityId::none && !explicitly_specialized(f, item)) f = t;
    auto d = declaration(f, item);
    return d && (d->flags & sema::decl_flag::consteval_);
  }

  // A default member initializer's tokens in the region ([class.mem.general]), retained for the
  // constructor or aggregate initialization that uses it.
  struct Dmi {
    u32 item = 0;
    u32 begin = 0;
    u32 end = 0;
    bool braced = false;
    bool invalid = false;
    sema::Construction form = sema::Construction::direct_list;
  };
  std::map<u32, std::pair<QualType, std::optional<ConstId>>> static_locals_;  // by entity (`global`)
  void set_dmi(EntityId field, Dmi d) { dmis_[std::to_underlying(field)] = d; }
  // A class's base-clause ([class.derived.general]), recorded when its head is analysed.
  void set_bases(EntityId cls, std::vector<sema::BaseRec> bases) { class_bases_[std::to_underlying(cls)] = std::move(bases); }
  struct ClassBasePattern {
    ast::BaseSpec base;
    u32 begin;
    u32 end;
    bool replay;
  };
  std::optional<Dmi> dmi(EntityId field) const {
    auto it = dmis_.find(std::to_underlying(field));
    if (it == dmis_.end()) return std::nullopt;
    return it->second;
  }

  // The implicitly defined special members needed by the unit ([special]).
  enum class SpecialMember { destructor, copy_constructor, copy_assignment, move_constructor, default_constructor, move_assignment };
  struct Synthesized {
    u32 item;  // the class's
    ast::DeclSema decl;
    SpecialMember kind;
  };
  const std::deque<Synthesized>& synthesized() const { return synth_; }
  // An implicit constructor of a dynamic class or a class containing one: emitted only where called.
  // A synthesized member of a dynamic class emitted only where something uses it: an implicit
  // constructor, or its destructor ([class.dtor]).
  bool emitted_on_use(EntityId member) const {
    if (dynamic_synthesized_.contains(member)) return true;
    const auto& entity = w_->entities.rec(member);
    const auto* owner = entity.kind == sema::EntityKind::destructor ? classes_.find(entity.scope) : nullptr;
    return owner && owner->dynamic;
  }
  // Its body, analysed on first use.
  const ast::BodySema& synthesized_body(std::size_t i);
  bool special_members(u32 item, EntityId cls, sema::ClassRec& rec, std::vector<engine::Diagnostic>* class_diags = nullptr,
                       sema::Sema* closure_context = nullptr, std::optional<u32> point = std::nullopt);
  // Marks the special members of class template specialization `cls` that their trailing
  // requires-clauses make ineligible ([special]/6), and reports a destructor that cannot be
  // selected ([class.dtor]/4).
  void eligible_special_members(u32 item, EntityId cls, sema::ClassRec& rec, u32 point);
  void declare_copy_assignment(u32 item, EntityId cls, sema::ClassRec& rec, bool user_move, std::optional<u32> point);
  void defaulted_assignments(u32 item, EntityId cls, sema::ClassRec& rec, std::optional<u32> point);
  sema::CopyAssignment require_copy_assignment(EntityId f);
  Ex copy_from(sema::Sema& sm, Ex src, QualType t, bool move = false);
  Ex copy_constructor_root(sema::Sema& sm, EntityId cls, const ast::DeclSema& decl, bool move);
  // Whether `f` is a constructor, destructor or assignment operator defaulted on its first
  // declaration: its definition is synthesized, not read from its item ([dcl.fct.def.default]).
  bool defaulted_special_member(EntityId f) const {
    const auto decl = member_decl(f);
    if (!decl || !defaulted_body(index(decl->item))) return false;
    const auto& entity = w_->entities.rec(f);
    return entity.kind == sema::EntityKind::constructor || entity.kind == sema::EntityKind::destructor ||
           (entity.kind == sema::EntityKind::function && w_->ins.names.spelling(entity.name) == "operator=");
  }
  std::optional<bool> defaulted_nonthrowing(EntityId f);
  bool synthesized_assignment(EntityId f) const { return copy_assignments_.contains(f); }

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
  // (`lazy_member`); its value is instantiated on first use (`member_value`). A native use
  // retains the storage and the initializer's native dependencies.
  struct StaticInitializer {
    u32 end;
    bool dependent_layout;
  };
  void set_static_initializer(u32 item, u32 ordinal, StaticInitializer initializer) {
    static_initializers_[{item, ordinal}] = initializer;
  }
  std::optional<StaticInitializer> static_initializer(u32 item, u32 ordinal) {
    item_sema(item);  // the template's analysis records it
    auto it = static_initializers_.find({item, ordinal});
    if (it == static_initializers_.end()) return std::nullopt;
    return it->second;
  }
  struct LazyMember {
    u32 item;
    u32 ordinal;
    ClassInstantiation inst;
    bool dependent_layout;
    enum class State : u8 { pending, running, done } state = State::pending;
    std::optional<std::size_t> initializer = std::nullopt;
  };
  void lazy_member(EntityId e, u32 item, u32 ordinal, const ClassInstantiation& inst, bool dependent_layout) {
    lazy_members_.try_emplace(std::to_underlying(e), LazyMember{item, ordinal, inst, dependent_layout});
  }
  bool is_lazy_member(EntityId e) const { return lazy_members_.contains(std::to_underlying(e)); }
  bool member_value(EntityId e, MemberDemand demand);
  bool use_member(EntityId e, MemberDemand demand, bool unevaluated) {
    auto it = lazy_members_.find(std::to_underlying(e));
    if (it == lazy_members_.end() || (unevaluated && it->second.dependent_layout)) return true;
    retain_member(e);
    return member_value(e, demand);
  }
  void retain_member(EntityId e) {
    if (!used_member(e)) used_members_.push_back(e);
  }
  bool used_member(EntityId e) const { return std::ranges::contains(used_members_, e); }
  std::optional<std::size_t> member_initializer_index(EntityId e) const {
    if (auto found = lazy_members_.find(std::to_underlying(e)); found != lazy_members_.end())
      return found->second.initializer;
    return std::nullopt;
  }
  bool emits_lazy_member(EntityId e) const {
    const auto i = member_initializer_index(e);
    return i && !member_initializers_[*i].type_only;
  }
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
    // A generic lambda operator's whose lambda is in a declaration part: that part's offset in the
    // item, where otherwise the body part is meant (`GenericClosure::part`).
    std::optional<u32> part{};
    // Where the body's part begins in `it`, its item.
    u32 body_offset(const items::Item& it) const { return part.value_or(it.body_begin - it.begin); }
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
    std::optional<ast::DeclSema> primary;  // the current item's written declaration, if it is the primary
  };
  // `instantiate_var(spec)` ([temp.inst]/7): the declaration of a variable template specialization,
  // its initializer analysed and evaluated with the arguments bound; none if `v` is no such
  // specialization, or its template is not defined before `point`.
  struct VarInstance;
  const VarInstance* variable_instance(EntityId v, u32 point, std::optional<QualType> potential_constant_type = std::nullopt);
  // The variable template specializations instantiated, in order: their item and record.
  struct VarInstance {
    EntityId spec;
    u32 item;
    Result<ast::ItemSema> cell;
    bool diagnosed_failure = false;
  };
  const std::deque<VarInstance>& variable_instances() const { return var_instances_; }
  bool variable_instantiation_failed(EntityId v, u32 point) const {
    const auto it = var_index_.find(std::to_underlying(v));
    if (it == var_index_.end()) return false;
    const VarInstance& in = var_instances_[it->second];
    return in.item < point && in.diagnosed_failure;
  }
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
    std::vector<sema::NestedType> alias_templates;
    std::vector<EntityId> friends;
    std::vector<ast::MemberRec> adl_friends;
    bool unapplied_field_layout = false;
  };
  ClassContent class_content(u32 item, const std::function<const ast::ItemSema&(u32)>& sema_of);
  bool class_assertion(u32 member) const {
    return r_->tokens[items_->items[member].begin].kind == lex::Tk::kw_static_assert;
  }
  bool valid_class_assertion(const Result<ast::ItemSema>& member) const;
  sema::PeerVirtualLayout peer_virtual_layout(u32 item, EntityId cls, const sema::ClassRec& rec,
                                             std::span<const ast::MemberRec> members);
  EntityId peer_automatic_primary_base(u32 item, EntityId cls, const sema::ClassRec& rec,
                                       std::span<const ast::MemberRec> members);
  // The general single-inheritance layout of a polymorphic class outside the peer profiles (#309).
  std::optional<sema::DynamicClass> dynamic_class(u32 item, EntityId cls, const sema::ClassRec& rec,
                                                 std::span<const ast::MemberRec> members);
  // Its implicit default and copy constructors, which install the vptr ([class.cdtor]).
  void dynamic_special_members(u32 item, EntityId cls, sema::ClassRec& rec);
  // What makes a defined polymorphic class, or one containing such objects, a dynamic one: its
  // final overriders, its table's layout and its implicit constructors.
  void complete_virtual_members(u32 item, EntityId cls, sema::ClassRec& rec, std::span<const ast::MemberRec> members,
                                bool member_declarations_valid);
  void pure_final_overriders(sema::ClassRec& rec, EntityId cls);
  std::optional<ast::DeclSema> member_declaration(EntityId member);
  EntityId synthesize_constructor(u32 item, EntityId cls, bool copy, bool nonthrowing, bool const_source);
  void validate_dynamic_constructors(sema::ClassRec& rec);
  bool default_initializes(const Synthesized& s, u32 point);
  ast::BodySema synthesize_body(const Synthesized& s, std::optional<u32> point);
  // A specialization's implicit default constructor runs default member initializers analysed for
  // it: what they demand and say, by `synth_` index.
  std::map<std::size_t, std::pair<std::vector<sema::Demand>, std::vector<engine::Diagnostic>>> synth_results_;
  std::map<EntityId, std::size_t> dynamic_synthesized_;  // synthesized constructors by entity: their `synth_` index
  // A complete class's size, as sema lays it out (`sizeof`).
  std::optional<u64> class_size(EntityId cls);
  // The implicitly defined default constructor's body ([class.default.ctor]/4).
  ast::BodySema implicit_default_constructor_body(const Synthesized& s);
  // `instantiate_class(spec)` ([temp.inst]/2, §B6.2): the class record of a class template
  // specialization, its member items analysed with the arguments bound, built whole and then
  // published; none when the template has no definition before `item`, or for a dependent one.
  const sema::ClassRec* instantiate_class(EntityId spec, u32 point);
  const sema::ClassRec* instantiate_member_class(EntityId cls, u32 point);
  const sema::ClassRec* replay_class(EntityId spec, u32 item, u32 point,
                                     std::span<const ast::TemplateParamSema> params, const sema::Bindings& args);
  std::optional<sema::BaseRec> instantiate_base(EntityId spec, const ClassBasePattern& pattern, u32 item, u32 point,
                                                std::span<const ast::TemplateParamSema> params, const sema::Bindings& args);
  // The member of a class template specialization a member function entity is, if it is one:
  // its pattern's item and the specialization.
  struct MemberOf {
    u32 item;
    EntityId spec;
  };
  std::map<u32, MemberOf> member_patterns_;
  std::map<EntityId, EntityId> instantiating_bases_;  // the direct base of each specialization whose members are being analysed
  std::optional<u32> out_of_class_definition(EntityId spec, MemberOf of);
  // A class template specialization's record of its member item `m`, analysed already.
  const ast::ItemSema& class_member_sema(u32 m, EntityId spec) {
    return class_member_cells_.at(std::pair(m, std::to_underlying(spec))).value;
  }
  bool require_enum_definition(EntityId e, u32 point);

  // The end of the translation unit (§B6.2): every specialization whose definition a cell
  // requires is instantiated, in order of first demand, and so are those its instance requires.
  void instantiate_all();
  void instantiate_variable_demands(std::span<const sema::Demand> demands, u32 point);
  void references(NodeId n, std::vector<EntityId>& out) const;
  void references(ConstId value, std::vector<EntityId>& out) const;
  void part_references(NodeId root, ast::SeqId<ast::ClosureDef> closures, std::vector<EntityId>& out) const;
  void check_type_only_instances(const std::set<u32>& native);
  bool has_type_only_instances() const {
    return !type_only_instances_.empty() || std::ranges::any_of(member_initializers_, &MemberInitializer::type_only);
  }
  void mark_type_only_instances(std::size_t first_function, std::size_t first_variable, std::size_t first_member);
  std::span<const Instance* const> instances() const { return instance_order_; }
  bool emits_instance(EntityId spec) const { return !type_only_instances_.contains(spec); }
  // Every specialization whose definition the unit requires, in order of first demand.
  std::span<const EntityId> required() const { return required_; }
  std::span<const EntityId> implicit_allocation_functions() const { return implicit_allocation_functions_; }
  // The C library functions the unit's builtins call (`World::string_function`), in order of first use.
  std::span<const EntityId> library_functions() const { return library_functions_; }
  std::span<const EntityId> template_parameter_objects() const { return template_parameter_objects_; }
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
  void analyze_all() { analyze_group(0, static_cast<u32>(items_->items.size())); }
  // `analyze_all` for the items [first, end) alone, a top-level item and its members: the others
  // are analysed only as these ask (#947).
  void analyze_group(u32 first, u32 end) {
    for (u32 k = first; k < end; ++k) {
      // Members of a class or namespace wait for those (M2); the enclosing item was diagnosed.
      if (!in_global_scope(k)) continue;
      const auto& item = item_sema(k);
      const bool ordinary = !templated_body(k);
      if (ordinary && item.diags.empty()) instantiate_variable_demands(item.demands, k);
      if (items_->items[k].kind == ItemKind::class_def) complete_class_items(k);
      if (items_->items[k].has_body() && items_->items[k].kind == ItemKind::function) {
        const auto& body = analyze_body(k);
        if (ordinary && body.diags.empty()) instantiate_variable_demands(body.demands, k);
      }
    }
    const bool reported = depth_reported_;
    instantiate_all();
    depth_reported_at_end_ = !reported && depth_reported_;
  }
  // Whether the instantiation-depth limit was first reached by the END worklist (instantiate_all)
  // rather than while the items were analysed (#993).
  bool depth_reported_at_end() const { return depth_reported_at_end_; }
  // Whether a class of the unit can need virtual support (sema::Questions).
  bool may_have_virtual_layouts() const { return may_have_virtual_layouts_; }
  // An item at namespace scope: in no class, whichever namespaces enclose it.
  bool in_global_scope(u32 k) const { return !in_class_[k]; }
  // A member of an ordinary class ([class.mem]): every class enclosing item `k` is a named or
  // unnamed class defined at namespace scope or in such a class, with no template-head and no
  // specialization (#306).
  bool ordinary_class_member(u32 k) const {
    u32 p = items_->items[k].parent;
    if (p == items::none || items_->items[p].kind != ItemKind::class_def) return false;
    for (; p != items::none && items_->items[p].kind == ItemKind::class_def; p = items_->items[p].parent)
      if (items_->items[p].template_depth || items_->items[p].specialization) return false;
    return p == items::none || !in_class_[p];
  }
  // The class item `k`'s definition, then its nested classes' ([class.nest]), then the members'
  // function bodies: complete-class contexts of every enclosing class ([class.mem.general]/8).
  void complete_class_items(u32 k) {
    class_def(k);
    for (u32 m : items_->items[k].members)
      if (named_nested_class(m) && class_entity(m) != EntityId::none) class_def(m);
    for (u32 m : items_->items[k].members) {
      if (items_->items[m].kind == ItemKind::function && items_->items[m].has_body()) analyze_body(m);
      else if (named_nested_class(m) && class_entity(m) != EntityId::none) complete_class_items(m);
    }
  }
  // An in-class definition of a named, non-template member class ([class.nest]).
  bool named_nested_class(u32 k) const {
    const items::Item& it = items_->items[k];
    return it.kind == ItemKind::class_def && it.unnamed_type_ordinal == items::none && !it.template_depth &&
           !it.specialization && it.parent != items::none && items_->items[it.parent].kind == ItemKind::class_def;
  }
  // What analysis reached, without analysing more (#170): an item's semantics and its body, if
  // analysed.
  const ast::ItemSema* analysed_item(u32 k) const { return item_cells_[k] ? &item_cells_[k]->value : nullptr; }
  std::span<const std::pair<u32, u32>> applied_alignment_specifiers(u32 item) const {
    if (const auto field = field_alignment_specifiers_.find(item); field != field_alignment_specifiers_.end())
      return field->second;
    const auto found = class_alignments_.find(item);
    if (found == class_alignments_.end()) return {};
    return found->second.specifiers;
  }
  const ast::BodySema* analysed_body(u32 k) const { return body_cells_[k] ? &body_cells_[k]->value : nullptr; }
  std::vector<engine::Diagnostic> diagnostics() const { return diagnostics(0, static_cast<u32>(items_->items.size())); }
  // Those of a group (#947): its items' cells, and every instance and END check its Program made
  // (another group making the same one reports the same diagnostics).
  std::vector<engine::Diagnostic> diagnostics(u32 first, u32 end) const {
    std::vector<engine::Diagnostic> r;
    for (u32 k = first; k < end; ++k) {
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
  // token is where it was expanded. Only for a region with its locations: the pipeline analyses
  // `PpTokens`, which has none, and places messages in `TuFront` (#947).
  pp::TokLoc site(engine::Loc at) const
    pre(!r_->locs.empty())
  {
    return r_->site(token(at));
  }
  // The region token of a diagnostic's location.
  u32 token(engine::Loc at) const {
    const items::Item& it = items_->items[index(ItemId(at.item))];
    return std::min<u32>(it.begin + at.offset, static_cast<u32>(r_->tokens.size() - 1));
  }
  u32 presumed_line(engine::Loc at) const { return r_->presumed_lines.at(token(at)); }
  sema::PresumedSource presumed_source(engine::Loc at) const {
    const auto i = token(at);
    const auto site = r_->presumed_sites.at(i);
    return {r_->presumed_files.at(site.file), r_->presumed_lines.at(i), site.column};
  }
  // The byte offset in its file of a diagnostic's location.
  u32 byte_offset(engine::Loc at) const { return site(at).offset; }

  // Spells a diagnostic argument: a name, a type, an integer, a string literal or why evaluation failed.
  std::string spell(char how, u64 raw) const {
    if (how == 'n') return std::string(w_->ins.names.spelling(ast::NameId(u32(raw))));
    if (how == 'r') return sema::spell_reason(raw);
    if (how == 'i') {
      const auto& value = w_->ins.values.get<ast::VInt>(ConstId(u32(raw)));
      const bool negative = ast::is_signed(w_->target, *w_->types.builtin_of(value.type)) && ast::is_negative(value.bits);
      return negative ? "-" + ast::wide::decimal(ast::wide::neg(value.bits)) : ast::wide::decimal(value.bits);
    }
    if (how == 'c') {
      std::string t = w_->values.narrow_string(ConstId(u32(raw))).value_or("");
      if (!t.empty() && t.back() == '\0') t.pop_back();  // the terminating null
      return t;
    }
    return w_->types.spell(ast::QualType(u32(raw)));
  }

private:
  struct ClassAlignment {
    u8 value = 0;  // the strictest written alignment, including a zero-only sequence
    u32 at = 0;    // in the item, for diagnostics
    u64 natural = 0;  // once the definition's natural alignment has been computed
    std::vector<std::pair<u32, u32>> specifiers;  // the applied ranges in the region
    bool dependent = false;  // a member's alignment is checked again when its class is instantiated
    u64 effective(u64 natural_alignment) const { return value ? value : natural_alignment; }
  };
  std::map<u32, ClassAlignment> class_alignments_;
  std::map<u32, std::vector<std::pair<u32, u32>>> field_alignment_specifiers_;
  void validate_class_alignment(u32 item, EntityId cls, sema::ClassRec& record, std::vector<engine::Diagnostic>& diags);
  bool has_unapplied_class_layout(u32 item, const ClassContent& content) const;
  bool in_attributed_namespace(u32 item) const;
  std::optional<EntityId> peer_root_destructor_key(EntityId cls) const;
  std::optional<DerivedVirtualDestructor> derived_virtual_destructor_layout(EntityId cls) const;
  bool class_layout_uncertain(EntityId cls, const sema::ClassRec& record, u32 point) const;
  std::optional<QualType> copy_move_constructor_parameter(const sema::MemberDeclRec& member, QualType owner) const;
  std::optional<QualType> assignment_parameter(const sema::MemberDeclRec& member, QualType owner) const;
  sema::TrivialCopyability class_trivial_copyability(u32 item, EntityId cls, const sema::ClassRec& rec);

  enum class State : u8 { empty, running, done };
  template <class V>
  struct Cell {
    State state = State::empty;
    Result<V> result;
  };

  const bool exceptions_enabled_;
  const bool rtti_enabled_;
  const ast::Compatibility compatibility_;
  sema::World* w_;
  const pp::Region* r_;
  ItemsView view_;
  const ItemsView* items_;
  std::set<NameProbe>* probes_ = nullptr;
  NameProbe last_probe_{Probe::declares, {}, "\n"};
  bool may_have_virtual_layouts_ = true;
  std::map<u32, u32> declared_scan_;  // by entity: the items `ensure_declared` has gone through
  std::shared_ptr<const Index> ix_;
  const std::vector<ItemId>& ids_ = ix_->ids;
  const std::unordered_map<u32, u32>& index_ = ix_->index;
  // An item's cells, made as the analysis reaches it: a group's Program reaches few (#947).
  template <class V>
  class Cells {
   public:
    const std::optional<Result<V>>& operator[](u32 k) const {
      auto it = cells_.find(k);
      return it == cells_.end() ? none_ : it->second;
    }
    std::optional<Result<V>>& at(u32 k) { return cells_[k]; }
    void add_items(std::set<u32>& out) const {
      for (const auto& [k, cell] : cells_)
        if (cell) out.insert(k);
    }
   private:
    std::map<u32, std::optional<Result<V>>> cells_;
    static inline const std::optional<Result<V>> none_{};
  };
  Cells<ast::ItemSema> item_cells_;
  Cells<ast::BodySema> body_cells_;
  Cells<ast::ClassDef> class_cells_;
  std::map<u32, u32> class_items_;     // by class entity: its defining item
  std::map<u32, EntityId> class_entities_;  // by class item: the class it defines
  // By member function or static data member entity: its declaration in this unit's class, an
  // item of this unit (#156).
  std::map<u32, DeclId> member_decls_;
  sema::Classes classes_{w_->ins, w_->types};  // this unit's class and enumeration records (#115)
  struct Classified {
    sema::CopyState state;
    bool calls = false;  // a trivial subobject operation is a call (`MemberDeclRec::trivial_call`)
  };
  Classified classify_special_member(u32 item, EntityId cls, const sema::ClassRec& rec, EntityId function,
                                     SpecialMember kind, bool const_source, std::optional<u32> point);
  bool copy_takes_const(const sema::ClassRec& rec, bool assignment) const;
  // A volatile class object or an array of them: no implicit copy or move operation takes one.
  bool volatile_class(QualType t) const {
    auto& types = w_->types;
    for (t = types.canonical(t); types.is_array(t);)
      t = types.canonical(types.add_cv(types.rec(t).inner, sema::Types::cv(t)));
    return types.is_class(t) && sema::Types::is_volatile(t);
  }
  bool calls_trivial_member(const sema::ClassRec& rec, const ast::DeclSema& decl, bool calls) const;
  void defaulted_constructors(u32 item, EntityId cls, sema::ClassRec& rec, bool implicit_copy, std::optional<u32> point);
  // An enumerator initializer can synchronously instantiate another enum definition.
  struct DefiningEnum {
    const DefiningEnum*& active;
    EntityId entity;
    const sema::EnumRec& record;
    const DefiningEnum* previous;

    DefiningEnum(const DefiningEnum*& active, EntityId entity, const sema::EnumRec& record)
        : active(active), entity(entity), record(record), previous(std::exchange(active, this)) {}
    DefiningEnum(const DefiningEnum&) = delete;
    ~DefiningEnum() { active = previous; }
  };
  const DefiningEnum* defining_enum_ = nullptr;
  // What each namespace-scope using-declaration introduces, by (item, name ordinal): the
  // declarations qualified lookup found at it ([namespace.udecl]/1).
  std::map<std::pair<u32, u32>, std::vector<DeclId>> using_targets_;
  struct AsmLabel {
    u32 item = 0, offset = 0;
    std::string spelling;
  };
  std::map<u32, AsmLabel> asm_labels_;  // the last written label per entity
  std::map<u32, ast::DeclSema> class_heads_;  // by class item: the class's record, for its members
  std::map<u32, lex::Tk> class_definition_keys_;  // by defining item: the parsed class-key
  std::map<u32, u32> class_templates_;        // by class template entity: its defining item
  std::map<std::pair<u32, u32>, Result<ast::ItemSema>> class_member_cells_;  // by (member item, specialization)
  std::set<u32> classes_instantiating_;
  std::set<u32> classes_instantiated_;  // by specialization: instantiated in this unit
  std::set<EntityId> failed_member_class_instantiations_;  // diagnosed while replaying the definition
  // A class can be blocked by its own assertion or one in a required class.
  std::set<EntityId> failed_class_assertions_;
  std::set<u32> items_with_failed_assertion_dependencies_;
  std::vector<std::pair<u32, u32>> class_member_order_;  // the cells above, in order of analysis
  struct ScopedEnumDefinition {
    u32 item;
    enum class State : u8 { pending, running, complete, failed } state = State::pending;
  };
  std::map<EntityId, ScopedEnumDefinition> scoped_enum_definitions_;
  u32 scoped_enum_depth_ = 0;
  std::map<u32, Dmi> dmis_;                       // by field entity
  std::set<EntityId> replaying_default_members_;
  std::set<std::tuple<EntityId, ItemId, u32>> replaying_function_defaults_;
  std::map<u32, std::vector<sema::BaseRec>> class_bases_;  // by class entity
  std::map<u32, ClassBasePattern> class_base_patterns_;  // by defining item
  struct AliasCallPattern { u32 item, begin, end; sema::AliasCallPattern call; };
  std::map<EntityId, AliasCallPattern> alias_call_patterns_;
  std::set<std::pair<u32, u32>> dependent_type_trait_operands_;  // source item and token offset
  std::set<EntityId> failed_type_trait_instantiations_;
  std::map<u32, u32> nested_constraint_operand_ends_;  // offsets in this program's token region
  const std::vector<std::string>& ns_of_ = ix_->ns_of;
  const std::vector<std::string>& eff_ns_ = ix_->eff_ns;
  const std::map<u32, std::vector<std::pair<std::string, std::string>>>& name_scopes_ = ix_->name_scopes;
  const std::vector<ast::GnuVisibility>& namespace_visibility_ = ix_->namespace_visibility;  // by lexical occurrence
  const std::string& name_ns(u32 item, u32 name) const {
    auto it = name_scopes_.find(item);
    return it != name_scopes_.end() && name < it->second.size() ? it->second[name].first : ns_of_[item];
  }
  const std::string& name_eff_ns(u32 item, u32 name) const {
    auto it = name_scopes_.find(item);
    return it != name_scopes_.end() && name < it->second.size() ? it->second[name].second : eff_ns_[item];
  }
  const std::vector<std::string>& inner_of_ = ix_->inner_of;
  const std::vector<bool>& in_class_ = ix_->in_class;
  const std::unordered_map<std::string_view, std::vector<std::pair<u32, u32>>>& by_name_ = ix_->by_name;
  std::map<std::string, EntityId> ns_entities_;
  std::map<u32, std::string> ns_paths_;           // by namespace entity
  const std::map<std::string, std::vector<InlineNamespace>>& inline_namespaces_ = ix_->inline_namespaces;
  // The target namespace of a qualified C-linkage function, by (item, declarator ordinal).
  std::map<std::pair<u32, u32>, std::string> qualified_c_functions_;
  std::vector<Directive> directives_ = ix_->directives;  // this Program resolves their targets

  // A deque: analysing a synthesized body can instantiate a class, which synthesizes more members
  // while the body's record is referred to.
  std::deque<Synthesized> synth_;
  std::map<std::size_t, ast::BodySema> synth_bodies_;
  // A copy or move assignment operator, implicit or defaulted on its first declaration.
  struct PendingCopyAssignment {
    Synthesized definition;
    sema::CopyAssignment state;
    std::optional<ast::BodySema> body = std::nullopt;
    std::vector<sema::Demand> demands{};
    bool calls = false;  // trivial, but its definition calls a subobject's operator
    bool checking = false;
    bool emitted = false;
  };
  std::map<EntityId, PendingCopyAssignment> copy_assignments_;
  // A constructor defaulted on its first declaration that is neither trivial nor deleted. It is
  // implicitly defined where something uses it ([dcl.fct.def.default]). `definition.item` is its
  // declaration's item.
  struct DefaultedConstructor {
    Synthesized definition;
    std::optional<Result<ast::BodySema>> body = std::nullopt;
    bool emitted = false;
  };
  std::map<EntityId, DefaultedConstructor> defaulted_constructors_;
  std::map<EntityId, Synthesized> defaulted_destructors_;  // nontrivial ones nothing uses yet
  std::map<EntityId, std::optional<bool>> defaulted_nonthrowing_;  // by special member; none while it is decided
  const Result<ast::BodySema>& defaulted_constructor_body(DefaultedConstructor& constructor);
  std::unique_ptr<sema::Questions> synthesis_questions(EntityId cls, u32 item);
  struct PendingMoveConstructor {
    Synthesized definition;
    ast::BodySema body;
    std::vector<sema::Demand> demands;
  };
  std::map<EntityId, PendingMoveConstructor> move_constructors_;
  std::deque<Instance> instances_;               // a deque: instances are referred to as they are added
  std::vector<const Instance*> instance_order_;  // the completed ones, in order of instantiation
  std::set<EntityId> type_only_instances_;  // specializations instantiated for type checking
  std::map<u32, std::size_t> instance_index_;    // by specialization: its slot in `instances_`
  std::set<u32> instantiating_;                  // specializations whose instantiation is running
  std::map<u32, u32> var_templates_;             // by variable template entity: its defining item
  std::map<u32, u32> explicit_specs_;            // by specialization: the item of its first explicit specialization
  std::map<u32, u32> class_points_;              // by class template specialization: its point of instantiation
  u32 spec_scan_ = 0;  // how many of the index's specializations `specializations_before` has analysed
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
  bool depth_reported_at_end_ = false;
  u32 members_running_ = 0;  // static data member initializers being instantiated, nested
  std::vector<engine::Diagnostic> end_diags_;    // the END phase's own
  std::vector<EntityId> required_;
  std::vector<EntityId> implicit_allocation_functions_;  // in order of first use
  std::vector<EntityId> library_functions_;              // in order of first use
  std::vector<EntityId> template_parameter_objects_;     // in order of first use
  bool has_typed_dynamic_cast_ = false;
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
  // [dcl.enum]: a scoped enumerator belongs only to the enumeration's scope.
  bool scoped_enumerator(const ast::DeclSema& declaration) const {
    if (declaration.kind != ast::DeclKind::enumerator || declaration.entity == EntityId::none) return false;
    const auto* enumeration = classes_.find_enum(w_->entities.rec(declaration.entity).scope);
    return enumeration && enumeration->scoped;
  }

  static constexpr u32 class_def_key(u32 item) { return item | 0x80000000u; }
  std::vector<u32> running_;
  std::set<u32> ordinary_friend_items_;  // the source item of each supported unqualified friend
  std::set<u32> templated_friend_items_;  // the item of each friend function a class template defines
  std::set<EntityId> unapplied_declaration_attributes_;
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
  // Whether the body at `definition` can be analysed: a member's body is a complete-class context
  // ([class.mem.general]/8), so not while its class's members are being declared.
  bool member_body_ready(u32 definition) const {
    const u32 parent = items_->items[definition].parent;
    if (parent == items::none || items_->items[parent].kind != ItemKind::class_def) return true;
    const auto cls = class_entities_.find(parent);
    return (cls != class_entities_.end() && classes_.is_defined(cls->second)) ||
           !std::ranges::contains(running_, class_def_key(parent));
  }
  struct DestructorDeclaration {
    u32 at;
    std::optional<bool> nonthrowing;  // a written exception specification, if present
    bool defaulted;
    std::optional<bool> implicit_nonthrowing{};  // the specification inferred from the class's subobjects
  };
  std::map<EntityId, DestructorDeclaration> destructor_declarations_;
  std::map<EntityId, std::vector<std::optional<std::pair<u32, u32>>>> member_default_tokens_;
  std::map<EntityId, EntityId> overridden_virtuals_;  // by overriding member's entity
  enum class ReplayState { pending, active, finished };
  struct PendingMemberDefault {
    NodeId* slot;  // the published default remains a replay handle until completion finishes
    std::size_t parameter;
    std::pair<u32, u32> tokens;
    ReplayState state = ReplayState::pending;
    NodeId result = NodeId::none;
  };
  // During completion of a class with a parenthesized defaulted destructor specification, a
  // member default can need the specification, and the specification can need an omitted default.
  struct ClassCompletion {
    EntityId cls;
    sema::ClassRec& record;
    Result<ast::ClassDef>& out;
    ClassCompletion* outer;
    std::map<std::pair<EntityId, u32>, PendingMemberDefault> defaults{};
    ReplayState destructor = ReplayState::pending;
  };
  ClassCompletion* class_completion_ = nullptr;
  ClassCompletion* class_completion(EntityId cls) const;
  void bind_destructor_and_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out);
  std::optional<bool> resolve_destructor_exception(ClassCompletion& completion, sema::Sema* caller, u32 at);
  NodeId resolve_member_default(ClassCompletion& completion, EntityId function, PendingMemberDefault& pending,
                                sema::Sema* caller, u32 at);
  void bind_member_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out,
                            ClassCompletion* completion = nullptr);
  NodeId bind_member_default(EntityId cls, EntityId function, std::size_t parameter, std::pair<u32, u32> tokens,
                            Result<ast::ClassDef>& out);
  std::map<EntityId, std::pair<u32, u32>> special_member_exception_tokens_;
  std::map<EntityId, std::optional<QualType>> special_member_exception_types_;
  // [class.compare.default]: what its complete class decides of a defaulted comparison operator
  // function: whether it is deleted ([class.compare.default], [class.eq]), and its type with
  // the implicit exception specification when none is written ([except.spec]).
  struct DefaultedComparison {
    bool deleted;
    QualType type;
    DeclId decl;
  };
  std::map<EntityId, DefaultedComparison> defaulted_comparisons_;
  void complete_defaulted_comparisons(EntityId cls, sema::ClassRec& rec);
  bool comparison_subobjects(sema::Sema& sm, EntityId cls, Ex x, Ex y, std::function_ref<bool(Ex, Ex)> each);
  std::optional<Ex> defaulted_equality(sema::Sema& sm, EntityId cls, Ex x, Ex y);
  // [class.spaceship]: a defaulted three-way comparison's return type R and its subobject
  // comparisons, each `xi <=> yi` as the synthesized three-way comparison of type R, then
  // `static_cast<R>(std::strong_ordering::equal)` for when every one is equal.
  struct ThreeWaySubobject {
    Ex initializer, variable, unequal, return_value;
    u32 local;
  };
  struct DefaultedThreeWay {
    QualType result;
    std::vector<ThreeWaySubobject> subobjects;
    Ex equal;
  };
  std::optional<DefaultedThreeWay> defaulted_three_way(sema::Sema& sm, EntityId cls, Ex x, Ex y, QualType declared);
  // The class and the two operands `x` and `y` of defaulted comparison `f`, whose parameters are
  // the locals `params` (a member's object parameter is `*this`).
  struct ComparisonOperands {
    EntityId cls;
    Ex x, y;
  };
  std::optional<ComparisonOperands> comparison_operands(sema::Sema& sm, const ast::DeclSema& f, std::span<const u32> params);
  void bind_special_member_exception(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out, EntityId entity);
  struct MemberException {
    u32 item;
    std::optional<std::pair<u32, u32>> condition_tokens;
  };
  struct MemberExceptionPattern {
    std::vector<u32> calls;
    std::vector<sema::Expansion> expansions;
    bool dependent_type_traits = false;
  };
  std::map<EntityId, MemberException> member_exceptions_;
  std::map<u32, std::optional<MemberExceptionPattern>> member_exception_patterns_;
  std::set<EntityId> replaying_member_exceptions_;
  std::map<EntityId, QualType> resolved_member_exceptions_;
  void check_member_exception_patterns(EntityId cls, const sema::ClassRec& rec, Result<ast::ClassDef>& out);
  void check_function_exception_pattern(EntityId entity, std::vector<ast::TemplateParamSema> head,
                                        std::span<const ast::ParamSema> params, u32 item, sema::Sema& caller);
  void check_default_member_initializers(const sema::ClassRec& rec, Result<ast::ClassDef>& out);
  void prepare_move_constructor(u32 item, EntityId cls, sema::ClassRec& rec);
  std::map<u32, std::vector<std::vector<ast::TemplateParamSema>>> outer_heads_;  // by item
  std::map<std::pair<u32, u32>, StaticInitializer> static_initializers_;  // by (member item, ordinal)
  std::map<std::pair<EntityId, MemberDemand>, bool> member_layout_demands_;
  std::map<u32, LazyMember> lazy_members_;                 // by member entity
  std::vector<EntityId> used_members_;                     // in order of first use
  std::set<u32> value_dependent_members_;
  std::map<u32, ConstId> member_inits_;  // by lazily initialized member
  std::map<u32, std::pair<u32, ConstId>> constants_;                // by entity: (item, value)
  std::map<u32, std::vector<std::pair<u32, ast::DeclSema>>> decls_;  // by entity: (item, record)
  std::map<EntityId, u32> constinit_declarations_;  // earliest declaration requiring static initialization
  std::map<u32, u32> definitions_;               // by function entity: its defining item
  std::map<u32, std::optional<irgen::FunctionIr>> irs_;  // by item: function_ir, none if it failed
  std::pmr::unsynchronized_pool_resource arena_;  // the frames' arena (§9.3)

  friend class ItemAnalysis;
  friend class AtItem;
  friend class AtomAnalysis;
  friend class BodyAnalysis;
};

std::set<EntityId> Program::uncertain_object_layouts(bool include_lifecycle_signatures) const {
  const auto& region = this->region();
  const auto& items = this->items().items;
  std::set<EntityId> uncertain;
  std::set<EntityId> annotated_tags;
  std::set<std::uint32_t> uncertain_definitions;
  // Sema erases alias types. Remember affected spellings across alias declarations so a
  // scalar alias that changed layout also makes records using it uncertain.
  std::set<std::string_view> aliases;
  auto uncertain_alias = [&](const items::Item& item, u32 end) {
    if (aliases.empty()) return false;
    for (std::uint32_t i = item.begin; i < end; ++i) {
      auto tok = region.tokens[i];
      if (tok.kind == lex::Tk::identifier && aliases.contains(region.name(tok)) &&
          !std::ranges::contains(item.names, i - item.begin, &items::DeclName::offset)) return true;
    }
    return false;
  };
  for (std::uint32_t k = 0; k < items.size(); ++k) {
    const auto* analysed = analysed_item(k);
    if (!analysed) continue;  // before reading the item: a group reads only what it analysed (#947)
    const auto& item = items[k];
    const bool definition = item.kind == items::ItemKind::class_def;
    const bool enum_definition = item.kind == items::ItemKind::enum_def;
    if (!analysed || (!definition && !enum_definition && item.kind != items::ItemKind::declaration)) continue;
    bool packed = definition && layout_may_be_packed(region, item.begin, item.brace_end);
    // The attributes immediately after the type's closing brace can still apply to
    // the type. Those within a following declarator apply to the declared name.
    // An alias can also carry such an attribute, into a declaration or an enum-base.
    bool type_annotated = record_layout_attribute(region, item, applied_alignment_specifiers(k)) ||
                          (!definition && uncertain_alias(item, enum_definition ? item.brace_begin : item.end));
    bool annotated = type_annotated ||
        ((definition || enum_definition) && layout_attribute(region, item.brace_end, item.end));
    if (!packed && !annotated) continue;
    if (definition && (packed || type_annotated)) uncertain_definitions.insert(k);
    for (auto d : w_->ins.seqs.elements(analysed->decls)) {
      if (d.entity == EntityId::none) continue;
      if (d.kind == ast::DeclKind::class_ && (packed || type_annotated)) {
        uncertain.insert(d.entity);
        if (!definition) annotated_tags.insert(d.entity);
      }
      if (type_annotated && d.kind == ast::DeclKind::enum_) uncertain.insert(d.entity);
      if (include_lifecycle_signatures && type_annotated && d.kind == ast::DeclKind::function &&
          (d.flags & (sema::decl_flag::constructor | sema::decl_flag::destructor))) uncertain.insert(d.entity);
      if (annotated && d.kind == ast::DeclKind::data_member) {
        uncertain.insert(w_->entities.rec(d.entity).scope);
        if (item.parent != items::none) uncertain_definitions.insert(item.parent);
      }
      if (annotated && d.kind == ast::DeclKind::type_alias) {
        if (w_->types.is_class(d.type)) uncertain.insert(w_->types.class_of(d.type));
        else aliases.insert(w_->ins.names.spelling(w_->entities.rec(d.entity).name));
      }
    }
  }
  // Instantiated classes have their own entities. Their packing and field declarations
  // come from the selected pattern, which may be a partial specialization.
  for (EntityId c : classes().classes()) {
    auto pattern = class_pattern_item(c);
    if (pattern && (uncertain_definitions.contains(*pattern) || annotated_tags.contains(w_->entities.rec(c).templ)))
      uncertain.insert(c);
  }
  return uncertain;
}

// Sema's questions, asked at an item's prefix (§B1.4).
class AtItem final : public sema::Questions {
public:
  AtItem(Program& p, u32 item, bool self) : from(p.ns_of(item)), p_(&p), item_(item), self_(self) {}
  bool exceptions_enabled() const override { return p_->exceptions_enabled(); }
  bool rtti_enabled() const override { return p_->rtti_enabled(); }
  ast::Compatibility compatibility() const override { return p_->compatibility(); }
  const sema::Classes& classes() override { return p_->classes(); }
  sema::CopyAssignment require_copy_assignment(EntityId f) override { return p_->require_copy_assignment(f); }
  std::optional<bool> defaulted_nonthrowing(EntityId f) override { return p_->defaulted_nonthrowing(f); }
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r, sema::Sema& context) override {
    return p_->publish_class(c, std::move(r), item_, context);
  }
  const sema::EnumRec* defining_enum(EntityId e) override { return p_->defining_enum(e); }
  bool require_enum_definition(EntityId e) override {
    const bool ready = p_->require_enum_definition(e, point());
    observed_enum_definition_failure |= !ready;
    return ready;
  }
  std::set<EntityId> uncertain_object_layouts() override { return p_->uncertain_object_layouts(); }
  bool peer_pure_dispatch(EntityId cls) override { return p_->peer_pure_dispatch(cls); }
  void use_typed_dynamic_cast() override { p_->has_typed_dynamic_cast_ = true; }
  u32 presumed_line(engine::Loc at) override { return p_->presumed_line(at); }
  sema::PresumedSource presumed_source(engine::Loc at) override { return p_->presumed_source(at); }
  bool in_default_member_initializer() const override { return initializer_source.has_value(); }
  std::vector<DeclId> visible_decls(ast::NameId name, std::span<const EntityId> directives = {},
                                    bool immediate_call = false) override {
    std::vector<std::string> paths;
    for (EntityId ns : directives) paths.push_back(p_->namespace_path(ns));
    const auto& scope = initializer_source ? p_->ns_of(initializer_source->item) : from;
    auto filter = immediate_call ? Program::LookupNames::immediate_call : Program::LookupNames::all;
    return p_->visible_decls(p_->world().ins.names.spelling(name), namespace_lookup_item(),
                             namespace_lookup_includes_item(name), scope, paths, filter);
  }
  std::vector<ast::DeclSema> friend_namespace_decls(EntityId ns, ast::NameId name) {
    std::vector<ast::DeclSema> declarations;
    for (DeclId id : p_->visible_decls(p_->world().ins.names.spelling(name), namespace_lookup_item(),
                                        namespace_lookup_includes_item(name), p_->namespace_path(ns),
                                        {}, Program::LookupNames::types, /*innermost_only=*/true))
      if (auto d = lookup_declaration(id); d && d->entity != EntityId::none) declarations.push_back(*d);
    return declarations;
  }
  // Name conflicts when introducing a template concern declarations that bind in this namespace
  // ([basic.scope.scope]), including direct using-declarations.
  bool namespace_binds_name(EntityId ns, ast::NameId name) {
    const auto declarations = p_->declared_in(p_->world().ins.names.spelling(name), p_->namespace_path(ns),
                                               namespace_lookup_item(), namespace_lookup_includes_item(name),
                                               /*inhabit=*/true);
    return std::ranges::any_of(declarations, [&](DeclId id) {
      auto d = lookup_declaration(id);
      return d && d->entity != EntityId::none;
    });
  }
  std::vector<ast::DeclSema> qualifier_decls(ast::NameId name, std::optional<EntityId> ns,
                                            std::span<const EntityId> directives) override {
    const auto spelling = p_->world().ins.names.spelling(name);
    const auto item = namespace_lookup_item();
    const auto self = namespace_lookup_includes_item();
    std::vector<std::string> paths;
    for (EntityId directive : directives) paths.push_back(p_->namespace_path(directive));
    const auto& scope = initializer_source ? p_->ns_of(initializer_source->item) : from;
    const auto ids = ns ? p_->qualified_decls(spelling, p_->namespace_path(*ns), item, self, Program::LookupNames::qualifiers)
                        : p_->visible_decls(spelling, item, self, scope, paths, Program::LookupNames::qualifiers);
    std::vector<ast::DeclSema> declarations;
    for (DeclId id : ids)
      if (auto d = p_->decl_sema(id)) declarations.push_back(*d);
    return declarations;
  }
  std::optional<EntityId> namespace_named(ast::NameId name, std::span<const EntityId> directives = {}) override {
    for (DeclId d : visible_decls(name, directives))
      if (auto s = p_->decl_sema(d); s && s->kind == ast::DeclKind::namespace_) return s->entity;
    return std::nullopt;
  }
  std::vector<ast::DeclSema> namespace_members(EntityId ns, ast::NameId name, bool type_only = false,
                                               bool direct_only = false, bool immediate_call = false) override {
    std::vector<ast::DeclSema> r;
    auto filter = type_only ? Program::LookupNames::types : Program::LookupNames::all;
    if (immediate_call) filter = Program::LookupNames::immediate_call;
    for (DeclId d : p_->qualified_decls(p_->world().ins.names.spelling(name), p_->namespace_path(ns),
                                       namespace_lookup_item(), namespace_lookup_includes_item(name),
                                       filter, direct_only))
      if (auto s = lookup_declaration(d); s && s->entity != EntityId::none) r.push_back(*s);
    return r;
  }
  std::vector<ast::DeclSema> adl_functions(EntityId ns, ast::NameId name, bool at_instantiation) override {
    if (at_instantiation) return p_->adl_functions(ns, name, point(), false);
    if (initializer_source) return p_->adl_functions(ns, name, initializer_source->item, false);
    return p_->adl_functions(ns, name, item_, self_);
  }
  const sema::ClassRec* adl_class(EntityId c, bool at_instantiation) override {
    bool complete = at_instantiation ? is_complete(c) :
        (completing_class != EntityId::none && completing_class == c) || is_complete_at_source(c);
    return complete ? p_->classes().find(c) : nullptr;
  }
  std::vector<DeclId> earlier_adl_friends(EntityId c, NameId name) override {
    const auto def = p_->class_items_.find(std::to_underlying(c));
    const u32 source = initializer_source ? initializer_source->item : item_;
    if (def == p_->class_items_.end() || p_->items().items[source].parent != def->second) return {};
    return p_->hidden_friends_before(p_->world().ins.names.spelling(name), source, def->second);
  }
  // The namespace unqualified lookup starts in: the item's, or the one a qualified declarator
  // names ([basic.lookup.unqual]/9).
  std::string from;
  ast::DeclSema decl_sema(DeclId d) override {
    if (auto s = lookup_declaration(d)) return *s;
    return invalid_decl();
  }
  ast::DeclSema friend_decl(const ast::MemberRec& f) override {
    if (auto s = p_->friend_decl(f)) return *s;
    return invalid_decl();
  }
  std::optional<ast::DeclSema> written_member(EntityId f) override {
    if (const auto decl = p_->member_decl(f)) return p_->decl_sema(*decl);
    // A defaulted comparison declared as a friend: its implicit exception specification.
    if (const auto found = p_->defaulted_comparisons_.find(f); found != p_->defaulted_comparisons_.end())
      return p_->decl_sema(found->second.decl);
    return std::nullopt;
  }
  std::optional<ast::DeclSema> class_friend(EntityId f) override { return p_->class_friend(f); }
  void use_allocation_function(EntityId f, bool aligned) override {
    auto& functions = p_->implicit_allocation_functions_;
    if (std::ranges::contains(functions, f)) return;
    if (aligned) {
      // Record the namespace path for ADL, even if source cannot name it yet. Visible lookup
      // consults the declarations at its point, independently of this identity map.
      p_->namespace_entity("std");
      p_->world().install_alignment_enum(p_->classes_);
    }
    functions.push_back(f);
  }
  void use_library_function(EntityId f) override {
    if (!std::ranges::contains(p_->library_functions_, f)) p_->library_functions_.push_back(f);
  }
  void use_template_parameter_object(EntityId e) override {
    if (!std::ranges::contains(p_->template_parameter_objects_, e)) p_->template_parameter_objects_.push_back(e);
  }
  bool plain_class_template_type(EntityId c) override { return p_->plain_class_template_type(c); }
  interp::Callee function_ir(EntityId f, bool& diagnosed_failure) override {
    if (special_member_exception && p_->world().entities.rec(f).scope == completing_class)
      return {.is_constexpr = p_->is_constexpr(f, point())};
    return p_->function_ir(f, point(), &diagnosed_failure);
  }
  std::optional<QualType> deduced_type(EntityId f) override { return p_->deduced_type(f, point()); }
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, const sema::GenericBinding&) override {
    return p_->generic_instance(spec, known, point());
  }
  void generic_defined(const sema::GenericClosure& g) override { p_->generic_defs_[std::to_underlying(g.call)] = g; }
  std::optional<interp::GlobalVar> global(EntityId v) override {
    const auto item = value_point(v);
    return p_->global(v, item, member_demand(item));
  }
  bool is_constexpr(EntityId f) override { return p_->is_constexpr(f, point()); }
  bool is_immediate_function(EntityId f) override { return p_->is_immediate_function(f, point()); }
  bool is_complete(EntityId c) override {
    // An enumerator initializer is read at its original class-member declaration, where the
    // enclosing class is incomplete ([class.mem.general]).
    if (replaying_scoped_enum != EntityId::none && c == instantiating) return false;
    if (layout_member_demand)
      return c == layout_member_demand->completing ||
             p_->is_complete(c, layout_member_demand->item, layout_member_demand->body, layout_member_demand->own);
    // A declaration replay uses the enclosing class's prefix at the written point.
    const u32 parent = p_->items().items[item_].parent;
    if (instantiating != EntityId::none &&
        (p_->named_nested_class(item_) || (parent != items::none && p_->named_nested_class(parent)))) {
      for (EntityId owner = instantiating; owner != EntityId::none && !p_->is_namespace(owner);
           owner = p_->world().entities.rec(owner).scope)
        if (owner == c) return false;
    }
    return at_point ? p_->is_complete(c, *at_point, true, true) : is_complete_at_item(c);
  }
  bool may_have_virtual_layouts() override { return p_->may_have_virtual_layouts(); }
  std::optional<NodeId> replay_default_member(sema::Sema& caller, EntityId field, EntityId required_base,
                                              bool fallback = false) override;
  std::optional<NodeId> replay_function_default(sema::Sema& caller, const ast::DefaultArg& value,
                                                EntityId function, QualType parameter, u32 at) override;
  std::optional<bool> replay_destructor_exception(sema::Sema& caller, EntityId cls, u32 at) override;
  std::optional<QualType> replay_member_exception(sema::Sema& caller, const ast::DeclSema& decl,
                                                 EntityId function, QualType type) override;
  bool deferred_exception(EntityId f) override {
    const auto slot = p_->member_exceptions_.find(f);
    return slot != p_->member_exceptions_.end() && slot->second.condition_tokens.has_value();
  }
  bool unresolved_special_member_exception(EntityId f) override {
    return p_->unresolved_special_member_exception(f);
  }
  std::optional<u32> default_instantiation_point;
  bool is_complete_at_source(EntityId c) override {
    // The initializer is a complete-class context at its member declaration ([class.mem.general]).
    if (initializer_source) return p_->is_complete(c, initializer_source->item, true);
    return is_complete_at_item(c);
  }
  bool class_layout_uncertain(EntityId c) override {
    const auto* record = p_->classes().find(c);
    return record && p_->class_layout_uncertain(c, *record, point());
  }
  engine::Loc source_loc(engine::Loc loc, u32 offset) override {
    if (!initializer_source) return loc;
    const auto& dmi = *initializer_source;
    return {std::to_underlying(p_->id(dmi.item)), dmi.begin - p_->items().items[dmi.item].begin + offset};
  }
  bool in_type_trait_pattern() const override { return retain_type_trait_pattern; }
  // A replay owning the trait's tokens, where a concrete operand is judged at its written source.
  // An atom's satisfaction is not one: its concrete operands keep the check's own point.
  bool in_type_trait_replay() const override { return type_trait_demand.has_value(); }
  void record_dependent_type_trait_operand(engine::Loc loc) override {
    p_->dependent_type_trait_operands_.emplace(loc.item, loc.offset);
  }
  // Where a type trait whose operands were dependent where it is written is answered: the
  // replay's demand, or where an atomic constraint's satisfaction is checked ([temp.constr.atomic]).
  std::optional<u32> type_trait_demand_point() const {
    return satisfying_atom ? std::optional(point()) : type_trait_demand;
  }
  bool replaying_type_trait_operand(engine::Loc loc) override {
    return type_trait_demand_point() && dependent_type_trait_operand(loc);
  }
  // Whether the type trait at `loc` had dependent operands where its template defines it.
  bool dependent_type_trait_operand(engine::Loc loc) const {
    return p_->dependent_type_trait_operands_.contains({loc.item, loc.offset});
  }
  std::optional<bool> type_trait_at_demand(EntityId c, engine::Loc loc) override {
    // A generic lambda's specialization outside any template: a trait whose operands were
    // dependent in its body is answered at the end, as a function template's instantiation is.
    if (generic_at_end && dependent_type_trait_operand(loc)) return p_->is_complete(c, p_->end(), true, true);
    if (!replaying_type_trait_operand(loc)) return std::nullopt;
    return p_->is_complete(c, *type_trait_demand_point(), true, true);
  }
  bool generic_at_end = false;
  bool satisfying_atom = false;
  bool type_trait_instantiation_failed(EntityId e) override {
    const bool failed_assertion = p_->failed_class_assertions_.contains(e);
    observed_class_assertion_failure |= failed_assertion;
    const bool failed = failed_assertion || p_->failed_type_trait_instantiations_.contains(e) ||
                        p_->failed_member_class_instantiations_.contains(e);
    observed_type_trait_failure |= failed;
    return failed;
  }
  bool variable_instantiation_failed(EntityId v) override {
    const bool failed = p_->variable_instantiation_failed(v, value_point(v));
    observed_variable_instantiation_failure |= failed;
    return failed;
  }
  std::optional<ConstId> constant_value(EntityId v) override {
    const auto item = value_point(v);
    return p_->constant_value(v, item, member_demand(item));
  }
  sema::ConstantCallEvaluation constant_call(sema::Sema& s, const sema::Ex& call) override {
    auto result = sema::Evaluator(s, *this).evaluate(call);
    return {result.value, result.reason, result.diagnosed_failure()};
  }
  sema::AliasCallPattern alias_call_pattern(EntityId e) override {
    auto it = p_->alias_call_patterns_.find(e);
    return it == p_->alias_call_patterns_.end() ? sema::AliasCallPattern{} : it->second.call;
  }
  std::optional<QualType> replay_alias_calls(sema::Sema& caller, const ast::DeclSema& alias,
                                            const sema::Bindings& args) override;
  bool instantiation_gave_up() override { return p_->instantiation_gave_up(); }
  std::optional<QualType> specialization_type(EntityId v) override { return p_->specialization_type(v, point()); }
  const std::vector<sema::Expansion>* pattern_expansions() override { return at_point ? p_->expansions(item_, 0) : nullptr; }
  std::optional<bool> satisfied(ConstId c) override { return p_->satisfied(c, point()); }
  void record_nested_constraint_operand(u32 begin, u32 end) override {
    p_->nested_constraint_operand_ends_.try_emplace(begin, end);
  }
  std::optional<u32> nested_constraint_operand_end(u32 begin) override {
    const auto end = p_->nested_constraint_operand_ends_.find(begin);
    return end == p_->nested_constraint_operand_ends_.end() ? std::nullopt : std::optional(end->second);
  }
  bool satisfaction_failed(ConstId c) override { return p_->satisfaction_failed(c); }
  bool value_dependent_member(EntityId v) override { return p_->is_value_dependent_member(v); }
  std::optional<ast::DeclSema> template_decl(EntityId t) override;
  std::optional<ast::DeclSema> replay_function_decl(EntityId function) override {
    return p_->replay_function_decl(function);
  }
  bool use_static_member(EntityId v, bool unevaluated) override {
    return p_->use_member(v, member_demand(value_point(v)), unevaluated);
  }
  std::vector<EntityId> defining_bases(EntityId c) override {
    std::vector<EntityId> r;
    if (auto b = p_->class_bases_.find(std::to_underlying(c)); b != p_->class_bases_.end())
      for (const sema::BaseRec& x : b->second) r.push_back(x.cls);
    return r;
  }
  bool explicit_specialization(EntityId c) override { return p_->explicitly_specialized(c, item_); }
  std::span<const sema::BaseRec> defining_base_records(EntityId c) override {
    if (auto b = p_->class_bases_.find(std::to_underlying(c)); b != p_->class_bases_.end()) return b->second;
    return {};
  }
  void static_local(EntityId v, QualType t, std::optional<ConstId> value) override {
    p_->static_locals_[std::to_underlying(v)] = {t, value};
  }
  std::optional<ast::DeclSema> earlier_member(ast::NameId name, bool type_only = false,
                                            std::vector<ast::DeclSema>* functions = nullptr) override {
    const items::Item& it = p_->items().items[item_];
    if (it.parent == items::none || p_->items().items[it.parent].kind != ItemKind::class_def) return std::nullopt;
    const EntityId owner = instantiating != EntityId::none ? instantiating : p_->class_entity(it.parent);
    return members_before(it.parent, item_, owner, name, type_only, functions);
  }
  std::optional<ast::DeclSema> enclosing_earlier_member(EntityId cls, ast::NameId name, bool type_only,
                                                       std::vector<ast::DeclSema>* functions) override {
    const auto key = std::to_underlying(cls);
    const auto definition = p_->class_items_.find(key);
    const auto pattern = p_->class_patterns_.find(key);
    if (definition == p_->class_items_.end() && pattern == p_->class_patterns_.end()) return std::nullopt;
    const u32 source = definition != p_->class_items_.end() ? definition->second : pattern->second;
    // The member of `cls` that contains this item: the nested class being defined.
    u32 limit = item_;
    while (limit != items::none && p_->items().items[limit].parent != source)
      limit = p_->items().items[limit].parent;
    if (limit == items::none) return std::nullopt;
    return members_before(source, limit, cls, name, type_only, functions);
  }
  // The declarations of `name` by the members of class item `cls_item` before its member `limit`.
  // A class declared there is a member only when it is nested in `owner` ([class.nest]): an
  // elaborated-type-specifier may also declare a namespace class.
  std::optional<ast::DeclSema> members_before(u32 cls_item, u32 limit, EntityId owner, ast::NameId name, bool type_only,
                                              std::vector<ast::DeclSema>* functions) {
    std::optional<ast::DeclSema> r;
    for (u32 m : p_->items().items[cls_item].members) {
      if (m == limit) break;
      // In a class template's instantiation, the specialization's own records of its members.
      const ast::ItemSema& ms = p_->class_patterns_.contains(std::to_underlying(owner)) &&
                                      p_->items().items[m].kind != ItemKind::access_spec
                                    ? p_->class_member_sema(m, owner)
                                    : p_->item_sema(m).value;
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(ms.decls)) {
        if (d.entity == EntityId::none || (d.flags & sema::decl_flag::friend_) ||
            (d.kind == ast::DeclKind::class_ &&
             (owner == EntityId::none || p_->world().entities.rec(d.entity).scope != owner)) ||
            p_->world().entities.rec(d.entity).name != name || p_->scoped_enumerator(d))
          continue;
        const bool type = sema::Sema::is_type(d.kind);
        if (type_only && !type) continue;
        if (functions && d.kind == ast::DeclKind::function) functions->push_back(d);
        // [basic.lookup.general]: a non-type hides a same-scope class or enum name.
        if (!type || !r || sema::Sema::is_type(r->kind)) r = d;
      }
    }
    return r;
  }
  // The same for the members analysed so far of a class template specialization being instantiated.
  std::optional<ast::DeclSema> members_so_far(EntityId spec, ast::NameId name, bool type_only,
                                              std::vector<ast::DeclSema>* functions) {
    const u32 pattern = p_->class_patterns_.at(std::to_underlying(spec));
    const auto& items = p_->items().items;
    const auto pending = std::ranges::find_if(items[pattern].members, [&](u32 m) {
      return items[m].kind != ItemKind::access_spec && !p_->class_member_cells_.contains(std::pair(m, std::to_underlying(spec)));
    });
    const u32 limit = pending == items[pattern].members.end() ? items::none : *pending;
    // A later member of that name is part of the lookup set or hides what encloses the class: the
    // body cannot be instantiated yet.
    const auto spelling = p_->world().ins.names.spelling(name);
    if (std::ranges::any_of(pending, items[pattern].members.end(), [&](u32 m) {
          return std::ranges::any_of(items[m].names, [&](const items::DeclName& n) {
            return n.name == spelling && !(n.flags & items::name_flag::friend_);
          });
        }))
      named_pending_member = true;
    const EntityId saved = std::exchange(instantiating, spec);
    auto found = members_before(pattern, limit, spec, name, type_only, functions);
    instantiating = saved;
    return found;
  }
  bool named_pending_member = false;
  // Past the closing brace of the class the item defines, that class is complete.
  bool own_class_complete = false;
  std::optional<u32> declaration_lookup_end;
  EntityId completing_class = EntityId::none;
  bool special_member_exception = false;
  std::optional<u32> class_default_argument_start;
  bool failed_class_completion_dependency = false;
  // The original tokens when checking or replaying a default member initializer.
  std::optional<Program::Dmi> initializer_source;
  bool checking_default_member = false;  // the declaration's semantic check
  bool observed_enum_definition_failure = false;
  bool retain_type_trait_pattern = false;
  bool observed_type_trait_failure = false;
  bool observed_class_assertion_failure = false;
  bool observed_variable_instantiation_failure = false;
  std::optional<u32> type_trait_demand;
  struct RestoreTypeTraitPattern {
    AtItem& q;
    bool saved = q.retain_type_trait_pattern;
    ~RestoreTypeTraitPattern() { q.retain_type_trait_pattern = saved; }
  };
  // At the initializer, lookup can find this declaration in its own namespace. Supply the
  // written template during replay as well, since the specialization does not declare a name.
  struct InitializerTemplate {
    NameId name;
    DeclId declaration;
    const ast::DeclSema* pattern;
  };
  std::optional<InitializerTemplate> initializer_template;
  struct RestoreInitializerTemplate {
    AtItem& q;
    std::optional<InitializerTemplate> saved = q.initializer_template;
    ~RestoreInitializerTemplate() { q.initializer_template = saved; }
  };
  // In a class template's instantiation (#88): completeness and constant evaluation are answered
  // at its point of instantiation ([temp.point]); lookup stays at the item.
  std::optional<u32> at_point;
  std::optional<Program::MemberDemand> layout_member_demand;
  struct RestoreLayoutMemberDemand {
    AtItem& q;
    std::optional<Program::MemberDemand> saved = q.layout_member_demand;
    ~RestoreLayoutMemberDemand() { q.layout_member_demand = saved; }
  };
  EntityId instantiating = EntityId::none;  // the class specialization whose member this is
  EntityId replaying_scoped_enum = EntityId::none;
  // In a re-analysed operand: the offsets of calls that were dependent where it appears,
  // which argument-dependent lookup at the point of use completes ([temp.dep.candidate]).
  std::vector<u32> instantiation_calls;
  bool dependent_call(sema::u32 offset) override { return std::ranges::contains(instantiation_calls, offset); }

private:
  static bool inherited_default_tokens(sema::Sema& replay, const pp::Region& region,
                                       const Program::Dmi& dmi, EntityId base);
  static bool inherited_default_arithmetic(sema::Sema& replay, NodeId root, EntityId base,
                                           const sema::ClassRec& record);

  // A replay sees the original namespace prefix and a complete enclosing class ([class.mem.general]).
  u32 namespace_lookup_item() const {
    return initializer_source ? initializer_source->item : declaration_lookup_end.value_or(item_);
  }
  bool namespace_lookup_includes_item(std::optional<NameId> name = std::nullopt) const {
    return !declaration_lookup_end && !initializer_source &&
           (self_ || (name && initializer_template && *name == initializer_template->name));
  }
  std::optional<ast::DeclSema> lookup_declaration(DeclId d) {
    if (initializer_template && initializer_template->declaration == d) return *initializer_template->pattern;
    return p_->decl_sema(d);
  }

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
    const auto definition = p_->class_items_.find(std::to_underlying(completing_class));
    // An implicit specialization has no defining item of its own. Its values are queried at
    // the specialization's demand point, while a written class sees its complete member list.
    return definition == p_->class_items_.end() ? point() : p_->last_member(definition->second);
  }
  Program::MemberDemand member_demand(u32 item) const {
    if (layout_member_demand) return *layout_member_demand;
    if (at_point) return {item};
    return {item, self_, own_class_complete, completing_class};
  }
  u32 point() const { return layout_member_demand ? layout_member_demand->item : at_point.value_or(item_); }
  Program* p_;
  u32 item_;
  bool self_;
};

// Keep the written form before replay can replace an immediate invocation or sizeof with a literal.
bool AtItem::inherited_default_tokens(sema::Sema& replay, const pp::Region& region,
                                      const Program::Dmi& dmi, EntityId base) {
  using enum Tk;
  auto tokens = std::span(region.tokens).subspan(dmi.begin, dmi.end - dmi.begin);
  if (dmi.braced) {
    if (tokens.size() < 2 || tokens.front().kind != l_brace || tokens.back().kind != r_brace) return false;
    tokens = tokens.subspan(1, tokens.size() - 2);
    if (!tokens.empty() && tokens.back().kind == comma) tokens = tokens.first(tokens.size() - 1);
  }
  Tk previous = eof;
  for (const auto token : tokens) {
    switch (token.kind) {
    case int_lit: case float_lit: {
      const auto& written = region.literal(token);
      if (written.bad || written.overflow || written.ud_suffix != lex::no_name) return false;
      break;
    }
    case identifier: {
      const auto name = replay.ins().names.intern(region.name(token));
      const auto member = replay.find_member(replay.current_class, name);
      if (!member || member->ambiguous || !member->field || member->owner != base) return false;
      break;
    }
    case l_paren:
      if (previous == identifier || previous == r_paren || previous == kw_this || lex::is_literal(previous)) return false;
      break;
    case r_paren: case kw_this: case arrow: case plus: case minus: case star: case slash: case percent:
      break;
    default:
      return false;
    }
    previous = token.kind;
  }
  return true;
}

bool AtItem::inherited_default_arithmetic(sema::Sema& replay, NodeId root, EntityId base,
                                          const sema::ClassRec& record) {
  const auto& tree = replay.ins().ast;
  const auto base_type = replay.types().pointer(replay.types().class_type(base));
  auto unparenthesized = [&](NodeId id) {
    while (id != NodeId::none && tree.is<ast::Paren>(id)) id = tree.get<ast::Paren>(id).operand.id;
    return id;
  };
  const auto field_read = [&](NodeId id) {
    id = unparenthesized(id);
    if (id == NodeId::none || !tree.is<ast::Member>(id)) return false;
    const auto member = tree.get<ast::Member>(id);
    if (!member.arrow || member.reference || member.info.category != ast::ValueCategory::lvalue ||
        member.field >= record.fields.size() || record.fields[member.field].entity != member.member ||
        !replay.types().is_arithmetic(member.info.type) ||
        sema::Types::is_volatile(replay.types().canonical(member.info.type))) return false;
    const auto object = unparenthesized(member.object.id);
    if (object == NodeId::none || !tree.is<ast::ImplicitConversion>(object)) return false;
    const auto adjusted = tree.get<ast::ImplicitConversion>(object);
    if (adjusted.conversion != ast::ConversionKind::derived_to_base || adjusted.function != base ||
        adjusted.info.type != base_type) return false;
    const auto self = unparenthesized(adjusted.operand.id);
    return self != NodeId::none && tree.is<ast::This>(self) && tree.get<ast::This>(self).info.type == *replay.this_type;
  };
  auto arithmetic = [&](const ast::ExprInfo& info) {
    return info.category == ast::ValueCategory::prvalue && replay.types().is_arithmetic(info.type);
  };
  std::vector<NodeId> pending{root};
  while (!pending.empty()) {
    NodeId id = pending.back();
    pending.pop_back();
    if (id == NodeId::none) return false;
    if (tree.is<ast::Literal>(id)) {
      if (!arithmetic(tree.get<ast::Literal>(id).info)) return false;
    } else if (tree.is<ast::ValueInit>(id)) {
      if (!arithmetic(tree.get<ast::ValueInit>(id).info)) return false;
    } else if (tree.is<ast::Paren>(id)) {
      const auto paren = tree.get<ast::Paren>(id);
      if (!arithmetic(paren.info)) return false;
      pending.push_back(paren.operand.id);
    } else if (tree.is<ast::Unary>(id)) {
      const auto unary = tree.get<ast::Unary>(id);
      if (!arithmetic(unary.info) || (unary.op != ast::UnaryOp::plus && unary.op != ast::UnaryOp::minus)) return false;
      pending.push_back(unary.operand.id);
    } else if (tree.is<ast::Binary>(id)) {
      const auto binary = tree.get<ast::Binary>(id);
      using enum ast::BinaryOp;
      if (!arithmetic(binary.info) ||
          (binary.op != add && binary.op != sub && binary.op != mul && binary.op != div && binary.op != rem))
        return false;
      pending.push_back(binary.rhs.id);
      pending.push_back(binary.lhs.id);
    } else if (tree.is<ast::ImplicitConversion>(id)) {
      const auto conversion = tree.get<ast::ImplicitConversion>(id);
      if (!arithmetic(conversion.info) || conversion.function != EntityId::none) return false;
      using enum ast::ConversionKind;
      switch (conversion.conversion) {
      case lvalue_to_rvalue:
        if (!field_read(conversion.operand.id)) return false;
        break;
      case integral: case floating: case floating_integral: case boolean:
        pending.push_back(conversion.operand.id);
        break;
      default:
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

bool valid_member_declarations(std::span<const engine::Diagnostic> diagnostics);

std::optional<NodeId> AtItem::replay_default_member(sema::Sema& caller, EntityId field, EntityId required_base,
                                                  bool fallback) {
  const auto dmi = p_->dmi(field);
  if (!dmi) return std::nullopt;
  const auto& entity = p_->world().entities.rec(field);
  const auto arguments = p_->class_args_.find(std::to_underlying(entity.scope));
  if (fallback && (arguments == p_->class_args_.end() || !caller.types().is_scalar(entity.type))) return std::nullopt;
  if (dmi->invalid) return NodeId::none;
  const u32 offset = dmi->begin - p_->items().items[dmi->item].begin;
  AtItem source(*p_, dmi->item, false);
  source.completing_class = entity.scope;
  source.initializer_source = *dmi;
  source.checking_default_member = checking_default_member;
  sema::Sema replay(p_->world(), caller.builder(), source, caller.diagnostics(), p_->id(dmi->item), offset);
  replay.current_class = entity.scope;
  replay.this_type = replay.types().pointer(replay.types().class_type(entity.scope));
  replay.unevaluated = caller.unevaluated;
  if (checking_default_member || arguments != p_->class_args_.end()) replay.template_scopes = p_->enclosing_heads(dmi->item);
  if (arguments != p_->class_args_.end()) {
    source.at_point = default_instantiation_point.value_or(point());
    replay.template_args = arguments->second;
  }
  replay.push_scope();
  const sema::Sema::DefaultMemberScope nested(replay, true);

  const sema::ClassRec* base = nullptr;
  if (!checking_default_member && required_base != EntityId::none) {
    base = replay.class_rec(required_base);
    if (!base || !inherited_default_tokens(replay, p_->region(), *dmi, required_base)) {
      replay.diag(0, Diag::not_supported, Tk::equal);
      return NodeId::none;
    }
  }

  auto& active = p_->replaying_default_members_;
  if (!active.insert(field).second) {
    // [dcl.init.aggr]/13: only a potentially evaluated use can require this initializer again.
    if (caller.unevaluated) return replay.value_init(entity.type, 0, 0).id;
    replay.diag(0, Diag::recursive_default_member_initializer, entity.name);
    return NodeId::none;
  }
  struct EndReplay {
    std::set<EntityId>& active;
    EntityId field;
    ~EndReplay() { active.erase(field); }
  } end_replay{active, field};

  Parser parser(p_->region(), dmi->begin, dmi->end, replay);
  parser.set_questions(source);
  const std::size_t diag_begin = replay.diag_count();
  const std::size_t end_diag_begin = p_->end_diags_.size();
  const auto instantiation_failed = [&] {
    return source.observed_enum_definition_failure ||
           !valid_member_declarations(std::span(p_->end_diags_).subspan(end_diag_begin));
  };
  Ex init = parser.member_initializer(entity.type, dmi->braced, dmi->form);
  if (instantiation_failed() || replay.has_error_since(diag_begin)) return NodeId::none;
  if (!init.ok() || !parser.at_end()) {
    if (parser.at_end()) replay.diag(parser.pos(), Diag::expected_expression);
    else replay.diag(parser.pos(), Diag::unexpected_token, parser.kind());
    return NodeId::none;
  }
  if (fallback) {
    const auto value = sema::Evaluator(replay, source).evaluate_initializer(init).value;
    if (instantiation_failed()) return NodeId::none;
    if (!value) return std::nullopt;
    const Ex folded = replay.literal(sema::Types::unqualified(entity.type), *value, ast::ValueCategory::prvalue, 0, 0);
    return folded.id;
  }
  if (!checking_default_member && (replay.dependent(init) || !replay.closures.empty())) {
    replay.diag(0, Diag::not_supported, Tk::equal);
    return NodeId::none;
  }

  NodeId root = caller.builder().commit(ast::Child{init.id, 0}).id;
  if (base && !inherited_default_arithmetic(replay, root, required_base, *base)) {
    replay.diag(init.begin, Diag::not_supported, Tk::equal);
    return NodeId::none;
  }
  replay.check_immediate_addresses(root, init.begin);
  auto check = [&](this const auto& self, const ast::Red& node) -> void {
    if (node.is<ast::ConstantProbe>()) return;
    if (node.is<ast::DefaultMemberInit>()) return;  // a nested replay checked this child
    if (node.is<ast::Throw>()) {
      replay.diag(node.offset(), Diag::not_supported, Tk::kw_throw);
    } else if (node.is<ast::Materialize>() && node.get<ast::Materialize>().destructor != EntityId::none) {
      replay.diag(node.offset(), Diag::default_member_initializer_cleanup);
    }
    node.for_each_child(self);
  };
  if (!checking_default_member) check(ast::Red(replay.ins().ast, root, init.begin));
  if (replay.has_error_since(diag_begin)) return NodeId::none;
  caller.demands.insert(caller.demands.end(), replay.demands.begin(), replay.demands.end());
  return init.id;
}


// [dcl.fct]/5: array and function parameter types can need adjustment after substitution.
QualType adjusted_parameter_type(sema::Types& types, QualType type) {
  if (types.is_array(type)) return types.add_cv(types.pointer(types.rec(type).inner), sema::Types::cv(type));
  if (types.is_function(type)) return types.pointer(type);
  return type;
}

std::optional<ast::DeclSema> AtItem::template_decl(EntityId t) {
  auto decl = p_->declaration(t, p_->end());
  const auto spec = p_->member_exceptions_.find(t);
  if (decl && spec != p_->member_exceptions_.end() && !spec->second.condition_tokens) {
    // Specialization uses the written bare noexcept. The declaration type used for
    // matching the member's out-of-class definition keeps its existing form.
    auto& types = p_->world().types;
    const auto f = types.rec(decl->type);
    decl->type = types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, true);
  }
  return decl;
}

std::optional<QualType> AtItem::replay_alias_calls(sema::Sema& caller, const ast::DeclSema& alias,
                                                  const sema::Bindings& args) {
  const auto& pattern = p_->alias_call_patterns_.at(alias.entity);
  const auto& item = p_->items_->items[pattern.item];
  AtItem q(*p_, pattern.item, false);
  q.at_point = default_instantiation_point.value_or(point());
  q.type_trait_demand = q.at_point;
  const auto before = caller.diag_count();
  sema::Sema s(p_->world(), caller.builder(), q, caller.diagnostics(), p_->id(pattern.item), 0);
  s.replaying_type_default = caller.replaying_type_default;
  s.replaying_alias_type_arguments = pattern.call.qualified_type_arguments;
  s.template_scopes.push_back(p_->world().ins.seqs.elements(alias.template_params));
  s.template_args = args;
  Parser ps(*p_->r_, item.begin, item.begin + pattern.end, s);
  ps.set_questions(q);
  ps.seek(item.begin + pattern.begin);
  ps.namespace_alias_type = pattern.call.class_result;
  // Substitution stops at an invalid argument before a later argument can instantiate a class.
  ps.substituting_partial_arguments = pattern.call.qualified_type_arguments;
  try {
    auto type = ps.type_id();
    if (type && ps.at_end() && !s.has_error_since(before)) return type;
  } catch (const TooDeep&) {
    // The parser has already diagnosed its limit.
  }
  caller.diagnosed_constant_calls += s.diagnosed_constant_calls;
  return std::nullopt;
}

std::optional<QualType> AtItem::replay_member_exception(sema::Sema& caller, const ast::DeclSema& decl,
                                                        EntityId function, QualType type) {
  const auto slot = p_->member_exceptions_.find(decl.entity);
  if (slot == p_->member_exceptions_.end()) return type;
  if (!slot->second.condition_tokens) {  // bare `noexcept`
    if (p_->explicitly_specialized(function, p_->end())) return type;
    auto& types = p_->world().types;
    const auto f = types.rec(type);
    return types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, true);
  }
  if (caller.in_discarded_constraint_operand) return type;  // [temp.constr.op]
  // An explicit specialization has its own declaration's ([temp.expl.spec]).
  if (p_->explicitly_specialized(function, p_->end())) return type;
  if (const auto cached = p_->resolved_member_exceptions_.find(function); cached != p_->resolved_member_exceptions_.end())
    return cached->second;
  const auto item = slot->second.item;
  const auto tokens = *slot->second.condition_tokens;
  // A member template's or a class template member's declaration in its class; a namespace-scope
  // function template is `decl` itself.
  const auto written = p_->member_decl(decl.entity);
  const bool member_of_class = (decl.flags & sema::decl_flag::member) != 0;
  if (member_of_class && !written) return std::nullopt;
  const auto& record = p_->world().entities.rec(function);
  auto& ins = p_->world().ins;
  ast::DeclSema member = decl;
  // A friend a class template defines: in the scope of the specialization that defines it (#259).
  const auto friend_of = (decl.flags & sema::decl_flag::friend_) ? p_->member_patterns_.find(std::to_underlying(decl.entity))
                                                                 : p_->member_patterns_.end();
  const bool class_friend = friend_of != p_->member_patterns_.end();
  const EntityId cls = member_of_class ? record.scope : class_friend ? friend_of->second.spec : EntityId::none;
  if (class_friend) {
    for (const ast::DeclSema& d : ins.seqs.elements(p_->class_member_sema(item, cls).decls))
      if (d.entity == decl.entity) member = d;
  } else if (written) {
    // An overload candidate may come from a declaration registered before its builder committed
    // the parameter lists. Read the committed declaration in this class specialization.
    const auto& cell = p_->member_patterns_.contains(std::to_underlying(decl.entity))
        ? p_->class_member_sema(item, record.scope)
        : p_->item_sema(item).value;
    member = ins.seqs.at(cell.decls, ast::number_of(written->ord));
  } else {
    // A namespace-scope function template: its declaration in the item that wrote the tokens.
    for (const ast::DeclSema& d : ins.seqs.elements(p_->item_sema(item).value.decls))
      if (d.entity == decl.entity) member = d;
  }
  const u32 item_begin = p_->items().items[item].begin;
  AtItem source(*p_, item, false);
  source.at_point = default_instantiation_point.value_or(point());
  if (cls != EntityId::none) source.completing_class = cls;
  const auto pattern = p_->member_exception_patterns_.find(item);
  if (pattern != p_->member_exception_patterns_.end()) {
    if (!pattern->second) return std::nullopt;
    if (pattern->second->dependent_type_traits) source.type_trait_demand = source.at_point;
  }
  // Errors in this instantiation are outside the immediate context of substitution
  // ([temp.deduct.general]). Preserve their original locations even inside a requirement.
  std::vector<engine::Diagnostic> instantiation_diags;
  struct PublishInstantiationDiagnostics {
    std::vector<engine::Diagnostic>& diagnostics;
    std::vector<engine::Diagnostic>& destination;
    ~PublishInstantiationDiagnostics() {
      for (engine::Diagnostic& diagnostic : diagnostics)
        if (!std::ranges::contains(destination, diagnostic)) destination.push_back(std::move(diagnostic));
    }
  } publish{instantiation_diags, p_->end_diags_};
  auto& diagnostics = source.type_trait_demand ? instantiation_diags : caller.diagnostics();
  sema::Sema replay(p_->world(), caller.builder(), source, diagnostics, p_->id(item), 0);
  if (pattern != p_->member_exception_patterns_.end()) {
    source.instantiation_calls = pattern->second->calls;
    replay.expansions_override = &pattern->second->expansions;
  }
  const auto before = replay.diag_count();
  if (class_friend) replay.current_class = cls;
  if (member_of_class) {
    replay.current_class = record.scope;
    // `this` points to the specialization's class, not to the class template's pattern.
    if (p_->this_type(decl)) {
      auto& types = p_->world().types;
      replay.this_type = types.pointer(types.add_cv(types.class_type(record.scope), types.rec(decl.type).fn_cv));
    }
  }
  replay.current_function = function;

  auto& active = p_->replaying_member_exceptions_;
  if (p_->too_deep(active.size(), item, tokens.first - item_begin)) return std::nullopt;
  if (!active.insert(function).second) {
    replay.diag(tokens.first - item_begin, Diag::recursive_noexcept);
    return std::nullopt;
  }
  struct EndReplay {
    std::set<EntityId>& active;
    EntityId function;
    ~EndReplay() { active.erase(function); }
  } end_replay{active, function};

  // The heads of the enclosing class templates, bound to the class specialization's arguments,
  // then the function template's own, bound to the specialization's; an ordinary member of a
  // class template has none of its own.
  auto head = ins.seqs.elements(member.template_params);
  const bool own_head = (member.flags & sema::decl_flag::template_) && !head.empty();
  const sema::u16 depth = head.empty() ? 0 : head.front().depth;
  replay.template_scopes = p_->enclosing_heads(item);
  if (own_head) replay.template_scopes.push_back(std::move(head));
  if (auto outer = p_->class_args_.find(std::to_underlying(class_friend ? cls : record.scope)); outer != p_->class_args_.end())
    replay.template_args = outer->second;
  if (own_head) {
    auto member_arguments = sema::Bindings::of(depth, record.args);
    replay.template_args.levels.resize(depth + 1);
    replay.template_args.levels[depth] = std::move(member_arguments.levels[depth]);
  }
  replay.push_scope();
  auto t = replay.templates();
  for (const auto& param : ins.seqs.elements(member.params)) {
    if (param.name == replay.no_name()) continue;
    if (replay.types().kind(param.type) == sema::TypeKind::pack_expansion) {
      replay.declare_param_pack(param.name);
      auto expansion = t.substitute_expansion(t.type_arg(param.type), replay.template_args);
      if (!expansion) {
        replay.diag(tokens.first - item_begin, Diag::not_supported, Tk::kw_noexcept);
        return std::nullopt;
      }
      for (const auto& element : *expansion)
        replay.declare_pack_element(param.name, adjusted_parameter_type(replay.types(), element.type));
      continue;
    }
    auto parameter = t.substitute(param.type, replay.template_args);
    if (!parameter) {
      replay.diag(tokens.first - item_begin, Diag::not_supported, Tk::kw_noexcept);
      return std::nullopt;
    }
    replay.declare_local(param.name, adjusted_parameter_type(replay.types(), *parameter),
                         sema::local_flag::parameter, 0, false);
  }

  Parser parser(p_->region(), item_begin, tokens.second, replay);
  parser.set_questions(source);
  parser.seek(tokens.first);
  Parser::Suffix exception;
  bool ok = true;
  bool nonthrowing = false;
  try {
    parser.exception_specifier(exception, ok);
    nonthrowing = ok && parser.noexcept_value(exception.noexcept_condition, ok);
  } catch (const TooDeep&) {
    return std::nullopt;
  }
  if (!ok || !parser.at_end() || replay.has_error_since(before)) return std::nullopt;
  caller.demands.insert(caller.demands.end(), replay.demands.begin(), replay.demands.end());
  const auto signature = replay.types().rec(type);
  auto resolved = replay.types().function(signature.inner, signature.params, signature.variadic, signature.linkage,
                                          signature.fn_cv, signature.fn_ref, nonthrowing);
  p_->resolved_member_exceptions_.emplace(function, resolved);
  return resolved;
}

std::optional<bool> AtItem::replay_destructor_exception(sema::Sema& caller, EntityId cls, u32 at) {
  auto* completion = p_->class_completion(cls);
  auto result = completion ? p_->resolve_destructor_exception(*completion, &caller, at) : std::nullopt;
  failed_class_completion_dependency |= !result;
  return result;
}

std::optional<NodeId> AtItem::replay_function_default(sema::Sema& caller, const ast::DefaultArg& value,
                                                       EntityId function, QualType parameter, u32 at) {
  const auto specialization = p_->world().entities.rec(function);
  if (auto* completion = p_->class_completion(specialization.scope)) {
    const u32 begin = p_->items().items[p_->index(value.source)].begin + value.source_begin;
    if (auto pending = completion->defaults.find({function, begin}); pending != completion->defaults.end()) {
      if (completing_class == completion->cls && class_default_argument_start && begin >= *class_default_argument_start) {
        caller.diag(at, Diag::default_arg_not_yet_defined);
        failed_class_completion_dependency = true;
        return NodeId::none;
      }
      const NodeId result = p_->resolve_member_default(*completion, function, pending->second, &caller, at);
      if (result != NodeId::none) return caller.builder().adopt(result);
      failed_class_completion_dependency = true;
      return NodeId::none;
    }
  }
  if (specialization.templ == EntityId::none || !p_->is_namespace(specialization.scope) ||
      !caller.types().is_scalar(parameter)) return std::nullopt;
  const u32 item = p_->index(value.source);
  const auto& written = p_->items().items[item];
  auto& seqs = p_->world().ins.seqs;
  const auto declarations = seqs.elements(p_->item_sema(item).value.decls);
  const auto decl = std::ranges::find(declarations, specialization.templ, &ast::DeclSema::entity);
  if (decl == declarations.end()) return std::nullopt;

  AtItem source(*p_, item, false);
  source.from = p_->namespace_path(specialization.scope);
  source.at_point = default_instantiation_point.value_or(point());
  source.instantiation_calls = seqs.elements(value.dependent_calls);
  sema::Sema replay(p_->world(), caller.builder(), source, caller.diagnostics(), value.source, 0);
  replay.unevaluated = caller.unevaluated;
  replay.immediate_context = caller.immediate_context || caller.is_consteval(function);
  replay.constant_reference_context = caller.constant_reference_context;
  replay.trial_initializer = caller.trial_initializer;
  const auto params = seqs.elements(decl->template_params);
  if (params.empty()) return std::nullopt;
  replay.template_scopes.push_back(params);
  replay.template_args = sema::Bindings::of(params.front().depth, specialization.args);
  replay.push_scope();
  const auto written_params = seqs.elements(decl->params);
  const auto default_param = std::ranges::find_if(written_params, [&](const ast::ParamSema& param) {
    if (param.default_arg == NodeId::none) return false;
    const auto arg = p_->world().ins.ast.get<ast::DefaultArg>(param.default_arg);
    return arg.source == value.source && arg.source_begin == value.source_begin && arg.source_end == value.source_end;
  });
  if (default_param == written_params.end()) return std::nullopt;
  for (const auto& param : std::ranges::subrange(written_params.begin(), default_param + 1)) {
    if (param.name == replay.no_name()) continue;
    const auto type = replay.templates().substitute(param.type, replay.template_args);
    if (!type) return std::nullopt;
    replay.declare_local(param.name, adjusted_parameter_type(replay.types(), *type),
                         sema::local_flag::parameter | sema::local_flag::default_argument_parameter, 0, false);
  }

  auto& active = p_->replaying_function_defaults_;
  const auto key = std::tuple{function, value.source, value.source_begin};
  if (!active.insert(key).second) {
    replay.diag(value.source_begin, Diag::default_arg_dependent);
    return NodeId::none;
  }
  struct EndReplay {
    decltype(active) set;
    decltype(key) entry;
    ~EndReplay() { set.erase(entry); }
  } end_replay{active, key};

  Parser parser(p_->region(), written.begin, written.begin + value.source_end, replay);
  parser.set_questions(source);
  parser.seek(written.begin + value.source_begin);
  const auto previous_diags = replay.diag_count();
  NodeId result;
  try {
    result = parser.default_argument(parameter);
  } catch (const TooDeep&) {
    return NodeId::none;
  }
  if (replay.has_error_since(previous_diags)) return NodeId::none;
  if (result == NodeId::none || !parser.at_end() || !replay.closures.empty() ||
      caller.builder().get<ast::DefaultArg>(result).source_end != 0) {
    replay.diag(value.source_begin, Diag::default_arg_dependent);
    return NodeId::none;
  }
  if (replay.needs_immediate_reference_check()) {
    const auto arg = caller.builder().get<ast::DefaultArg>(result).arg;
    const NodeId root = caller.builder().commit(ast::Child{arg.id, 0}).id;
    replay.check_immediate_addresses(root, value.source_begin);
    if (replay.has_error_since(previous_diags)) return NodeId::none;
  }
  caller.retain_immediate_functions(replay);
  caller.demands.insert(caller.demands.end(), replay.demands.begin(), replay.demands.end());
  return result;
}

// Sema's questions in an instantiation of the function template that item `item` defines, at
// the end of the translation unit (§B6.2). Unqualified names are looked up at the template's own
// item, as its definition was ([temp.res], [temp.nondep]); argument-dependent lookup for the
// calls that were dependent, completeness and constant evaluation are answered at the end.
// (Fable's decision on #88 asks non-dependent constructs at the template's context; the end
// answers the same unless a later declaration changes them, which [temp.res.general]/6 makes
// ill-formed, no diagnostic required.)
class AtInstantiation final : public sema::Questions {
public:
  AtInstantiation(Program& p, u32 item) : p_(&p), item_(item), at_(p, item, true) {
    at_.default_instantiation_point = p.end();
  }
  bool exceptions_enabled() const override { return p_->exceptions_enabled(); }
  bool rtti_enabled() const override { return p_->rtti_enabled(); }
  ast::Compatibility compatibility() const override { return p_->compatibility(); }
  sema::AliasCallPattern alias_call_pattern(EntityId e) override { return at_.alias_call_pattern(e); }
  std::optional<QualType> replay_alias_calls(sema::Sema& caller, const ast::DeclSema& alias,
                                           const sema::Bindings& args) override {
    return at_.replay_alias_calls(caller, alias, args);
  }
  std::optional<NodeId> replay_default_member(sema::Sema& caller, EntityId field, EntityId required_base,
                                              bool fallback) override {
    return at_.replay_default_member(caller, field, required_base, fallback);
  }
  std::optional<NodeId> replay_function_default(sema::Sema& caller, const ast::DefaultArg& value,
                                                EntityId function, QualType parameter, u32 at) override {
    return at_.replay_function_default(caller, value, function, parameter, at);
  }
  std::optional<bool> replay_destructor_exception(sema::Sema& caller, EntityId cls, u32 at) override {
    return at_.replay_destructor_exception(caller, cls, at);
  }
  std::optional<QualType> replay_member_exception(sema::Sema& caller, const ast::DeclSema& decl,
                                                 EntityId function, QualType type) override {
    return at_.replay_member_exception(caller, decl, function, type);
  }
  bool deferred_exception(EntityId f) override { return at_.deferred_exception(f); }
  bool unresolved_special_member_exception(EntityId f) override { return at_.unresolved_special_member_exception(f); }
  const sema::Classes& classes() override { return p_->classes(); }
  sema::CopyAssignment require_copy_assignment(EntityId f) override { return p_->require_copy_assignment(f); }
  std::optional<bool> defaulted_nonthrowing(EntityId f) override { return p_->defaulted_nonthrowing(f); }
  std::set<EntityId> uncertain_object_layouts() override { return at_.uncertain_object_layouts(); }
  bool peer_pure_dispatch(EntityId cls) override { return at_.peer_pure_dispatch(cls); }
  void use_typed_dynamic_cast() override { at_.use_typed_dynamic_cast(); }
  u32 presumed_line(engine::Loc at) override { return at_.presumed_line(at); }
  sema::PresumedSource presumed_source(engine::Loc at) override { return at_.presumed_source(at); }
  bool in_default_member_initializer() const override { return at_.in_default_member_initializer(); }
  bool class_layout_uncertain(EntityId c) override { return at_.class_layout_uncertain(c); }
  bool explicit_specialization(EntityId c) override { return at_.explicit_specialization(c); }
  const sema::ClassRec& publish_class(EntityId c, sema::ClassRec r, sema::Sema& context) override {
    return p_->publish_class(c, std::move(r), item_, context);
  }
  std::vector<DeclId> visible_decls(ast::NameId name, std::span<const EntityId> directives = {},
                                    bool immediate_call = false) override {
    return at_.visible_decls(name, directives, immediate_call);
  }
  std::vector<ast::DeclSema> qualifier_decls(ast::NameId name, std::optional<EntityId> ns,
                                            std::span<const EntityId> directives) override {
    return at_.qualifier_decls(name, ns, directives);
  }
  std::optional<EntityId> namespace_named(ast::NameId name, std::span<const EntityId> directives = {}) override {
    return at_.namespace_named(name, directives);
  }
  std::vector<ast::DeclSema> namespace_members(EntityId ns, ast::NameId name, bool type_only = false,
                                               bool direct_only = false, bool immediate_call = false) override {
    return at_.namespace_members(ns, name, type_only, direct_only, immediate_call);
  }
  std::vector<ast::DeclSema> adl_functions(EntityId ns, ast::NameId name, bool at_instantiation) override {
    return at_instantiation ? p_->adl_functions(ns, name, p_->end(), false) : at_.adl_functions(ns, name, false);
  }
  const sema::ClassRec* adl_class(EntityId c, bool at_instantiation) override {
    if (!at_instantiation) return at_.adl_class(c, false);
    return is_complete(c) ? p_->classes().find(c) : nullptr;
  }
  ast::DeclSema decl_sema(DeclId d) override { return at_.decl_sema(d); }
  ast::DeclSema friend_decl(const ast::MemberRec& f) override { return at_.friend_decl(f); }
  std::optional<ast::DeclSema> written_member(EntityId f) override { return at_.written_member(f); }
  std::optional<ast::DeclSema> class_friend(EntityId f) override { return at_.class_friend(f); }
  void use_allocation_function(EntityId f, bool aligned) override { at_.use_allocation_function(f, aligned); }
  void use_library_function(EntityId f) override { at_.use_library_function(f); }
  void use_template_parameter_object(EntityId e) override { at_.use_template_parameter_object(e); }
  bool plain_class_template_type(EntityId c) override { return at_.plain_class_template_type(c); }
  interp::Callee function_ir(EntityId f, bool& diagnosed_failure) override {
    return p_->function_ir(f, p_->end(), &diagnosed_failure);
  }
  std::optional<QualType> deduced_type(EntityId f) override { return p_->deduced_type(f, p_->end()); }
  sema::GenericResult generic_instance(EntityId spec, const sema::GenericClosure* known, const sema::GenericBinding&) override {
    return p_->generic_instance(spec, known, p_->end());
  }
  void generic_defined(const sema::GenericClosure& g) override { p_->generic_defs_[std::to_underlying(g.call)] = g; }
  std::optional<interp::GlobalVar> global(EntityId v) override { return p_->global(v, p_->end()); }
  bool is_constexpr(EntityId f) override { return p_->is_constexpr(f, p_->end()); }
  bool is_immediate_function(EntityId f) override { return p_->is_immediate_function(f, p_->end()); }
  bool is_complete(EntityId c) override { return p_->is_complete(c, p_->end(), true, true); }
  bool may_have_virtual_layouts() override { return p_->may_have_virtual_layouts(); }
  bool is_complete_at_source(EntityId c) override { return at_.is_complete_at_source(c); }
  std::optional<bool> defining_enum_is_scoped(EntityId e) override { return at_.defining_enum_is_scoped(e); }
  std::optional<bool> defining_enum_is_fixed(EntityId e) override { return at_.defining_enum_is_fixed(e); }
  bool require_enum_definition(EntityId e) override { return p_->require_enum_definition(e, p_->end()); }
  // A type trait whose operands were dependent where the template defines it is answered at the
  // point of instantiation, the end ([temp.point]); a concrete one keeps its definition's answer.
  std::optional<bool> type_trait_at_demand(EntityId c, engine::Loc loc) override {
    if (!at_.dependent_type_trait_operand(loc)) return std::nullopt;
    return p_->is_complete(c, p_->end(), true, true);
  }
  engine::Loc source_loc(engine::Loc loc, u32 offset) override { return at_.source_loc(loc, offset); }
  bool type_trait_instantiation_failed(EntityId e) override { return at_.type_trait_instantiation_failed(e); }
  bool variable_instantiation_failed(EntityId v) override { return p_->variable_instantiation_failed(v, p_->end()); }
  std::optional<ConstId> constant_value(EntityId v) override { return p_->constant_value(v, p_->end()); }
  sema::ConstantCallEvaluation constant_call(sema::Sema& s, const sema::Ex& call) override {
    auto result = sema::Evaluator(s, *this).evaluate(call);
    return {result.value, result.reason, result.diagnosed_failure()};
  }
  // In a member's body instantiated while its class is, for a deduced return type: the members
  // declared so far.
  std::optional<ast::DeclSema> earlier_member(ast::NameId name, bool type_only, std::vector<ast::DeclSema>* functions) override {
    return instantiating_class ? at_.members_so_far(*instantiating_class, name, type_only, functions) : std::nullopt;
  }
  std::optional<EntityId> instantiating_class;  // the class of the member, not complete yet
  bool instantiation_gave_up() override { return p_->instantiation_gave_up(); }
  std::optional<QualType> specialization_type(EntityId v) override { return p_->specialization_type(v, p_->end()); }
  const std::vector<sema::Expansion>* pattern_expansions() override { return p_->expansions(item_, 1); }
  std::optional<bool> satisfied(ConstId c) override { return p_->satisfied(c, p_->end()); }
  void record_nested_constraint_operand(u32 begin, u32 end) override {
    at_.record_nested_constraint_operand(begin, end);
  }
  std::optional<u32> nested_constraint_operand_end(u32 begin) override {
    return at_.nested_constraint_operand_end(begin);
  }
  bool satisfaction_failed(ConstId c) override { return p_->satisfaction_failed(c); }
  bool value_dependent_member(EntityId v) override { return p_->is_value_dependent_member(v); }
  std::optional<ast::DeclSema> template_decl(EntityId t) override { return at_.template_decl(t); }
  std::optional<ast::DeclSema> replay_function_decl(EntityId function) override {
    return at_.replay_function_decl(function);
  }
  bool use_static_member(EntityId v, bool unevaluated) override {
    return p_->use_member(v, {p_->end()}, unevaluated);
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

// A direct base must be a complete non-union class ([class.derived], [class.union.general]).
std::optional<EntityId> complete_base_class(sema::Sema& s, QualType type, u32 at, bool allow_polymorphic = false) {
  if (!s.types().is_class(type) || s.types().is_dependent(type)) {
    s.diag(at, Diag::base_not_class, type);
    return std::nullopt;
  }
  if (!s.require_complete(type, at)) return std::nullopt;
  EntityId base = s.types().class_of(type);
  if (const sema::ClassRec* r = s.classes().find(base)) {
    if (r->union_) {
      s.diag(at, Diag::base_not_class, type);
      return std::nullopt;
    }
    if (r->is_final) {
      s.diag(at, Diag::final_base, s.types().class_type(base));
      return std::nullopt;
    }
    if (r->polymorphic && !allow_polymorphic) {
      s.diag(at, Diag::virtual_base_class, type);
      return std::nullopt;
    }
  }
  return base;
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
    s_.declaration_part = true;
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
    ps_.declare_class = [this](NameId n, Tk key, u32 at) -> std::optional<QualType> {
      if (p_->items().items[k_].parent != items::none &&
          p_->items().items[p_->items().items[k_].parent].kind == ItemKind::class_def)
        return std::nullopt;
      EntityId e = introduce_namespace_class(n);
      record_class_key(e, key, at);  // an error leaves the earlier type available for recovery
      return s_.types().class_type(e);
    };
  }

  // Analyses a member item of a class template specialization again, instantiating the
  // initializer of its static data member with declarator `ordinal` ([temp.inst]/3).
  ItemAnalysis(Program& p, u32 item, Program::ClassInstantiation inst, u32 ordinal,
               std::optional<Program::MemberDemand> layout_demand) : ItemAnalysis(p, item, std::move(inst)) {
    member_value_ = ordinal;
    member_layout_demand_ = layout_demand;
  }

  // `var` analyses a variable template's definition for the specialization it names, with the
  // template's arguments bound ([temp.inst]/7).
  ItemAnalysis(Program& p, u32 item, Program::VarInstantiation var) : ItemAnalysis(p, item) {
    var_ = std::move(var);
    q_.at_point = var_->point;
    q_.type_trait_demand = var_->point;
    s_.template_args = var_->args;
  }
  bool has_member_initial_value() const { return member_initial_value_; }
  bool type_trait_failure() const { return q_.observed_type_trait_failure; }
  bool class_assertion_failure() const { return q_.observed_class_assertion_failure; }
  bool variable_instantiation_failure() const { return q_.observed_variable_instantiation_failure; }

  Result<ast::ItemSema> replay_scoped_enum(EntityId e) {
    q_.replaying_scoped_enum = e;
    return run();
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
      if (member && class_head_alignment_ahead()) {
        unsupported_class_alignment();
        break;
      }
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
      if (member && class_head_alignment_ahead()) {
        unsupported_class_alignment();
      } else if (member && it_->unnamed_type_ordinal != items::none) {
        unnamed_union_member();
      } else if (member && p_->named_nested_class(k_)) {
        class_definition();
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
                    b_.seq(std::span<const ast::ClosureDef>(s_.closures)), asm_hidden_symbol_, asm_directive_at_};
    out_.value = b_.commit(v);
    s_.check_immediate_addresses(out_.value.tree);
    if (!s_.closures.empty()) s_.check_labels();
    for (const ast::ClosureDef& c : p_->world().ins.seqs.elements(out_.value.closures))
      s_.check_immediate_addresses(c.body, c.body_begin);
    p_->declaration_closures(out_.value.closures, k_, 0);
    out_.demands = std::move(s_.demands);
    return std::move(out_);
  }

private:
  // [dcl.asm]: this form takes a parenthesized constant-evaluated operand. Leave immediate
  // calls in the typed expression so evaluation can construct their result in the receiver.
  Ex namespace_asm_expression() {
    const u32 at = ps_.pos();
    ps_.bump();
    if (!ps_.expect(Tk::l_paren) || !ps_.expect(Tk::l_paren)) return {NodeId::none, at, ps_.pos()};
    ++s_.immediate_context;
    Ex operand = ps_.expression();
    --s_.immediate_context;
    if (!ps_.expect(Tk::r_paren) || !ps_.expect(Tk::r_paren) || !ps_.expect(Tk::semi))
      return {NodeId::none, at, ps_.pos()};
    if (!ps_.at_end()) {
      s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
      return {NodeId::none, at, ps_.pos()};
    }
    return operand;
  }

  static std::optional<std::string_view> hidden_asm_symbol(std::string_view bytes) {
    if (!bytes.starts_with(".hidden")) return std::nullopt;
    bytes.remove_prefix(std::string_view(".hidden").size());
    const auto first = bytes.find_first_not_of(" \t");
    if (first == 0 || first == std::string_view::npos) return std::nullopt;
    bytes.remove_prefix(first);
    if (bytes.ends_with('\n')) bytes.remove_suffix(1);
    auto start = [](char ch) {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '.' || ch == '$';
    };
    if (bytes.empty() || !start(bytes.front())) return std::nullopt;
    if (!std::ranges::all_of(bytes.substr(1), [&](char ch) { return start(ch) || (ch >= '0' && ch <= '9'); }))
      return std::nullopt;
    return bytes;
  }

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
    DeclAttributes leading_attributes;
    // [dcl.link]: a linkage-specification has no attribute-specifier-seq.
    // (GNU attributes there are accepted, as GCC and Clang do.)
    std::vector<ast::AnnotationSema> annotations;
    const int attributes = ps_.skip_attributes(&leading_attributes, Parser::attr_std | Parser::attr_gnu, &annotations);
    if ((attributes & Parser::attr_std) && ps_.is(Tk::kw_extern) && ps_.kind(1) == Tk::string_lit) {
      s_.diag(attrs_at, Diag::attributes_not_allowed);
      return bad();
    }
    if (ps_.is(Tk::kw_extern) && ps_.kind(1) == Tk::string_lit) {
      u32 at = ps_.pos() + 1;
      auto l = linkage_named(p_->region(), it_->begin + at);
      if (!l) s_.diag(at, Diag::unknown_linkage);
      lang = l.value_or(LangLinkage::cxx);
      explicit_lang = true;
      single_linkage_spec_ = true;  // [dcl.link]: it declares as if `extern` were given
      ps_.bump();
      ps_.bump();
      while (ps_.is(Tk::string_lit) && ps_.tok().has(lex::tok_flag::continued)) ps_.bump();
      ps_.skip_attributes(&leading_attributes, Parser::attr_std | Parser::attr_gnu, &annotations);
    }
    ps_.set_linkage(lang);
    // `template<>`: an explicit specialization ([temp.expl.spec]) of a template or its member.
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
      const Tk key = ps_.kind();
      if (class_head_alignment_ahead()) return unsupported_class_alignment();
      if (Parser::is_class_key(key) && ps_.kind(1) == Tk::identifier && ps_.kind(2) != Tk::less) {
        s_.diag(ps_.pos() + 1, Diag::specialization_needs_arguments, ps_.name_of(ps_.tok(1)));
        return bad();
      }
      if (Parser::is_class_key(key) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::less) {
        ps_.bump();
        NameId name = ps_.name_of(ps_.tok());
        u32 name_at = ps_.pos();
        ps_.bump();
        auto spec = explicit_class(name, name_at);
        if (!spec) return bad();
        if (!ps_.expect(Tk::semi)) return bad();
        ast::DeclSema sem = class_decl(*spec, false);
        sem.flags |= sema::decl_flag::specialization;
        if (!class_declared(sem, key, name_at)) return bad();
        return;
      }
    }
    // A template-head ([temp.pre]): what follows declares a template or a concept.
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
      const Tk key = ps_.kind();
      if (class_head_alignment_ahead()) return unsupported_class_alignment();
      if (Parser::is_class_key(key) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::semi) {
        NameId name = ps_.name_of(ps_.tok(1));
        u32 name_at = ps_.pos() + 1;
        ps_.bump();
        ps_.bump();
        ps_.bump();
        ast::DeclSema sem = class_template_decl(name, false);
        if (!class_declared(sem, key, name_at)) return bad();
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
        const auto begin = ps_.pos();
        const auto calls = ps_.deferred_immediate_calls();
        const auto class_calls = ps_.deferred_class_immediate_calls();
        const auto checks = ps_.deferred_argument_checks();
        const auto ranks = ps_.deferred_rank_checks();
        const auto type_arguments = ps_.deferred_alias_type_arguments();
        ps_.namespace_alias_type = true;
        auto t = ps_.type_id();
        ps_.namespace_alias_type = false;
        if (!t || !alias_template(name, name_at, *t)) return bad();
        sema::AliasCallPattern pattern;
        pattern.rank_checks = ps_.deferred_rank_checks() != ranks;
        pattern.qualified_type_arguments = ps_.deferred_alias_type_arguments() != type_arguments;
        using Kind = sema::AliasCallPattern::Kind;
        if (ps_.deferred_immediate_calls() != calls) {
          pattern.kind = ps_.deferred_argument_checks() == checks ? Kind::retained : Kind::may_erase;
          pattern.class_result = ps_.deferred_class_immediate_calls() != class_calls;
        } else if (!pattern.rank_checks && !s_.templates().structural_operand(*t)) {
          pattern.kind = Kind::unrepresented;
        }
        if (pattern.kind != Kind::none || pattern.rank_checks || pattern.qualified_type_arguments)
          p_->alias_call_patterns_.emplace(decls_.back().entity, Program::AliasCallPattern{k_, begin, ps_.pos(), pattern});
        if (!ps_.expect(Tk::semi)) bad();
        return;
      }
      // `template<…> struct X<args>;` declares a partial specialization ([temp.spec.partial]).
      if (Parser::is_class_key(key) && ps_.kind(1) == Tk::identifier && ps_.kind(2) == Tk::less) {
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
        sem.constraints = partial_constraints_;
        if (!class_declared(sem, key, name_at)) return bad();
        return;
      }
      if (ps_.is(Tk::kw_template) || Parser::is_class_key(key) ||
          ps_.is(Tk::kw_using) || ps_.is(Tk::kw_concept) || (it_->kind != ItemKind::function && !ps_.starts_declaration())) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        return bad();
      }
      template_ = true;
    }
    if (ps_.is(Tk::semi)) {
      if (!annotations.empty()) s_.diag(annotations.front().offset, Diag::attributes_not_allowed);
      ps_.bump();
      return;
    }
    if (ps_.is(Tk::kw_static_assert)) {
      if (!annotations.empty()) s_.diag(annotations.front().offset, Diag::attributes_not_allowed);
      return static_assert_declaration();
    }
    if (!template_ && !explicit_ && ps_.is(Tk::kw_asm)) {
      const u32 at = ps_.pos();
      if (!p_->world().target.supports(ast::NativeOperation::namespace_hidden_asm) ||
          ps_.kind(1) != Tk::l_paren || ps_.kind(2) != Tk::l_paren) {
        s_.diag(at, Diag::not_supported, Tk::kw_asm);
        return bad();
      }
      if (attributes || !annotations.empty()) {
        s_.diag(attrs_at, Diag::attributes_not_allowed);
        return bad();
      }
      const auto bytes = sema::Evaluator(s_, q_).evaluate_bytes(namespace_asm_expression());
      if (!bytes) return;
      const auto symbol = hidden_asm_symbol(*bytes);
      if (!symbol) {
        s_.diag(at, Diag::namespace_asm_directive);
        return;
      }
      asm_hidden_symbol_ = s_.ins().names.intern(*symbol);
      asm_directive_at_ = at;
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
          sc = ps_.committed_nested_name(ctor_at, next);
          if (!sc) return bad();
          auto flags = constructor_flags(ctor_at, dtor);
          if (!flags) return bad();
          return out_of_class_special(*sc->type, next - ctor_at, *flags);
        }
      }
    }
    if (class_head_alignment_ahead()) return aligned_class_declaration();
    if (!ps_.starts_declaration()) {
      if (lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon))
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // templates, using, static_assert, …
      else
        s_.diag(ps_.pos(), Diag::expected_declaration);
      return bad();
    }
    s_.defer_type_access = true;
    Parser::TypeAliasChecks return_alias_checks;
    DeclSpec spec = template_ ? ps_.function_template_decl_specifiers(return_alias_checks) : ps_.decl_specifiers();
    spec.attributes.visibility = std::max(spec.attributes.visibility, leading_attributes.visibility);
    spec.attributes.weak |= leading_attributes.weak;
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
      // its definition. A standalone declaration introduces the name in this scope.
      ps_.bump();
      if (!annotations.empty()) s_.diag(annotations.front().offset, Diag::attributes_not_allowed);
      auto sem = class_decl(class_entity(spec.class_name), false);
      sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(spec.annotations));
      if (spec.ok && !class_declared(sem, spec.class_key, spec.class_at)) return bad();
      if (!spec.ok || sema::Types::cv(spec.type) != sema::Cv::none || spec.is_static || spec.is_extern) bad();
      return;
    }
    if (!spec.ok && !spec.has_type && ps_.qualified_ahead()) {
      // A diagnosed qualified type is still at the current token.
      s_.settle_type_access(EntityId::none);
      bad();
      if (it_->kind != ItemKind::function) ps_.skip_to_semi();
      return;
    }
    spec.annotations = std::move(annotations);
    bool is_function_def = it_->kind == ItemKind::function;
    u32 declarators = 0;
    do {
      s_.defer_type_access = true;
      const bool saved_defaults = std::exchange(ps_.consteval_parameter_defaults, spec.is_consteval);
      parse::Declarator d = ps_.declarator(spec.type, false, spec.is_typedef);
      ps_.consteval_parameter_defaults = saved_defaults;
      // [dcl.spec.auto.general]: a function with a placeholder type is the only declarator.
      if (d.ok && s_.types().has_placeholder(spec.type) && s_.types().is_function(d.type) && (declarators > 0 || ps_.is(Tk::comma))) {
        s_.diag(d.name_at, Diag::auto_not_allowed);
        s_.settle_type_access(EntityId::none);
        bad();
        ps_.skip_to_semi();
        return;
      }
      ++declarators;
      if (d.ok && d.function && !return_alias_checks.empty())
        d.type = ps_.function_with_return_alias_checks(d.type, return_alias_checks);
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
    for (DeclId d : p_->qualified_decls(p_->world().ins.names.spelling(n), p_->namespace_path(*sc->ns), k_, false,
                                       Program::LookupNames::using_declaration))
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
    if (auto o = s_.own_function_defaults.find(f); o != s_.own_function_defaults.end())
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
    if (sema::is_allocation_name(n)) return true;  // checked by allocation_declaration_ok
    sema::Types& types = s_.types();
    if (op == "=" || op == "()" || op == "[]" || op == "->") {
      s_.diag(d.name_at, Diag::operator_member_only, d.name);
      return false;
    }
    std::size_t count = d.params.size();
    bool unary_only = op == "~" || op == "!", binary_only = !(unary_only || op == "+" || op == "-" || op == "*" || op == "&" ||
                                                              op == "++" || op == "--");
    if (count == 0 || count > 2 || (unary_only && count != 1) || (binary_only && count != 2) || types.rec(d.type).variadic) {
      s_.diag(d.name_at, Diag::operator_arity, d.name);
      return false;
    }
    // [over.inc]/1: the second parameter of a postfix form has type `int`.
    if ((op == "++" || op == "--") && count == 2 && !types.is_dependent(d.params[1].type) &&
        types.canonical(sema::Types::unqualified(d.params[1].type)) != types.builtin(sema::Builtin::int_)) {
      s_.diag(d.params[1].at, Diag::operator_postfix_parameter, d.name);
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

  u32 after_definition_attributes() {
    const u32 back = it_->begin + ps_.pos();
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
    const u32 after = it_->begin + ps_.pos();
    ps_.seek(back);
    return after;
  }

  bool supported_definition_attributes(u32 after) {
    if (!layout_attribute(p_->region(), it_->brace_end, after, LayoutAttributes::trailing_definition)) return true;
    s_.diag(it_->brace_end - it_->begin, Diag::post_definition_layout_attribute);
    return false;
  }

  // `typedef class-specifier X …;` with an unnamed class or enumeration: X names it for linkage
  // purposes ([dcl.typedef]/9), and so is its name here; the declarator after the braces.
  std::optional<std::pair<NameId, u32>> typedef_name_after_braces() {
    u32 back = it_->begin + ps_.pos();  // `seek` takes a region index, `pos` is in the item
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
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
  bool same_scope(const ast::DeclSema& d) {
    const sema::Entities& entities = s_.world().entities;
    EntityId scope = entities.rec(d.entity).scope;
    // The entity lives in the enumeration, but an unscoped enumerator also binds its name in
    // the enumeration's enclosing scope ([dcl.enum]/12).
    if (d.kind == ast::DeclKind::enumerator) scope = entities.rec(scope).scope;
    return scope == ns_entity();
  }
  bool same_function_parameters(QualType a, QualType b, bool compare_ref = true, bool compare_cv = true) {
    const auto& types = s_.types();
    const auto& interner = s_.world().ins.types;
    const auto first = interner.get<ast::TFunction>(types.canonical(a));
    const auto second = interner.get<ast::TFunction>(types.canonical(b));
    return first.params == second.params && first.variadic == second.variadic && (!compare_cv || first.cv == second.cv) &&
           (!compare_ref || first.ref == second.ref);
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
    // A partial specialization's associated constraints and their fingerprint, for its identity,
    // its matching and its ordering (Fable's decision on #234).
    partial_constraints_ = partial_constraints(partial_constraints_fp_);
    // C++20: arguments that are the primary's own are allowed when the partial specialization is
    // more constrained ([temp.spec.partial]/9.3, [temp.func.order]/6.2).
    if (own && !t.more_specialized_partial(head, *full, params, t.own_args(params), partial_constraints_, tmpl.constraints)) {
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
    if (!t.more_specialized_partial(head, *full, params, primary, partial_constraints_, tmpl.constraints)) {
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
    return s_.class_specialization(tmpl->entity, *full, partial_constraints_ == ConstId::none ? sema::Fp128{} : partial_constraints_fp_);
  }
  // `template<…> class-key X<args> { … }`: a partial specialization of class template X
  // ([temp.spec.partial]). Its members are analysed as those of `X<args>`, the specialization of
  // its own parameters it is the pattern of, as a class template's are as its current
  // instantiation's.
  void partial_class_definition(Tk key, NameId name, u32 name_at) {
    auto spec = partial_class(name, name_at);
    if (!spec) return bad();
    EntityId pattern = *spec;
    // Its base-clause, as a class template's: a dependent base is substituted with the deduced
    // arguments when a specialization it matches is instantiated (`instantiate_base`).
    std::vector<sema::BaseRec> bases;
    std::optional<Program::ClassBasePattern> base_pattern;
    if (ps_.is(Tk::colon) && !specialization_base_clause(key, name, pattern, bases, base_pattern)) return bad();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // `final`
      return bad();
    }
    if (auto prior = p_->declaration(pattern, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(name_at, Diag::class_redefinition, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(pattern, true);
    sem.flags |= sema::decl_flag::template_ | sema::decl_flag::partial;
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    sem.constraints = partial_constraints_;
    if (!class_declared(sem, key, name_at)) return bad();
    if (!bases.empty()) p_->set_bases(pattern, std::move(bases));
    if (base_pattern) p_->class_base_patterns_.emplace(k_, *base_pattern);
    p_->current_instantiation(pattern, k_, sem);
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
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
    sem.entity = s_.variable_specialization(tmpl->entity, *full,
                                            partial_constraints_ == ConstId::none ? sema::Fp128{} : partial_constraints_fp_);
    sem.flags = u16(flags | sema::decl_flag::template_ | sema::decl_flag::partial);
    sem.params = b_.seq(std::span<const ast::ParamSema>{});
    sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    sem.constraints = partial_constraints_;
    if (auto prior = p_->declaration(sem.entity, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    if (d.replay_template_args_at)
      p_->partial_replays_.emplace(k_, Program::PartialReplay{*d.replay_template_args_at, *tmpl, s_.dependent_calls});
    return true;
  }

  // The base-clause of an explicit or partial specialization `self` of class template `name`
  // ([class.derived.general]): a union has none ([class.union.general]/1).
  bool specialization_base_clause(Tk key, NameId name, EntityId self, std::vector<sema::BaseRec>& bases,
                                  std::optional<Program::ClassBasePattern>& pattern) {
    if (key == Tk::kw_union) {
      s_.diag(ps_.pos(), Diag::union_with_base);
      return false;
    }
    return base_clause(key, name, bases, pattern, self);
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
  void explicit_class_definition(Tk key, NameId name, u32 name_at) {
    auto spec = explicit_class(name, name_at);
    if (!spec) return bad();
    std::vector<sema::BaseRec> bases;
    std::optional<Program::ClassBasePattern> no_pattern;  // the class is not a template
    if (ps_.is(Tk::colon) && !specialization_base_clause(key, name, *spec, bases, no_pattern)) return bad();
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // `final`
      return bad();
    }
    if (auto prior = p_->declaration(*spec, k_); prior && (prior->flags & sema::decl_flag::definition)) {
      s_.diag(name_at, Diag::class_redefinition, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(*spec, true);
    sem.flags |= sema::decl_flag::specialization;
    if (!class_declared(sem, key, name_at)) return bad();
    if (!bases.empty()) p_->set_bases(*spec, std::move(bases));
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
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
    sem.constraints = associated_constraints();
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    p_->declared(sem, k_);
    return true;
  }
  // A lambda in a constraint's definition belongs to no declaration; its closure type there is a
  // pattern, and each satisfaction has its own (`AtomAnalysis`).
  sema::Sema::ClosureOwner constraint_owner() {
    const EntityId around = s_.current_class != EntityId::none ? s_.current_class : ns_entity();
    return {around, ast::Linkage::internal, ast::mix(ast::seed("ycxx.parse.constraint-closure"), s_.world().ins.items.fp(p_->id(k_)))};
  }
  // A template-head's requires-clause ([temp.pre]/9), after it: its normal form, and its tokens'
  // fingerprint for the declaration's identity.
  void head_requires_clause() {
    if (!ps_.is(Tk::kw_requires)) return;
    ps_.bump();
    u32 begin = ps_.pos();
    const sema::Sema::ClosureOwnerScope owner(s_, constraint_owner());
    head_requires_ = ps_.constraint_expression(true, &head_constraint_.expression);
    head_constraint_.tokens = clause_fp(begin, ps_.pos());
    constraints_fp_ = ast::mix(constraints_fp_, head_constraint_.tokens);
  }
  // A declarator's trailing requires-clause ([dcl.decl]/4).
  void trailing_requires_clause() {
    if (!ps_.is(Tk::kw_requires)) return;
    ps_.bump();
    u32 begin = ps_.pos();
    const sema::Sema::ClosureOwnerScope owner(s_, constraint_owner());
    trailing_requires_ = ps_.constraint_expression(true, &trailing_constraint_.expression);
    trailing_constraint_.tokens = clause_fp(begin, ps_.pos());
    constraints_fp_ = ast::mix(ast::mix(constraints_fp_, u64(1)), trailing_constraint_.tokens);
  }
  // A member-declarator's trailing requires-clause ([class.mem.general]), with the function's
  // parameters in scope ([basic.scope.param]). Only a templated function has one
  // ([dcl.decl.general]/4). A non-template member of a class template specialization reads it as
  // its pattern does, then substitutes the specialization's arguments into its atoms' mappings
  // ([temp.constr.normal]): nothing in it is checked before the member is used ([temp.inst]/17),
  // and a mapping that cannot be formed leaves it unsatisfied ([temp.constr.atomic]/3).
  // Each member-declarator has its own ([class.mem.general]). A non-templated function's is
  // diagnosed, then read and dropped, so the member is still declared.
  bool member_requires_clause(const std::vector<parse::Param>& params) {
    if (!member_template_) {
      trailing_requires_ = ConstId::none;
      trailing_constraint_ = {};
    }
    if (!ps_.is(Tk::kw_requires)) return true;
    const bool nontemplated = !member_template_ && !s_.templated();
    if (nontemplated) s_.diag(ps_.pos(), Diag::nontemplated_constraints);
    const std::size_t errors = s_.diag_count();
    const bool substitute = inst_ && !member_template_;
    sema::Bindings bound;
    if (substitute) bound = std::exchange(s_.template_args, sema::Bindings{});
    s_.push_scope();
    for (const parse::Param& prm : params)
      if (prm.name != s_.no_name()) s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, prm.at, false);
    trailing_requires_clause();
    s_.pop_scope();
    if (substitute) {
      s_.template_args = bound;
      const auto normal = s_.templates().substitute_constraint(trailing_requires_, bound);
      // An empty disjunction: never satisfied.
      trailing_requires_ = normal ? *normal
                                  : s_.ins().intern_value(ast::VConstraint{1, s_.ins().intern_seq(std::span<const ConstId>{})});
    }
    if (nontemplated) {
      trailing_requires_ = ConstId::none;
      trailing_constraint_ = {};
    }
    return s_.diag_count() == errors;
  }
  // The identity a non-template member's trailing requires-clause gives it ([basic.scope.scope]/4):
  // its own tokens, never a template-head's; none without one.
  sema::Fp128 member_constraints_fp() const {
    return trailing_requires_ == ConstId::none ? sema::Fp128{}
                                               : ast::mix(ast::mix(sema::Fp128{}, u64(1)), trailing_constraint_.tokens);
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
  // A partial specialization's associated constraints ([temp.constr.decl]/3: its head's
  // type-constraints, then its requires-clause) and, into `fp`, their tokens' fingerprint, as
  // `associated_constraints` gives them, without mixing into `constraints_fp_`.
  ConstId partial_constraints(sema::Fp128& fp) {
    sema::Templates t = s_.templates();
    ConstId c = ConstId::none;
    fp = constraints_fp_;
    for (ConstId tc : s_.head_constraints) {
      c = t.constraint_op(0, c, tc);
      fp = ast::mix(fp, s_.world().ins.values.fp(tc));
    }
    return t.constraint_op(0, c, head_requires_);
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
    ConstId nf;
    {
      const sema::Sema::ClosureOwnerScope owner(s_, constraint_owner());
      nf = ps_.constraint_expression(false);
    }
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
    sem.constraints = associated_constraints();
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

  bool record_class_key(EntityId c, Tk key, u32 at) {
    bool is_union = key == Tk::kw_union;
    auto previous = sema::Templates(s_.world(), p_->classes_).is_union_class(c);
    if (previous && *previous != is_union) {
      s_.diag(at, Diag::conflicting_declaration, s_.world().entities.rec(c).name);
      return false;
    }
    p_->classes_.record_class_key(c, is_union);
    return true;
  }

  bool class_declared(const ast::DeclSema& sem, Tk key, u32 at) {
    if (!record_class_key(sem.entity, key, at)) return false;
    if (sem.flags & sema::decl_flag::definition) p_->class_definition_keys_.emplace(k_, key);
    decls_.push_back(sem);
    p_->declared(sem, k_, inst_.has_value());
    return true;
  }

  bool class_head_alignment_ahead() const {
    return Parser::is_class_key(ps_.kind()) && ps_.kind(ps_.past_attributes(1)) == Tk::kw_alignas;
  }

  void unsupported_class_alignment() {
    s_.diag(ps_.pos() + ps_.past_attributes(1), Diag::not_supported, Tk::kw_alignas);
    bad();
  }

  // [dcl.align]: evaluate every specifier, even when another is stricter.
  std::optional<Program::ClassAlignment> read_alignment(std::vector<ast::AnnotationSema>& annotations, bool field = false) {
    sema::Sema::ImmediateReferenceScope references(s_, /*allowed=*/true);
    sema::Sema::ClassAlignmentOperand operand_context(s_);
    Program::ClassAlignment alignment;
    alignment.at = ps_.pos();
    while (ps_.is(Tk::kw_alignas)) {
      const u32 at = ps_.pos(), begin = ps_.index();
      ps_.bump();
      if (!ps_.expect(Tk::l_paren)) return std::nullopt;
      const u32 operand = ps_.index();
      const auto diags = s_.diag_count();
      std::optional<QualType> type;
      if (ps_.starts_type_id()) type = ps_.type_id();
      Ex e;
      if (type && (ps_.is(Tk::r_paren) || ps_.is(Tk::ellipsis))) {
        if (ps_.is(Tk::ellipsis) || (!field && s_.types().is_dependent(*type))) {
          s_.diag(at, Diag::not_supported, Tk::kw_alignas);
          return std::nullopt;
        }
        e = s_.size_of(*type, true, at, ps_.pos());
      } else {
        s_.truncate_diags(diags);
        ps_.seek(operand);
        e = s_.promote(ps_.constant_expression());
      }
      if (!e.ok()) return std::nullopt;
      if (layout_attribute(p_->region(), operand, ps_.index())) {
        s_.diag(at, Diag::class_alignment_uncertain_layout);
        return std::nullopt;
      }
      if (ps_.is(Tk::ellipsis)) {
        s_.diag(at, Diag::not_supported, Tk::kw_alignas);
        return std::nullopt;
      }
      if (!ps_.expect(Tk::r_paren)) return std::nullopt;
      alignment.specifiers.push_back({begin, ps_.index()});
      ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
      if (field && s_.dependent(e)) {
        alignment.dependent = true;
        continue;
      }
      const auto value = s_.types().is_integral(e.type()) ? sema::Evaluator(s_, q_).evaluate(e).value : std::nullopt;
      if (!value || !s_.ins().values.is<ast::VInt>(*value)) {
        s_.diag(at, field ? Diag::field_alignment_not_constant : Diag::class_alignment_not_constant);
        return std::nullopt;
      }
      const auto bits = s_.ins().values.get<ast::VInt>(*value).bits;
      if (bits.hi != 0 || (bits.lo != 0 && !std::has_single_bit(bits.lo))) {
        s_.diag(at, field ? Diag::field_alignment_invalid : Diag::class_alignment_invalid);
        return std::nullopt;
      }
      // The native frame layouts on both targets support object alignment up to 16 bytes.
      if (bits.lo > 16) {
        s_.diag(at, field ? Diag::field_alignment_unsupported : Diag::class_alignment_unsupported);
        return std::nullopt;
      }
      if (bits.lo > alignment.value) {
        alignment.value = u8(bits.lo);
        alignment.at = at;
      }
    }
    return alignment;
  }

  // [dcl.type.elab]: the attribute sequence is allowed on a standalone class declaration.
  void aligned_class_declaration() {
    const Tk key = ps_.kind();
    if (key == Tk::kw_union || single_linkage_spec_) return unsupported_class_alignment();
    ps_.bump();
    std::vector<ast::AnnotationSema> annotations;
    ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
    auto alignment = read_alignment(annotations);
    if (!alignment) return bad();
    if (!ps_.is(Tk::identifier) || (ps_.kind(1) != Tk::semi && ps_.kind(1) != Tk::eof)) {
      s_.diag(alignment->at, Diag::attributes_not_allowed);
      return bad();
    }
    const NameId name = ps_.name_of(ps_.tok());
    const u32 at = ps_.pos();
    ps_.bump();
    if (!ps_.expect(Tk::semi)) return bad();
    auto sem = class_decl(class_entity(name), false);
    sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(annotations));
    for (DeclId previous : q_.visible_decls(name))
      if (auto d = p_->decl_sema(previous); d && d->entity != EntityId::none && same_scope(*d) &&
          (d->kind == ast::DeclKind::enum_ ||
           (d->kind == ast::DeclKind::type_alias && s_.types().canonical(d->type) != s_.types().canonical(sem.type)) ||
           (d->kind == ast::DeclKind::class_ && (d->flags & sema::decl_flag::template_)))) {
        s_.diag(at, Diag::conflicting_declaration, name);
        return bad();
      }
    if (clashes_with_using(name, at, ast::DeclKind::class_, sem.type)) return bad();
    if (layout_attribute(p_->region(), it_->begin, it_->end, LayoutAttributes::affecting_layout, alignment->specifiers)) {
      s_.diag(alignment->at, Diag::not_supported, Tk::kw_alignas);
      return bad();
    }
    const auto cls = std::to_underlying(sem.entity);
    if (const auto definition = p_->class_items_.find(cls); definition != p_->class_items_.end() && definition->second < k_) {
      const auto written = p_->class_alignments_.find(definition->second);
      if (written == p_->class_alignments_.end()) {
        s_.diag(alignment->at, Diag::class_alignment_missing);
        return bad();
      }
      p_->class_def(definition->second);
      const auto& prior = written->second;
      if (!prior.natural && (prior.value || alignment->value)) {
        s_.diag(alignment->at, Diag::class_alignment_uncertain_layout);
        return bad();
      }
      if (alignment->value && alignment->value < prior.natural) {
        s_.diag(alignment->at, Diag::class_alignment_weak);
        return bad();
      }
      if (prior.natural && alignment->effective(prior.natural) != prior.effective(prior.natural)) {
        s_.diag(alignment->at, Diag::class_alignment_mismatch);
        return bad();
      }
    }
    if (!class_declared(sem, key, at)) return bad();
    p_->class_alignments_.emplace(k_, std::move(*alignment));
  }

  // A class definition at namespace scope ([class.pre]): `class-key identifier { … }`, then
  // its declarators. The member-specification is the members' items (§4.2); `class_def` reads
  // them. In an ordinary class it defines a nested class ([class.nest]), whose declarators are
  // members of the enclosing class.
  void class_definition() {
    if (!supported_definition_attributes(after_definition_attributes())) return bad();
    const bool nested = p_->in_class(k_);
    const EntityId owner = inst_ ? inst_->spec : nested ? p_->class_entity(it_->parent) : EntityId::none;
    if (nested && owner == EntityId::none) return bad();
    // Names in the class-head are looked up in the enclosing class first ([basic.lookup.unqual]).
    if (nested) s_.current_class = owner;
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
    std::vector<ast::AnnotationSema> leading_annotations;
    ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &leading_annotations);
    bool td = ps_.accept(Tk::kw_typedef);  // `typedef class-specifier declarators;` ([dcl.typedef])
    Tk key = ps_.kind();
    if (!Parser::is_class_key(key)) {
      s_.diag(ps_.pos(), Diag::not_supported, key);  // other specifiers before the class-key
      return bad();
    }
    ps_.bump();
    std::vector<ast::AnnotationSema> annotations;
    ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
    if (!annotations.empty() && (template_ || explicit_))
      s_.diag(annotations.front().offset, Diag::annotation_not_supported);
    std::optional<Program::ClassAlignment> alignment;
    if (ps_.is(Tk::kw_alignas)) {
      if (key == Tk::kw_union || template_ || explicit_) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_alignas);
        return bad();
      }
      alignment = read_alignment(annotations);
      if (!alignment) return bad();
      if (!ps_.is(Tk::identifier) || ps_.kind(1) == Tk::coloncolon || ps_.kind(1) == Tk::less) {
        s_.diag(alignment->at, Diag::not_supported, Tk::kw_alignas);
        return bad();
      }
    }
    // BOOTSTRAP: -Wmaybe-uninitialized: a pair and a flag, not std::optional. At -O3 GCC 16.2
    // reports the optional's payload as possibly uninitialized at `named->first` (#748).
    // Without the bug: `auto named = td ? typedef_name_after_braces() : std::nullopt;`,
    // `!named`, `named->first`, `named->second`.
    std::pair<NameId, u32> named{};
    bool has_named = false;
    if (td)
      if (auto n = typedef_name_after_braces()) named = *n, has_named = true;
    if (!ps_.is(Tk::identifier) && !has_named) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // unnamed classes, attributes
      return bad();
    }
    const u32 head_index = ps_.index();
    NameId name = ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : named.first;
    u32 name_at = ps_.is(Tk::identifier) ? ps_.pos() : named.second;
    if (ps_.is(Tk::identifier)) ps_.bump();
    // `class O::I { … }`: the definition of a class declared in class O ([class.pre]/3).
    EntityId qualified_owner = EntityId::none;
    if (ps_.is(Tk::coloncolon) && !nested && !template_ && !explicit_ && !td && !alignment) {
      ps_.seek(head_index);
      qualified_owner = member_class_qualifier(name, name_at);
      if (qualified_owner == EntityId::none) return bad();
      s_.current_class = qualified_owner;  // names after the class-head-name ([basic.lookup.unqual])
    }
    const bool member_class = nested || qualified_owner != EntityId::none;
    const auto entity_of = [&] {
      return member_class ? nested_class_entity(name, nested ? owner : qualified_owner) : class_entity(name);
    };
    if (explicit_) return explicit_class_definition(key, name, name_at);
    if (template_ && ps_.is(Tk::less)) return partial_class_definition(key, name, name_at);
    const auto final_here = [&] { return ps_.is(Tk::identifier) && p_->region().name(ps_.tok()) == "final"; };
    const bool is_final = final_here();
    if (is_final) {
      if (key == Tk::kw_union || template_) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::identifier);
        return bad();
      }
      ps_.bump();
      if (final_here()) {
        s_.diag(ps_.pos(), Diag::duplicate_final);
        return bad();
      }
    }
    std::vector<sema::BaseRec> bases;
    std::optional<Program::ClassBasePattern> base_pattern;
    if (ps_.is(Tk::colon)) {
      if (key == Tk::kw_union) {
        s_.diag(ps_.pos(), Diag::union_with_base);  // [class.union.general]/1
        return bad();
      }
      if (inst_) {
        // Instantiating the owner declares the member class. Its bases are part of its definition.
        ps_.seek(it_->brace_begin);
      } else if (!base_clause(key, name, bases, base_pattern, member_class ? entity_of() : EntityId::none,
                              nested && !s_.template_scopes.empty())) {
        return bad();
      }
    }
    if (!ps_.is(Tk::l_brace)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      return bad();
    }
    if (member_class) {
      if (base_pattern) p_->class_base_patterns_.emplace(k_, *base_pattern);
      return member_class_definition(key, name, name_at, entity_of(), owner, is_final, bases, annotations,
                                     leading_annotations, at, td);
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
    sem.is_final = is_final;
    if (!template_) sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(annotations));
    if (!class_declared(sem, key, name_at)) return bad();
    if (alignment) p_->class_alignments_.emplace(k_, std::move(*alignment));
    s_.own.push_back({name, sem});
    if (!bases.empty()) p_->set_bases(s_.types().class_of(sem.type), std::move(bases));
    if (base_pattern) p_->class_base_patterns_.emplace(k_, *base_pattern);
    if (template_) p_->current_instantiation(s_.types().class_of(sem.type), k_, sem);
    // The declarators after the closing brace: `struct S { … } s, *p;`, where S is complete.
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
    q_.own_class_complete = true;
    if (ps_.accept(Tk::semi)) {
      if (!leading_annotations.empty()) s_.diag(leading_annotations.front().offset, Diag::attributes_not_allowed);
      return;
    }
    if (ps_.at_end()) {  // the `;` is missing; the definition stands ([class.pre]/1)
      ps_.expect(Tk::semi);
      return;
    }
    // Namespace names introduced by members precede the declarators after the closing brace.
    u32 after_members = k_ + 1;
    const auto& items = p_->items().items;
    while (after_members < items.size() && items[after_members].parent != items::none &&
           items[after_members].parent >= k_)
      ++after_members;
    q_.declaration_lookup_end = after_members;
    DeclSpec spec;
    spec.begin = at;
    spec.type = sem.type;
    spec.has_type = true;
    spec.is_typedef = td;
    spec.annotations = std::move(leading_annotations);
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

  // The entity of the class named `n` that is a member of class `owner` ([class.nest]).
  EntityId nested_class_entity(NameId n, EntityId owner) {
    return s_.world().entities.entity({.kind = sema::EntityKind::class_, .name = n, .scope = owner});
  }

  // [class.access.general]: a member class keeps the access of its first declaration.
  bool set_member_class_access(ast::DeclSema& sem, NameId name, u32 name_at) {
    const auto key = p_->class_definition_keys_.find(it_->parent);
    sem.access = key != p_->class_definition_keys_.end() && key->second == Tk::kw_class
                     ? ast::Access::private_ : ast::Access::public_;
    for (u32 member : p_->items().items[it_->parent].members) {
      if (member == k_) break;
      const auto& item = p_->items().items[member];
      if (item.kind != ItemKind::access_spec) continue;
      const Tk label = p_->region().tokens[item.begin].kind;
      sem.access = label == Tk::kw_public ? ast::Access::public_ :
                   label == Tk::kw_protected ? ast::Access::protected_ : ast::Access::private_;
    }
    if (auto previous = q_.earlier_member(name, /*type_only=*/true);
        previous && (previous->entity != sem.entity || previous->access != sem.access)) {
      s_.diag(name_at, Diag::conflicting_declaration, name);
      return false;
    }
    return true;
  }

  // `class-key attribute-specifier-seq(opt) identifier ;` in class `owner`: it declares a member
  // class, or redeclares one an earlier member declared ([class.nest], [dcl.type.elab]/3).
  void member_class_declaration(EntityId owner) {
    const Tk key = ps_.kind();
    ps_.bump();
    std::vector<ast::AnnotationSema> annotations;
    ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
    const NameId name = ps_.name_of(ps_.tok());
    const u32 name_at = ps_.pos();
    ps_.bump();
    const EntityId entity = nested_class_entity(name, owner);
    if (s_.declared_template_param(name)) {
      s_.diag(name_at, Diag::redeclared_local, name);
      return bad();
    }
    if (name == s_.world().entities.rec(owner).name) {  // [class.mem.general]: not the class's name
      s_.diag(name_at, Diag::member_named_as_class, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(entity, false);
    if (!set_member_class_access(sem, name, name_at)) return bad();
    sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(annotations));
    if (!class_declared(sem, key, name_at)) return bad();
    s_.own.push_back({name, sem});
    if (!ps_.expect(Tk::semi)) bad();
  }

  // The class a qualified class-head-name `O::I` names the member of ([class.pre]/3), with `name`
  // and `name_at` set to I's; none, diagnosed, unless O is a complete ordinary class that has
  // declared a member class I and this namespace encloses O.
  EntityId member_class_qualifier(NameId& name, u32& name_at) {
    const u32 at = ps_.pos();
    u32 next = 0;
    const auto diags = s_.diag_count();
    bool alias_failed = false;
    auto scope = ps_.committed_nested_name(0, next, &alias_failed);
    if (!scope) {
      if (!alias_failed && !s_.has_error_since(diags)) s_.diag(at, Diag::expected_unqualified_id);
      return EntityId::none;
    }
    if (ps_.kind(next) != Tk::identifier || ps_.kind(next + 1) == Tk::coloncolon || !scope->type ||
        !s_.types().is_class(*scope->type) || s_.types().is_dependent(*scope->type)) {
      s_.diag(at, Diag::not_supported, Tk::coloncolon);  // namespace-qualified and templated class heads
      return EntityId::none;
    }
    for (u32 j = 0; j < next; ++j) ps_.bump();
    name = ps_.name_of(ps_.tok());
    name_at = ps_.pos();
    ps_.bump();
    const EntityId owner = s_.types().class_of(*scope->type);
    const sema::EntityRec& o = s_.world().entities.rec(owner);
    if (o.kind != sema::EntityKind::class_ || o.templ != EntityId::none ||
        !p_->is_namespace(p_->outermost_class_scope(owner))) {
      s_.diag(at, Diag::not_supported, Tk::coloncolon);  // out-of-class definitions for #1187
      return EntityId::none;
    }
    const sema::ClassRec* record = q_.is_complete(owner) ? p_->classes().find(owner) : nullptr;
    const EntityId member = nested_class_entity(name, owner);
    if (!record || !std::ranges::contains(record->types, member, &sema::NestedType::entity)) {
      s_.diag(name_at, Diag::no_member, name, *scope->type);
      return EntityId::none;
    }
    // The definition is in a namespace enclosing the class ([class.pre]/3).
    EntityId ns = owner;
    while (ns != EntityId::none && s_.world().entities.rec(ns).kind != sema::EntityKind::namespace_)
      ns = s_.world().entities.rec(ns).scope;
    if (!Program::encloses(p_->ns_of(k_), ns == EntityId::none ? std::string() : p_->namespace_path(ns))) {
      s_.diag(name_at, Diag::member_class_namespace, name);
      return EntityId::none;
    }
    return owner;
  }

  // A member class's definition: in its class (`owner`, [class.nest]), or, with `owner` none,
  // after a qualified class-head-name at namespace scope ([class.pre]/3). The declarators after
  // its braces are members of `owner`, or namespace-scope declarations.
  void member_class_definition(Tk key, NameId name, u32 name_at, EntityId entity, EntityId owner, bool is_final,
                               std::vector<sema::BaseRec>& bases, std::vector<ast::AnnotationSema>& annotations,
                               std::vector<ast::AnnotationSema>& leading_annotations, u32 at, bool td) {
    if (s_.declared_template_param(name)) {
      s_.diag(name_at, Diag::redeclared_local, name);
      return bad();
    }
    if (owner != EntityId::none && name == s_.world().entities.rec(owner).name) {  // [class.mem.general]
      s_.diag(name_at, Diag::member_named_as_class, name);
      return bad();
    }
    ast::DeclSema sem = class_decl(entity, true);
    if (owner != EntityId::none && !set_member_class_access(sem, name, name_at)) return bad();
    // An earlier definition: in the class, whose members were analysed above, or out of class in
    // an earlier top-level item, which is analysed only when asked (#947).
    p_->ensure_declared(entity, k_);
    const auto& definitions = inst_ ? p_->class_patterns_ : p_->class_items_;
    if (const auto def = definitions.find(std::to_underlying(entity)); def != definitions.end() && def->second != k_) {
      s_.diag(name_at, Diag::class_redefinition, name);
      return bad();
    }
    sem.is_final = is_final;
    sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(annotations));
    if (!class_declared(sem, key, name_at)) return bad();
    if (owner != EntityId::none) s_.own.push_back({name, sem});
    if (!bases.empty()) p_->set_bases(entity, std::move(bases));
    // The declarators after the closing brace, where the class is complete.
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
    q_.own_class_complete = true;
    if (ps_.accept(Tk::semi)) {
      if (!leading_annotations.empty()) s_.diag(leading_annotations.front().offset, Diag::attributes_not_allowed);
      return;
    }
    if (ps_.at_end()) {  // the `;` is missing; the definition stands ([class.pre]/1)
      ps_.expect(Tk::semi);
      return;
    }
    DeclSpec spec;
    spec.begin = at;
    spec.type = sem.type;
    spec.has_type = true;
    spec.annotations = std::move(leading_annotations);
    if (owner != EntityId::none) {
      s_.current_class = owner;
      if (!td) return member_declarators(spec, class_decl(owner, true));
      // `typedef class-specifier declarators;` in a class: member typedef-names ([dcl.typedef]).
      do {
        parse::Declarator d = ps_.declarator(spec.type, false, true);
        if (!d.ok || !d.named || d.qualified || !member_alias(class_decl(owner, true), d.name, d.name_at, d.type))
          return bad();
      } while (ps_.accept(Tk::comma));
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
    s_.current_class = EntityId::none;
    spec.is_typedef = td;
    do {
      parse::Declarator d = ps_.declarator(spec.type, false, td);
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
    if (!supported_definition_attributes(after_definition_attributes())) return bad();
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
    if (!class_declared(head, key, at)) return bad();
    p_->class_def(k_);

    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
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
  // private for a `class`, public for a `struct` ([class.access.base]/2). `self` is the class
  // being defined when it is a specialization, whose other specializations may be bases.
  bool base_clause(Tk key, NameId name, std::vector<sema::BaseRec>& bases,
                   std::optional<Program::ClassBasePattern>& pattern, EntityId self = EntityId::none,
                   bool replay = false) {
    const bool template_pattern = template_ || replay;
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
      if (template_pattern && !ps_.is(Tk::identifier) && !ps_.is(Tk::coloncolon) && !ps_.is(Tk::kw_decltype)) {
        s_.diag(at, Diag::not_supported, ps_.kind());  // pack-index-specifiers and other base forms (#308)
        return false;
      }
      u32 deferred = ps_.deferred_argument_checks();
      DeclSpec spec;
      {
        AtItem::RestoreTypeTraitPattern restore{q_};
        q_.retain_type_trait_pattern = template_pattern;
        ps_.class_template_base = template_pattern;
        spec = ps_.decl_specifiers();
        ps_.class_template_base = false;
      }
      if (!spec.ok) return false;
      if (template_pattern) {
        if (!spec.has_type || ps_.is(Tk::comma) || ps_.is(Tk::ellipsis) || s_.names_pack(spec.type)) {
          s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // multiple bases and base packs (#308)
          return false;
        }
        Tk last = p_->region().tokens[it_->begin + ps_.pos() - 1].kind;
        if (last == Tk::kw_const || last == Tk::kw_volatile || spec.elaborated || spec.is_static || spec.is_extern ||
            spec.is_inline || spec.is_constexpr || spec.is_consteval || spec.is_constinit || spec.is_typedef) {
          s_.diag(ps_.pos() - 1, Diag::not_supported, last);
          return false;
        }
        spec.type = sema::Types::unqualified(types.canonical(spec.type));
        pattern = Program::ClassBasePattern{{spec.type, access, 0, 0}, at, ps_.pos(), deferred != ps_.deferred_argument_checks()};
        if (types.is_dependent(spec.type)) return true;
      }
      if (!types.is_class(spec.type)) {
        s_.diag(at, Diag::base_not_class, spec.type);
        return false;
      }
      EntityId b = types.class_of(spec.type);
      // A class template's own pattern is dependent and returned above, so a nondependent base
      // named like it is another of its specializations, complete or not on its own.
      const bool itself = self != EntityId::none ? b == self
                          : !template_ && p_->world().entities.rec(b).name == name &&
                                p_->world().entities.rec(b).scope == ns_entity();
      if (itself) {
        s_.diag(at, Diag::incomplete_type, spec.type);  // the class itself: incomplete here
        return false;
      }
      const bool single_base = bases.empty() && !ps_.is(Tk::comma);
      if (!complete_base_class(s_, spec.type, at, single_base)) return false;
      // A written cv-qualifier is no class-or-decltype; its type's cv is ignored ([class.derived.general]/2).
      const auto written_cv = [&](u32 token) {
        const Tk k = p_->region().tokens[it_->begin + token].kind;
        return k == Tk::kw_const || k == Tk::kw_volatile;
      };
      if (written_cv(at) || written_cv(ps_.pos() - 1)) {
        s_.diag(at, Diag::base_not_class, spec.type);
        return false;
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
    s_.current_class = cls;
    u32 at = ps_.pos();
    const bool definition = it_->kind == ItemKind::enum_def;
    const EntityId replay = q_.replaying_scoped_enum;
    const u32 after_attributes = definition ? after_definition_attributes() : it_->end;
    if (definition && !supported_definition_attributes(after_attributes)) return bad();
    const bool td = cls == EntityId::none && ps_.accept(Tk::kw_typedef);  // `typedef enum … { … } E;`
    if (!ps_.is(Tk::kw_enum)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      return bad();
    }
    ps_.bump();
    const bool scoped = ps_.accept(Tk::kw_class) || ps_.accept(Tk::kw_struct);
    ps_.skip_attributes();
    const auto typedef_name = td ? typedef_name_after_braces() : std::nullopt;
    const bool unnamed = !ps_.is(Tk::identifier) && !typedef_name;
    NameId name = s_.no_name();
    u32 name_at = at;
    if (ps_.is(Tk::identifier)) {
      name = ps_.name_of(ps_.tok());
      name_at = ps_.pos();
      ps_.bump();
    } else if (typedef_name) {
      std::tie(name, name_at) = *typedef_name;
    } else {
      if (scoped || !definition || (p_->in_class(k_) && cls == EntityId::none) || td ||
          !(ps_.is(Tk::l_brace) || ps_.is(Tk::colon))) {
        s_.diag(ps_.pos(), Diag::expected_unqualified_id);
        return bad();
      }
      if (after_attributes < it_->end && p_->region().tokens[after_attributes].kind != Tk::semi) {
        s_.diag(after_attributes - it_->begin, Diag::not_supported, p_->region().tokens[after_attributes].kind);
        return bad();
      }
    }
    std::optional<QualType> fixed;
    if (replay != EntityId::none) {
      // The class declaration already supplied the fixed underlying type.
      fixed = p_->classes_.find_enum(replay)->underlying;
      ps_.seek(it_->brace_begin);
    } else if (ps_.accept(Tk::colon)) {
      // [dcl.enum]/2: a fixed underlying type, an integral type.
      const auto saved = s_.nested_enum_members_begin;
      if (cls != EntityId::none) s_.nested_enum_members_begin = s_.own.size();
      DeclSpec u = ps_.decl_specifiers();
      s_.nested_enum_members_begin = saved;
      if (!u.ok) return bad();
      if (!types.is_integral(u.type) || u.is_static || u.is_extern || u.is_inline || u.is_typedef ||
          u.is_constexpr || u.is_consteval || u.is_constinit) {
        s_.diag(u.begin, Diag::enum_base_not_integral);
        return bad();
      }
      // Enumerator values are kept in 64 bits, so a 128-bit underlying type is not yet (#1176).
      if (types.is(u.type, sema::Builtin::int128) || types.is(u.type, sema::Builtin::unsigned_int128)) {
        s_.diag(u.begin, Diag::not_supported, Tk::kw___int128);
        return bad();
      }
      fixed = sema::Types::unqualified(types.canonical(u.type));
    } else if (scoped) {
      fixed = s_.builtin(sema::Builtin::int_);  // [dcl.enum]/5: int unless given
    }
    if (!definition && !fixed) {
      s_.diag(name_at, Diag::enum_base_required, name);
      return bad();
    }
    if (!ps_.expect(definition ? Tk::l_brace : Tk::semi)) return bad();
    const EntityId scope = cls != EntityId::none ? cls : ns_entity();
    EntityId e = EntityId::none;
    auto publish_head = [&] {
      ast::DeclSema sem = class_decl(e, definition);
      sem.kind = ast::DeclKind::enum_;
      sem.type = types.enum_type(e);
      if (replay == EntityId::none) {
        decls_.push_back(sem);
        p_->declared(sem, k_);
      }
      if (!unnamed) s_.own.push_back({name, sem});
    };
    if (!unnamed) {
      if (cls == EntityId::none && !enum_redeclaration_ok(name, name_at, scoped, fixed, definition)) return bad();
      // A member class of the same name ([basic.scope.scope]/4).
      if (cls != EntityId::none && replay == EntityId::none)
        if (auto previous = q_.earlier_member(name, /*type_only=*/true); previous && previous->kind == ast::DeclKind::class_) {
          s_.diag(name_at, Diag::conflicting_declaration, name);
          return bad();
        }
      // [support.dynamic]: the implicit prototypes use this scoped enum with a fixed base.
      // Check its shape on the first written declaration too.
      if (cls == EntityId::none && p_->ns_of(k_) == "std" && s_.ins().names.spelling(name) == "align_val_t") {
        if (!scoped) {
          s_.diag(name_at, Diag::enum_scope_mismatch, name);
          return bad();
        }
        if (!fixed || *fixed != types.builtin(s_.world().target.size_type)) {
          s_.diag(name_at, Diag::enum_base_mismatch, name);
          return bad();
        }
      }
      e = replay == EntityId::none
              ? s_.world().entities.entity({.kind = sema::EntityKind::enum_, .name = name, .scope = scope}) : replay;
      QualType t = types.enum_type(e);
      if (cls == EntityId::none && alias_clash(name, t)) {
        s_.diag(name_at, Diag::conflicting_declaration, name);
        return bad();
      }
      if (cls == EntityId::none && clashes_with_using(name, name_at, ast::DeclKind::enum_, t)) return bad();
      publish_head();
    } else if (ps_.is(Tk::r_brace)) {
      s_.diag(at, Diag::not_supported, Tk::kw_enum);  // an unnamed enumeration with no linkage name
      return bad();
    }
    sema::EnumRec er{scoped, fixed.has_value(), definition, fixed.value_or(s_.builtin(sema::Builtin::int_)), std::nullopt, {}};
    if (!definition) {
      p_->classes_.publish_enum(e, std::move(er));
      return;
    }
    if (scoped && inst_ && replay == EntityId::none) {
      // [temp.inst]/3–4: class instantiation needs the declaration and fixed type. The list is
      // instantiated when referenced in a context that requires the enumeration's definition.
      er.defined = false;
      p_->classes_.publish_enum(e, std::move(er));
      p_->scoped_enum_definitions_.try_emplace(e, Program::ScopedEnumDefinition{k_});
      // Keep declaration ordinals for any fields written after the closing brace. The empty
      // records need draft sequences from this builder, like the other declarations in the item.
      for (const auto& declaration : s_.ins().seqs.elements(p_->item_sema(k_).value.decls))
        if (declaration.kind == ast::DeclKind::enumerator) decls_.push_back(empty_declaration());
      return enum_declarators(cls, types.enum_type(e), at, td);
    }
    // The enumerator list sees the definition's record as the list grows (#156).
    std::optional<Program::DefiningEnum> defining;
    if (unnamed) s_.defining_unnamed_enum_scope = scope;
    else defining.emplace(p_->defining_enum_, e, er);
    const std::size_t first_enumerator = s_.own.size();
    if (cls != EntityId::none) s_.nested_enum_members_begin = first_enumerator;
    auto enumerator_entity = [&](NameId n) {
      return s_.world().entities.entity(
          {.kind = sema::EntityKind::enumerator, .name = n, .linkage = ast::Linkage::none, .scope = e});
    };
    auto no_enum_type = [&](u32 pos) {
      s_.diag(pos, Diag::enum_no_type, unnamed ? s_.world().ins.names.intern("(unnamed enum)") : name);
    };
    // The enumerators: each the previous one's value plus one, or its initializer's, a converted
    // constant expression of the underlying type when it is fixed ([dcl.enum]/5).
    // The template checks the list with dependent values; replay evaluates them ([temp.inst]).
    const bool pattern = cls != EntityId::none && !inst_ && types.is_dependent(types.class_type(cls));
    struct EnumValue {
      std::int64_t bits = 0;
      bool big = false;  // an unsigned value of 2^63 or more
    };
    std::optional<EnumValue> next = EnumValue{};
    // Before the closing brace an enumerator has its initializer's type, or the previous one's,
    // or the fixed underlying type ([dcl.enum]/5); its value is kept until the type is chosen.
    QualType prev_type = fixed.value_or(s_.builtin(sema::Builtin::int_));
    const auto initializer_type = [&](QualType t) {
      if (!types.is_enum(t)) return sema::Types::unqualified(t);
      return s_.deferred_enum_layout(t) ? s_.builtin(sema::Builtin::dependent) : s_.underlying_of(t);
    };
    std::set<NameId> enumerator_names;
    std::vector<std::optional<EnumValue>> values;
    while (!ps_.is(Tk::r_brace) && !ps_.at_end()) {
      if (!ps_.is(Tk::identifier)) {
        s_.diag(ps_.pos(), Diag::expected_unqualified_id);
        break;
      }
      NameId en = ps_.name_of(ps_.tok());
      u32 en_at = ps_.pos();
      ps_.bump();
      ps_.skip_attributes();  // an enumerator's attribute-specifier-seq ([dcl.enum]/1)
      auto value = next;
      if (ps_.accept(Tk::equal)) {
        Ex x = ps_.constant_expression();
        Ex v = s_.promote(x);
        if (pattern && s_.dependent(v) &&
            (types.is_dependent(v.type()) || types.is_integral(v.type()) || s_.is_unscoped_enum(v.type()))) {
          value.reset();
          if (!fixed) prev_type = initializer_type(x.type());
        } else {
          auto c = v.ok() ? sema::Evaluator(s_, q_).evaluate(v).value : std::nullopt;
          if (!c || s_.world().values.rec(*c).kind != sema::ValueKind::integer || !types.is_integral(v.type())) {
            if (x.ok()) s_.diag(x.begin, Diag::bound_not_constant);
            // A nested enum instantiation can diagnose the error outside this item's result.
            if (replay != EntityId::none) return;
            if (unnamed && inst_) return bad();
          } else if (sema::shape(s_.world().target, *types.builtin_of(v.type())).bits > 64) {
            // Enumerator values are kept in 64 bits and typed by their initializers until the
            // closing brace ([dcl.enum]/5), so a 128-bit initializer is not yet (#1176).
            s_.diag(x.begin, Diag::not_supported, Tk::kw___int128);
            return bad();
          } else {
            sema::IntShape sh = sema::shape(s_.world().target, *types.builtin_of(v.type()));
            sema::u64 bits = s_.world().values.rec(*c).bits;
            const bool extend = sh.is_signed && sh.bits < 64 && ((bits >> (sh.bits - 1)) & 1);
            value = EnumValue{std::int64_t(extend ? bits | ~((sema::u64{1} << sh.bits) - 1) : bits),
                              !sh.is_signed && (bits >> 63) != 0};
            if (fixed) {
              Ex lit = s_.literal(v.type(), *c, ast::ValueCategory::prvalue, x.begin, x.end);
              if (s_.narrows(lit, *fixed)) s_.diag(x.begin, Diag::narrowing, v.type(), *fixed);
            } else {
              prev_type = initializer_type(x.type());
            }
          }
        }
      } else if (!value) {
        if (!fixed) prev_type = s_.builtin(sema::Builtin::dependent);
      } else if (!values.empty()) {
        if (const auto& previous = values.back(); previous && previous->big && previous->bits == -1) {
          no_enum_type(en_at);  // [dcl.enum]/5: no type holds the incremented value
          value->big = false;
        }
        // An incremented value outside the previous type widens to an integral type that holds it.
        if (!fixed && !holds(prev_type, value->bits, value->big)) {
          for (sema::Builtin b : {sema::Builtin::int_, sema::Builtin::unsigned_int, sema::Builtin::long_, sema::Builtin::unsigned_long})
            if (holds(s_.builtin(b), value->bits, value->big)) {
              prev_type = s_.builtin(b);
              break;
            }
        }
        if (fixed) {
          QualType ll = s_.builtin(value->big ? sema::Builtin::unsigned_long_long : sema::Builtin::long_long);
          Ex lit = s_.literal(ll, s_.world().values.integer(ll, sema::u64(value->bits)), ast::ValueCategory::prvalue, en_at, en_at);
          if (s_.narrows(lit, *fixed)) s_.diag(en_at, Diag::narrowing, ll, *fixed);
        }
      }
      // The next implicit value, past INT64_MAX into the unsigned values.
      next.reset();
      if (value)
        next = EnumValue{std::int64_t(sema::u64(value->bits) + 1),
                         value->big || value->bits == std::numeric_limits<std::int64_t>::max()};
      if (!enumerator_names.insert(en).second)
        s_.diag(en_at, Diag::conflicting_declaration, en);  // [dcl.enum]/2: unique within the list
      // [basic.scope.scope]: an unscoped enumerator is declared in the enclosing scope.
      if (!scoped && cls != EntityId::none) {
        const auto previous = q_.earlier_member(en);
        if (s_.world().entities.rec(cls).name == en || (previous && previous->kind != ast::DeclKind::enum_))
          s_.diag(en_at, Diag::conflicting_declaration, en);
      } else if (!scoped) {
        const std::string_view spelling = s_.world().ins.names.spelling(en);
        for (DeclId p : p_->declared_in(spelling, p_->ns_of(k_), k_, false, /*inhabit=*/true))
          if (auto ds = p_->decl_sema(p); ds && ds->entity != EntityId::none && ds->kind != ast::DeclKind::class_ &&
                                          ds->kind != ast::DeclKind::enum_) {
            s_.diag(en_at, Diag::conflicting_declaration, en);
            break;
          }
      }
      const EntityId x = unnamed ? EntityId::none : enumerator_entity(en);
      const ConstId c = value ? s_.world().values.integer(prev_type, sema::u64(value->bits)) : ConstId::none;
      if (!unnamed) {
        if (!value) p_->value_dependent_member(x);
        else if (!pattern) p_->set_constant(x, c, k_);
      }
      er.enumerators.push_back({en, x, ConstId::none});
      values.push_back(value);
      ast::DeclSema xs = class_decl(e, true);
      xs.kind = ast::DeclKind::enumerator;
      xs.entity = x;
      xs.type = prev_type;  // inside the list
      s_.own.push_back({en, xs, unnamed || pattern ? c : ConstId::none});
      if (!ps_.accept(Tk::comma)) {
        // [dcl.enum]/1: the enumerator-list is comma-separated.
        if (!ps_.is(Tk::r_brace) && !ps_.at_end()) s_.diag(ps_.pos(), Diag::expected_token, Tk::comma);
        break;
      }
    }
    s_.nested_enum_members_begin.reset();
    // [dcl.enum]/7: without a fixed type, the type holds every value, and is no larger than int
    // unless a value fits neither int nor unsigned int; which one is the implementation's. As the
    // Itanium C++ ABI compilers choose (#220): with no negative value, `unsigned int` if every
    // value fits, else `unsigned long`; else `int` if every value fits, else `long`. An empty
    // enumeration has the values of `unsigned int`.
    const bool has_dependent_value = std::ranges::any_of(values, [](const auto& value) { return !value; });
    if (!fixed && has_dependent_value) {
      er.underlying.reset();
    } else if (!fixed) {
      using sema::Builtin;
      const bool negative = std::ranges::any_of(values, [](const auto& v) { return v->bits < 0 && !v->big; });
      Builtin u = Builtin::unsigned_int;
      if (!negative) {
        sema::u64 hi = 0;
        for (const auto& v : values) hi = std::max(hi, sema::u64(v->bits));
        if (hi > std::numeric_limits<std::uint32_t>::max()) u = Builtin::unsigned_long;
      } else if (std::ranges::any_of(values, [](const auto& v) { return v->big; })) {
        no_enum_type(name_at);  // [dcl.enum]/7: no integral type holds them all
        u = Builtin::long_;
      } else {
        auto [lo, hi] = std::ranges::minmax(values | std::views::transform([](const auto& v) { return v->bits; }));
        u = lo >= std::numeric_limits<std::int32_t>::min() && hi <= std::numeric_limits<std::int32_t>::max() ? Builtin::int_
                                                                                                             : Builtin::long_;
      }
      er.underlying = s_.builtin(u);
      // [conv.prom]/3: the first of int, unsigned int, long, unsigned long that holds every
      // value of the enumeration (its range, [dcl.enum]/8: these hold it when they hold the
      // enumerators).
      Builtin p = Builtin::unsigned_long;
      for (Builtin c : {Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long}) {
        const bool all = std::ranges::all_of(values, [&](const auto& v) { return holds(s_.builtin(c), v->bits, v->big); });
        if (all) {
          p = c;
          break;
        }
      }
      er.promotion = s_.builtin(p);
    }
    if (unnamed) {
      if (er.enumerators.empty() || !ps_.expect(Tk::r_brace)) return bad();
      s_.defining_unnamed_enum_scope.reset();
      if (inst_ && s_.has_error_since(0)) return bad();
      // [dcl.enum]/12: the first enumerator and underlying type identify the enum in its scope.
      // An enum with dependent values and no fixed base has no underlying type until replay.
      const QualType identity_type = er.underlying ? types.canonical(*er.underlying) : s_.builtin(sema::Builtin::dependent);
      e = s_.world().entities.entity({.kind = sema::EntityKind::enum_, .name = name,
                                       .type = identity_type, .scope = scope,
                                       .enum_linkage_name = er.enumerators.front().name});
      publish_head();
    } else {
      defining.reset();
    }
    if (replay != EntityId::none && s_.has_error_since(0)) return;
    // Past the closing brace an enumerator has the enumeration's type ([dcl.enum]/5), in the
    // declarators that follow too.
    const QualType t = types.enum_type(e);
    for (std::size_t i = 0; i < er.enumerators.size(); ++i) {
      sema::EnumeratorRec& xr = er.enumerators[i];
      if (unnamed) xr.entity = enumerator_entity(xr.name);
      if (values[i] && er.underlying) {
        xr.value = s_.world().values.integer(*er.underlying, sema::u64(values[i]->bits));
        p_->set_constant(xr.entity, xr.value, k_);
      } else {
        p_->value_dependent_member(xr.entity);
      }
      sema::Sema::OwnDecl& own = s_.own[first_enumerator + i];
      own.sema.entity = xr.entity;
      own.sema.type = t;
      own.enum_initializer = ConstId::none;
      if (replay == EntityId::none) decls_.push_back(own.sema);
    }
    p_->classes_.publish_enum(e, std::move(er));
    if (replay != EntityId::none) return;
    // The scoped names were visible while parsing the enumerator list only.
    if (scoped) s_.own.resize(first_enumerator);
    enum_declarators(cls, t, at, td);
  }

  void enum_declarators(EntityId cls, QualType t, u32 at, bool td) {
    ps_.seek(it_->brace_end);
    ps_.skip_attributes(nullptr, 0, nullptr, false, Parser::attr_gnu);
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
    if (cls != EntityId::none) {
      return member_declarators(spec, class_decl(cls, true), /*fields_only=*/true);
    }
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

  void static_assert_declaration() {
    tree_ = ps_.static_assert_declaration().id;
    if (!ps_.at_end()) s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
  }

  // A member-declaration ([class.mem.general]) of the class item enclosing this one.
  void member_declaration() {
    EntityId ce = inst_ ? inst_->spec : p_->class_entity(it_->parent);
    if (ce == EntityId::none) return bad();
    ast::DeclSema c = class_decl(ce, true);
    s_.current_class = ce;  // names in a member declaration are looked up in the class first
    if (ps_.accept(Tk::semi)) return;
    if (ps_.is(Tk::kw_static_assert)) {
      const sema::Sema::ClassPrefixScope prefix(s_, ce);
      const std::size_t before = p_->end_diags_.size();
      static_assert_declaration();
      if (q_.observed_type_trait_failure || q_.observed_variable_instantiation_failure ||
          q_.observed_enum_definition_failure ||
          !valid_member_declarations(std::span(p_->end_diags_).subspan(before)))
        tree_ = NodeId::none;
      return;
    }
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
      ps_.member_alias_type = true;
      ps_.replayed_member_alias_type = !s_.template_scopes.empty();
      auto t = ps_.type_id();
      ps_.replayed_member_alias_type = false;
      ps_.member_alias_type = false;
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
      // `template<…> using N = type-id;`: a member alias template ([temp.alias], [temp.mem]).
      if (ps_.is(Tk::kw_using) && ps_.kind(1) == Tk::identifier && ps_.kind(ps_.past_attributes(2)) == Tk::equal) {
        ps_.bump();
        NameId n = ps_.name_of(ps_.tok());
        u32 at = ps_.pos();
        ps_.bump();
        ps_.skip_attributes();
        ps_.bump();
        ps_.member_alias_type = true;
        auto t = ps_.type_id();
        ps_.member_alias_type = false;
        if (!t || !member_alias(c, n, at, *t, /*alias_template=*/true)) return bad();
        if (!ps_.expect(Tk::semi)) bad();
        return;
      }
    }
    // `class-key identifier ;` declares a member class ([class.nest], [dcl.type.elab]/3).
    if (!member_template_ && Parser::is_class_key(ps_.kind()) &&
        ps_.kind(ps_.past_attributes(1)) == Tk::identifier && ps_.kind(ps_.past_attributes(1) + 1) == Tk::semi)
      return member_class_declaration(ce);
    if (!member_template_ && s_.template_scopes.empty() && !inst_)
      ps_.member_template_name = [this](NameId name) -> std::optional<bool> {
        std::string_view spelling = p_->world().ins.names.spelling(name);
        bool found = false;
        p_->ask(Program::Probe::class_members, spelling, p_->ns_of(k_));
        for (auto [item, ordinal] : p_->named(spelling)) {
          if (!Program::passes(*p_->ix_, Program::Probe::class_members, p_->ns_of(k_), item, ordinal)) continue;
          const auto& member = p_->items().items[item];
          if (member.parent != it_->parent) continue;
          const auto& n = member.names[ordinal];
          if (n.member_elaborated_candidate || (n.flags & (items::name_flag::friend_ | items::name_flag::specialization))) continue;
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
      std::optional<u32> explicit_at;
      auto flags = constructor_flags(ctor_at, dtor, dtor ? &virtual_at : nullptr, ctor ? &explicit_at : nullptr);
      if (!flags) return bad();
      if (explicit_at && (!s_.template_scopes.empty() || inst_ || member_template_)) {
        s_.diag(*explicit_at, Diag::not_supported, Tk::kw_explicit);
        return bad();
      }
      return special_member(c, ctor, *flags, virtual_at, explicit_at.has_value());
    }
    std::vector<ast::AnnotationSema> annotations;
    if (ps_.is(Tk::l_square) && ps_.kind(1) == Tk::l_square) {
      u32 attrs_at = ps_.pos();
      if (!ps_.balanced_attributes()) return bad();  // [class.mem.general]
      ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
      // [class.access.spec]: an access label has no attribute-specifier-seq.
      if (ps_.is(Tk::kw_public) || ps_.is(Tk::kw_protected) || ps_.is(Tk::kw_private)) {
        s_.diag(attrs_at, Diag::attributes_not_allowed);
        return bad();
      }
    }
    std::optional<Program::ClassAlignment> alignment;
    if (ps_.is(Tk::kw_alignas)) {
      alignment = read_alignment(annotations, /*field=*/true);
      if (!alignment) return bad();
    }
    if (!ps_.conversion_id_ahead() && !Parser::starts_decl_specifier(ps_.kind()) && !ps_.starts_declaration()) {
      s_.diag(ps_.pos(), lex::is_keyword(ps_.kind()) || ps_.is(Tk::l_square) || ps_.is(Tk::coloncolon) || ps_.is(Tk::tilde)
                             ? Diag::not_supported
                             : Diag::expected_declaration,
              ps_.kind());
      return bad();
    }
    // The elaborated name can introduce a namespace class only at this decl-specifier.
    std::optional<ast::DeclSema> introduced;
    const items::DeclName* candidate = !it_->names.empty() && it_->names.back().member_elaborated_candidate
                                           ? &it_->names.back() : nullptr;
    auto declare_class = std::move(ps_.declare_class);
    if (candidate && p_->namespace_class_friend(k_) && !member_template_ && !inst_) {
      ps_.declare_class = [&](NameId name, Tk key, u32 at) -> std::optional<QualType> {
        if (at != candidate->offset) return std::nullopt;
        std::optional<ast::DeclSema> visible_type;
        for (DeclId id : p_->visible_decls(candidate->name, k_, false, p_->ns_of(k_), {}, Program::LookupNames::types)) {
          auto d = p_->decl_sema(id);
          if (!d) continue;
          if (visible_type && visible_type->entity != d->entity) {
            s_.diag(at, Diag::ambiguous_name, name);
            return std::nullopt;
          }
          visible_type = *d;
        }
        if (visible_type) {
          if (visible_type->kind != ast::DeclKind::class_ || (visible_type->flags & sema::decl_flag::template_)) {
            s_.diag(at, Diag::conflicting_declaration, name);
            return std::nullopt;
          }
          if (!record_class_key(visible_type->entity, key, at)) return std::nullopt;
          return visible_type->type;
        }
        for (DeclId id : p_->declared_in(candidate->name, p_->ns_of(k_), k_, false, /*inhabit=*/true)) {
          auto d = p_->decl_sema(id);
          if (d && d->kind == ast::DeclKind::namespace_) {
            s_.diag(at, Diag::conflicting_declaration, name);
            return std::nullopt;
          }
        }
        EntityId entity = class_entity(name);
        introduced = class_decl(entity, false);
        s_.member_introduction = sema::Sema::MemberIntroduction{name, *introduced, key == Tk::kw_union};
        return introduced->type;
      };
    }
    DeclSpec spec = ps_.decl_specifiers(/*in_type_id=*/false, nullptr, /*allow_conversion_id=*/true, /*class_member=*/true);
    spec.annotations = std::move(annotations);
    ps_.declare_class = std::move(declare_class);
    auto commit_introduction = [&] {
      if (introduced && decls_.size() == candidate->ordinal && out_.diags.empty() &&
          record_class_key(introduced->entity, spec.class_key, spec.class_at)) {
        decls_.push_back(*introduced);
        p_->declared(*introduced, k_);
      }
    };
    if (spec.is_extern || spec.is_constinit) {
      s_.diag(spec.begin, Diag::not_supported, spec.is_extern ? Tk::kw_extern : Tk::kw_constinit);
      return bad();
    }
    if (spec.ok && spec.elaborated && !ps_.is(Tk::semi) &&
        (!s_.types().is_class(spec.type) || s_.is_union_type(spec.type) != (spec.class_key == Tk::kw_union))) {
      s_.diag(spec.class_at, Diag::conflicting_declaration, spec.class_name);
      return bad();
    }
    if (!spec.ok) return bad();
    if (spec.elaborated && ps_.is(Tk::semi)) {
      s_.diag(spec.begin, Diag::not_supported, Tk::kw_struct);  // a member class declaration
      return bad();
    }
    // A member typedef ([class.mem], [dcl.typedef]): each declarator a type nested in the class.
    if (spec.is_typedef) {
      if (alignment) {
        s_.diag(alignment->at, Diag::attributes_not_allowed);
        return bad();
      }
      if (spec.mutable_at) {
        s_.diag(*spec.mutable_at, Diag::mutable_placement);
        return bad();
      }
      do {
        parse::Declarator d = ps_.declarator(spec.type, false, true);
        if (!d.ok || !d.named || d.qualified || !member_alias(c, d.name, d.name_at, d.type)) return bad();
      } while (ps_.accept(Tk::comma));
      if (ps_.expect(Tk::semi)) commit_introduction();
      else bad();
      return;
    }
    member_declarators(spec, c, /*fields_only=*/false, alignment);
    commit_introduction();
  }

  void member_declarators(const DeclSpec& spec, const ast::DeclSema& c, bool fields_only = false,
                          const std::optional<Program::ClassAlignment>& alignment = std::nullopt) {
    sema::Types& types = s_.types();
    const std::span<const std::pair<u32, u32>> applied = alignment ? std::span(alignment->specifiers)
                                                                  : std::span<const std::pair<u32, u32>>{};
    if (alignment) p_->field_alignment_specifiers_.try_emplace(k_, alignment->specifiers);
    const bool shared_overlap = layout_attribute(p_->region(), it_->begin, ps_.index(),
                                                 LayoutAttributes::leading_affecting_layout, applied, "no_unique_address");
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
      auto exception = class_member;
      if (p_->defaulted_body(k_)) exception = defaulted_assignment;
      else if (member_template_) exception = member_template;
      const bool replay_array_bounds = !spec.is_static && !member_template_ && !conversion &&
                                       s_.templated() && p_->named_nested_class(it_->parent);
      parse::Declarator d = ps_.declarator(spec.type, /*abstract=*/false, /*typedef_=*/false, conversion, exception,
                                          /*defer_ref_check=*/true, /*reject_parameter_defaults=*/false,
                                          replay_array_bounds);
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
      const bool function = types.is_function(d.type);
      if (alignment && (function || ps_.is(Tk::colon))) {
        s_.diag(alignment->at, Diag::attributes_not_allowed);
        return bad();
      }
      if (alignment && (spec.is_static || types.is_reference(d.type))) {
        s_.diag(alignment->at, Diag::not_supported, Tk::kw_alignas);
        return bad();
      }
      if (fields_only && (function || spec.is_static)) {
        s_.diag(d.name_at, Diag::not_supported, Tk::kw_enum);
        return bad();
      }
      const sema::MemberDeclRec* overridden = function && !member_template_ ? overridden_member(c.entity, d) : nullptr;
      const bool is_virtual = spec.virtual_at.has_value() || overridden;
      if (is_virtual) {
        const u32 at = spec.virtual_at.value_or(d.name_at);
        if (!function || spec.is_static) {
          s_.diag(at, Diag::virtual_placement);
          return bad();
        }
        if (!virtual_member_scope(c.entity, at)) return bad();
        if (types.has_placeholder(types.rec(d.type).inner)) {
          s_.diag(d.name_at, Diag::virtual_placeholder_return);
          return bad();
        }
        if (spec.is_consteval && s_.ins().names.spelling(d.name) == "operator=") {
          s_.diag(spec.begin, Diag::not_supported, Tk::kw_consteval);  // implicit virtual assignments (#309)
          return bad();
        }
        if (ps_.is(Tk::kw_requires)) {
          s_.diag(ps_.pos(), Diag::virtual_constraints);
          return bad();
        }
        if (d.has_noexcept) {
          const auto f = types.rec(d.type);
          // In a class template, each specialization decides it, to compare with what it overrides.
          const bool pattern = !inst_ && !s_.template_scopes.empty();
          bool dependent = pattern && d.noexcept_tokens;
          if (inst_ && d.noexcept_tokens) read_deferred_noexcept(d);
          const bool nonthrowing = !dependent &&
                                   ps_.noexcept_value(d.noexcept_condition, d.ok, pattern ? &dependent : nullptr);
          if (!d.ok) return bad();
          d.type = types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, nonthrowing && !dependent);
        }
        if (overridden && !validate_override(d, *overridden, spec.is_consteval)) return bad();
      }
      std::optional<u32> final_at;
      if (function) {
        if (!member_virt_specifiers(overridden != nullptr, &final_at, types.is_dependent(d.type))) return bad();
        if (final_at) {
          if (!is_virtual && !may_override_dependent_base(types.is_dependent(d.type))) {
            s_.diag(*final_at, Diag::final_nonvirtual);
            return bad();
          }
          // Implicit copy and move assignment can override a virtual operator= (#309).
          if (s_.ins().names.spelling(d.name) == "operator=") {
            s_.diag(*final_at, Diag::not_supported, Tk::identifier);
            return bad();
          }
          if (it_->kind == ItemKind::function && !ps_.at_end()) {
            s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
            return bad();
          }
        }
        const auto& owner = s_.world().entities.rec(c.entity);
        const bool ordinary_member = !spec.is_static && s_.template_scopes.empty() && !inst_ &&
                                     p_->is_namespace(p_->outermost_class_scope(c.entity));
        // Replay the written object ref-qualifier for the supported deleted copy-assignment form.
        // member_function_or_static checks the copy-assignment parameter and declaration context.
        const bool deleted_template_assignment = !spec.is_static && !member_template_ &&
            owner.templ != EntityId::none && p_->is_namespace(owner.scope) &&
            s_.ins().names.spelling(d.name) == "operator=" && plain_deleted_body();
        if (!is_virtual && !conversion && !ordinary_member && !deleted_template_assignment &&
            !ps_.check_named_ref_qualifier(d)) return bad();
      }
      // A member whose type became a function type through substitution is a data member of
      // function type, which cannot exist ([temp.inst], [class.mem.general]).
      if (inst_ && types.is_function(d.type) && !d.function) {
        s_.diag(d.name_at, Diag::member_of_function_type, d.type);
        return bad();
      }
      // [dcl.stc]: mutable is allowed only on a non-static data member with a
      // non-const, non-reference type.
      if (spec.mutable_at && (function || spec.is_static ||
                             types.is_reference(d.type) || sema::Types::is_const(types.innermost_array_element(types.canonical(d.type))))) {
        s_.diag(*spec.mutable_at, Diag::mutable_placement);
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
        if (!member_function_or_static(spec, d, c, is_virtual, final_at.has_value(), overridden)) return bad();
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
      if (!s_.require_concrete_object(d.type, d.name_at)) return bad();
      u8 field_alignment = 0;
      u8 written_field_alignment = 0;
      if (alignment && alignment->value && !alignment->dependent && !types.is_dependent(d.type)) {
        const auto natural = s_.layout_of(d.type, /*align=*/true);
        if (!natural) {
          s_.diag(alignment->at, Diag::not_supported, Tk::kw_alignas);
          return bad();
        }
        if (alignment->value < *natural) {
          s_.diag(alignment->at, Diag::field_alignment_weak);
          return bad();
        }
        written_field_alignment = alignment->value;
        if (written_field_alignment > *natural) field_alignment = written_field_alignment;
      }
      if (p_->class_rec_is_union(it_->parent) && !variant_member(d)) return bad();
      if (member_declared(d.name)) {
        s_.diag(d.name_at, Diag::redeclared_member, d.name);
        return bad();
      }
      ast::DeclSema sem = class_decl(c.entity, false);
      sem.kind = ast::DeclKind::data_member;
      sem.type = d.type;
      sem.bit_width = bit_width;
      sem.field_alignment = field_alignment;
      sem.written_field_alignment = written_field_alignment;
      sem.is_mutable = spec.mutable_at.has_value();
      sem.linkage = ast::Linkage::none;
      sem.flags = sema::decl_flag::definition;
      if (shared_overlap || layout_attribute(p_->region(), it_->begin + d.begin, ps_.index(),
                                             LayoutAttributes::affecting_layout, {}, "no_unique_address")) {
        const NameId attr_name = s_.ins().names.intern("no_unique_address");
        const NodeId attr = s_.node(ast::Attribute{s_.no_name(), attr_name, b_.list({})}, d.begin, d.begin).id;
        sem.attributes = b_.seq({attr});
      }
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::data_member, d.name, d.type, LangLinkage::cxx, ast::Linkage::none, {}, c.entity});
      // A default member initializer ([class.mem.general]) is a complete-class context: it is
      // checked after the class is complete. Retain its tokens for that check and for replay.
      // A scalar constant is also kept for initialization paths that use its value.
      Ex init;
      ConstId value = ConstId::none;
      if (ps_.is(Tk::equal) || ps_.is(Tk::l_brace)) {
        bool braced = ps_.is(Tk::l_brace) || ps_.kind(1) == Tk::l_brace;
        const auto form = ps_.accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
        u32 from = ps_.pos();
        std::optional<Program::Dmi> written;
        if (inst_) {
          // [temp.inst]/12: parsing or folding a default member initializer can instantiate a
          // scoped enumeration through another dependency. Reuse the pattern's token extent.
          const auto declarations = p_->item_sema(k_).value.decls;
          if (decls_.size() < s_.ins().seqs.size(declarations)) {
            const auto field = s_.ins().seqs.at(declarations, decls_.size());
            written = p_->dmi(field.entity);
          }
        }
        if (written && written->begin == it_->begin + from) {
          ps_.seek(written->end);
          p_->set_dmi(sem.entity, *written);
        } else {
          const std::size_t n = s_.diag_count();
          init = ps_.member_initializer(d.type, braced, form);
          auto v = init.ok() && types.is_scalar(d.type) ? constant_initializer(init) : std::nullopt;
          s_.truncate_diags(n);
          if (v) value = *v;
          else init = {};
          p_->set_dmi(sem.entity, {.item = k_, .begin = it_->begin + from, .end = it_->begin + ps_.pos(),
                                  .braced = braced, .form = form});
        }
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
      return layout_attribute(p_->region(), begin, end, LayoutAttributes::any);
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
    u64 width = v && values.rec(*v).kind == sema::ValueKind::integer && !values.rec(*v).wide ? values.rec(*v).bits : 0;
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

  EntityId introduce_namespace_class(NameId name) {
    EntityId entity = class_entity(name);
    if (p_->in_attributed_namespace(k_)) p_->unapplied_declaration_attributes_.insert(entity);
    return entity;
  }

  // A friend declaration in class `c` ([class.friend]): `friend class X;` or `friend X;`, a class
  // (declared in the enclosing namespace when lookup finds none, [dcl.type.elab]/4), or a function
  // declaration, of a namespace-scope function or of another class's member. The friend is not a
  // member, and the declaration makes no name visible to ordinary lookup ([class.friend]/7).
  void friend_declaration() {
    std::vector<ast::AnnotationSema> annotations;
    ps_.skip_attributes(nullptr, Parser::attr_std | Parser::attr_gnu, &annotations);
    std::optional<u32> friend_at;
    if (ps_.is(Tk::kw_friend)) {
      friend_at = ps_.pos();
      ps_.bump();
    }
    // [class.friend], [temp.friend]: look up and substitute the type of the friend.
    // The designated class need not be complete here. The declaration introduces no name.
    if (friend_at && ps_.is(Tk::identifier) && ps_.kind(1) == Tk::semi) {
      const NameId name = ps_.name_of(ps_.tok());
      const auto* param = s_.declared_template_param(name);
      if (param && param->kind == ast::TemplateArgKind::type && !param->pack) {
        const auto type = s_.class_template_parameter_type(*param, name, ps_.pos());
        if (!type) return bad();
        auto& types = s_.types();
        const EntityId entity = types.is_class(*type) ? types.class_of(*type) : EntityId::none;
        auto sem = class_decl(entity, false);
        sem.type = *type;
        sem.flags = sema::decl_flag::friend_;
        decls_.push_back(sem);
        ps_.bump();
        ps_.bump();
        return;
      }
    }
    const bool definition = it_->kind == ItemKind::function;
    if (!definition && !annotations.empty()) {
      s_.diag(annotations.front().offset, Diag::attributes_not_allowed);
      return bad();
    }
    // [temp.friend]: in a class template, a friend function definition defines a function for
    // each specialization (`templated_friend`); its other friends are not supported yet (#259).
    const bool templated = !p_->enclosing_heads(k_).empty() || inst_;
    // `template <…> friend R f(…) {…}`: a friend function template the class defines.
    bool friend_template = false;
    if (ps_.is(Tk::kw_template) && ps_.kind(1) == Tk::less && definition && !friend_at &&
        (p_->namespace_class_friend(k_) || p_->namespace_class_template_friend(k_))) {
      if (!ps_.template_head(u16(p_->enclosing_heads(k_).size()))) return bad();
      // [temp.local]/6: its parameters do not take the class template's names.
      for (std::size_t h = 0; h + 1 < s_.template_scopes.size(); ++h)
        for (const ast::TemplateParamSema& outer : s_.template_scopes[h])
          for (const ast::TemplateParamSema& own : s_.template_scopes.back())
            if (own.name == outer.name && own.name != s_.no_name()) {
              s_.diag(0, Diag::redeclared_local, own.name);
              return bad();
            }
      head_requires_clause();
      friend_template = true;
    }
    if (ps_.is(Tk::kw_template) && ps_.kind(1) == Tk::less && !definition && !friend_at) return friend_class_template();
    // A class template's friend classes are analysed again in each specialization, with its
    // arguments; a friend function it declares without defining is not supported yet (#259).
    if (ps_.is(Tk::kw_template) || (templated && definition && !p_->namespace_class_template_friend(k_))) {
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
      // [dcl.type.elab]/2: a class-key does not take a template parameter's name.
      if (s_.declared_template_param(n)) {
        s_.diag(ps_.pos() + 1, Diag::elaborated_template_param, n);
        return bad();
      }
      auto t = s_.class_name(n, false);
      sem.entity = t && types.is_class(*t) ? types.class_of(*t) : introduce_namespace_class(n);
      decls_.push_back(sem);
      ps_.bump();
      ps_.bump();
      ps_.bump();
      return;
    }
    DeclSpec spec = ps_.decl_specifiers(false, &friend_at);
    if (!spec.ok || !friend_at) return bad();
    if (!spec.annotations.empty() && !definition) {
      s_.diag(spec.annotations.front().offset, Diag::attributes_not_allowed);
      return bad();
    }
    spec.annotations = std::move(annotations);
    if (ps_.accept(Tk::semi)) {
      // A dependent type in a class template's pattern: each specialization's friend is its own.
      if (templated && !types.is_class(spec.type) && types.is_dependent(spec.type)) return;
      if (!types.is_class(spec.type)) {
        s_.diag(spec.begin, Diag::not_supported, Tk::kw_friend);  // a friend that is not a class
        return bad();
      }
      sem.entity = types.class_of(spec.type);
      decls_.push_back(sem);
      return;
    }
    if (templated && !definition) {
      s_.diag(*friend_at, Diag::not_supported, Tk::kw_friend);  // a friend function of a template (#1179)
      return bad();
    }
    parse::Declarator d = ps_.declarator(spec.type, false);
    if (!d.ok || !d.named) return bad();
    // [dcl.attr.annotation]: a non-defining friend declaration cannot have an annotation.
    if (!definition && !d.annotations.empty()) {
      s_.diag(d.annotations.front().offset, Diag::attributes_not_allowed);
      return bad();
    }
    if (literal_operator_named(d.name) &&
        (!defaults_allowed(d.params, types.is_function(d.type), d.name) || !literal_operator_ok(spec, d, LangLinkage::cxx, false)))
      return bad();
    const bool c_context = enclosing_linkage() == LangLinkage::c;
    const Tk name_token = p_->region().tokens[it_->begin + d.name_at].kind;
    const std::string_view name = s_.ins().names.spelling(d.name);
    // An operator-function-id that names a punctuator ([over.oper.general]); `declare` checks that
    // a non-member can have it, and its parameters.
    const bool operator_function = name_token == Tk::kw_operator && name.size() > 8 && name.starts_with("operator") &&
                                   !literal_operator_named(d.name) && !sema::is_allocation_name(name) &&
                                   name != lex::conversion_function_name;
    const bool ordinary = (definition || !c_context) && !d.qualified && !d.template_args &&
                          (p_->namespace_class_friend(k_) || templated) &&
                          (name_token == Tk::identifier || operator_function);
    const bool deleted_operator = operator_function && !templated && !friend_template && deleted_body();  // templates: #1181
    // [class.compare.default]: `friend bool operator==(C, C) = default;`, and `<=>` likewise.
    const bool defaulted_comparison = definition && name_token == Tk::kw_operator &&
                                      (name == "operator==" || name == "operator<=>") && p_->defaulted_body(k_);
    if (!types.is_function(d.type) ||
        (definition ? !ordinary || !d.function || !ps_.at_end() || !it_->has_body() ||
                        (p_->region().tokens[it_->body_begin].kind != Tk::l_brace && !deleted_operator &&
                         !defaulted_comparison)
                    : !ps_.is(Tk::semi))) {
      s_.diag(d.name_at, Diag::not_supported, Tk::kw_friend);
      return bad();
    }
    if (defaulted_comparison) {
      // A class template's defaulted comparison is not done yet (#295).
      if (templated) {
        s_.diag(d.name_at, Diag::not_supported, Tk::kw_default);
        return bad();
      }
      if (!defaulted_comparison_ok(d, types.class_type(p_->class_entity(it_->parent)), false)) return bad();
      // [dcl.fct.def.default]: implicitly constexpr; constexpr-suitable or not, a call is
      // constant only when its subobject comparisons are ([expr.const]).
      if (!spec.is_consteval) spec.is_constexpr = true;
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
      if (templated || friend_template) return templated_friend(spec, d, friend_template);
      if (!declare(spec, std::move(d), LangLinkage::cxx, false, definition, /*ordinary_friend=*/true)) return bad();
      p_->ordinary_friend_items_.insert(k_);
      if (!definition) ps_.expect(Tk::semi);
      return;
    }
    sem.kind = ast::DeclKind::function;
    sem.type = d.type;
    sem.has_written_noexcept = d.has_noexcept;
    sem.has_trailing_return = d.trailing_return;
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
      // [dcl.inline]: a friend declaration using inline must define the function or
      // follow an inline declaration. This path handles nondefining member friends.
      if (m->is_virtual && spec.is_inline) {
        const auto earlier = p_->declaration(m->entity, k_);
        if (!earlier || !(earlier->flags & sema::decl_flag::inline_)) {
          s_.diag(*friend_at, Diag::not_supported, Tk::kw_friend);
          return bad();
        }
      }
      if (m->is_virtual && layout_attribute(p_->region(), it_->begin, it_->end, LayoutAttributes::any))
        p_->unapplied_declaration_attributes_.insert(m->entity);
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

  // `template <…> friend class X;` ([temp.friend]/1), in a class or a class template: every
  // specialization of class template X, from its primary or a partial specialization, is a friend,
  // so its members can name the class's private and protected members ([class.friend]/2).
  // Unqualified, lookup for X does not leave the innermost enclosing namespace ([dcl.type.elab]/4);
  // when it finds no class template there, X is one of that namespace, which the declaration does
  // not make visible ([dcl.meaning.general]/2.3): a later declaration of the template there names
  // it. Qualified, `N::X` is a class template N declared earlier.
  // The template-heads must agree ([temp.over.link]/6); their constraints are not compared yet
  // (#1182). Friend function templates without a definition are not supported yet (#1179).
  void friend_class_template() {
    const u32 at = ps_.pos();
    if (!ps_.template_head(u16(p_->enclosing_heads(k_).size()))) return bad();
    head_requires_clause();
    const auto unsupported = [&] {
      s_.diag(at, Diag::not_supported, Tk::kw_friend);
      return bad();
    };
    if (!ps_.is(Tk::kw_friend) || (ps_.kind(1) != Tk::kw_class && ps_.kind(1) != Tk::kw_struct)) return unsupported();
    // The name, after a nested-name-specifier naming a namespace, if any.
    u32 next = 2;
    std::optional<EntityId> ns;
    if (ps_.kind(2) != Tk::identifier || ps_.kind(3) == Tk::coloncolon) {
      auto scope = ps_.nested_name(2, next);
      if (!scope || !scope->ns || scope->type) return unsupported();
      ns = *scope->ns;
    }
    if (ps_.kind(next) != Tk::identifier || ps_.kind(next + 1) != Tk::semi) return unsupported();
    const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.back();
    if (!pack_last(head)) {
      s_.diag(at, Diag::pack_not_last);
      return bad();
    }
    // [temp.param]/12: a friend class template's declaration gives no default arguments.
    if (std::ranges::any_of(head, &ast::TemplateParamSema::has_default)) {
      s_.diag(at, Diag::friend_template_default);
      return bad();
    }
    const u32 name_at = ps_.pos() + next;
    const NameId name = ps_.name_of(ps_.tok(next));
    // [temp.local]/6: neither its parameters nor its name redeclare a template parameter in scope.
    for (std::size_t h = 0; h < s_.template_scopes.size(); ++h)
      for (const ast::TemplateParamSema& outer : s_.template_scopes[h]) {
        const bool shadowed = h + 1 < s_.template_scopes.size() &&
                              std::ranges::any_of(head, [&](const ast::TemplateParamSema& own) { return own.name == outer.name; });
        if (outer.name != s_.no_name() && (shadowed || (!ns && outer.name == name))) {
          s_.diag(shadowed ? at : name_at, Diag::redeclared_local, outer.name);
          return bad();
        }
      }
    // A using-directive contributes at its common ancestor ([namespace.udir]), which can
    // be outside the innermost namespace that unqualified friend lookup may consider.
    // The elaborated name is looked up as a type ([basic.lookup.elab]).
    const std::vector<ast::DeclSema> found =
        ns ? q_.namespace_members(*ns, name, /*type_only=*/true) : q_.friend_namespace_decls(ns_entity(), name);
    EntityId templ = EntityId::none;
    for (const ast::DeclSema& prior : found) {
      if (prior.entity == EntityId::none || (prior.flags & sema::decl_flag::friend_) ||
          (prior.flags & (sema::decl_flag::specialization | sema::decl_flag::partial)))
        continue;
      // [basic.scope.scope]/4: another kind of entity of this name in the namespace.
      if (prior.kind != ast::DeclKind::class_ || !(prior.flags & sema::decl_flag::template_)) {
        s_.diag(name_at, Diag::conflicting_declaration, name);
        return bad();
      }
      if (prior.entity == templ) continue;
      if (templ != EntityId::none) {
        s_.diag(name_at, Diag::ambiguous_name, name);
        return bad();
      }
      auto match = same_template_head(s_.world().ins.seqs.elements(prior.template_params), head);
      if (match != true) {
        s_.diag(name_at, match ? Diag::conflicting_declaration : Diag::friend_template_head_not_supported, name);
        return bad();
      }
      templ = prior.entity;
    }
    if (templ == EntityId::none) {
      if (ns) {
        s_.diag(name_at, Diag::no_friend_match, name);
        return bad();
      }
      if (q_.namespace_binds_name(ns_entity(), name)) {
        s_.diag(name_at, Diag::conflicting_declaration, name);
        return bad();
      }
      // Its head as the namespace's declaration of the template has it, at depth 0, so that
      // declaration names the same entity ([temp.over.link]/6).
      std::vector<ast::TemplateParamSema> own = head;
      for (ast::TemplateParamSema& p : own) {
        if (p.kind == ast::TemplateArgKind::value) p.type = s_.rebased(p.type, head);
        p.depth = 0;
      }
      sema::EntityRec t{sema::EntityKind::class_template, name, {}, LangLinkage::cxx, ast::Linkage::external, {}, ns_entity()};
      t.head = head_fp(own);
      templ = s_.world().entities.entity(t);
    }
    ast::DeclSema sem = class_decl(templ, false);
    sem.flags = sema::decl_flag::friend_;
    decls_.push_back(sem);
    while (!ps_.is(Tk::semi)) ps_.bump();
    ps_.bump();
  }

  // The paired binders in a namespace template and its friend declaration. A friend inside a
  // class template has an extra enclosing head whose parameters cannot match the namespace
  // template's own parameters, even when those two depths happen to have the same number.
  struct FriendHeadDepth {
    u16 prior, current;
    const FriendHeadDepth* outer;
  };

  static bool same_friend_parameter_depth(u16 a, u16 b, const FriendHeadDepth& head) {
    for (auto h = &head; h; h = h->outer)
      if (h->prior == a || h->current == b) return h->prior == a && h->current == b;
    return false;
  }

  std::optional<bool> same_friend_parameter_value(ConstId a, ConstId b, const FriendHeadDepth& head) {
    auto& values = s_.ins().values;
    if (a == ConstId::none || b == ConstId::none) return a == b;
    const bool dependent_a = values.is_dependent(a), dependent_b = values.is_dependent(b);
    if (!dependent_a || !dependent_b) return a == b;
    if (!values.is<ast::VDepParam>(a) || !values.is<ast::VDepParam>(b)) return std::nullopt;
    const auto x = values.get<ast::VDepParam>(a), y = values.get<ast::VDepParam>(b);
    return x.index == y.index && same_friend_parameter_depth(x.depth, y.depth, head);
  }

  std::optional<bool> same_friend_parameter_args(std::span<const ast::TemplateArg> a, std::span<const ast::TemplateArg> b,
                                                  const FriendHeadDepth& head) {
    using enum ast::TemplateArgKind;
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      const auto& x = a[i];
      const auto& y = b[i];
      if (x.kind != y.kind) return false;
      std::optional<bool> match;
      if (x.kind == pack)
        match = same_friend_parameter_args(s_.ins().seqs.elements(x.elems), s_.ins().seqs.elements(y.elems), head);
      else if (x.kind == template_ && (x.templ != EntityId::none || y.templ != EntityId::none))
        match = x.templ == y.templ;
      else {
        match = same_friend_parameter_type(x.type, y.type, head);
        if (match == true && x.kind == value) match = same_friend_parameter_value(x.value, y.value, head);
      }
      if (match != true) return match;
    }
    return true;
  }

  // Compare the represented type without substitution, which can erase dependent syntax or
  // accidentally capture a free parameter. None means the represented types cannot be compared.
  std::optional<bool> same_friend_parameter_type(QualType a, QualType b, const FriendHeadDepth& head) {
    using enum sema::TypeKind;
    sema::Types& types = s_.types();
    a = types.canonical(a);
    b = types.canonical(b);
    if (!types.is_dependent(a) || !types.is_dependent(b)) return a == b;
    const auto x = types.rec(a), y = types.rec(b);
    if (x.kind != y.kind || sema::Types::cv(a) != sema::Types::cv(b)) return false;
    auto inner = [&] { return same_friend_parameter_type(x.inner, y.inner, head); };
    switch (x.kind) {
    case template_param:
      return x.index == y.index && x.pack == y.pack && same_friend_parameter_depth(x.depth, y.depth, head);
    case pointer:
    case pack_expansion:
      return inner();
    case reference:
      return x.rvalue_ref == y.rvalue_ref ? inner() : std::optional(false);
    case dep_name:
      return x.name == y.name ? inner() : std::optional(false);
    case transform:
      return x.trait == y.trait ? inner() : std::optional(false);
    case member_pointer:
      if (auto owner = same_friend_parameter_type(x.member_owner, y.member_owner, head); owner != true) return owner;
      return inner();
    case array:
      if (auto bound = same_friend_parameter_value(x.bound_value, y.bound_value, head); bound != true) return bound;
      return inner();
    case class_: {
      const auto& first = s_.world().entities.rec(x.entity);
      const auto& second = s_.world().entities.rec(y.entity);
      if (first.templ == EntityId::none || second.templ == EntityId::none) return std::nullopt;
      if (first.templ != second.templ) return false;
      return same_friend_parameter_args(first.args, second.args, head);
    }
    case dep_spec:
      if (x.index != y.index || x.pack != y.pack || !same_friend_parameter_depth(x.depth, y.depth, head)) return false;
      return same_friend_parameter_args(s_.ins().seqs.elements(x.args), s_.ins().seqs.elements(y.args), head);
    default:
      return std::nullopt;
    }
  }

  // [temp.over.link]/6: after pairing each head's own binders, compare the kinds, packs and
  // non-type parameter types. A template template parameter adds another pair of binders.
  std::optional<bool> same_template_head(std::span<const ast::TemplateParamSema> a, std::span<const ast::TemplateParamSema> b,
                                          const FriendHeadDepth* outer = nullptr) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    const FriendHeadDepth head{a.front().depth, b.front().depth, outer};
    auto templates = s_.templates();
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].kind != b[i].kind || a[i].pack != b[i].pack) return false;
      if (a[i].kind == ast::TemplateArgKind::value) {
        if (!templates.structural_operand(a[i].type) || !templates.structural_operand(b[i].type)) return std::nullopt;
        if (auto match = same_friend_parameter_type(a[i].type, b[i].type, head); match != true) return match;
      }
      if (a[i].kind == ast::TemplateArgKind::template_) {
        auto left = s_.ins().seqs.elements(a[i].params), right = s_.ins().seqs.elements(b[i].params);
        if (auto match = same_template_head(left, right, &head); match != true) return match;
      }
    }
    return true;
  }

  // A friend function defined in a class template ([temp.friend]/1, [class.friend]/6-7): for each
  // specialization, an inline function of the class's namespace with the arguments substituted
  // into its type, which only argument-dependent lookup finds through the specialization. Its
  // body is instantiated with the class's arguments when it is odr-used ([temp.inst]/4); the
  // class template's own analysis declares the pattern.
  // A friend function template (`own_head`) is the same, a function template of the namespace
  // whose specializations are instantiated from this item.
  void templated_friend(const DeclSpec& spec, const parse::Declarator& d, bool own_head) {
    sema::Types& types = s_.types();
    if (!operator_function_ok(d)) return bad();
    const sema::TypeRec& r = types.rec(d.type);
    if (r.fn_cv != sema::Cv::none || r.fn_ref != ast::RefQual::none) {
      s_.diag(d.name_at, Diag::cv_on_non_member);  // [dcl.fct]/6
      return bad();
    }
    if (spec.is_constinit) {
      s_.diag(spec.begin, Diag::constinit_function);  // [dcl.constinit]/1
      return bad();
    }
    const bool internal = Program::unnamed(p_->ns_of(k_));
    ast::DeclSema sem = class_decl(EntityId::none, true);
    sem.kind = ast::DeclKind::function;
    sem.type = d.type;
    sem.has_written_noexcept = d.has_noexcept;
    sem.has_trailing_return = d.trailing_return;
    sem.linkage = internal ? ast::Linkage::internal : ast::Linkage::external;
    sem.flags = sema::decl_flag::friend_ | sema::decl_flag::inline_ | sema::decl_flag::definition;
    if (spec.is_constexpr) sem.flags |= sema::decl_flag::constexpr_;
    if (spec.is_consteval) sem.flags |= sema::decl_flag::consteval_;
    if (r.variadic) sem.flags |= sema::decl_flag::variadic;
    std::vector<ast::ParamSema> params;
    for (const parse::Param& p : d.params)
      params.push_back({EntityId::none, p.type, p.name, p.default_arg, p.element ? sema::param_flag::pack_element : u16(0)});
    sem.params = b_.seq(std::span<const ast::ParamSema>(params));
    if (own_head) {
      // Its head, positional, and its constraints are part of it ([temp.over.link]).
      sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
      sem.flags |= sema::decl_flag::template_;
      sema::EntityRec e{sema::EntityKind::function_template, d.name, d.type, LangLinkage::cxx, sem.linkage, {}, ns_entity()};
      e.head = head_fp(s_.template_scopes.back());
      sem.constraints = associated_constraints();
      sem.head_constraint = head_constraint_;
      e.constraints = constraints_fp_;
      sem.entity = s_.world().entities.entity(e);
    } else {
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::function, d.name, d.type, LangLinkage::cxx, sem.linkage, {}, ns_entity()});
    }
    // A dependent noexcept-specifier is evaluated for each specialization when it is needed
    // ([except.spec]/13), in the specialization's scope.
    if (d.noexcept_tokens) p_->member_exceptions_.emplace(sem.entity, Program::MemberException{k_, d.noexcept_tokens});
    // A function the namespace defined already is redefined ([basic.def.odr]/1); two
    // specializations defining one function are `instantiate_class`'s to report.
    if (inst_ && !own_head) {
      const auto known = p_->definitions_.find(std::to_underlying(sem.entity));
      const bool ours = known != p_->definitions_.end() && known->second == k_;
      if (!ours && p_->function_defined_before(sem.entity, inst_->point)) {
        s_.diag(d.name_at, Diag::redefinition, d.name);
        return bad();
      }
    }
    decls_.push_back(sem);
    p_->templated_friend_items_.insert(k_);
    // Its declaration, an ordinary class's or a class template specialization's, not the
    // pattern's: what makes it constexpr or consteval ([dcl.constexpr]), and for a template the
    // definition its specializations are instantiated from (`Program::instance`).
    if (inst_ || p_->enclosing_heads(k_).empty()) p_->declared(sem, k_);
  }

  // A typedef-name or alias-declaration that is a member of class `c` ([class.mem],
  // [dcl.typedef]): a type of the class's scope, `C::name` outside it.
  // A member alias template's entity is (class, name, head), as a namespace one's is
  // (`alias_template`).
  bool member_alias(const ast::DeclSema& c, NameId name, u32 at, QualType type, bool alias_template = false) {
    if (literal_operator_named(name)) {
      s_.diag(at, Diag::literal_operator_scope);
      return false;
    }
    if (s_.types().has_placeholder(type)) {  // [dcl.spec.auto.general]/6
      s_.diag(at, Diag::auto_not_allowed);
      return false;
    }
    // [temp.local]/6: not a template parameter's name; [class.mem.general]: not the class's.
    if (s_.declared_template_param(name)) {
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
    // A member class of the same name ([basic.scope.scope]/4: a typedef-name and a class name).
    if (auto previous = q_.earlier_member(name, /*type_only=*/true); previous && previous->kind == ast::DeclKind::class_) {
      s_.diag(at, Diag::conflicting_declaration, name);
      return false;
    }
    sema::EntityRec e{alias_template ? sema::EntityKind::alias_template : sema::EntityKind::type_alias, name, {},
                      LangLinkage::cxx, ast::Linkage::none, {}, c.entity};
    if (alias_template) {
      const std::vector<ast::TemplateParamSema>& head = s_.template_scopes.back();
      if (!pack_last(head)) {
        s_.diag(0, Diag::pack_not_last);
        return false;
      }
      e.head = head_fp(head);
    }
    ast::DeclSema sem = class_decl(s_.world().entities.entity(e), false);
    sem.kind = ast::DeclKind::type_alias;
    sem.type = type;
    sem.linkage = ast::Linkage::none;
    sem.flags = sema::decl_flag::member;
    if (alias_template) {
      sem.flags |= sema::decl_flag::template_;
      sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
    }
    decls_.push_back(sem);
    s_.own.push_back({name, sem});
    // Its declaration, which `Templates::apply_template` reads when it is a template argument.
    if (alias_template) p_->declared(sem, k_);
    return true;
  }

  // [class.compare.default], [class.eq]: a defaulted `operator==` or `operator<=>` of class `cls`, a
  // const member taking `const C&` (the implicit object parameter is the first `const C&`), or a
  // friend taking two `const C&` or two `C`; `operator==` returns `bool` ([class.spaceship] takes
  // any return type of `<=>`).
  bool defaulted_comparison_ok(const parse::Declarator& d, QualType cls, bool member) {
    sema::Types& types = s_.types();
    const sema::TypeRec f = types.rec(d.type);
    const QualType ref = types.canonical(types.lvalue_ref(types.add_cv(cls, sema::Cv::const_)));
    const QualType value = types.canonical(cls);
    auto all = [&](QualType t) {
      return std::ranges::all_of(f.params, [&](QualType p) { return types.canonical(p) == t; });
    };
    const bool ok = !f.variadic && (member ? f.params.size() == 1 && all(ref) && f.fn_cv == sema::Cv::const_ &&
                                                 f.fn_ref != ast::RefQual::rvalue
                                           : f.params.size() == 2 && (all(ref) || all(value)));
    if (!ok) {
      s_.diag(d.name_at, Diag::defaulted_comparison_declaration, cls);
      return false;
    }
    if (s_.ins().names.spelling(d.name) == "operator==" && types.canonical(f.inner) != types.builtin(sema::Builtin::bool_)) {
      s_.diag(d.name_at, Diag::defaulted_equality_return);
      return false;
    }
    // [class.spaceship]/2: a placeholder is `auto` alone, not `decltype(auto)`, a constrained or
    // cv-qualified `auto`, or one in a compound type.
    if (types.has_placeholder(f.inner)) {
      const sema::TypeRec r = types.rec(f.inner);
      if (r.kind != sema::TypeKind::placeholder || r.decltype_auto || r.entity != EntityId::none ||
          sema::Types::cv(f.inner) != sema::Cv::none) {
        s_.diag(d.name_at, Diag::defaulted_three_way_placeholder);
        return false;
      }
    }
    return true;
  }

  // Whether the item's "body" is `= delete` ([dcl.fct.def.delete]).
  bool deleted_body() const {
    const pp::Region& r = p_->region();
    return it_->has_body() && r.tokens[it_->body_begin].kind == Tk::equal && it_->body_begin + 1 < it_->body_end &&
           r.tokens[it_->body_begin + 1].kind == Tk::kw_delete;
  }

  bool plain_deleted_body() const {
    return deleted_body() && it_->body_end - it_->body_begin == 3 &&
           p_->region().tokens[it_->body_end - 1].kind == Tk::semi;
  }

  // A qualified definition of a namespace member redeclares an earlier declaration in its
  // target namespace or inline namespace set ([dcl.meaning.general], [basic.scope.scope]).
  bool define_namespace_member(const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition,
                               u32 ordinal, bool explicit_lang) {
    const std::string ns = p_->namespace_path(*d.scope.ns);
    const QualType type = s_.types().canonical(sem.type);
    // BOOTSTRAP: -Wmaybe-uninitialized: a value and a flag, not std::optional. At -O3 GCC 16.2
    // reports the optional's payload as possibly uninitialized after `if (!mem)` (#748).
    // Without the bug: `std::optional<ast::DeclSema> mem;`, `mem = ds;`, `mem->entity`.
    ast::DeclSema mem{};
    bool has_mem = false;
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
      mem = *ds, has_mem = true;
      c_namespace = std::move(target);
    }
    if (!has_mem) {
      s_.diag(d.name_at, linkage_conflict ? Diag::linkage_conflict : Diag::no_namespace_member, d.name);
      return false;
    }
    auto latest = p_->declaration(mem.entity, k_);
    if (definition && latest && (latest->flags & sema::decl_flag::definition)) {
      s_.diag(d.name_at, Diag::redefinition, d.name);
      return false;
    }
    sem.entity = mem.entity;
    sem.linkage = mem.linkage;
    if (c_namespace && (mem.flags & sema::decl_flag::lang_c)) {
      sem.type = mem.type;
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
    const sema::EntityRec& e = p_->world().entities.rec(r.entity);
    // An explicit specialization of a member names the specialization's member of that type, the
    // one whose constraints are satisfied ([temp.expl.spec]); another definition repeats its
    // trailing requires-clause ([temp.over.link]/6).
    if (!member_template && explicit_)
      return !r.ineligible && (r.constraints == ConstId::none || p_->satisfied(r.constraints, k_).value_or(true));
    if (!member_template) return e.constraints == member_constraints_fp();
    return e.head == head_fp(s_.template_scopes.back()) && e.constraints == constraints_fp_;
  }
  // A constructor or destructor defined outside its class: `N::C::C(…)`, `C::~C()`.
  void out_of_class_special(QualType cls, u32 next, u16 flags) {
    EntityId c = s_.types().class_of(cls);
    for (u32 j = 0; j < next; ++j) ps_.bump();
    bool ctor = !ps_.is(Tk::tilde);
    const bool specialized_destructor = explicit_ && !ctor;
    const bool definition = it_->kind == ItemKind::function;
    u32 name_at = ps_.pos();
    if (!ctor && p_->defaulted_body(k_)) {
      s_.diag(name_at, Diag::defaulted_destructor_out_of_class);
      return bad();
    }
    if (!ctor) ps_.bump();
    ps_.bump();
    EntityId owner = s_.world().entities.rec(c).scope;
    if (specialized_destructor) {
      const auto& entity = s_.world().entities.rec(c);
      if (entity.templ == EntityId::none) {
        s_.diag(name_at, Diag::not_supported, Tk::kw_template);
        return bad();
      }
      if (p_->explicitly_specialized(c, k_)) {
        s_.diag(name_at, Diag::member_of_explicit_specialization, entity.name);
        return bad();
      }
      if (!Program::encloses(p_->ns_of(k_), p_->enclosing_namespace_path(owner))) {
        s_.diag(name_at, Diag::conflicting_declaration, entity.name);
        return bad();
      }
    }
    if (flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) {
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
    bool nonthrowing = ctor && constructor_exception_specifier(fs, cls, ok);
    if (!ok) return bad();
    if (!member_requires_clause(fs.params)) return bad();
    std::vector<QualType> ps;
    for (const parse::Param& p : fs.params) ps.push_back(p.type);
    QualType type = s_.types().function(s_.builtin(sema::Builtin::void_), ps, fs.variadic, LangLinkage::cxx, fs.cv,
                                       ast::RefQual::none, nonthrowing);
    // Complete here: this unit has analysed the class, so it knows the members' declarations.
    const sema::ClassRec* cr = q_.is_complete(c) ? p_->classes_.find(c) : nullptr;
    // A constructor template's definition: the innermost head is its own ([temp.mem]/1).
    const bool member_template = member_template_ || (template_ && s_.world().entities.rec(c).templ == EntityId::none);
    const sema::MemberDeclRec* m = nullptr;
    if (cr)
      for (const sema::MemberDeclRec& r : cr->members)
        if ((ctor ? r.is_ctor : r.is_dtor) && (r.is_virtual || same_function_parameters(r.type, type)) &&
            same_template(r, member_template))
          m = &r;
    if (!m) {
      s_.diag(name_at, Diag::no_member_declared, p_->world().entities.rec(c).name, cls);
      return bad();
    }
    auto md = p_->member_decl(m->entity);
    auto mem = md ? p_->decl_sema(*md) : std::nullopt;
    if (!mem || (!definition && !specialized_destructor)) return bad();
    if ((mem->flags & sema::decl_flag::constexpr_) != (flags & sema::decl_flag::constexpr_)) {
      s_.diag(name_at, Diag::constexpr_redeclared, m->name);
      return bad();
    }
    if ((mem->flags & sema::decl_flag::consteval_) != (flags & sema::decl_flag::consteval_)) {  // [dcl.constexpr]/1
      s_.diag(name_at, Diag::consteval_redeclared, m->name);
      return bad();
    }
    if (ctor && p_->unresolved_special_member_exception(m->entity))
      return bad();  // class completion reports an invalid specification
    const QualType declared_type = mem->type;
    if (ctor && s_.types().rec(declared_type).fn_noexcept != nonthrowing) {
      s_.diag(name_at, Diag::conflicting_declaration, m->name);
      return bad();
    }
    if ((flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) && !finish_constructor_declarator()) return bad();
    if (specialized_destructor && !m->is_virtual) {
      if (!check_destructor_specialization(*cr, *m, fs, name_at, definition)) return bad();
    } else if (mem->flags & sema::decl_flag::definition) {
      s_.diag(name_at, Diag::redefinition, m->name);
      return bad();
    }
    if (m->is_virtual) {
      if (const auto prior = p_->declaration(m->entity, k_); prior && (prior->flags & sema::decl_flag::definition)) {
        s_.diag(name_at, Diag::redefinition, m->name);
        return bad();
      }
      if (cr->dynamic && !template_ && !inst_) {
        // [except.spec]: the definition repeats the declaration's specification or none.
        ps_.exception_specifier(fs, ok);
        if (!ok) return bad();
        std::optional<bool> nonthrowing;
        if (fs.has_noexcept) nonthrowing = ps_.noexcept_value(fs.noexcept_condition, ok);
        if (!ok) return bad();
        if (nonthrowing && nonthrowing != cr->destructor_noexcept) {
          s_.diag(name_at, Diag::conflicting_declaration, m->name);
          return bad();
        }
        if (fs.cv != sema::Cv::none || !fs.params.empty() || fs.variadic) {
          s_.diag(name_at, Diag::conflicting_declaration, m->name);  // [class.dtor]/1
          return bad();
        }
      } else if (template_ || inst_) {
        // A class template's: each specialization has its own table ([temp.mem.func]).
        ps_.exception_specifier(fs, ok);
        if (!ok) return bad();
        if (fs.cv != sema::Cv::none || !fs.params.empty() || fs.variadic) {
          s_.diag(name_at, Diag::conflicting_declaration, m->name);  // [class.dtor]/1
          return bad();
        }
      } else {
        const bool derived = p_->derived_virtual_destructor_layout(c).has_value();
        if ((!derived && !p_->peer_root_destructor_key(c)) || flags || template_ || explicit_ || inst_ || !it_->has_body() ||
            p_->region().tokens[it_->body_begin].kind != Tk::l_brace ||
            (derived && (it_->body_begin + 1 >= it_->end || p_->region().tokens[it_->body_begin + 1].kind != Tk::r_brace)) ||
            p_->in_attributed_namespace(k_) ||
            layout_attribute(p_->region(), it_->begin, it_->body_begin, LayoutAttributes::any) ||
            !Program::encloses(p_->ns_of(k_), p_->namespace_path(owner)) ||
            fs.cv != sema::Cv::none || !fs.params.empty() || fs.variadic) {
          s_.diag(name_at, Diag::virtual_definition);
          return bad();
        }
        ps_.exception_specifier(fs, ok);
        if (!ok) return bad();
        if (fs.has_noexcept && !ps_.noexcept_value(fs.noexcept_condition, ok)) {
          if (ok) s_.diag(name_at, Diag::conflicting_declaration, m->name);
          return bad();
        }
        if ((!derived && !fs.has_noexcept) || fs.noexcept_condition || !ps_.at_end()) {
          s_.diag(name_at, Diag::virtual_definition);
          return bad();
        }
      }
    }
    // Its record, rebuilt in this frame (the declaration's lists belong to the class's item).
    ast::DeclSema sem = *mem;
    if (ctor || m->is_virtual || specialized_destructor) sem.has_written_noexcept = fs.has_noexcept;
    if (specialized_destructor) {
      using namespace sema::decl_flag;
      // The written in-class declaration supplies the pattern. This definition names the member
      // of the selected class specialization and supplies its own inline flag ([temp.expl.spec]).
      sem.entity = m->entity;
      sem.type = m->type;
      sem.flags = u16(flags | member | destructor | specialization | (definition ? sema::decl_flag::definition : 0));
      p_->specialization_named_at(sem.entity, name_at);
    } else {
      sem.flags |= flags | sema::decl_flag::definition;
    }
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

  bool check_destructor_specialization(const sema::ClassRec& cls, const sema::MemberDeclRec& member,
                                       Parser::Suffix& suffix, u32 name_at, bool definition) {
    if (member.is_deleted || (definition && (!it_->has_body() || p_->region().tokens[it_->body_begin].kind != Tk::l_brace))) {
      s_.diag(name_at, Diag::not_supported, Tk::tilde);
      return false;
    }
    if (definition && p_->explicitly_specialized(member.entity, k_)) {
      const auto key = std::to_underlying(member.entity);
      const auto previous = p_->definitions_.find(key);
      if (previous != p_->definitions_.end() && previous->second >= p_->explicit_specs_.at(key) && previous->second < k_) {
        s_.diag(name_at, Diag::redefinition, member.name);
        return false;
      }
    }
    bool ok = true;
    ps_.exception_specifier(suffix, ok);
    if (!ok) return false;
    // [except.spec]: a destructor without a written specification uses the implicit one.
    const auto original = p_->destructor_declarations_.find(member.entity);
    std::optional<bool> nonthrowing;
    if (suffix.has_noexcept) nonthrowing = ps_.noexcept_value(suffix.noexcept_condition, ok);
    else if (original != p_->destructor_declarations_.end()) nonthrowing = original->second.implicit_nonthrowing;
    if (!ok) return false;
    if (nonthrowing != cls.destructor_noexcept) {
      s_.diag(name_at, Diag::conflicting_declaration, member.name);
      return false;
    }
    if (definition) return finish_constructor_declarator();
    ps_.skip_attributes();
    return ps_.expect(Tk::semi);
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
    while (ps_.kind(next) == Tk::kw_constexpr || ps_.kind(next) == Tk::kw_consteval || ps_.kind(next) == Tk::kw_inline ||
           ps_.kind(next) == Tk::kw_static || ps_.kind(next) == Tk::kw_virtual ||
           (ps_.kind(next) == Tk::kw_explicit && ps_.kind(next + 1) != Tk::l_paren))
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

  std::optional<u16> constructor_flags(u32 count, bool destructor, std::optional<u32>* virtual_at = nullptr,
                                       std::optional<u32>* explicit_at = nullptr) {
    using namespace sema::decl_flag;
    u16 flags = 0;
    u32 end = ps_.pos() + count;
    while (ps_.pos() < end) {
      if (ps_.skip_attributes()) continue;
      Tk kind = ps_.kind();
      if (kind == Tk::kw_consteval || kind == Tk::kw_constexpr) {
        // [dcl.spec]/2: at most one of them.
        if (flags & (kind == Tk::kw_consteval ? constexpr_ : consteval_)) {
          s_.diag(ps_.pos(), Diag::constexpr_specifiers);
          return std::nullopt;
        }
      }
      if (kind == Tk::kw_consteval) {
        if (destructor) {  // [dcl.constexpr]/2
          s_.diag(ps_.pos(), Diag::consteval_destructor);
          return std::nullopt;
        }
        if (flags & consteval_) s_.diag(ps_.pos(), Diag::duplicate_specifier, kind);
        flags |= consteval_ | inline_;  // [dcl.constexpr]/1: implicitly inline
        ps_.bump();
        continue;
      }
      if (kind == Tk::kw_static) {
        s_.diag(ps_.pos(), Diag::static_constructor);
        return std::nullopt;
      }
      if (kind == Tk::kw_explicit) {
        if (!explicit_at) {
          s_.diag(ps_.pos(), Diag::explicit_constructor_placement);
          return std::nullopt;
        }
        if (*explicit_at) {
          s_.diag(ps_.pos(), Diag::duplicate_specifier, kind);
          return std::nullopt;
        }
        *explicit_at = ps_.pos();
        ps_.bump();
        continue;
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

  bool constructor_exception_specifier(parse::Parser::Suffix& fs, QualType cls, bool& ok) {
    if (!ps_.is(Tk::kw_noexcept)) return false;
    s_.push_scope();
    for (const parse::Param& p : fs.params)
      if (p.name != s_.no_name()) s_.declare_local(p.name, p.type, sema::local_flag::parameter, p.at, false);
    const auto saved_this = std::exchange(s_.this_type, s_.types().pointer(cls));
    ps_.exception_specifier(fs, ok);
    const bool nonthrowing = ok && ps_.noexcept_value(fs.noexcept_condition, ok);
    s_.this_type = saved_this;
    s_.pop_scope();
    if (ps_.is(Tk::kw_noexcept) || ps_.is(Tk::kw_throw)) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      ok = false;
    }
    return nonthrowing;
  }

  // The declarator's tokens end at the body.
  bool finish_constructor_declarator() {
    ps_.skip_attributes();
    if (ps_.at_end()) return true;
    s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
    return false;
  }

  // A specialization being instantiated has its substituted base, dependent in the pattern or not.
  const sema::ClassRec* direct_polymorphic_base(EntityId cls) {
    std::vector<EntityId> bases = q_.defining_bases(cls);
    if (inst_)
      if (const auto own = p_->instantiating_bases_.find(cls); own != p_->instantiating_bases_.end()) bases = {own->second};
    const auto* base = bases.size() == 1 ? p_->classes_.find(bases.front()) : nullptr;
    return base && base->polymorphic ? base : nullptr;
  }

  // [class.virtual]: neither name hiding nor access cuts off overriding. In the supported
  // single-base chain the closest matching declaration already satisfies any earlier ones.
  const sema::MemberDeclRec* overridden_member(EntityId cls, const parse::Declarator& d) {
    const auto& types = s_.types();
    const bool conversion = s_.ins().names.spelling(d.name) == lex::conversion_function_name;
    for (const auto* base = direct_polymorphic_base(cls); base;
         base = base->bases.size() == 1 ? p_->classes_.find(base->bases.front().cls) : nullptr) {
      auto match = std::ranges::find_if(base->members, [&](const sema::MemberDeclRec& member) {
        return member.is_virtual && !member.is_dtor && member.name == d.name &&
               same_function_parameters(member.type, d.type) &&
               (!conversion || types.canonical(types.rec(member.type).inner) == types.canonical(types.rec(d.type).inner));
      });
      if (match != base->members.end()) return &*match;
    }
    return nullptr;
  }

  bool validate_override(const parse::Declarator& d, const sema::MemberDeclRec& overridden, bool immediate) {
    if (overridden.is_final) {
      s_.diag(d.name_at, Diag::virtual_override_final);
      return false;
    }
    if (immediate != overridden.is_consteval) {  // [class.virtual]
      s_.diag(d.name_at, Diag::virtual_override_consteval);
      return false;
    }
    const auto& types = s_.types();
    const auto derived = types.rec(d.type);
    const auto base = types.rec(overridden.type);
    // In a class template's definition, a dependent return type or condition is its specializations' to check.
    if (types.is_dependent(derived.inner) || (!inst_ && !s_.template_scopes.empty() && d.has_noexcept)) return true;
    if (types.canonical(derived.inner) != types.canonical(base.inner)) {
      s_.diag(d.name_at, Diag::virtual_override_return);
      return false;
    }
    if (base.fn_noexcept && !derived.fn_noexcept) {
      s_.diag(d.name_at, Diag::virtual_override_noexcept);
      return false;
    }
    return true;
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
    const bool specialized = !s_.template_scopes.empty() || inst_ || p_->items().items[it_->parent].specialization;
    const bool has_base = !q_.defining_bases(cls).empty() || (inst_ && p_->instantiating_bases_.contains(cls));
    if ((has_base && !direct_polymorphic_base(cls)) ||
        !p_->is_namespace(specialized ? s_.world().entities.rec(cls).scope : p_->outermost_class_scope(cls))) {
      s_.diag(at, Diag::not_supported, Tk::kw_virtual);
      return false;
    }
    return true;
  }

  // The condition of a virtual member's deferred noexcept-specifier, with the specialization's
  // arguments bound and the parameters in scope ([except.spec]).
  void read_deferred_noexcept(parse::Declarator& d) {
    const u32 resume = ps_.index();
    ps_.seek(d.noexcept_tokens->first);
    s_.push_scope();
    for (const parse::Param& p : d.params) s_.declare_local(p.name, p.type, sema::local_flag::parameter, 0, false);
    Parser::Suffix suffix;
    ps_.exception_specifier(suffix, d.ok);
    s_.pop_scope();
    ps_.seek(resume);
    d.noexcept_condition = suffix.noexcept_condition;
    d.noexcept_tokens.reset();
  }

  // In a class template's definition, a base or a signature can be dependent: each specialization
  // checks what its member overrides ([temp.res.general]).
  bool may_override_dependent_base(bool dependent_member) {
    if (inst_ || s_.template_scopes.empty()) return false;
    const auto base = p_->class_base_patterns_.find(it_->parent);
    return base != p_->class_base_patterns_.end() && (dependent_member || s_.types().is_dependent(base->second.base.type));
  }

  // The constructor and destructor path supports its existing override checks only.
  bool member_virt_specifiers(bool overrides, std::optional<u32>* final_at = nullptr, bool dependent_member = false) {
    bool written_override = false;
    while (ps_.is(Tk::identifier)) {
      const auto spelling = s_.ins().names.spelling(ps_.name_of(ps_.tok()));
      const bool is_final = spelling == "final";
      if (!is_final && spelling != "override") break;
      if (member_template_ || (is_final && !final_at)) {
        s_.diag(ps_.pos(), Diag::not_supported, Tk::identifier);
        return false;
      }
      if (is_final) {
        if (*final_at) {
          s_.diag(ps_.pos(), Diag::duplicate_final);
          return false;
        }
        *final_at = ps_.pos();
      } else {
        if ((!overrides && !may_override_dependent_base(dependent_member)) || written_override) {
          s_.diag(ps_.pos(), written_override ? Diag::duplicate_override : Diag::override_without_base);
          return false;
        }
        written_override = true;
      }
      ps_.bump();
    }
    return true;
  }

  // A constructor or destructor declared in class `c` ([class.ctor], [class.dtor]), possibly
  // deleted ([dcl.fct.def.delete]).
  void special_member(const ast::DeclSema& c, bool ctor, u16 flags, std::optional<u32> virtual_at, bool is_explicit) {
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
    const auto* base = ctor ? nullptr : direct_polymorphic_base(c.entity);
    const bool overrides = base && base->virtual_destructor;
    const bool is_virtual = virtual_at.has_value() || overrides;
    const bool defaulted = p_->defaulted_body(k_);
    // A dependent base's destructor can be virtual: the specialization decides what it defaults.
    const bool deferred_defaulting = !ctor && defaulted && may_override_dependent_base(false);
    const bool ordinary_defaulting = defaulted && !is_virtual && !deferred_defaulting;
    const bool defaulted_default = ordinary_defaulting && ctor && fs.params.empty() && !fs.variadic;
    const bool defaulted_dtor = ordinary_defaulting && !ctor;
    // BOOTSTRAP: -Wmaybe-uninitialized: a pair and a flag, not std::optional. At -O3 GCC 16.2
    // reports the optional's payload as possibly uninitialized at `*exception_tokens`, read
    // under `if (exception_tokens)` (#748). Without the bug:
    //   std::optional<std::pair<u32, u32>> exception_tokens;
    std::pair<u32, u32> exception_tokens{};
    bool has_exception_tokens = false;
    bool written_noexcept = false;
    if ((ctor || defaulted_dtor) && !member_template_ && s_.template_scopes.empty() && !inst_ &&
        ps_.is(Tk::kw_noexcept) && ps_.kind(1) == Tk::l_paren) {
      const u32 begin = ps_.index();
      ps_.bump();
      ps_.bump();
      unsigned depth = 1;
      while (!ps_.at_end() && depth) {
        if (ps_.is(Tk::l_paren)) ++depth;
        if (ps_.is(Tk::r_paren)) --depth;
        ps_.bump();
      }
      if (depth || ps_.is(Tk::kw_noexcept) || ps_.is(Tk::kw_throw)) {
        s_.diag(ps_.pos(), Diag::unexpected_token, ps_.kind());
        return bad();
      }
      fs.has_noexcept = true;
      exception_tokens = {begin, ps_.index()};
      has_exception_tokens = true;
    } else if (ctor) {
      written_noexcept = constructor_exception_specifier(fs, c.type, ok);
    }
    if (!ok) return bad();
    std::optional<bool> destructor_noexcept;
    if (!ctor) {
      if (is_virtual && !virtual_member_scope(c.entity, virtual_at.value_or(name_at))) return bad();
      if (fs.cv != sema::Cv::none || ps_.is(Tk::amp) || ps_.is(Tk::ampamp) || !fs.params.empty() || fs.variadic) {
        s_.diag(name_at, Diag::not_supported, Tk::tilde);
        return bad();
      }
      if (!has_exception_tokens) {
        ps_.exception_specifier(fs, ok);
        if (!ok) return bad();
        if (fs.has_noexcept) {
          destructor_noexcept = ps_.noexcept_value(fs.noexcept_condition, ok);
          if (!ok) return bad();
        }
      }
      ps_.skip_attributes();
      if (!member_virt_specifiers(overrides)) return bad();
      if (is_virtual && !ps_.at_end() && !ps_.is(Tk::semi)) {
        s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
        return bad();
      }
      if ((is_virtual || deferred_defaulting) && defaulted) flags |= sema::decl_flag::constexpr_ | sema::decl_flag::inline_;
      written_noexcept = (is_virtual || defaulted_dtor) && destructor_noexcept.value_or(false);
    }
    // A constructor's or prospective destructor's trailing requires-clause ([class.mem.general],
    // [class.dtor]/4), before its ctor-initializer or body.
    if (!member_requires_clause(fs.params)) return bad();
    if (ordinary_defaulting) {
      // [dcl.fct.def.default]: a first in-class defaulted special member. What its class makes of
      // it is decided when the class is complete (`defaulted_constructors`).
      if (!finish_constructor_declarator()) return bad();
      auto copy_reference = [&] {
        if (fs.params.size() != 1 || fs.variadic) return false;
        const parse::Param& p = fs.params.front();
        if (p.default_arg != NodeId::none || p.default_tokens) return false;
        const auto ref = types.rec(p.type);
        return ref.kind == sema::TypeKind::reference &&
               sema::Types::unqualified(types.canonical(ref.inner)) == types.canonical(c.type);
      };
      if (member_template_ || !(defaulted_dtor || defaulted_default || copy_reference())) {
        s_.diag(name_at, Diag::defaulted_copy_declaration);
        return bad();
      }
      if (!p_->is_namespace(p_->outermost_class_scope(c.entity))) {
        s_.diag(name_at, Diag::not_supported, Tk::kw_default);
        return bad();
      }
      // A first-declaration defaulted special member is inline and, when constexpr-suitable,
      // implicitly constexpr ([dcl.fct.def.default]).
      flags |= sema::decl_flag::constexpr_ | sema::decl_flag::inline_;
    } else if (ctor && (flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) && it_->has_body() &&
               !finish_constructor_declarator()) {
      return bad();
    }
    if (ctor && !member_virt_specifiers(false)) return bad();
    std::vector<QualType> ps;
    for (const parse::Param& p : fs.params) ps.push_back(p.type);
    // [except.spec]: a defaulted constructor's implicit specification is decided where it is
    // needed (`defaulted_nonthrowing`), a destructor's when its class is complete.
    const bool nonthrowing = fs.has_noexcept ? written_noexcept : defaulted_dtor;
    QualType type = types.function(s_.builtin(sema::Builtin::void_), ps, fs.variadic, LangLinkage::cxx, fs.cv,
                                   ast::RefQual::none, nonthrowing);
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
    sem.has_written_noexcept = fs.has_noexcept;
    sem.linkage = ast::Linkage::external;
    sem.flags = flags;
    sem.is_virtual = is_virtual;
    sem.is_explicit = is_explicit;
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
      sem.head_constraint = head_constraint_;
      sem.trailing_constraint = trailing_constraint_;
      e.constraints = constraints_fp_;
      sem.entity = s_.world().entities.entity(e);
    } else {
      sema::EntityRec e{ctor ? sema::EntityKind::constructor : sema::EntityKind::destructor, name, type,
                        LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
      e.constraints = member_constraints_fp();
      sem.constraints = trailing_requires_;
      sem.trailing_constraint = trailing_constraint_;
      sem.entity = s_.world().entities.entity(e);
    }
    // One declaration of each ([class.mem.general]/5). An exception specification cannot
    // distinguish otherwise identical special members ([over.load]).
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_) break;
      for (const ast::DeclSema& o : p_->world().ins.seqs.elements(earlier(m).decls)) {
        // Different trailing requires-clauses declare different functions ([basic.scope.scope]/4):
        // overloads, or prospective destructors ([class.dtor]/4).
        const bool same_constraints = o.entity != EntityId::none &&
                                      p_->world().entities.rec(o.entity).constraints == member_constraints_fp();
        const bool redeclared_constructor =
            ctor && !member_template_ && (o.flags & sema::decl_flag::constructor) &&
            !(o.flags & sema::decl_flag::template_) &&
            same_function_parameters(o.type, sem.type) && same_constraints;
        const bool redeclared_destructor = !ctor && (o.flags & sema::decl_flag::destructor) && same_constraints;
        if (o.entity == sem.entity || redeclared_constructor || redeclared_destructor) {
          s_.diag(name_at, Diag::redeclared_member, name);
          return bad();
        }
      }
    }
    if (has_exception_tokens) p_->special_member_exception_tokens_.emplace(sem.entity, exception_tokens);
    if (!ctor) p_->destructor_declarations_.emplace(sem.entity, Program::DestructorDeclaration{name_at, destructor_noexcept, defaulted});
    remember_member_defaults(sem.entity, fs.params);
    p_->declared(sem, k_);
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), {}, ConstId::none}, name_at, ps_.pos()));
    decls_.push_back(sem);
    if (it_->kind != ItemKind::function && !ps_.expect(Tk::semi)) bad();
  }

  // Require directly spelled scalar types for the first native virtual definitions. Canonical
  // types erase attributes and mode changes carried by aliases.
  bool plain_virtual_signature(const items::Item& source, u32 type_end, u32 name_at, bool check_exception) {
    const auto& region = p_->region();
    const auto tokens = std::span(region.tokens);
    const u32 end = source.has_body() ? source.body_begin : source.end;
    if (type_end <= source.begin || name_at + 1 >= end || tokens[name_at].kind != Tk::identifier ||
        tokens[name_at + 1].kind != Tk::l_paren ||
        layout_attribute(region, source.begin, end, LayoutAttributes::any)) return false;
    const auto scalar_specifier = [](Tk kind) {
      return Parser::simple_type_keyword(kind) || kind == Tk::kw_const || kind == Tk::kw_volatile;
    };
    const auto result = tokens.subspan(source.begin, type_end - source.begin);
    if (!std::ranges::all_of(result, [&](const lex::Tok& token) {
          return scalar_specifier(token.kind) || token.kind == Tk::kw_virtual;
        })) return false;
    u32 at = name_at + 2;
    while (at < end && tokens[at].kind != Tk::r_paren) {
      const u32 first = at;
      while (at < end && scalar_specifier(tokens[at].kind)) ++at;
      if (at == first || at == end) return false;
      if (tokens[at].kind == Tk::identifier) ++at;
      if (at == end || tokens[at].kind != Tk::comma) break;
      ++at;
    }
    if (at == end || tokens[at].kind != Tk::r_paren) return false;
    if (!check_exception) return true;
    ++at;
    while (at < end && (tokens[at].kind == Tk::kw_const || tokens[at].kind == Tk::kw_volatile)) ++at;
    // The general exception parser does not introduce parameter names in the condition (#219).
    // Require a scope-independent spelling on both declarations of the emitted function.
    if (at + 1 >= end || tokens[at].kind != Tk::kw_noexcept || tokens[at + 1].kind != Tk::l_paren)
      return true;
    return at + 3 < end && (tokens[at + 2].kind == Tk::kw_true || tokens[at + 2].kind == Tk::kw_false) &&
           tokens[at + 3].kind == Tk::r_paren;
  }

  bool native_virtual_definition(EntityId cls, const sema::ClassRec& record, EntityId function,
                                 const parse::Declarator& d, const ast::DeclSema& sem) {
    const auto slot = sema::native_virtual_root_slot(record, function);
    const bool derived = record.peer_automatic_primary_base != EntityId::none;
    // Check these key definitions as declarations. Native ownership of their RTTI remains unsupported.
    const bool borrowed_key = sema::borrowed_dynamic_cast_key(record, s_.types()) == function;
    if ((!derived && !borrowed_key && record.peer_virtual_layout != sema::PeerVirtualLayout::root) ||
        !slot || template_ || explicit_ || inst_ ||
        sem.storage != ast::Storage::none || (sem.flags & (sema::decl_flag::inline_ | sema::decl_flag::deleted |
                                                          sema::decl_flag::lang_c)) ||
        it_->kind != ItemKind::function || !it_->has_body() || !d.asm_label.empty()) return false;
    const bool key_function = *slot == 0;
    if (key_function && !derived && !borrowed_key && !sema::native_virtual_table_slots(record)) return false;
    const auto& region = p_->region();
    if (region.tokens[it_->body_begin].kind != Tk::l_brace ||
        !plain_virtual_signature(*it_, it_->begin + d.qualifier_at, it_->begin + d.name_at,
                                 /*check_exception=*/true)) return false;
    // The automatic derived layout certifies one written int() override. Its out-of-class
    // definition must also use exactly that prototype, without aliases, attributes or suffixes.
    if (derived && (!key_function || region.tokens[it_->begin].kind != Tk::kw_int || d.qualifier_at != 1 ||
                    it_->body_begin != it_->begin + d.name_at + 3)) return false;

    const auto& entity = s_.world().entities.rec(cls);
    auto defining = p_->class_items_.find(std::to_underlying(cls));
    if (entity.linkage != ast::Linkage::external || defining == p_->class_items_.end()) return false;
    const auto& head = p_->items().items[defining->second];
    const auto key = region.tokens[head.begin].kind;
    if (key != Tk::kw_struct && key != Tk::kw_class) return false;
    if (key_function) {
      if (region.first_visibility_pragma && *region.first_visibility_pragma < it_->body_begin) return false;
      const auto& declarations = p_->decls_.at(std::to_underlying(cls));
      if (p_->in_attributed_namespace(k_) || std::ranges::any_of(declarations, [&](const auto& decl) {
            return p_->in_attributed_namespace(decl.first);
          })) return false;
    }

    // Compare the written qualifier with the owning namespace and class to exclude aliases.
    const auto& owner = p_->ns_of(defining->second);
    const auto& here = p_->ns_of(k_);
    if (!Program::encloses(here, owner)) return false;
    std::string full = owner.empty() ? "" : owner + "::";
    full += s_.ins().names.spelling(entity.name);
    full += "::";
    std::string written;
    for (u32 at = it_->begin + d.qualifier_at; at < it_->begin + d.name_at; ++at) {
      const auto token = region.tokens[at];
      if (token.kind == Tk::identifier) written += region.name(token);
      else if (token.kind == Tk::coloncolon) written += "::";
      else return false;
    }
    const auto relative = std::string_view(full).substr(here.empty() ? 0 : here.size() + 2);
    if (written != relative && written != full && written != "::" + full) return false;
    if (std::ranges::any_of(head.members, [&](u32 member) {
          return region.tokens[p_->items().items[member].begin].kind == Tk::kw_using;
        })) return false;
    // A skipped attribute or an alias in an earlier virtual declaration can change the ABI
    // evidence used to identify the key function and the table entries.
    for (const auto& member : record.members) {
      if (!member.is_virtual) continue;
      const auto decl = p_->member_decl(member.entity);
      if (!decl) return false;
      const auto& source = p_->items().items[p_->index(decl->item)];
      if (source.kind != ItemKind::declaration || source.names.size() != 1) return false;
      const u32 name_at = source.begin + source.names.front().offset;
      if (!plain_virtual_signature(source, name_at, name_at, key_function || member.entity == function)) return false;
    }
    return true;
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
    QualType own = s_.templates().resolve_dep_names(
        sem.type, [&](QualType q, NameId n, std::optional<std::span<const ast::TemplateArg>> args) -> std::optional<QualType> {
          if (!s_.types().is_class(q) || s_.types().class_of(q) != cls) return std::nullopt;
          if (args) {  // `typename C<U>::template M<args>`: the class's member alias template M
            for (const auto& [name, mt, me] : cr->alias_templates)
              if (name == n) return s_.templates().apply_template(me, *args);
            return std::nullopt;
          }
          for (const auto& [name, mt, me] : cr->types)
            if (name == n) return mt;
          return std::nullopt;
        });
    if (s_.types().canonical(own) != s_.types().canonical(sem.type)) sem.type = own;
    const bool deferred_class_exception = fn && !member_template_ && d.has_noexcept && s_.templated() &&
                                          s_.world().entities.rec(cls).templ != EntityId::none;
    const sema::MemberDeclRec* m = nullptr;
    QualType matched_type = sem.type;
    for (const sema::MemberDeclRec& r : cr->members) {
      if (r.name != d.name || r.is_function != fn || !same_template(r, member_template_)) continue;
      auto& types = s_.types();
      QualType candidate_type = sem.type;
      // A bare in-class specification already gives the member a nonthrowing type. A written
      // specification on this templated definition still uses the deferred declarator path.
      if (deferred_class_exception && !r.is_virtual && types.rec(r.type).fn_noexcept) {
        const auto f = types.rec(candidate_type);
        candidate_type = types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, true);
      }
      if (types.canonical(r.type) == types.canonical(candidate_type) ||
          (r.is_virtual && same_function_parameters(r.type, sem.type) &&
           types.canonical(types.rec(r.type).inner) == types.canonical(types.rec(sem.type).inner))) {
        if (candidate_type != sem.type && d.noexcept_condition) {
          // [except.spec]: a condition must agree with the earlier bare specification.
          bool ok = true;
          const bool nonthrowing = !s_.dependent(*d.noexcept_condition) && ps_.noexcept_value(d.noexcept_condition, ok);
          if (!ok) return false;
          if (!nonthrowing) {
            s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
            return false;
          }
        }
        m = &r;
        matched_type = candidate_type;
      }
    }
    if (!m || (!fn && !m->is_static)) {
      s_.diag(d.name_at, Diag::no_member_declared, d.name, *t);
      return false;
    }
    auto md = p_->member_decl(m->entity);
    auto mem = md ? p_->decl_sema(*md) : std::nullopt;
    if (!mem) return false;
    sem.type = matched_type;
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
      if (m->is_consteval != spec.is_consteval) {  // [dcl.constexpr]
        s_.diag(d.name_at, Diag::consteval_redeclared, d.name);
        return false;
      }
      const bool pattern = !inst_ && s_.types().is_dependent(s_.types().class_type(cls));
      if (!cr->dynamic && !pattern && !native_virtual_definition(cls, *cr, m->entity, d, sem)) {
        s_.diag(d.name_at, Diag::virtual_definition);
        return false;
      }
      // Ordinary named declarators defer exception checks. This definition must agree with the
      // in-class virtual declaration, including its already checked exception specification.
      auto& types = s_.types();
      if (d.has_noexcept) {
        bool ok = true;
        bool dependent = pattern && d.noexcept_tokens;
        const bool nonthrowing = !dependent && ps_.noexcept_value(d.noexcept_condition, ok, pattern ? &dependent : nullptr);
        if (!ok) return false;
        const auto f = types.rec(sem.type);
        sem.type = types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, nonthrowing && !dependent);
      }
      if (types.canonical(sem.type) != types.canonical(m->type)) {
        s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
        return false;
      }
      sem.is_virtual = true;
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

  bool attach_function_annotations(ast::DeclSema& declaration, const DeclSpec& spec,
                                   const parse::Declarator& d, bool supported) {
    if (spec.annotations.empty() && d.annotations.empty()) return true;
    const u32 at = (spec.annotations.empty() ? d.annotations : spec.annotations).front().offset;
    if (!supported) {
      s_.diag(at, Diag::annotation_not_supported);
      return false;
    }
    std::vector<ast::AnnotationSema> occurrences = spec.annotations;
    occurrences.insert(occurrences.end(), d.annotations.begin(), d.annotations.end());
    declaration.annotations = b_.seq(std::span<const ast::AnnotationSema>(occurrences));
    return true;
  }

  // [dcl.init.aggr]/1, [dcl.constexpr]: the supported direct-list static initialization has
  // only flat, public fields whose values fit the static integer representation.
  bool supports_static_constexpr_aggregate(QualType type) {
    auto& types = s_.types();
    if (!types.is_class(type) || types.is_dependent(type)) return false;
    const auto* record = s_.class_rec(types.class_of(type));
    return record && !record->union_ && !record->closure && !record->user_ctors &&
           !record->layout_requires_virtual_support && !record->unapplied_layout && record->bases.empty() &&
           record->destructor_usable && record->destructor == EntityId::none &&
           std::ranges::all_of(record->fields, [&](const sema::FieldRec& field) {
             const QualType field_type = types.canonical(field.type);
             return record->access_of(field.entity) == ast::Access::public_ && field.bit_width == 0 &&
                    !sema::Types::is_volatile(field_type) && types.is_integral(field_type) &&
                    sema::shape(s_.world().target, *types.builtin_of(field_type)).bits <= 64;
           });
  }

  // A member function or static data member of class `c` declared in its class ([class.mfct],
  // [class.static]).
  bool member_function_or_static(const DeclSpec& spec, parse::Declarator& d, ast::DeclSema c, bool is_virtual,
                                 bool is_final, const sema::MemberDeclRec* overridden) {
    sema::Types& types = s_.types();
    bool fn = types.is_function(d.type);
    if (fn && !member_operator_ok(d, spec.is_static)) return false;
    if (!defaults_allowed(d.params, fn, d.name)) return false;
    if (fn && !member_requires_clause(d.params)) return false;
    bool is_def = it_->kind == ItemKind::function;
    const bool assignment = fn && s_.ins().names.spelling(d.name) == "operator=";
    const bool defaulted = assignment && p_->defaulted_body(k_);
    const bool deleted = assignment && plain_deleted_body();
    // [class.compare.default]: `bool operator==(const C&) const = default;`, and `<=>` likewise.
    const bool comparison = fn &&
                            (s_.ins().names.spelling(d.name) == "operator==" || s_.ins().names.spelling(d.name) == "operator<=>") &&
                            p_->defaulted_body(k_);
    if (comparison) {
      if (spec.is_static || is_virtual || member_template_ || !s_.template_scopes.empty() || inst_) {
        s_.diag(d.name_at, Diag::not_supported, Tk::kw_default);
        return false;
      }
      if (!defaulted_comparison_ok(d, c.type, true)) return false;
    }
    if (defaulted || deleted) {
      using sema::TypeKind;
      const auto parameter = p_->assignment_parameter({.name = d.name, .type = d.type, .is_function = true,
                                                       .is_static = spec.is_static, .is_template = member_template_,
                                                       .defaults = {}}, c.type);
      const auto& owner = s_.world().entities.rec(c.entity);
      const bool supported_class = p_->is_namespace(p_->outermost_class_scope(c.entity)) &&
                                   (defaulted || (s_.template_scopes.empty() && !inst_) || owner.templ != EntityId::none);
      if (!supported_class || (!defaulted && (!parameter || types.rec(*parameter).rvalue_ref)) ||
          (defaulted && (is_virtual || spec.is_consteval))) {
        s_.diag(d.name_at, Diag::not_supported, defaulted ? Tk::kw_default : Tk::kw_delete);
        return false;
      }
      if (defaulted) {
        if (!finish_constructor_declarator()) return false;  // a requires-clause
        // [dcl.fct.def.default]: another parameter or return type is ill-formed. Other differences
        // from the implicit declaration make the operator deleted (`defaulted_assignments`).
        const auto f = types.rec(d.type);
        if (!parameter || types.rec(*parameter).kind != TypeKind::reference ||
            types.canonical(f.inner) != types.lvalue_ref(c.type)) {
          s_.diag(d.name_at, Diag::defaulted_copy_assignment_declaration);
          return false;
        }
      }
    }
    ast::DeclSema sem = class_decl(c.entity, false);
    if (!attach_function_annotations(sem, spec, d, fn && !member_template_ && !inst_ &&
                                     s_.template_scopes.empty())) return false;
    sem.type = d.type;
    sem.has_written_noexcept = fn && d.has_noexcept;
    sem.has_trailing_return = fn && d.trailing_return;
    sem.linkage = ast::Linkage::external;
    sem.is_virtual = is_virtual;
    sem.is_final = is_final;
    u16 flags = sema::decl_flag::member;
    if (spec.is_static) flags |= sema::decl_flag::static_;
    if (spec.is_constexpr || defaulted || (comparison && !spec.is_consteval)) flags |= sema::decl_flag::constexpr_;
    if (spec.is_consteval) flags |= sema::decl_flag::consteval_;
    if (deleted) flags |= sema::decl_flag::deleted;
    if (spec.is_inline || spec.is_constexpr || spec.is_consteval || is_def) flags |= sema::decl_flag::inline_;
    if (member_declared(d.name, fn ? std::optional(d.type) : std::nullopt, assignment || sem.is_virtual, spec.is_static)) {
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
        if (!is_virtual || is_def || assignment || ps_.kind(1) == Tk::kw_default || ps_.kind(1) == Tk::kw_delete) {
          s_.diag(ps_.pos(), Diag::not_supported, Tk::equal);
          return false;
        }
        // [class.abstract], [class.mem.general]: the post-expansion token must be exactly `0`.
        if (ps_.kind(1) != Tk::int_lit || p_->region().spelling(ps_.tok(1)) != "0" ||
            (ps_.kind(2) != Tk::comma && ps_.kind(2) != Tk::semi)) {
          s_.diag(ps_.pos(), Diag::pure_specifier);
          return false;
        }
        ps_.bump();
        ps_.bump();
        sem.is_pure = true;
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
        sem.head_constraint = head_constraint_;
        sem.trailing_constraint = trailing_constraint_;
        e.constraints = constraints_fp_;
        sem.entity = s_.world().entities.entity(e);
      } else {
        // A trailing requires-clause is part of its identity ([basic.scope.scope]/4) and of its
        // viability ([over.match.viable]/3).
        sema::EntityRec e{sema::EntityKind::function, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
        e.constraints = member_constraints_fp();
        sem.constraints = trailing_requires_;
        sem.trailing_constraint = trailing_constraint_;
        sem.entity = s_.world().entities.entity(e);
      }
      if (d.noexcept_tokens || (member_template_ && d.has_noexcept && !d.noexcept_condition))
        p_->member_exceptions_.emplace(sem.entity, Program::MemberException{k_, d.noexcept_tokens});
      if (overridden) p_->overridden_virtuals_.emplace(sem.entity, overridden->entity);
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
      const sema::Sema::ImmediateReferenceScope references(s_, spec.is_constexpr);
      const auto& owner = s_.world().entities.rec(c.entity);
      const sema::Sema::ClassPrefixScope initializer(s_,
          !s_.templated() && owner.templ == EntityId::none ? c.entity : EntityId::none, d.name);
      if (spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);
      const sema::Sema::StaticConstexprInitializerScope constexpr_initializer(s_, spec.is_constexpr);
      // The member owns the closure types of its initializer's lambdas; its identity needs no type.
      const sema::EntityRec member{sema::EntityKind::variable, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
      const sema::Sema::ClosureOwnerScope closures(
          s_, sema::Sema::ClosureOwner{s_.world().entities.reserve(member),
                                       owner.linkage == ast::Linkage::internal || Program::unnamed(p_->ns_of(k_))
                                           ? ast::Linkage::internal : ast::Linkage::external});
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
        // An inline static member defines an object ([class.static.data]).
        if (!s_.require_complete(d.type, d.name_at) || !s_.require_concrete_object(d.type, d.name_at) ||
            !s_.require_supported_virtual_layout(d.type, d.name_at) ||
            !s_.check_destructor_access(d.type, d.name_at)) return false;
        flags |= sema::decl_flag::definition;
      }
      sem.entity = s_.world().entities.entity(
          {sema::EntityKind::variable, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity});
      sem.flags = flags;
      // The written list forms here apply to base-free namespace-scope owners.
      const bool braced = ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace);
      bool list_initialization = false;
      if (braced && (spec.is_inline || spec.is_constexpr) && s_.template_scopes.empty() && !inst_ &&
          p_->is_namespace(p_->outermost_class_scope(c.entity)) && q_.defining_base_records(c.entity).empty()) {
        const QualType element = types.is_array(d.type) ? types.rec(d.type).inner : d.type;
        const bool scalar = types.is_integral(element) || types.is_pointer(element) ||
                            (types.is_standard_floating(element) &&
                             ast::computes(ast::float_format(s_.ins(), s_.world().target, element)));
        list_initialization = scalar ||
                              (spec.is_constexpr && ps_.is(Tk::l_brace) && supports_static_constexpr_aggregate(d.type));
      }
      auto declare_member_name = [&] {
        s_.own.push_back({d.name, sem});
        if (!member_value_) p_->declared(sem, k_);
      };
      // [basic.scope.pdecl]: the member's name and type are visible in its initializer.
      const bool visible_in_initializer = list_initialization || !s_.class_prefix_bases(c.entity).empty();
      if (visible_in_initializer) declare_member_name();
      // A member whose initializer is in error is still declared, so its uses say nothing more.
      const u32 ordinal = u32(decls_.size());
      auto declare_member = [&](ConstId v, const Ex& e) {
        if (!visible_in_initializer) declare_member_name();
        declarators_.push_back(s_.node(ast::Declarator{ordinal, sema::Sema::at(e, d.begin), v}, d.begin, ps_.pos()));
        decls_.push_back(sem);
      };
      // [temp.inst]/3: a class template specialization declares its static data members; the
      // initializer of one defined in the class (inline or constexpr) is instantiated when the
      // member is used (`Program::member_value`), from the extent the template's analysis recorded.
      if (inst_ && !early && (spec.is_inline || spec.is_constexpr) && ps_.is(Tk::equal) && member_value_ != ordinal) {
        if (auto initializer = p_->static_initializer(k_, ordinal)) {
          ps_.seek(initializer->end);
          p_->lazy_member(sem.entity, k_, ordinal, *inst_, initializer->dependent_layout);
          declare_member(ConstId::none, {});
          return true;
        }
      }
      if (list_initialization || early || ps_.accept(Tk::equal)) {
        const AtItem::RestoreLayoutMemberDemand restore{q_};
        if (member_value_ == ordinal) q_.layout_member_demand = member_layout_demand_;
        u32 at;
        if (list_initialization) {
          const auto form = ps_.accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
          at = ps_.pos();
          init = ps_.member_initializer(d.type, true, form);
        } else {
          using Replay = Parser::TemplateArgumentReplay;
          const Parser::TemplateArgumentReplayScope replay(ps_, s_.templated() && !early &&
              (spec.is_inline || spec.is_constexpr) ? Replay::static_initializer : Replay::none);
          const auto layout_arguments = ps_.dependent_layout_arguments();
          Ex e = early ? *early : ps_.assignment_expression();
          if (!inst_)
            p_->set_static_initializer(k_, ordinal, {ps_.index(), ps_.dependent_layout_arguments() != layout_arguments});
          at = e.begin;
          init = s_.convert(e, d.type, at);
        }
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
          s_.diag(at, Diag::not_supported, Tk::equal);
          declare_member(ConstId::none, {});
          return false;
        }
        // Scalar lists and aggregate elements use the same constant initializer path ([dcl.init.list]).
        // The general evaluator needs class result storage to return a class value (#25).
        sema::Evaluator evaluator(s_, q_);
        const bool aggregate = spec.is_constexpr && types.is_class(d.type) && (s_.as<ast::InitList>(init) || s_.as<ast::Lambda>(init));
        sema::Evaluated v = list_initialization || aggregate ? evaluator.evaluate_initializer(init) : evaluator.evaluate(init);
        if (!v.value) {
          s_.diag(at, usable ? Diag::not_constant : Diag::nonconstant_initializer, v.reason);
          declare_member(ConstId::none, {});
          return false;
        }
        value = *v.value;
        if (usable) p_->set_constant(sem.entity, value, k_);
        if (member_value_ == ordinal) {
          member_initial_value_ = true;
          p_->set_member_init(sem.entity, value);
        }
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
  // A union cannot have a reference member ([class.union.general]). Completing a named
  // union checks whether it has a written destructor for nontrivial variants.
  bool variant_member(const parse::Declarator& d) {
    sema::Types& types = s_.types();
    if (types.is_reference(d.type)) {
      s_.diag(d.name_at, Diag::union_reference, d.name);
      return false;
    }
    QualType et = d.type;
    while (types.is_array(et)) et = types.rec(et).inner;
    const bool supported_scope = inst_
        ? p_->is_namespace(s_.world().entities.rec(inst_->spec).scope)
        : s_.template_scopes.empty();
    const bool named = p_->items().items[it_->parent].unnamed_type_ordinal == items::none;
    const bool nontrivial_destruction = s_.destructor_of(et) != EntityId::none;
    if (q_.classes().copy_of(et).first ||
        (!(supported_scope && named && nontrivial_destruction) && (nontrivial_destruction || s_.has_member_inits(et)))) {
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

  bool member_declared(NameId n, std::optional<QualType> function = std::nullopt, bool compare_parameters = false,
                       bool is_static = false) {
    // Functions clash when they have the same type and are both templates with equivalent heads and
    // constraints, or both not templates ([temp.over.link]/4-6, [over.load]).
    auto clash = [&](const ast::DeclSema& d) {
      if (!function || d.kind != ast::DeclKind::function) return true;
      auto& types = s_.types();
      if (types.canonical(d.type) != types.canonical(*function)) {
        const auto previous = types.rec(d.type);
        const auto current = types.rec(*function);
        if (s_.ins().names.spelling(n) == lex::conversion_function_name) {
          // The conversion result and implicit object distinguish overloads.
          // Exception specifications do not distinguish overloads.
          if (types.canonical(previous.inner) != types.canonical(current.inner) ||
              previous.fn_cv != current.fn_cv)
            return false;
        } else {
          const bool ref_qualified = previous.fn_ref != ast::RefQual::none || current.fn_ref != ast::RefQual::none;
          const bool compare_object = !is_static && !(d.flags & sema::decl_flag::static_);
          if (!(compare_parameters || d.is_virtual || ref_qualified) ||
              !same_function_parameters(d.type, *function, false, compare_object))
            return false;
        }
        if (previous.fn_ref != current.fn_ref) {
          if (previous.fn_ref != ast::RefQual::none && current.fn_ref != ast::RefQual::none) return false;
          // GCC distinguishes the unqualified and ref-qualified implicit objects too.
          if (s_.compatibility() == ast::Compatibility::gcc && !is_static && !(d.flags & sema::decl_flag::static_))
            return false;
        }
      }
      bool templ = (d.flags & sema::decl_flag::template_) != 0;
      if (templ != member_template_) return false;
      const sema::EntityRec& e = p_->world().entities.rec(d.entity);
      // Non-template functions with different trailing requires-clauses do not correspond
      // ([basic.scope.scope]/4).
      if (!templ) return e.constraints == member_constraints_fp();
      return e.head == head_fp(s_.template_scopes.back()) && e.constraints == constraints_fp_;
    };
    for (const auto& o : s_.own)
      if (o.name == n && clash(o.sema)) return true;
    for (u32 m : p_->items().items[it_->parent].members) {
      if (m == k_) break;
      const ast::ItemSema& ms = earlier(m);
      for (const ast::DeclSema& d : p_->world().ins.seqs.elements(ms.decls))
        if (d.entity != EntityId::none && d.kind != ast::DeclKind::class_ && p_->world().entities.rec(d.entity).name == n &&
            !p_->scoped_enumerator(d) && clash(d))
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

  ast::DeclSema empty_declaration() {
    ast::DeclSema d = invalid_decl();
    d.type = s_.builtin(sema::Builtin::void_);  // an interned type, for the record's fingerprint
    d.attributes = b_.seq(std::span<const NodeId>{});
    d.params = b_.seq(std::span<const ast::ParamSema>{});
    d.template_params = b_.seq(std::span<const ast::TemplateParamSema>{});
    return d;
  }

  // The rest of the skeleton's names get empty records, so ordinals stay aligned.
  void bad() {
    while (decls_.size() < it_->names.size()) decls_.push_back(empty_declaration());
  }

  bool allocation_declaration_ok(const DeclSpec& spec, const parse::Declarator& d, LangLinkage lang,
                                 std::string_view name) {
    auto unsupported = [&] {
      s_.diag(d.name_at, Diag::allocation_declaration, d.name);
      return false;
    };
    if (!d.function || d.qualified || ns_entity() != EntityId::none || lang != LangLinkage::cxx || template_ ||
        explicit_ || spec.is_static || spec.is_typedef || spec.is_consteval || deleted_body() || !d.asm_label.empty())
      return unsupported();

    auto& world = s_.world();
    if (world.matches_reserved_placement_function(name, d.type)) return defaults_allowed(d.params, true, d.name);
    if (spec.is_inline || spec.is_constexpr) return unsupported();
    if (!world.matches_tagged_allocation_function(name, d.type, d.has_noexcept)) {
      if (!world.matches_allocation_function(name, d.type)) return unsupported();
      return defaults_allowed(d.params, true, d.name);
    }
    // [over.oper.general]/5 and [basic.stc.dynamic.allocation]/1: a tagged overload can use
    // ordinary default arguments, with the sole restriction on the allocation size parameter.
    if (sema::is_allocating_name(name) && !d.params.empty()) {
      const auto& first = d.params.front();
      if (first.default_arg != NodeId::none || first.default_tokens) {
        s_.diag(first.at, Diag::default_arg_not_allowed);
        return false;
      }
    }
    return true;
  }

  // The early bound needs the scalar clause count. A pack can change that count, and naming
  // the primary needs lookup at its declaration point, which this early parse cannot supply.
  bool fixed_scalar_array_initializer(std::string_view declarator_name) {
    u32 depth = 1;  // the opening brace
    for (u32 ahead = 1; depth != 0; ++ahead) {
      const Tk token = ps_.kind(ahead);
      if (token == Tk::ellipsis || token == Tk::eof ||
          (token == Tk::identifier && p_->region().name(ps_.tok(ahead)) == declarator_name))
        return false;
      // Array clauses cannot have designators. Keep their usual diagnostics before replay.
      if (depth == 1 && token == Tk::period &&
          (ps_.kind(ahead - 1) == Tk::l_brace || ps_.kind(ahead - 1) == Tk::comma))
        return false;
      if (token == Tk::l_brace) ++depth;
      if (token == Tk::r_brace) --depth;
    }
    return true;
  }

  // One init-declarator at namespace scope; false after an error that leaves no record.
  bool declare(const DeclSpec& spec, parse::Declarator d, LangLinkage lang, bool explicit_lang,
               bool is_function_def, bool ordinary_friend = false) {
    sema::Types& types = s_.types();
    const std::string_view name = s_.ins().names.spelling(d.name);
    const bool allocation = sema::is_allocation_name(name);
    if (allocation ? !allocation_declaration_ok(spec, d, lang, name)
                   : !defaults_allowed(d.params, types.is_function(d.type) && !spec.is_typedef, d.name))
      return false;
    if (literal_operator_named(d.name) && !literal_operator_ok(spec, d, lang, template_)) return false;
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
    sema::Sema::ImmediateReferenceScope references(
        s_, (spec.is_constexpr || spec.is_constinit) && !types.is_function(d.type),
        /*trial=*/!types.is_function(d.type));
    u32 ord = static_cast<u32>(decls_.size());
    // [dcl.link]: a single declaration is treated as `extern` for name linkage and definition.
    const bool effective_extern = spec.is_extern || single_linkage_spec_;
    // `T a[] = {…}`: the bound from the list ([dcl.array]/8), parsed here and used below.
    std::unique_ptr<sema::Braced> early;
    const bool fn = types.is_function(d.type);
    // [basic.link]/3–4: static, certain const declarations, and names in unnamed namespaces.
    const auto has_internal_linkage = [&] {
      return spec.is_static || Program::unnamed(p_->ns_of(k_)) ||
             (!fn && sema::Types::is_const(d.type) && !effective_extern && !spec.is_inline && !template_ && !explicit_);
    };
    const auto declaration_entity = [&](sema::EntityKind kind, QualType type, ast::Linkage linkage, sema::Fp128 head) {
      sema::EntityRec record{kind, d.name, type, lang, linkage,
          {p_->id(k_), ast::make_decl_ord(ast::DeclClass::syntactic, ord)},
          lang == LangLinkage::c ? EntityId::none : ns_entity()};
      record.head = head;
      record.constraints = constraints_fp_;
      return record;
    };
    // What owns the closure types of the initializer's lambdas, where the initializer is read before
    // the variable is declared: an identity needs no type (docs/design/closures.md, decision 3). A
    // variable template's and a specialization's are theirs. Where the final identity differs (a const
    // array without a bound, a deduced const type, an explicit specialization), the closure keeps
    // this one, with internal linkage.
    const auto initializer_owner = [&]() -> std::optional<sema::Sema::ClosureOwner> {
      if (fn) return {};
      if (var_) return variable_closures(var_->spec, true);
      if (d.qualified || d.name == s_.no_name()) return {};
      // A partial specialization's are its definition's only.
      if (template_ && d.template_args) return constraint_owner();
      const bool internal = explicit_ || has_internal_linkage() || (spec.is_constexpr && !effective_extern && !spec.is_inline && !template_);
      const auto linkage = internal ? ast::Linkage::internal : ast::Linkage::external;
      const auto record = template_
          ? declaration_entity(sema::EntityKind::variable_template, d.type, linkage, head_fp(s_.template_scopes.front()))
          : declaration_entity(sema::EntityKind::variable, d.type, linkage, head_fp({}));
      const bool shared = linkage == ast::Linkage::external && (spec.is_inline || template_);
      return sema::Sema::ClosureOwner{s_.world().entities.reserve(record),
                                      shared ? ast::Linkage::external : ast::Linkage::internal};
    };
    if (d.unknown_bound && ps_.is(Tk::equal) && ps_.kind(1) == Tk::string_lit) {
      // `char s[] = "…"`: the bound from the literal, null included ([dcl.init.string]/1); the
      // initialization below checks the element type.
      d.type = types.array(d.type, ps_.literal_at(1).units.size() + 1);
    } else if (d.unknown_bound && !(ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
      // `extern T a[];`: an incomplete type ([dcl.array]/6); a definition needs a bound.
      d.type = types.array_unknown(d.type);
      if (!effective_extern) {
        s_.diag(d.name_at, Diag::incomplete_type, d.type);
        return false;
      }
    } else if (d.unknown_bound) {
      ps_.accept(Tk::equal);
      const bool replay = template_ && !var_ && !explicit_ && !d.qualified && !d.template_args &&
                          types.is_scalar(d.type) && !types.is_dependent(d.type) && !types.has_placeholder(d.type) &&
                          fixed_scalar_array_initializer(name);
      using Replay = Parser::TemplateArgumentReplay;
      const Parser::TemplateArgumentReplayScope initializer(ps_, replay ? Replay::variable_initializer : Replay::none);
      const sema::Sema::ClosureOwnerScope closures(s_, initializer_owner());
      early = ps_.braced_init_list();
      u64 n = s_.deduce_bound(d.type, *early);
      if (n == 0) {
        s_.diag(early->begin, Diag::bound_not_constant);
        return false;
      }
      d.type = types.array(d.type, n);
    }
    if (!fn && spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);  // [dcl.constexpr]
    // Use of a placeholder's own name in a lambda that begins its initializer ([dcl.spec.auto.general]).
    if (!fn && types.kind(d.type) == sema::TypeKind::placeholder && !template_ && !var_ && !explicit_ &&
        !d.qualified && d.name != s_.no_name() && (ps_.is(Tk::equal) || ps_.is(Tk::l_brace))) {
      u32 start = 1;
      while (ps_.kind(start) == Tk::l_paren) ++start;
      if (ps_.kind(start) == Tk::l_square && ps_.kind(start + 1) != Tk::l_square)
        s_.namespace_lambda = sema::Sema::NamespaceLambda{ns_entity(), d.name};
    }
    // A variable template retains its initializer's pattern ([temp.res]). The placeholder path
    // reads the initializer before publishing the declaration ([dcl.type.auto.deduct]).
    const bool pattern = template_ && !var_ && !fn;
    using Replay = Parser::TemplateArgumentReplay;
    const Parser::TemplateArgumentReplayScope initializer(ps_, (pattern || var_) && !(d.qualified && d.scope.type)
        ? Replay::variable_initializer : Replay::none);
    AtItem::RestoreTypeTraitPattern restore{q_};
    q_.retain_type_trait_pattern = pattern;
    std::optional<Ex> early_expr;
    if (!fn && types.has_placeholder(d.type)) {
      const sema::Sema::ClosureOwnerScope closures(s_, initializer_owner());
      const bool deduced = deduce_variable(d, early, early_expr, pattern);
      s_.namespace_lambda.reset();
      if (!deduced) return false;
    }
    ast::DeclSema sem{};
    sem.kind = fn ? ast::DeclKind::function : ast::DeclKind::variable;
    sem.type = d.type;
    sem.has_written_noexcept = fn && d.has_noexcept;
    sem.has_trailing_return = fn && d.trailing_return;
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
    bool definition = fn ? is_function_def || deleted : !(effective_extern && !has_init);
    if (!fn && definition &&
        (!s_.require_concrete_object(d.type, d.name_at) || !s_.require_supported_virtual_layout(d.type, d.name_at)))
      return false;
    if (!attach_function_annotations(sem, spec, d, fn && !template_ && !explicit_)) return false;
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
          if (!same_target && (lang != LangLinkage::c || ds->kind == ast::DeclKind::enumerator || (ordinary_function && !prior_c)))
            continue;
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
      // A member's definition repeats its trailing requires-clause ([temp.over.link]/6).
      if (class_member && fn && !member_requires_clause(d.params)) return false;
      if (class_member && fn && !member_template_) {
        sem.constraints = trailing_requires_;
        sem.trailing_constraint = trailing_constraint_;
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
        if (s_.declared_template_param(d.name)) {
          s_.diag(d.name_at, Diag::redeclared_local, d.name);
          return false;
        }
        for (const parse::Param& prm : d.params)
          if (prm.name != s_.no_name() && s_.declared_template_param(prm.name)) {
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
        sem.head_constraint = head_constraint_;
        sem.trailing_constraint = trailing_constraint_;
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
        if (fn && s_.compatibility() == ast::Compatibility::spec) {
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
        if ((p.kind == ast::DeclKind::function) != fn || p.kind == ast::DeclKind::type_alias || p.kind == ast::DeclKind::enumerator) {
          s_.diag(d.name_at, Diag::conflicting_declaration, d.name);
          return false;
        }
        const bool canonical_match = fn && (allocation || ordinary_friend ||
                                            (p.flags & (sema::decl_flag::friend_ | sema::decl_flag::lang_c)) ||
                                            lang == LangLinkage::c);
        const QualType prior_type = canonical_match ? types.canonical(p.type) : p.type;
        const QualType current_type = canonical_match ? types.canonical(sem.type) : sem.type;
        if (fn && prior_type != current_type) {
          const sema::TypeRec& a = types.rec(prior_type);
          const sema::TypeRec& c = types.rec(current_type);
          if (a.params == c.params && a.variadic == c.variadic && a.inner == c.inner &&
              a.fn_noexcept == c.fn_noexcept)
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
      // A function template's dependent exception specification, replayed for each specialization
      // when it is needed ([except.spec]/13).
      if (fn && template_ && (d.noexcept_tokens || (d.has_noexcept && !d.noexcept_condition)) &&
          p_->member_exceptions_.emplace(sem.entity, Program::MemberException{k_, d.noexcept_tokens}).second &&
          d.noexcept_tokens)
        p_->check_function_exception_pattern(sem.entity, s_.template_scopes.front(), params, k_, s_);
      if (fn) {  // for a later declarator of this declaration (`merge_defaults`)
        std::vector<NodeId>& own = s_.own_function_defaults[sem.entity];
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
    const auto visibility = std::max(spec.attributes.visibility, d.attributes.visibility);
    const bool ordinary_external = !template_ && !explicit_ && !ordinary_friend &&
                                   sem.linkage == ast::Linkage::external && (!d.qualified || d.scope.ns);
    if (ordinary_external && visibility == ast::GnuVisibility::hidden &&
        (fn || (!var_ && !(sem.flags & sema::decl_flag::inline_))))
      sem.hidden_visibility = true;
    if (ordinary_external && !(sem.flags & (sema::decl_flag::inline_ | sema::decl_flag::member))) {
      const bool object_reference = !fn && !var_ && !definition && !reference &&
                                    p_->world().target.triple == ast::x86_64_linux.triple;
      if ((fn || object_reference) && (spec.attributes.weak || d.attributes.weak)) sem.explicit_weak = true;
      if (fn) {
        sem.explicit_default_visibility = visibility == ast::GnuVisibility::default_;
        // A qualified definition uses the namespace facts from its resolved declaration.
        if (!d.qualified) sem.namespace_visibility = p_->namespace_visibility_[k_];
      }
    }

    // The initializer ([dcl.init]); the name is in scope from the end of its declarator.
    // An explicit or partial specialization, or an instantiation, declares no name: the
    // template's name still names the template in its initializer.
    if (!var_ && !(sem.flags & (sema::decl_flag::specialization | sema::decl_flag::partial | sema::decl_flag::friend_)))
      s_.own.push_back({d.name, sem});
    p_->declared(sem, k_);
    const AtItem::RestoreInitializerTemplate restore_initializer{q_};
    if (has_init && !d.qualified) {
      const ast::DeclSema* primary = nullptr;
      if (var_ && var_->primary) primary = &*var_->primary;
      else if (pattern && s_.world().entities.rec(sem.entity).kind == sema::EntityKind::variable_template) primary = &sem;
      if (primary)
        q_.initializer_template = AtItem::InitializerTemplate{
            d.name, {p_->id(k_), ast::make_decl_ord(ast::DeclClass::syntactic, ord)}, primary};
    }
    Ex init;
    sema::ConstId value = sema::ConstId::none;
    // A variable owns the closure types of its initializer's lambdas (docs/design/closures.md).
    std::optional<sema::Sema::ClosureOwner> owner;
    if (!fn) owner = variable_closures(sem.entity, spec.is_inline || template_ || var_);
    const sema::Sema::ClosureOwnerScope closures(s_, owner);
    // [dcl.constinit]: a reachable declaration's requirement applies to initialization.
    if (spec.is_constinit) {
      auto entry = p_->constinit_declarations_.try_emplace(sem.entity, k_).first;
      entry->second = std::min(entry->second, k_);
    }
    const auto constinit_decl = p_->constinit_declarations_.find(sem.entity);
    const bool requires_constinit = constinit_decl != p_->constinit_declarations_.end() && constinit_decl->second <= k_;
    const bool replay_defaults = !fn && definition && !spec.is_inline && !spec.is_constexpr && !requires_constinit &&
                                 !template_ && !var_ && !explicit_ && !(d.qualified && d.scope.type) &&
                                 s_.can_replay_default_members(d.type) && !p_->inline_declared(sem.entity, k_);
    const sema::Sema::DefaultMemberScope defaults(s_, replay_defaults);
    // An initializer that is not a constant expression initializes the variable dynamically
    // ([basic.start.dynamic]), by the unit's initialization function, unless it must be constant
    // ([dcl.constexpr], [dcl.constinit]) or is not supported yet: an inline variable's or a
    // variable template's (#368), and a scalar's the front end has no code for.
    auto dynamic_ok = [&](u32 at, std::uint64_t reason) {
      if (EntityId entity = s_.folded_immediate_result; entity != EntityId::none) {
        s_.diag(at, Diag::immediate_address, s_.world().entities.rec(entity).name);
        return false;
      }
      if (spec.is_constexpr || requires_constinit) {
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
      } else if (!v.dependent && !v.diagnosed_failure()) {
        dynamic_ok(at, v.reason);
      }
    };
    if (fn && has_init && !is_function_def) {
      s_.diag(ps_.pos(), Diag::function_initialized, d.name);
      ps_.skip_to_semi();
    } else if (!fn && (early || ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
      // List-initialization; constant initialization when every element is a constant
      // ([basic.start.static]).
      const auto form = ps_.accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
      auto b = early ? std::move(early) : ps_.braced_init_list();
      init = s_.list_init(d.type, *b, form, /*extend_reference=*/true);
      if (init.ok() && !pattern) {
        if (ordinary_reference) {
          evaluate_reference(b->begin);
        } else {
          const auto v = sema::Evaluator(s_, q_).evaluate_initializer(init);
          if (v.value) {
            value = *v.value;
            if (spec.is_constexpr) p_->set_constant(sem.entity, *v.value, k_);
          } else if (!v.diagnosed_failure()) {
            dynamic_ok(b->begin, v.reason);
          }
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
          // BOOTSTRAP: -Wmaybe-uninitialized: the literal's value, not the std::optional<Literal>,
          // is kept. At -O3 GCC 16.2 reports the optional's payload as possibly uninitialized at
          // `lit->value` (#748). Without the bug:
          //   auto lit = types.is_enum(d.type) ? s_.as<ast::Literal>(init) : std::nullopt;
          //   ... lit ? sema::Evaluated{lit->value, 0, true} : ...
          ConstId lit = ConstId::none;
          if (types.is_enum(d.type))
            if (auto l = s_.as<ast::Literal>(init)) lit = l->value;
          // Evaluate an aggregate or character array by its elements ([dcl.init.aggr], [dcl.init.string]).
          sema::Evaluator evaluator(s_, q_);
          sema::Evaluated v = lit != ConstId::none ? sema::Evaluated{lit, 0, true}
                                  : s_.as<ast::InitList>(init) ? evaluator.evaluate_initializer(init) : evaluator.evaluate(init);
          if (v.value) {
            value = *v.value;
            if (spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                      types.is_integral(d.type)))
              p_->set_constant(sem.entity, *v.value, k_);
          } else if (!v.diagnosed_failure()) {
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
        } else if (!v.diagnosed_failure()) {
          dynamic_ok(open, v.reason);
        }
      } else if (init.ok()) {
        // A class object: constant initialization when the constructor call is a constant
        // expression, for a class the evaluator can represent ([basic.start.static]).
        sema::Evaluated v = sema::Evaluator(s_, q_).evaluate_initializer(init);
        if (v.value) {
          value = *v.value;
          if (spec.is_constexpr) p_->set_constant(sem.entity, *v.value, k_);
        } else if (!v.diagnosed_failure()) {
          dynamic_ok(open, v.reason);
        }
      }
    } else if (pattern) {
      // Its specializations' types are complete, or not ([temp.inst]/7).
    } else if (ordinary_reference && definition) {
      s_.diag(d.name_at, Diag::reference_uninitialized);  // [dcl.ref]/5
    } else if (!fn && definition && !s_.require_complete(d.type, d.name_at)) {
      // A definition needs a complete object type ([basic.def]).
    } else if (!fn && definition && !types.is_dependent(d.type) && s_.has_user_ctors(element_type(d.type))) {
      // Default-initialization by a constructor of an object or each element. A selected trivial
      // defaulted tag constructor has no dynamic work ([basic.start.static]).
      init = s_.default_init(d.type, d.name_at, ps_.pos());
      if (init.ok()) {
        const bool trivial = s_.as<ast::DefaultInit>(init).has_value();
        const QualType object = types.canonical(d.type);
        const auto tag = trivial && s_.is_defaulted_tag_initializer(init) ? constant_initializer(init) : std::nullopt;
        if (trivial && !tag) {
          // A trivial default constructor does nothing: static storage is zero ([basic.start.static]).
          if (spec.is_constexpr) s_.diag(d.name_at, Diag::constexpr_uninitialized, d.name);
          else if (sema::Types::is_const(object) && !s_.const_default_constructible(object))
            s_.diag(d.name_at, Diag::const_uninitialized, d.name);
        } else if (tag) {
          value = *tag;
          if (spec.is_constexpr) p_->set_constant(sem.entity, value, k_);
        } else {
          dynamic_ok(d.name_at, sema::reason_unsupported);
        }
      }
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
    } else if (!fn && sema::Types::is_const(d.type) && !effective_extern) {
      s_.diag(d.name_at, Diag::const_uninitialized, d.name);
    }
    if (!fn && definition && !pattern) s_.check_destructor_access(d.type, d.name_at);
    if (value != ConstId::none && (requires_constinit || s_.folded_immediate_result != EntityId::none))
      if (auto entity = s_.immediate_result_entity(value))
        s_.diag(init.begin, Diag::immediate_address, s_.world().entities.rec(*entity).name);
    s_.permit_constant_initializer(d.begin, d.type, value);
    declarators_.push_back(s_.node(ast::Declarator{ord, sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
    decls_.push_back(sem);
    if (!d.asm_label.empty()) {
      // GCC uses the last label. The other modes diagnose a conflicting redeclaration.
      std::string earlier = p_->asm_label(sem.entity);
      if (p_->compatibility() != ast::Compatibility::gcc && !earlier.empty() && earlier != d.asm_label) {
        s_.diag(d.name_at, Diag::asm_label_conflict, d.name);
        return false;
      }
      auto& label = p_->asm_labels_[std::to_underlying(sem.entity)];
      if (label.spelling.empty() || std::pair{k_, d.name_at} > std::pair{label.item, label.offset})
        label = {k_, d.name_at, d.asm_label};
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
  // The owner of the closure types of the lambdas in namespace-scope variable `e`'s initializer:
  // of vague linkage when `shared`, a definition that may appear in several units, and `e` has
  // external linkage (docs/design/closures.md, decision 4).
  sema::Sema::ClosureOwner variable_closures(EntityId e, bool shared) {
    const bool external = shared && s_.world().entities.rec(e).linkage == ast::Linkage::external;
    return {e, external ? ast::Linkage::external : ast::Linkage::internal};
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
  std::vector<Ex> declarators_;
  NodeId tree_ = NodeId::none;  // the root when it is not a DeclGroup (a static_assert)
  NameId asm_hidden_symbol_{~u32{0}};
  u32 asm_directive_at_ = 0;
  std::optional<Program::ClassInstantiation> inst_;
  std::optional<Program::VarInstantiation> var_;
  // Analysing a member item again for one static data member's value (`Program::member_value`):
  // the ordinal of the declarator whose initializer is instantiated.
  std::optional<u32> member_value_;
  std::optional<Program::MemberDemand> member_layout_demand_;
  bool member_initial_value_ = false;
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
  ast::WrittenConstraint head_constraint_{}, trailing_constraint_{};
  sema::Fp128 constraints_fp_{};               // their tokens' fingerprint, for identity
  ConstId partial_constraints_ = ConstId::none;  // a partial specialization's, from `partial_arguments`
  sema::Fp128 partial_constraints_fp_{};
};

const Result<ast::ItemSema>& Program::item_sema(u32 item) {
  if (!item_cells_[item]) {
    // A cycle is an internal error (§A2).
    contract_assert(std::ranges::find(running_, item) == running_.end());
    running_.push_back(item);
    ItemAnalysis analysis(*this, item);
    item_cells_.at(item) = analysis.run();
    if (analysis.class_assertion_failure()) items_with_failed_assertion_dependencies_.insert(item);
    running_.pop_back();
  }
  return *item_cells_[item];
}

// The declaration of friend `f` of a class: the one its class template's specialization made
// ([temp.friend]), else its own.
std::optional<ast::DeclSema> Program::friend_decl(const ast::MemberRec& f) {
  if (member_patterns_.contains(std::to_underlying(f.entity))) return class_friend(f.entity);
  return decl_sema(f.decl);
}

// The declaration a class template specialization made of the friend function or function
// template `f` it defines, if it is one (#259).
std::optional<ast::DeclSema> Program::class_friend(EntityId f) {
  auto m = member_patterns_.find(std::to_underlying(f));
  if (m == member_patterns_.end()) return std::nullopt;
  auto cell = class_member_cells_.find(std::pair(m->second.item, std::to_underlying(m->second.spec)));
  if (cell == class_member_cells_.end()) return std::nullopt;
  for (const ast::DeclSema& d : w_->ins.seqs.elements(cell->second.value.decls))
    if (d.entity == f && (d.flags & sema::decl_flag::friend_)) return d;
  return std::nullopt;
}

std::optional<ast::DeclSema> Program::decl_sema(DeclId d) {
  if (ast::class_of(d.ord) != ast::DeclClass::syntactic) return std::nullopt;
  // The members of a class whose item is still being analysed (its declarators after the
  // brace ask for the class's completeness) see its head, which was declared first.
  if (u32 k = index(d.item); std::ranges::find(running_, k) != running_.end()) {
    if (ast::number_of(d.ord) == 0 && class_heads_.contains(k)) return class_heads_.at(k);
    return std::nullopt;
  }
  const ast::ItemSema& s = settle(index(d.item)).value;
  u32 n = ast::number_of(d.ord);
  if (n >= w_->ins.seqs.size(s.decls)) return std::nullopt;
  return completed_declaration(w_->ins.seqs.at(s.decls, n));
}

// ---------------------------------------------------------------- class_def

bool valid_member_declarations(std::span<const engine::Diagnostic> diagnostics) {
  return std::ranges::none_of(diagnostics, [](const engine::Diagnostic& d) {
    const auto kind = sema::sema_diag_of(d.id);
    return !kind || sema::message_of(*kind).severity == lex::Severity::error;
  });
}

bool Program::valid_class_assertion(const Result<ast::ItemSema>& member) const {
  return w_->ins.ast.is<ast::StaticAssert>(member.value.tree) && valid_member_declarations(member.diags);
}

// The members of the class item `item` from its member items' records, read by `sema_of`: the
// class's own, or a class template specialization's (#88).
Program::ClassContent Program::class_content(u32 item, const std::function<const ast::ItemSema&(u32)>& sema_of) {
  ClassContent out;
  std::vector<ast::MemberRec>& members = out.members;
  const items::Item& it = items_->items[item];
  std::vector<sema::FieldRec>& fields = out.fields;
  std::vector<sema::MemberDeclRec>& statics = out.statics;
  std::vector<sema::NestedType>& nested = out.nested;
  // [class.access.general]: the definition's class-key sets default access, including when
  // these declarations are replayed for a specialization.
  const auto key = class_definition_keys_.find(item);
  ast::Access access = key != class_definition_keys_.end() && key->second == lex::Tk::kw_class
                           ? ast::Access::private_ : ast::Access::public_;
  for (u32 m : it.members) {
    const items::Item& mi = items_->items[m];
    if (mi.kind == ItemKind::access_spec) {
      lex::Tk k = r_->tokens[mi.begin].kind;
      access = k == lex::Tk::kw_public ? ast::Access::public_ : k == lex::Tk::kw_protected ? ast::Access::protected_
                                                                                          : ast::Access::private_;
      continue;
    }
    const ast::ItemSema& ms = sema_of(m);
    bool inspect_field_layout = !out.unapplied_field_layout;
    // The default member initializers' values, by declaration ordinal.
    std::map<u32, ConstId> inits;
    if (ms.tree != ast::NodeId::none && w_->ins.ast.is<ast::DeclGroup>(ms.tree))
      for (ast::Child c : w_->ins.ast.children(w_->ins.ast.get<ast::DeclGroup>(ms.tree).declarators)) {
        auto dr = w_->ins.ast.get<ast::Declarator>(c.id);
        inits[dr.decl] = dr.value;
      }
    for (u32 i = 0; i < w_->ins.seqs.size(ms.decls); ++i) {
      const auto& names = items_->items[m].names;
      if (i < names.size() && names[i].member_elaborated_candidate) continue;
      ast::DeclSema d = w_->ins.seqs.at(ms.decls, i);
      if (d.entity == EntityId::none || scoped_enumerator(d)) continue;
      // A friend declaration names a function or class that is not a member ([class.friend]/1).
      if (d.flags & sema::decl_flag::friend_) {
        if (std::ranges::find(out.friends, d.entity) == out.friends.end()) out.friends.push_back(d.entity);
        if (ordinary_friend_items_.contains(m) || templated_friend_items_.contains(m))
          out.adl_friends.push_back({{id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.entity, ast::Access::none});
        continue;
      }
      members.push_back({{id(m), ast::make_decl_ord(ast::DeclClass::syntactic, i)}, d.entity, access});
      if (d.kind == ast::DeclKind::data_member) {
        if (inspect_field_layout) {
          out.unapplied_field_layout = layout_attribute(*r_, mi.begin, mi.end, LayoutAttributes::affecting_layout,
                                                        applied_alignment_specifiers(m));
          inspect_field_layout = false;
        }
        const bool no_unique_address = std::ranges::any_of(w_->ins.seqs.elements(d.attributes), [&](NodeId attr) {
          return w_->ins.names.spelling(w_->ins.ast.get<ast::Attribute>(attr).name) == "no_unique_address";
        });
        fields.push_back({w_->entities.rec(d.entity).name, d.type, d.entity, inits.contains(i) ? inits[i] : ConstId::none,
                        dmis_.contains(std::to_underlying(d.entity)), d.bit_width, d.is_mutable, no_unique_address,
                        d.field_alignment, d.written_field_alignment});
      } else if (d.kind == ast::DeclKind::type_alias && (d.flags & sema::decl_flag::template_))
        out.alias_templates.push_back({w_->entities.rec(d.entity).name, d.type, d.entity});
      else if (d.kind == ast::DeclKind::enum_ || d.kind == ast::DeclKind::type_alias)
        nested.push_back({w_->entities.rec(d.entity).name, d.type, d.entity});
      // A member class ([class.nest]), once however often it is declared. An unnamed class is
      // reached through its declarators only.
      else if (d.kind == ast::DeclKind::class_ && !w_->ins.names.spelling(w_->entities.rec(d.entity).name).empty() &&
               !std::ranges::contains(nested, d.entity, &sema::NestedType::entity))
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
        statics.back().is_explicit = d.is_explicit;
        statics.back().is_final = d.is_final;
        statics.back().is_pure = d.is_pure;
        statics.back().is_consteval = (d.flags & sema::decl_flag::consteval_) != 0;
        if (!(d.flags & sema::decl_flag::template_)) statics.back().constraints = d.constraints;
        if (auto overridden = overridden_virtuals_.find(d.entity); overridden != overridden_virtuals_.end())
          statics.back().overridden_virtual = overridden->second;
        if (defaulted_member_definition(m, d)) {
          if (w_->ins.names.spelling(statics.back().name) == "operator=") statics.back().explicit_defaulted_assignment = true;
          // Until the class is complete (`defaulted_constructors`).
          else if (statics.back().is_ctor) statics.back().defaulted = sema::CopyState::unsupported;
        }
      }
    }
  }
  return out;
}

sema::PeerVirtualLayout Program::peer_virtual_layout(u32 item, EntityId cls, const sema::ClassRec& rec,
                                                   std::span<const ast::MemberRec> members) {
  using enum sema::PeerVirtualLayout;
  const auto& head = items_->items[item];
  const auto& entity = w_->entities.rec(cls);
  if (head.template_depth || head.specialization || in_class(item) || unnamed(ns_of(item)) ||
      entity.templ != EntityId::none || rec.union_ || !rec.types.empty()) return none;
  const bool derived = !rec.bases.empty();
  const auto has_attributes = [&](u32 begin, u32 end) { return layout_attribute(*r_, begin, end, LayoutAttributes::any); };
  if (layout_may_be_packed(*r_, head.begin, head.brace_end) || has_attributes(head.begin, head.brace_begin) ||
      has_attributes(head.brace_end, head.end)) return none;
  const auto written_member = [&](EntityId f) -> std::optional<ast::DeclSema> {
    const auto decl = member_decl(f);
    if (!decl) return std::nullopt;
    const auto& source = items_->items[index(decl->item)];
    if (derived && has_attributes(source.begin, source.end)) return std::nullopt;
    return decl_sema(*decl);
  };
  if (derived) {
    const auto key = r_->tokens[head.begin].kind;
    if ((key != lex::Tk::kw_struct && key != lex::Tk::kw_class) || entity.linkage != ast::Linkage::external ||
        !rec.friends.empty() || !sema::native_virtual_derived_declarations(rec, classes_, w_->types, written_member))
      return none;
    if (std::ranges::any_of(head.members, [&](u32 m) {
          const auto& source = items_->items[m];
          return has_attributes(source.begin, source.end) || r_->tokens[source.begin].kind == lex::Tk::kw_using ||
                 std::ranges::any_of(source.names, [](const items::DeclName& name) {
                   return (name.flags & items::name_flag::type) != 0;
                 });
        })) return none;
  } else if (!sema::native_virtual_root_declarations(rec, w_->types, written_member)) {
    return none;
  }
  // A forward declaration can carry attributes that the ordinary parser does not apply.
  const auto declarations = decls_.find(std::to_underlying(cls));
  if (declarations != decls_.end() && std::ranges::any_of(declarations->second, [&](const auto& decl) {
        const auto& prior = items_->items[decl.first];
        return decl.first < item && has_attributes(prior.begin, prior.end);
      })) return none;
  auto decl = members.begin();
  u32 previous = items::none;
  for (const sema::FieldRec& field : rec.fields) {
    const auto type = w_->types.builtin_of(field.type);
    if (field.bit_width || !type || !ast::is_integral(*type) || !w_->target.has(*type) || w_->target[*type].size > 8)
      return none;
    decl = std::ranges::find(decl, members.end(), field.entity, &ast::MemberRec::entity);
    if (decl == members.end()) return none;
    const u32 source_item = index(decl->decl.item);
    ++decl;
    if (source_item == previous) continue;
    previous = source_item;
    const auto& source = items_->items[source_item];
    if (source.names.empty() || has_attributes(source.begin, source.end)) return none;
    // An alias can carry a GNU alignment or mode attribute that sema erases. Use only builtin
    // scalar specifiers written directly in the member declaration for the initial layout slice.
    const auto specifiers = std::span(r_->tokens).subspan(source.begin, source.names.front().offset);
    if (!std::ranges::all_of(specifiers, [](const lex::Tok& token) {
      using enum lex::Tk;
      return Parser::simple_type_keyword(token.kind) || token.kind == kw_const || token.kind == kw_volatile ||
             token.kind == kw_mutable;
    })) return none;
  }
  return derived ? primary_base : root;
}

// These implicit constructors have no observable work before installing the derived vptr.
// Read the directly written declarations as canonical types can erase ABI-changing attributes.
EntityId Program::peer_automatic_primary_base(u32 item, EntityId cls, const sema::ClassRec& rec,
                                              std::span<const ast::MemberRec> members) {
  using enum lex::Tk;
  if (rec.bases.size() != 1 || rec.fields.size() != 1 || rec.is_abstract()) return EntityId::none;
  const auto& edge = rec.bases.front();
  if (edge.access != ast::Access::public_ || edge.dependent_in_pattern) return EntityId::none;
  const auto* base = classes_.find(edge.cls);
  const auto definition = class_items_.find(std::to_underlying(edge.cls));
  if (!base || !base->defined || !base->is_abstract() || !base->bases.empty() || !base->fields.empty() ||
      definition == class_items_.end()) return EntityId::none;

  const auto tokens = [&](u32 begin, u32 end) { return std::span(r_->tokens).subspan(begin, end - begin); };
  const auto names = [&](lex::Tok token, EntityId entity) {
    return token.kind == identifier && r_->name(token) == w_->ins.names.spelling(w_->entities.rec(entity).name);
  };
  const auto ordinary_class = [&](u32 index, EntityId entity, const sema::ClassRec& record,
                                  std::ptrdiff_t written_members) {
    const auto& head = items_->items[index];
    const auto& owner = w_->entities.rec(entity);
    return sema::native_virtual_class(record) && record.destructor_usable && !record.default_deleted &&
           !record.union_ && !record.closure && !record.unapplied_layout && !record.has_explicit_alignment() &&
           record.types.empty() && record.friends.empty() && owner.linkage == ast::Linkage::external &&
           owner.templ == EntityId::none && is_namespace(owner.scope) && !unnamed(namespace_path(owner.scope)) &&
           !head.template_depth && !head.specialization && !in_class(index) &&
           !layout_attribute(*r_, head.brace_end, head.end, LayoutAttributes::any) &&
           std::ranges::count_if(head.members, [&](u32 member) {
             return items_->items[member].kind != ItemKind::access_spec;
           }) == written_members;
  };
  const u32 base_item = definition->second;
  if (!ordinary_class(item, cls, rec, 2) || !ordinary_class(base_item, edge.cls, *base, 1)) return EntityId::none;

  const auto class_key = [](lex::Tk k) { return k == kw_struct || k == kw_class; };
  const auto& base_source = items_->items[base_item];
  const auto base_head = tokens(base_source.begin, base_source.brace_begin);
  if (base_head.size() != 2 || !class_key(base_head[0].kind) || !names(base_head[1], edge.cls)) return EntityId::none;
  const auto& derived_source = items_->items[item];
  const auto head = tokens(derived_source.begin, derived_source.brace_begin);
  if ((head.size() != 4 && head.size() != 5) || !class_key(head[0].kind) || !names(head[1], cls) ||
      head[2].kind != colon || (head.size() == 5 && head[3].kind != kw_public) || !names(head.back(), edge.cls))
    return EntityId::none;
  const auto visible = visible_decls(r_->name(head.back()), item, /*self=*/false);
  if (visible.empty() || !std::ranges::all_of(visible, [&](DeclId id) {
        const auto decl = decl_sema(id);
        return decl && decl->kind == ast::DeclKind::class_ && decl->entity == edge.cls;
      })) return EntityId::none;

  const auto& field = rec.fields.front();
  if (rec.access_of(field.entity) != ast::Access::public_ || field.dmi || field.init != ConstId::none ||
      field.bit_width || field.no_unique_address || w_->types.builtin_of(field.type) != sema::Builtin::int_)
    return EntityId::none;
  const auto member = std::ranges::find(members, field.entity, &ast::MemberRec::entity);
  if (member == members.end()) return EntityId::none;
  const auto& field_source = items_->items[index(member->decl.item)];
  const auto field_tokens = tokens(field_source.begin, field_source.end);
  if (field_tokens.size() != 3 || field_tokens[0].kind != kw_int || !names(field_tokens[1], field.entity) ||
      field_tokens[2].kind != semi) return EntityId::none;

  const auto slot = [&](u32 owner, const sema::ClassRec& record, bool pure) -> const sema::MemberDeclRec* {
    const auto m = std::ranges::find(record.members, true, &sema::MemberDeclRec::is_virtual);
    if (m == record.members.end() || m->is_pure != pure || record.access_of(m->entity) != ast::Access::public_ ||
        std::ranges::find(std::next(m), record.members.end(), true, &sema::MemberDeclRec::is_virtual) != record.members.end())
      return nullptr;
    const auto decl = member_decl(m->entity);
    if (!decl) return nullptr;
    const auto& source = items_->items[index(decl->item)];
    if (source.parent != owner || source.kind != ItemKind::declaration) return nullptr;
    auto written = tokens(source.begin, source.end);
    if (pure) {
      if (written.empty() || written.front().kind != kw_virtual) return nullptr;
      written = written.subspan(1);
    }
    if (written.size() != (pure ? 7u : 6u) || written[0].kind != kw_int || !names(written[1], m->entity) ||
        written[2].kind != l_paren || written[3].kind != r_paren || written.back().kind != semi) return nullptr;
    if (pure ? written[4].kind != equal || written[5].kind != int_lit || r_->spelling(written[5]) != "0"
             : written[4].kind != identifier || r_->name(written[4]) != "override") return nullptr;
    return &*m;
  };
  const auto* pure = slot(base_item, *base, true);
  const auto* override = slot(item, rec, false);
  return pure && override && override->overridden_virtual == pure->entity ? edge.cls : EntityId::none;
}

bool Program::has_unapplied_class_layout(u32 item, const ClassContent& content) const {
  const auto& source = items_->items[item];
  return content.unapplied_field_layout || layout_may_be_packed(*r_, source.begin, source.brace_end) ||
         record_layout_attribute(*r_, source, applied_alignment_specifiers(item));
}

// A namespace attribute can set visibility on otherwise unannotated declarations.
bool Program::in_attributed_namespace(u32 item) const {
  const auto& source = items_->items;
  for (u32 parent = source[item].parent; parent != items::none; parent = source[parent].parent) {
    const auto& scope = source[parent];
    if (scope.kind == ItemKind::namespace_def &&
        layout_attribute(*r_, scope.begin, scope.brace_begin, LayoutAttributes::any)) return true;
  }
  return false;
}

// The supported class parameters have default-visible ABI names. The ordinary class
// records erase most GNU attributes, so reject declarations with unapplied attributes.
bool Program::plain_class_template_type(EntityId cls) const {
  if (r_->first_visibility_pragma || r_->has_clang_attribute_pragma) return false;
  const auto attributed = [&](u32 index) {
    const auto& item = items_->items[index];
    const bool definition = item.kind == ItemKind::class_def;
    return in_attributed_namespace(index) ||
           layout_attribute(*r_, item.begin, definition ? item.brace_begin : item.end,
                            LayoutAttributes::any, applied_alignment_specifiers(index)) ||
           (definition && layout_attribute(*r_, item.brace_end, item.end, LayoutAttributes::any));
  };
  for (EntityId e = cls; e != EntityId::none && !is_namespace(e); e = w_->entities.rec(e).scope) {
    const auto definition = class_items_.find(std::to_underlying(e));
    if (definition == class_items_.end() || attributed(definition->second)) return false;
    const auto decls = decls_.find(std::to_underlying(e));
    if (decls != decls_.end() && std::ranges::any_of(decls->second, [&](const auto& d) { return attributed(d.first); }))
      return false;
  }
  return true;
}

bool Program::plain_class_template_function(EntityId pattern, u32 definition) const {
  if (r_->first_visibility_pragma || r_->has_clang_attribute_pragma) return false;
  const auto attributed = [&](u32 index) {
    const auto& all = items_->items;
    const auto& item = all[index];
    if (in_attributed_namespace(index) ||
        layout_attribute(*r_, item.begin, item.has_body() ? item.body_begin : item.end, LayoutAttributes::any))
      return true;
    for (u32 parent = item.parent; parent != items::none; parent = all[parent].parent)
      if (all[parent].kind == ItemKind::class_def &&
          layout_attribute(*r_, all[parent].begin, all[parent].brace_begin,
                           LayoutAttributes::any, applied_alignment_specifiers(parent)))
        return true;
    return false;
  };
  if (attributed(definition)) return false;
  const auto decls = decls_.find(std::to_underlying(pattern));
  return decls == decls_.end() ||
         std::ranges::none_of(decls->second, [&](const auto& d) { return attributed(d.first); });
}

// A declaration can change a class's RTTI binding or representation even if the class
// definition itself is unannotated.
bool Program::class_rtti_declaration_has_unapplied_abi(EntityId cls, u32 k, bool friend_declaration) const {
  const auto& item = items_->items[k];
  const bool definition = item.kind == ItemKind::class_def;
  const u32 head_end = definition ? item.brace_begin : item.end;
  // A friend that introduces a class records its namespace attributes in
  // introduce_namespace_class. A friend that only names a known class does not inherit them.
  return r_->first_visibility_pragma || r_->has_clang_attribute_pragma ||
         has_unapplied_declaration_attributes(cls) || (!friend_declaration && in_attributed_namespace(k)) ||
         layout_attribute(*r_, item.begin, head_end, LayoutAttributes::any) ||
         (definition && layout_attribute(*r_, item.brace_end, item.end, LayoutAttributes::any));
}

// The class, including earlier declarations and later friends of its slots, must use the
// ordinary default-visible ABI for an external table reference.
bool Program::peer_root_vtable_visible(EntityId cls) const {
  if (r_->first_visibility_pragma || has_unapplied_declaration_attributes(cls)) return false;
  const auto declarations = decls_.find(std::to_underlying(cls));
  if (declarations == decls_.end() || std::ranges::any_of(declarations->second, [&](const auto& d) {
        const auto& item = items_->items[d.first];
        const u32 end = item.kind == ItemKind::class_def ? item.brace_begin : item.end;
        return in_attributed_namespace(d.first) || layout_attribute(*r_, item.begin, end, LayoutAttributes::any);
      })) return false;
  for (const auto& member : classes_.rec(cls).members) {
    if (!member.is_virtual) continue;
    const auto decl = member_decl(member.entity);
    if (!decl || has_unapplied_declaration_attributes(member.entity)) return false;
    const auto& item = items_->items[index(decl->item)];
    if (layout_attribute(*r_, item.begin, item.end, LayoutAttributes::any)) return false;
  }
  return true;
}

std::optional<EntityId> Program::automatic_derived_vtable_key(EntityId cls) const {
  if (r_->has_clang_attribute_pragma) return std::nullopt;
  const EntityId base = classes_.rec(cls).peer_automatic_primary_base;
  if (base == EntityId::none || !peer_root_vtable_visible(cls) || !peer_root_vtable_visible(base)) return std::nullopt;
  // The class certificate identifies one direct slot on each class. The pure base must
  // still have only its declaration; a definition can belong only to the derived override.
  const auto slot = [&](EntityId owner) {
    const auto& members = classes_.rec(owner).members;
    return std::ranges::find(members, true, &sema::MemberDeclRec::is_virtual)->entity;
  };
  const auto pure = decls_.find(std::to_underlying(slot(base)));
  const EntityId key = slot(cls);
  const auto derived = decls_.find(std::to_underlying(key));
  if (pure == decls_.end() || pure->second.size() != 1 ||
      (pure->second.front().second.flags & sema::decl_flag::definition) || derived == decls_.end()) return std::nullopt;

  const auto& declarations = derived->second;
  const auto definition = std::ranges::find_if(declarations, [](const auto& d) {
    return d.second.flags & sema::decl_flag::definition;
  });
  if (definition == declarations.end())
    return declarations.size() == 1 ? std::optional(EntityId::none) : std::nullopt;
  const auto declared = member_decl(key);
  constexpr u16 excluded = sema::decl_flag::inline_ | sema::decl_flag::deleted | sema::decl_flag::lang_c;
  if (declarations.size() != 2 || !declared || definition->first == index(declared->item) ||
      !definition->second.is_virtual || (definition->second.flags & excluded) ||
      in_attributed_namespace(definition->first) ||
      !asm_label(key).empty()) return std::nullopt;
  return key;
}

Program::TypedCastRtti Program::peer_typed_dynamic_cast(EntityId source, EntityId target) const {
  using sema::PeerVirtualLayout;
  const auto* base = classes_.find(source);
  const auto* derived = classes_.find(target);
  if (!w_->target.supports(ast::NativeOperation::peer_typed_dynamic_cast) || !base || !derived ||
      r_->has_clang_attribute_pragma)
    return TypedCastRtti::unsupported;
  // The automatic-primary-base certificate covers the pure base, the public edge and the
  // directly declared override. A complete-unit check must still leave the key to a peer.
  if (derived->peer_automatic_primary_base == source) {
    const auto key = automatic_derived_vtable_key(target);
    return key && *key == EntityId::none ? TypedCastRtti::keyless_base : TypedCastRtti::unsupported;
  }
  // The primary-base profile establishes public nonvirtual inheritance at offset zero.
  if (base->peer_virtual_layout != PeerVirtualLayout::root ||
      derived->peer_virtual_layout != PeerVirtualLayout::primary_base || derived->bases.size() != 1 ||
      derived->bases.front().cls != source)
    return TypedCastRtti::unsupported;
  const auto base_key = sema::borrowed_dynamic_cast_key(*base, w_->types);
  const auto derived_key = sema::borrowed_dynamic_cast_key(*derived, w_->types);
  if (base_key == EntityId::none || derived_key == EntityId::none) return TypedCastRtti::unsupported;
  const auto override = std::ranges::find(derived->members, derived_key, &sema::MemberDeclRec::entity);
  if (override->overridden_virtual != base_key) return TypedCastRtti::unsupported;

  const auto peer_owned = [&](EntityId cls, const sema::ClassRec& record, EntityId key) {
    const auto& owner = w_->entities.rec(cls);
    if (!record.defined || record.has_explicit_alignment() || class_layout_uncertain(cls, record, end()) ||
        owner.linkage != ast::Linkage::external || owner.templ != EntityId::none ||
        !is_namespace(owner.scope) || unnamed(namespace_path(owner.scope)) ||
        w_->ins.names.spelling(owner.name).empty() || !peer_root_vtable_visible(cls) || !asm_label(key).empty())
      return false;
    // The only declaration of each key must be its declaration in the class.
    // This reads the final declaration set, including later definitions and qualified friends.
    const auto found = decls_.find(std::to_underlying(key));
    if (found == decls_.end() || found->second.size() != 1) return false;
    const auto& [item, decl] = found->second.front();
    constexpr u16 excluded = sema::decl_flag::definition | sema::decl_flag::inline_ |
                             sema::decl_flag::deleted | sema::decl_flag::template_;
    return !(decl.flags & excluded) && !decl.hidden_visibility &&
           items_->items[item].kind == ItemKind::declaration && !in_attributed_namespace(item);
  };
  return peer_owned(source, *base, base_key) && peer_owned(target, *derived, derived_key)
             ? TypedCastRtti::imported : TypedCastRtti::unsupported;
}

bool Program::peer_pure_dispatch(EntityId cls) const {
  using enum lex::Tk;
  const auto* record = classes_.find(cls);
  if (!record || !record->defined || !record->is_abstract() || !sema::native_virtual_class(*record) ||
      record->peer_virtual_layout != sema::PeerVirtualLayout::none ||
      record->union_ || record->closure || !record->bases.empty() || !record->fields.empty() ||
      !record->types.empty() || !record->friends.empty() || record->unapplied_layout || record->explicit_alignment)
    return false;
  const auto& owner = w_->entities.rec(cls);
  const auto definition = class_items_.find(std::to_underlying(cls));
  if (owner.linkage != ast::Linkage::external || owner.templ != EntityId::none ||
      !is_namespace(owner.scope) || unnamed(namespace_path(owner.scope)) ||
      w_->ins.names.spelling(owner.name).empty() || definition == class_items_.end()) return false;
  const auto& head = items_->items[definition->second];
  const auto key = r_->tokens[head.begin].kind;
  if ((key != kw_struct && key != kw_class) || head.template_depth || head.specialization ||
      layout_attribute(*r_, head.brace_end, head.end, LayoutAttributes::any) || !peer_root_vtable_visible(cls)) return false;

  const auto integer = [&](QualType type) {
    const auto builtin = w_->types.builtin_of(type);
    return builtin == sema::Builtin::int_ || builtin == sema::Builtin::unsigned_int;
  };
  // Sema canonicalizes aliases and noexcept conditions and can erase ABI attributes. Read the
  // whole declaration to admit only a directly written fixed integer prototype.
  const auto plain_signature = [&](const items::Item& source, bool has_parameter) {
    const auto tokens = std::span(r_->tokens).subspan(source.begin, source.end - source.begin);
    std::size_t at = 0;
    const auto accept = [&](lex::Tk kind) {
      if (at == tokens.size() || tokens[at].kind != kind) return false;
      ++at;
      return true;
    };
    const auto written_int = [&] {
      if (accept(kw_signed) || accept(kw_unsigned)) {
        accept(kw_int);
        return true;
      }
      if (!accept(kw_int)) return false;
      if (!accept(kw_signed)) accept(kw_unsigned);
      return true;
    };
    if (!accept(kw_virtual) || !written_int() || !accept(identifier) || !accept(l_paren)) return false;
    if (has_parameter) {
      if (!written_int()) return false;
      accept(identifier);
    } else {
      accept(kw_void);
    }
    if (!accept(r_paren)) return false;
    accept(kw_const);
    return accept(kw_noexcept) && accept(equal) && accept(int_lit) && r_->spelling(tokens[at - 1]) == "0" &&
           accept(semi) && at == tokens.size();
  };

  unsigned slots = 0;
  for (const auto& member : record->members) {
    if (!member.is_virtual) {
      if (member.copy_assignment == sema::CopyAssignment::none || member.explicit_defaulted_assignment ||
          member_decl(member.entity)) return false;
      continue;
    }
    if (++slots > 2 || !member.is_pure || member.is_final || record->access_of(member.entity) != ast::Access::public_)
      return false;
    const auto& entity = w_->entities.rec(member.entity);
    const auto signature = w_->types.rec(member.type);
    if (entity.kind != sema::EntityKind::function || entity.scope != cls || entity.templ != EntityId::none ||
        entity.linkage != ast::Linkage::external || entity.lang != LangLinkage::cxx || !signature.fn_noexcept ||
        signature.variadic || signature.fn_ref != ast::RefQual::none || signature.params.size() > 1 ||
        (signature.fn_cv != sema::Cv::none && signature.fn_cv != sema::Cv::const_) ||
        !integer(signature.inner) || !std::ranges::all_of(signature.params, integer)) return false;
    // Rechecked after whole-unit analysis by native emission. A competing definition or an
    // ABI attribute on a qualified friend must not turn this into a direct entry or another ABI.
    const auto declarations = decls_.find(std::to_underlying(member.entity));
    if (declarations == decls_.end() || declarations->second.size() != 1) return false;
    const auto& [item, decl] = declarations->second.front();
    constexpr u16 excluded = sema::decl_flag::definition | sema::decl_flag::inline_ | sema::decl_flag::deleted |
                             sema::decl_flag::template_;
    const auto& source = items_->items[item];
    if ((decl.flags & excluded) || source.kind != ItemKind::declaration || source.parent != definition->second ||
        source.names.size() != 1 || !plain_signature(source, !signature.params.empty())) return false;
  }
  return slots != 0;
}

std::optional<EntityId> Program::peer_root_destructor_key(EntityId cls) const {
  using enum lex::Tk;
  if (r_->has_clang_attribute_pragma) return std::nullopt;
  const auto* record = classes_.find(cls);
  if (!record || !record->defined || record->dynamic ||
      !record->virtual_destructor || !record->destructor_noexcept.value_or(false) ||
      !record->destructor_usable || record->is_abstract() || record->union_ || record->closure ||
      !record->bases.empty() || !record->fields.empty() || !record->types.empty() || !record->friends.empty() ||
      record->unapplied_layout || record->explicit_alignment) return std::nullopt;

  const auto& owner = w_->entities.rec(cls);
  const auto definition = class_items_.find(std::to_underlying(cls));
  if (owner.linkage != ast::Linkage::external || owner.templ != EntityId::none ||
      !is_namespace(owner.scope) || unnamed(namespace_path(owner.scope)) ||
      w_->ins.names.spelling(owner.name).empty() || definition == class_items_.end()) return std::nullopt;
  const auto& head = items_->items[definition->second];
  const auto class_key = r_->tokens[head.begin].kind;
  if ((class_key != kw_struct && class_key != kw_class) || head.template_depth || head.specialization ||
      layout_may_be_packed(*r_, head.begin, head.brace_end) ||
      layout_attribute(*r_, head.brace_end, head.end, LayoutAttributes::any) ||
      !peer_root_vtable_visible(cls)) return std::nullopt;

  // The two directly written public declarations fix the key and the ordinary root ABI.
  // Reading the tokens avoids accepting an erased annotation, alias or exception condition.
  constexpr std::array key_tokens{kw_virtual, kw_int, identifier, l_paren, r_paren, kw_noexcept, semi};
  constexpr std::array destructor_tokens{kw_virtual, tilde, identifier, l_paren, r_paren, kw_noexcept, semi};
  EntityId key = EntityId::none;
  EntityId destructor = EntityId::none;
  for (u32 item : head.members) {
    const auto& source = items_->items[item];
    if (source.kind == ItemKind::access_spec) continue;
    if (source.kind != ItemKind::declaration || source.names.size() != 1 || destructor != EntityId::none)
      return std::nullopt;
    const auto decl = std::ranges::find_if(record->members, [&](const sema::MemberDeclRec& member) {
      const auto written = member_decl(member.entity);
      return written && index(written->item) == item;
    });
    const bool is_destructor = key != EntityId::none;
    if (decl == record->members.end() || !decl->is_virtual || decl->is_dtor != is_destructor ||
        record->access_of(decl->entity) != ast::Access::public_ || !asm_label(decl->entity).empty()) return std::nullopt;
    const auto tokens = std::span(r_->tokens).subspan(source.begin, source.end - source.begin);
    if (!std::ranges::equal(tokens, is_destructor ? destructor_tokens : key_tokens, std::ranges::equal_to{}, &lex::Tok::kind))
      return std::nullopt;
    if (is_destructor) destructor = decl->entity;
    else key = decl->entity;
  }
  if (key == EntityId::none || destructor != record->destructor) return std::nullopt;
  return key;
}

bool Program::peer_root_destructor_producer(EntityId cls) const {
  const auto key = peer_root_destructor_key(cls);
  if (!key) return false;
  const auto destructor = classes_.rec(cls).destructor;
  const auto def = definitions_.find(std::to_underlying(destructor));
  const auto key_declarations = decls_.find(std::to_underlying(*key));
  const auto destructor_declarations = decls_.find(std::to_underlying(destructor));
  return def != definitions_.end() && in_global_scope(def->second) &&
         key_declarations != decls_.end() && key_declarations->second.size() == 1 &&
         destructor_declarations != decls_.end() && destructor_declarations->second.size() == 2;
}

// Itanium C++ ABI §2.4–2.5 for a polymorphic class with at most one base, which is public,
// nonvirtual and itself dynamic: its vptr is at offset zero, its own or its primary base's (#309).
// A class template's specializations are dynamic classes, its pattern is not (#1153). Local classes,
// internal linkage and other base shapes are #309's follow-ups.
std::optional<sema::DynamicClass> Program::dynamic_class(u32 item, EntityId cls, const sema::ClassRec& rec,
                                                        std::span<const ast::MemberRec> members) {
  const auto& head = items_->items[item];
  const auto& entity = w_->entities.rec(cls);
  // A class in an unnamed namespace is TU-local ([basic.link]): its table needs local symbols (#1155).
  if (w_->types.is_dependent(w_->types.class_type(cls)) || rec.union_ || rec.closure || rec.unapplied_layout || entity.linkage != ast::Linkage::external || unnamed(ns_of(item)) || rec.bases.size() > 1)
    return std::nullopt;
  for (EntityId scope = entity.scope; !is_namespace(scope); scope = w_->entities.rec(scope).scope) {
    const auto& owner = w_->entities.rec(scope);
    if (owner.kind != sema::EntityKind::class_ || owner.templ != EntityId::none) return std::nullopt;
  }
  // Sema does not carry an alignment attribute through `decltype` of a variable declared with an
  // attributed alias, so such a field's layout is not known here (#1167).
  // Only the declaration's specifiers, before its first declarator, are inspected; an alias
  // declared elsewhere is #1167's.
  for (const sema::FieldRec& field : rec.fields) {
    const auto member = std::ranges::find(members, field.entity, &ast::MemberRec::entity);
    if (member == members.end()) continue;
    const auto& source = items_->items[index(member->decl.item)];
    const u32 specifiers = source.names.empty() ? source.end - source.begin : source.names.front().offset;
    if (std::ranges::contains(std::span(r_->tokens).subspan(source.begin, specifiers), lex::Tk::kw_decltype,
                              &lex::Tok::kind))
      return std::nullopt;
  }
  sema::DynamicClass out;
  if (!rec.bases.empty()) {
    const sema::BaseRec& edge = rec.bases.front();
    const sema::ClassRec* base = classes_.find(edge.cls);
    if (edge.access != ast::Access::public_ || !base || !base->dynamic) return std::nullopt;
    out.slots = base->dynamic->slots;
    out.primary_base = true;
  }
  const auto replace_destructor = [&](EntityId destructor, bool pure) {
    bool replaced = false;
    for (auto& slot : out.slots) {
      if (slot.kind == sema::VirtualSlotKind::function) continue;
      slot.function = destructor;
      slot.pure = pure;
      replaced = true;
    }
    return replaced;
  };
  for (const sema::MemberDeclRec& m : rec.members) {
    // Immediate virtuals participate in class semantics but have no ABI entry or key.
    if (!m.is_virtual || m.is_consteval) continue;
    if (m.is_template) return std::nullopt;
    if (m.is_dtor) {
      if (!replace_destructor(m.entity, m.is_pure)) {
        out.slots.push_back({m.entity, sema::VirtualSlotKind::complete_destructor, m.is_pure});
        out.slots.push_back({m.entity, sema::VirtualSlotKind::deleting_destructor, m.is_pure});
      }
    } else if (m.overridden_virtual != EntityId::none) {
      const auto slot = out.slot_of(m.overridden_virtual);
      if (!slot) return std::nullopt;
      out.slots[*slot] = {m.entity, sema::VirtualSlotKind::function, m.is_pure};
    } else {
      out.slots.push_back({m.entity, sema::VirtualSlotKind::function, m.is_pure});
    }
    // The key function: declared in the class, not pure, and not inline there. An implicit
    // specialization has none: its members are defined wherever they are used.
    if (!head.template_depth && out.key_function == EntityId::none && !m.is_pure && !m.is_deleted) {
      const auto decl = member_decl(m.entity);
      const auto sem = decl ? decl_sema(*decl) : std::nullopt;
      if (!sem) return std::nullopt;
      using namespace sema::decl_flag;
      if (!(sem->flags & (definition | inline_ | constexpr_ | consteval_ | deleted))) out.key_function = m.entity;
    }
  }
  // An implicit destructor overrides a virtual base destructor ([class.dtor]).
  if (rec.virtual_destructor && rec.declared_destructor == EntityId::none) {
    if (rec.destructor == EntityId::none || !replace_destructor(rec.destructor, false)) return std::nullopt;
  }
  return out;
}

// A member's declaration: a class template specialization's own, not its pattern's.
std::optional<ast::DeclSema> Program::member_declaration(EntityId member) {
  const auto decl = member_decl(member);
  if (!decl) return std::nullopt;
  const auto of = member_patterns_.find(std::to_underlying(member));
  if (of == member_patterns_.end()) return decl_sema(*decl);
  for (const ast::DeclSema& d : w_->ins.seqs.elements(class_member_sema(of->second.item, of->second.spec).decls))
    if (d.entity == member) return d;
  return std::nullopt;
}

std::optional<u64> Program::class_size(EntityId cls) {
  const u32 key = std::to_underlying(cls);
  const auto written = class_items_.find(key);
  const auto pattern = class_patterns_.find(key);  // a specialization's
  if (written == class_items_.end() && pattern == class_patterns_.end()) return std::nullopt;
  const u32 item = pattern != class_patterns_.end() ? pattern->second : written->second;
  AtItem q(*this, item, true);
  q.own_class_complete = true;
  if (pattern != class_patterns_.end()) q.at_point = end();  // a specialization is complete where it is instantiated
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;
  sema::Sema s(*w_, b, q, diags, id(item), 0);
  return s.layout_of(w_->types.class_type(cls), false);
}

EntityId Program::synthesize_constructor(u32 item, EntityId cls, bool copy, bool nonthrowing, bool const_source) {
  sema::Types& types = w_->types;
  const QualType ct = types.class_type(cls);
  ast::Builder b(w_->ins, &arena_);
  const std::string cname(w_->ins.names.spelling(w_->entities.rec(cls).name));
  std::vector<QualType> ps;
  std::vector<ast::ParamSema> params;
  if (copy) {
    ps.push_back(types.lvalue_ref(const_source ? types.add_cv(ct, sema::Cv::const_) : ct));
    params.push_back({EntityId::none, ps[0], w_->ins.names.intern("other"), NodeId::none, 0});
  }
  const QualType type = types.function(types.builtin(sema::Builtin::void_), ps, false, LangLinkage::cxx, sema::Cv::none,
                                       ast::RefQual::none, nonthrowing);
  ast::DeclSema d{};
  d.kind = ast::DeclKind::function;
  d.type = type;
  d.linkage = ast::Linkage::external;
  d.storage = ast::Storage::none;
  d.access = ast::Access::public_;
  d.flags = sema::decl_flag::member | sema::decl_flag::implicit | sema::decl_flag::definition | sema::decl_flag::inline_ |
            sema::decl_flag::constructor;
  d.attributes = b.seq(std::span<const NodeId>{});
  d.constraint = NodeId::none;
  d.constraints = ast::ConstId::none;
  d.contracts = NodeId::none;
  d.params = b.seq(std::span<const ast::ParamSema>(params));
  d.template_params = b.seq(std::span<const ast::TemplateParamSema>{});
  d.entity = w_->entities.entity({sema::EntityKind::constructor, w_->ins.names.intern(cname), type, LangLinkage::cxx,
                                  ast::Linkage::external, {}, cls});
  synth_.push_back({item, b.commit(d), copy ? SpecialMember::copy_constructor : SpecialMember::default_constructor});
  return synth_.back().decl.entity;
}

// A dynamic class's implicit constructors install its vptr, so they are functions of their own;
// a class containing one copies it through that class's copy constructor ([class.copy.ctor]).
void Program::dynamic_special_members(u32 item, EntityId cls, sema::ClassRec& rec) {
  auto& types = w_->types;
  const QualType ct = types.class_type(cls);
  if (rec.union_ || rec.closure || types.is_dependent(ct)) return;
  std::vector<QualType> subobjects;
  for (const sema::BaseRec& b : rec.bases) subobjects.push_back(types.class_type(b.cls));
  for (const sema::FieldRec& f : rec.fields) subobjects.push_back(f.type);
  if (!rec.user_ctors && !rec.default_deleted && rec.defaulted_default_ctor == EntityId::none) {
    // [except.spec]: potentially throwing when a subobject's default initialization may be.
    // Default member initializers and user-declared constructors count as potentially throwing
    // (an approximation, #1157).
    const auto nothrow_default = [&](this auto&& self, QualType t) -> bool {
      t = types.innermost_array_element(t);
      if (!types.is_class(t)) return true;
      const auto* r = classes_.find(types.class_of(t));
      if (!r) return false;
      if (r->implicit_default_ctor != EntityId::none)
        return types.rec(w_->entities.rec(r->implicit_default_ctor).type).fn_noexcept;
      return !r->user_ctors && !r->closure && std::ranges::none_of(r->fields, &sema::FieldRec::dmi) &&
             std::ranges::all_of(r->bases, [&](const sema::BaseRec& b) { return self(types.class_type(b.cls)); }) &&
             std::ranges::all_of(r->fields, [&](const sema::FieldRec& f) { return self(f.type); });
    };
    const bool nonthrowing = std::ranges::none_of(rec.fields, &sema::FieldRec::dmi) &&
                             std::ranges::all_of(subobjects, nothrow_default);
    rec.implicit_default_ctor = synthesize_constructor(item, cls, false, nonthrowing, true);
    dynamic_synthesized_.emplace(rec.implicit_default_ctor, synth_.size() - 1);
  }
  // A virtual destructor defaulted on its first declaration is defined there ([dcl.fct.def.default]).
  // One that is not virtual waits in `defaulted_destructors_`, as in any class.
  if (rec.dynamic && rec.virtual_destructor && rec.declared_destructor != EntityId::none && rec.destructor == rec.declared_destructor) {
    const auto declaration = destructor_declarations_.find(rec.declared_destructor);
    auto sem = member_declaration(rec.declared_destructor);
    if (declaration != destructor_declarations_.end() && declaration->second.defaulted && sem) {
      sem->flags |= sema::decl_flag::definition | sema::decl_flag::inline_;
      synth_.push_back({item, *sem, SpecialMember::destructor});
    }
  }
  if (rec.copy_ctor == EntityId::none && !rec.copy_deleted && rec.defaulted_copy != sema::CopyState::unsupported &&
      std::ranges::none_of(rec.members, [&](const sema::MemberDeclRec& m) {
        if (m.is_ctor && copy_move_constructor_parameter(m, ct)) return true;
        const auto assignment = assignment_parameter(m, ct);
        return assignment && types.rec(*assignment).rvalue_ref;
      }))
  {
    rec.copy_ctor = synthesize_constructor(item, cls, true, false, rec.defaulted_copy_const);
    dynamic_synthesized_.emplace(rec.copy_ctor, synth_.size() - 1);
  }
}

void Program::complete_virtual_members(u32 item, EntityId cls, sema::ClassRec& rec, std::span<const ast::MemberRec> members,
                                       bool member_declarations_valid) {
  // A template's own class is no object's type: nothing lays it out or makes one ([temp.dep.type]).
  if (w_->types.is_dependent(w_->types.class_type(cls))) {
    rec.virtual_layout_unsupported = false;
    return;
  }
  if (rec.polymorphic) {
    const bool ordinary = w_->entities.rec(cls).templ == EntityId::none;
    if (ordinary) rec.peer_virtual_layout = peer_virtual_layout(item, cls, rec, members);
    if (ordinary && rec.peer_virtual_layout == sema::PeerVirtualLayout::none)
      rec.peer_automatic_primary_base = peer_automatic_primary_base(item, cls, rec, members);
    if (rec.peer_virtual_layout == sema::PeerVirtualLayout::none && rec.peer_automatic_primary_base == EntityId::none &&
        member_declarations_valid)
      rec.dynamic = dynamic_class(item, cls, rec, members);
  }
  rec.virtual_layout_unsupported = (rec.polymorphic && !rec.dynamic) ||
      std::ranges::any_of(rec.bases, [&](const sema::BaseRec& base) {
        return classes_.virtual_layout_unsupported(w_->types.class_type(base.cls));
      }) ||
      std::ranges::any_of(rec.fields, [&](const sema::FieldRec& field) {
        return classes_.virtual_layout_unsupported(field.type);
      });
  if (member_declarations_valid && rec.layout_requires_virtual_support && !rec.virtual_layout_unsupported)
    dynamic_special_members(item, cls, rec);
}

// [class.abstract]: the pure virtual functions that are final overriders, from its base's and its
// own. An immediate one counts as any other.
void Program::pure_final_overriders(sema::ClassRec& rec, EntityId cls) {
  if (!rec.polymorphic || w_->types.is_dependent(w_->types.class_type(cls))) return;
  if (rec.bases.size() == 1)
    if (const auto* base = classes_.find(rec.bases.front().cls)) rec.pure_final_overriders = base->pure_final_overriders;
  for (const auto& member : rec.members) {
    if (member.overridden_virtual != EntityId::none) std::erase(rec.pure_final_overriders, member.overridden_virtual);
    if (member.is_pure) rec.pure_final_overriders.push_back(member.entity);
  }
}

// [class.default.ctor]/2, [class.copy.ctor]/10: an implicit constructor whose initialization
// cannot be formed is deleted. Its body is analysed once the class is complete.
bool Program::default_initializes(const Synthesized& s, u32 point) {
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;
  AtItem q(*this, s.item, true);
  q.own_class_complete = true;
  q.at_point = point;
  sema::Sema sm(*w_, b, q, diags, id(s.item), 0);
  sm.current_class = w_->entities.rec(s.decl.entity).scope;
  sm.current_function = s.decl.entity;
  sm.in_function_body = true;
  sm.this_type = this_type(s.decl);
  sm.push_scope();
  const sema::ClassRec& rec = classes_.rec(sm.current_class);
  bool valid = true;
  for (const sema::BaseRec& base : rec.bases) {
    const QualType type = w_->types.class_type(base.cls);
    const auto* record = classes_.find(base.cls);
    valid &= (record && record->dynamic
                  ? sm.construct(type, {}, 0, 0, sema::Construction::default_init, sema::ConstructorDestination::polymorphic_base)
                  : sm.default_init(type, 0, 0)).ok();
  }
  for (const sema::FieldRec& f : rec.fields)
    if (!f.dmi && f.init == ConstId::none) valid &= !w_->types.is_reference(f.type) && sm.default_init(f.type, 0, 0).ok();
  sm.pop_scope();
  return valid && !sm.has_error_since(0);
}

void Program::validate_dynamic_constructors(sema::ClassRec& rec) {
  const auto valid = [&](EntityId ctor) {
    const auto synthesized = dynamic_synthesized_.find(ctor);
    if (synthesized == dynamic_synthesized_.end()) return true;
    const Synthesized& s = synth_[synthesized->second];
    const auto point = class_points_.find(std::to_underlying(w_->entities.rec(ctor).scope));
    if (point == class_points_.end()) return synthesized_body(synthesized->second).tree != NodeId::none;
    // A specialization's is defined at the end of the unit. Whether it is deleted is asked where
    // the class is instantiated, where its subobjects are complete, and does not depend on its
    // default member initializers ([temp.inst]/3).
    if (s.kind == SpecialMember::copy_constructor) return synthesize_body(s, point->second).tree != NodeId::none;
    return default_initializes(s, point->second);
  };
  if (rec.implicit_default_ctor != EntityId::none && !valid(rec.implicit_default_ctor)) {
    rec.implicit_default_ctor = EntityId::none;
    rec.default_deleted = true;
  }
  if (rec.copy_ctor != EntityId::none && !valid(rec.copy_ctor)) {
    rec.copy_ctor = EntityId::none;
    rec.copy_deleted = true;
  }
}

std::optional<Program::DerivedVirtualDestructor> Program::derived_virtual_destructor_layout(EntityId cls) const {
  using enum lex::Tk;
  const auto& target = w_->target;
  if (!target.supports(ast::NativeOperation::derived_virtual_destructor) || r_->has_clang_attribute_pragma)
    return std::nullopt;
  const auto* derived = classes_.find(cls);
  if (!derived || derived->dynamic || derived->bases.size() != 1 || !derived->fields.empty()) return std::nullopt;
  const auto& edge = derived->bases.front();
  if (edge.access != ast::Access::public_ || edge.dependent_in_pattern) return std::nullopt;
  const auto* base = classes_.find(edge.cls);
  if (!base || !base->bases.empty() || base->fields.size() != 1) return std::nullopt;

  const auto plain_destructor = [&](EntityId owner, const sema::ClassRec& record,
                                    bool primary) -> const items::Item* {
    const auto& entity = w_->entities.rec(owner);
    const auto definition = class_items_.find(std::to_underlying(owner));
    if (!record.defined || record.is_final || record.union_ || record.closure || record.is_abstract() ||
        !record.virtual_destructor || !record.destructor_usable || !record.destructor_noexcept.value_or(false) ||
        record.has_explicit_alignment() || record.unapplied_layout || !record.types.empty() || !record.friends.empty() ||
        entity.linkage != ast::Linkage::external || entity.templ != EntityId::none || !is_namespace(entity.scope) ||
        unnamed(namespace_path(entity.scope)) || w_->ins.names.spelling(entity.name).empty() ||
        definition == class_items_.end() || !peer_root_vtable_visible(owner)) return nullptr;
    const auto& head = items_->items[definition->second];
    const auto key = r_->tokens[head.begin].kind;
    if ((key != kw_class && key != kw_struct) || head.template_depth || head.specialization ||
        layout_attribute(*r_, head.brace_end, head.end, LayoutAttributes::any) ||
        std::ranges::count_if(head.members, [&](u32 k) {
          return items_->items[k].kind != ItemKind::access_spec;
        }) != (primary ? 2 : 1)) return nullptr;

    const auto declaration = member_decl(record.destructor);
    const auto member = std::ranges::find(record.members, record.destructor, &sema::MemberDeclRec::entity);
    if (!declaration || member == record.members.end() || !member->is_dtor || !member->is_virtual ||
        member->is_pure || member->is_deleted || member->is_final ||
        record.access_of(member->entity) != ast::Access::public_ || !asm_label(member->entity).empty()) return nullptr;
    const auto& written = items_->items[index(declaration->item)];
    if (written.parent != definition->second || written.kind != ItemKind::declaration) return nullptr;
    auto tokens = std::span(r_->tokens).subspan(written.begin, written.end - written.begin);
    const auto accept = [&](lex::Tk kind) {
      if (tokens.empty() || tokens.front().kind != kind) return false;
      tokens = tokens.subspan(1);
      return true;
    };
    const bool written_virtual = accept(kw_virtual);
    if ((primary && !written_virtual) || !accept(tilde) || tokens.empty() ||
        tokens.front().kind != identifier || r_->name(tokens.front()) != w_->ins.names.spelling(entity.name)) return nullptr;
    tokens = tokens.subspan(1);
    if (!accept(l_paren) || !accept(r_paren)) return nullptr;
    accept(kw_noexcept);
    if (!primary && !tokens.empty() && tokens.front().kind == identifier && r_->name(tokens.front()) == "override")
      tokens = tokens.subspan(1);
    return accept(semi) && tokens.empty() ? &written : nullptr;
  };

  const auto* base_destructor = plain_destructor(edge.cls, *base, true);
  if (!base_destructor || !plain_destructor(cls, *derived, false)) return std::nullopt;
  const auto& field = base->fields.front();
  const auto pointer = w_->types.rec(field.type);
  if (field.dmi || field.bit_width || field.no_unique_address ||
      base->access_of(field.entity) != ast::Access::public_ || pointer.kind != sema::TypeKind::pointer ||
      w_->types.is_function(pointer.inner)) return std::nullopt;
  // A canonical pointer can hide alignment carried through an alias or decltype. The
  // only other member is a directly written, unadorned pointer to a builtin type.
  const auto& members = items_->items[base_destructor->parent].members;
  const auto field_item = std::ranges::find_if(members, [&](u32 k) {
    const auto& source = items_->items[k];
    return &source != base_destructor && source.kind != ItemKind::access_spec;
  });
  if (field_item == members.end()) return std::nullopt;
  const auto& source = items_->items[*field_item];
  const auto tokens = std::span(r_->tokens).subspan(source.begin, source.end - source.begin);
  if (source.kind != ItemKind::declaration || tokens.size() < 4 || tokens.back().kind != semi) return std::nullopt;
  const auto name = tokens[tokens.size() - 2];
  if (name.kind != identifier || r_->name(name) != w_->ins.names.spelling(field.name)) return std::nullopt;
  const auto prefix = tokens.first(tokens.size() - 2);
  const auto star_token = std::ranges::find(prefix, star, &lex::Tok::kind);
  const auto cv = [](lex::Tok token) { return token.kind == kw_const || token.kind == kw_volatile; };
  if (star_token == prefix.begin() || star_token == prefix.end() ||
      !std::all_of(prefix.begin(), star_token, [&](lex::Tok token) { return cv(token) || Parser::simple_type_keyword(token.kind); }) ||
      !std::all_of(std::next(star_token), prefix.end(), cv)) return std::nullopt;

  // A dynamic root gets the vptr before its field. The derived record reuses that base at zero.
  const std::array fields{ast::scalar_placed(target.pointer_size, target.pointer_align)};
  const auto base_layout = ast::record_layout({}, fields, false, base->pod, std::to_underlying(edge.cls), &target);
  const std::array bases{base_layout.placed};
  const auto layout = ast::record_layout(bases, {}, false, derived->pod, std::to_underlying(cls));
  if (base_layout.size != 16 || base_layout.align != 8 || layout.size != 16 || layout.align != 8 ||
      layout.base_offsets.front() != 0) return std::nullopt;
  return DerivedVirtualDestructor{edge.cls, derived->destructor, base->destructor, layout.size};
}

std::optional<Program::DerivedVirtualDestructor> Program::derived_virtual_destructor_ownership(EntityId cls) const {
  auto layout = derived_virtual_destructor_layout(cls);
  if (!layout) return std::nullopt;
  const auto derived = decls_.find(std::to_underlying(layout->destructor));
  const auto base = decls_.find(std::to_underlying(layout->base_destructor));
  const auto definition = definitions_.find(std::to_underlying(layout->destructor));
  if (base == decls_.end() || base->second.size() != 1 || definitions_.contains(std::to_underlying(layout->base_destructor)) ||
      derived == decls_.end()) return std::nullopt;
  if (definition == definitions_.end()) {
    if (derived->second.size() != 1) return std::nullopt;
  } else {
    if (derived->second.size() != 2 || !in_global_scope(definition->second)) return std::nullopt;
    layout->owns_derived_key = true;
  }
  return layout;
}

bool Program::peer_root_destructor_observer(EntityId function, EntityId cls) const {
  using enum lex::Tk;
  const auto& observer = w_->entities.rec(function);
  const auto& root = w_->entities.rec(cls);
  if (observer.kind != sema::EntityKind::function || observer.templ != EntityId::none ||
      observer.lang != LangLinkage::cxx || observer.linkage != ast::Linkage::external ||
      observer.scope != root.scope || !asm_label(function).empty() || has_unapplied_declaration_attributes(function))
    return false;
  const auto& types = w_->types;
  const auto signature = types.rec(observer.type);
  if (!signature.fn_noexcept || signature.variadic || !types.is_void(signature.inner) ||
      signature.params.size() != 1 || !types.is_pointer(signature.params.front())) return false;
  const auto pointee = types.pointee(signature.params.front());
  if (!types.is_class(pointee) || types.class_of(pointee) != cls || sema::Types::cv(pointee) != sema::Cv::none)
    return false;

  // The complete unit may add an ABI-affecting redeclaration after the destructor. These
  // bare declarations name the root directly and carry no attributes or language linkage.
  const auto declarations = decls_.find(std::to_underlying(function));
  if (declarations == decls_.end()) return false;
  const auto scope = namespace_path(observer.scope);
  return std::ranges::all_of(declarations->second, [&](const auto& entry) {
    const auto& [item, decl] = entry;
    const auto& source = items_->items[item];
    if (source.kind != ItemKind::declaration || source.template_depth || source.specialization ||
        source.names.size() != 1 || ns_of(item) != scope ||
        in_attributed_namespace(item) || decl.hidden_visibility) return false;
    for (u32 parent = source.parent; parent != items::none; parent = items_->items[parent].parent)
      if (items_->items[parent].kind != ItemKind::namespace_def) return false;
    const auto tokens = std::span(r_->tokens).subspan(source.begin, source.end - source.begin);
    std::size_t at = 0;
    const auto accept = [&](lex::Tk kind) {
      if (at == tokens.size() || tokens[at].kind != kind) return false;
      ++at;
      return true;
    };
    const auto named = [&](NameId name) {
      return accept(identifier) && r_->name(tokens[at - 1]) == w_->ins.names.spelling(name);
    };
    if (!accept(kw_void) || !named(observer.name) || !accept(l_paren) || !named(root.name) || !accept(star)) return false;
    accept(identifier);  // optional parameter name
    return accept(r_paren) && accept(kw_noexcept) && accept(semi) && at == tokens.size();
  });
}

bool Program::supports_base_object_variant(EntityId function, const std::set<EntityId>& uncertain_layouts,
                                            std::vector<EntityId>* layout_inputs) const {
  using sema::EntityKind;
  const auto layout_known = [&](EntityId e) {
    if (uncertain_layouts.contains(e)) return false;
    if (layout_inputs) layout_inputs->push_back(e);
    return true;
  };
  const auto& fn = w_->entities.rec(function);
  if ((fn.kind != EntityKind::constructor && fn.kind != EntityKind::destructor) ||
      fn.linkage != ast::Linkage::external || fn.lang != ast::LangLinkage::cxx || fn.templ != EntityId::none ||
      r_->first_visibility_pragma || !layout_known(function) || !asm_label(function).empty()) return false;
  const auto* record = classes_.find(fn.scope);
  if (!record || !record->defined || record->union_ || !record->bases.empty() || record->polymorphic ||
      record->layout_requires_virtual_support || record->unapplied_layout ||
      !std::ranges::all_of(record->fields, [&](const sema::FieldRec& field) {
        const auto type = w_->types.rec(field.type);
        return field.bit_width == 0 && !field.no_unique_address && w_->types.is_scalar(field.type) &&
               (type.kind != sema::TypeKind::enum_ || layout_known(type.entity));
      })) return false;

  // Check every declaration, including class introductions and unannotated definitions reached
  // through a differently annotated namespace reopening.
  const auto plain_declarations = [&](EntityId entity, u16 excluded_flags) {
    if (has_unapplied_declaration_attributes(entity)) return false;
    const auto found = decls_.find(std::to_underlying(entity));
    return found != decls_.end() && std::ranges::all_of(found->second, [&](const auto& declared) {
      const auto& [item, decl] = declared;
      const auto& source = items_->items[item];
      const bool type_definition = source.kind == ItemKind::class_def || source.kind == ItemKind::enum_def;
      const u32 head_end = type_definition ? source.brace_begin : source.has_body() ? source.body_begin : source.end;
      return !(decl.flags & excluded_flags) && !decl.hidden_visibility && !source.specialization &&
             source.template_depth == 0 && !in_attributed_namespace(item) &&
             !layout_attribute(*r_, source.begin, head_end, LayoutAttributes::any) &&
             !(type_definition && layout_attribute(*r_, source.brace_end, source.end, LayoutAttributes::any));
    });
  };
  constexpr u16 excluded = sema::decl_flag::inline_ | sema::decl_flag::deleted | sema::decl_flag::template_;
  if (!plain_declarations(function, excluded)) return false;
  const auto plain_named_type = [&](EntityId scope) {
    while (!is_namespace(scope)) {
      const auto& owner = w_->entities.rec(scope);
      if ((owner.kind != EntityKind::class_ && owner.kind != EntityKind::enum_) ||
          owner.linkage != ast::Linkage::external || owner.templ != EntityId::none ||
          !layout_known(scope) ||
          w_->ins.names.spelling(owner.name).empty() || !plain_declarations(scope, sema::decl_flag::template_)) return false;
      scope = owner.scope;
    }
    return !unnamed(namespace_path(scope));
  };
  if (!plain_named_type(fn.scope)) return false;

  // Types named by the encoding can restrict linkage or visibility, or change the call ABI.
  // Classes passed by value and callback signatures require a broader call ABI check.
  const auto plain_type = [&](this auto&& check, QualType type) -> bool {
    const auto rec = w_->types.rec(type);
    switch (rec.kind) {
      using enum sema::TypeKind;
      case builtin: return true;
      case class_:
      case enum_: return plain_named_type(rec.entity);
      case pointer:
      case reference:
      case array: return check(rec.inner);
      case member_pointer: return check(rec.member_owner) && check(rec.inner);
      default: return false;
    }
  };
  const auto signature = w_->types.rec(fn.type);
  return signature.kind == sema::TypeKind::function && std::ranges::all_of(signature.params, [&](QualType param) {
    return !w_->types.is_class(param) && plain_type(param);
  });
}

bool Program::class_layout_uncertain(EntityId cls, const sema::ClassRec& record, u32 point) const {
  std::set<EntityId> visited;
  const auto annotated_declaration = [&](EntityId tag) {
    const auto found = decls_.find(std::to_underlying(tag));
    return found != decls_.end() && std::ranges::any_of(found->second, [&](const auto& declared) {
      const u32 item = declared.first;
      return item < point && !(declared.second.flags & sema::decl_flag::definition) &&
             record_layout_attribute(*r_, items_->items[item], applied_alignment_specifiers(item));
    });
  };
  auto uncertain = [&](this auto&& visit, EntityId c, const sema::ClassRec& rec) -> bool {
    if (!visited.insert(c).second) return false;
    const EntityId pattern = w_->entities.rec(c).templ;
    if (rec.unapplied_layout || annotated_declaration(c) ||
        (pattern != EntityId::none && annotated_declaration(pattern))) return true;
    const auto subobject = [&](EntityId sub) {
      const auto* layout = classes_.find(sub);
      return layout && visit(sub, *layout);
    };
    // A pointer or reference field uses the target's pointer layout. Only stored objects
    // contribute their own class layout, including the elements of member arrays.
    return std::ranges::any_of(rec.bases, [&](const sema::BaseRec& base) { return subobject(base.cls); }) ||
           std::ranges::any_of(rec.fields, [&](const sema::FieldRec& field) {
             const QualType type = w_->types.innermost_array_element(field.type);
             return w_->types.is_class(type) && subobject(w_->types.class_of(type));
           });
  };
  return uncertain(cls, record);
}

void Program::validate_class_alignment(u32 item, EntityId cls, sema::ClassRec& record,
                                      std::vector<engine::Diagnostic>& diags) {
  auto written = class_alignments_.find(item);
  const auto declared = decls_.find(std::to_underlying(cls));
  if (declared == decls_.end()) return;
  const auto& declarations = declared->second;
  auto prior_alignment = [&](const auto& declaration) {
    return declaration.first < item && class_alignments_.contains(declaration.first);
  };
  const auto& source = items_->items[item];
  if (written == class_alignments_.end()) {
    if (std::ranges::any_of(declarations, prior_alignment)) {
      const u32 at = source.names.empty() ? 0 : source.names.front().offset;
      diags.push_back({{std::to_underlying(id(item)), at}, sema::diag_id(Diag::class_alignment_missing), {}});
    }
    return;
  }

  auto& alignment = written->second;
  const bool needs_proven_layout = alignment.value || std::ranges::any_of(declarations, [&](const auto& declaration) {
    return prior_alignment(declaration) && class_alignments_.at(declaration.first).value != 0;
  });
  AtItem q(*this, item, false);
  ast::Builder b(w_->ins, &arena_);
  sema::Sema s(*w_, b, q, diags, id(item), 0);
  const auto layout = s.record_layout(record, cls);
  if (!layout) {
    if (needs_proven_layout) s.diag(alignment.at, Diag::not_supported, Tk::kw_alignas);
    return;
  }
  if (class_layout_uncertain(cls, record, item)) {
    if (needs_proven_layout) s.diag(alignment.at, Diag::class_alignment_uncertain_layout);
    return;
  }
  alignment.natural = layout->align;
  if (alignment.value && alignment.value < alignment.natural) {
    s.diag(alignment.at, Diag::class_alignment_weak);
    return;
  }

  // [dcl.align]: absent on a non-defining declaration is fine. A written zero takes the
  // completed class's natural alignment, and still requires a written specifier on the definition.
  for (const auto& declaration : declarations | std::views::filter(prior_alignment)) {
    const u32 previous = declaration.first;
    const auto& prior = class_alignments_.at(previous);
    if (prior.value && prior.value < alignment.natural) {
      s.diag_at({std::to_underlying(id(previous)), prior.at}, Diag::class_alignment_weak);
      return;
    }
    if (prior.effective(alignment.natural) != alignment.effective(alignment.natural)) {
      s.diag(alignment.at, Diag::class_alignment_mismatch);
      return;
    }
  }
  if (alignment.value > alignment.natural) record.explicit_alignment = alignment.value;
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
  bool member_declarations_valid = true;
  bool can_complete = true;
  ClassContent c = class_content(item, [&](u32 m) -> const ast::ItemSema& {
    const auto& member = item_sema(m);
    member_declarations_valid &= valid_member_declarations(member.diags);
    can_complete &= !items_with_failed_assertion_dependencies_.contains(m);
    if (class_assertion(m)) can_complete &= valid_class_assertion(member);
    return member.value;
  });
  std::vector<ast::MemberRec>& members = c.members;
  std::vector<sema::FieldRec>& fields = c.fields;
  std::vector<sema::MemberDeclRec>& statics = c.statics;
  std::vector<sema::NestedType>& nested = c.nested;
  if (cls && *cls != EntityId::none) {
    // Every unit that defines the class builds its own record (#115), and synthesizes the special
    // members its own object needs.
    sema::ClassRec rec;
    rec.member_declarations_valid = member_declarations_valid;
    if (const auto head = class_heads_.find(item); head != class_heads_.end()) rec.is_final = head->second.is_final;
    rec.union_ = class_rec_is_union(item);
    rec.unapplied_layout = has_unapplied_class_layout(item, c);
    rec.fields = std::move(fields);
    rec.members = std::move(statics);
    rec.types = std::move(nested);
    rec.alias_templates = std::move(c.alias_templates);
    if (auto b = class_bases_.find(std::to_underlying(*cls)); b != class_bases_.end()) rec.bases = b->second;
    for (const ast::MemberRec& m : members) rec.access.push_back({m.entity, m.access});
    rec.friends = std::move(c.friends);
    rec.adl_friends = c.adl_friends;
    if (!can_complete) failed_class_assertions_.insert(*cls);
    // Classify the written declarations before special_members appends implicit assignments.
    if (can_complete && member_declarations_valid) rec.trivial_copyability = class_trivial_copyability(item, *cls, rec);
    const bool implicit_move = can_complete && special_members(item, *cls, rec, &out.diags);
    pure_final_overriders(rec, *cls);
    if (can_complete) {
      complete_virtual_members(item, *cls, rec, members, member_declarations_valid);
      validate_class_alignment(item, *cls, rec, out.diags);
    }
    // Keep the written declarations for error recovery in member bodies. An assertion
    // failure still prevents completion and cannot supply layout or class properties.
    rec.defined = can_complete;
    // Complete-class checks see the full member set before any function bodies are read.
    classes_.publish(*cls, std::move(rec), [&](sema::ClassRec& fresh) {
      // First: default arguments and member initializers may compare objects of the class.
      complete_defaulted_comparisons(*cls, fresh);
      if (special_member_exception_tokens_.contains(fresh.declared_destructor))
        bind_destructor_and_defaults(*cls, fresh, out);
      else
        bind_member_defaults(*cls, fresh, out);
      for (const auto& member : fresh.members)
        if (member.is_ctor) bind_special_member_exception(*cls, fresh, out, member.entity);
      check_member_exception_patterns(*cls, fresh, out);
      if (member_declarations_valid && implicit_move) prepare_move_constructor(item, *cls, fresh);
      if (member_declarations_valid) check_default_member_initializers(fresh, out);
      validate_dynamic_constructors(fresh);
    });
  }
  ast::Builder b(w_->ins, &arena_);
  std::vector<ast::BaseSpec> bases;
  if (auto p = class_base_patterns_.find(item); p != class_base_patterns_.end()) {
    bases.push_back(p->second.base);
  } else if (cls && *cls != EntityId::none) {
    if (auto i = class_bases_.find(std::to_underlying(*cls)); i != class_bases_.end())
      for (const sema::BaseRec& base : i->second) bases.push_back({w_->types.class_type(base.cls), base.access, 0, 0});
  }
  def.bases = b.seq(std::span<const ast::BaseSpec>(bases));
  def.members = b.seq(std::span<const ast::MemberRec>(members));
  def.friends = b.seq(std::span<const ast::MemberRec>(c.adl_friends));
  def.synthesized = b.seq(std::span<const ast::DeclSema>{});
  def.nested = b.seq(std::span<const EntityId>{});
  out.value = b.commit(def);
  running_.pop_back();
  class_cells_.at(item) = std::move(out);
  return *class_cells_[item];
}

Program::ClassCompletion* Program::class_completion(EntityId cls) const {
  for (auto* completion = class_completion_; completion; completion = completion->outer)
    if (completion->cls == cls) return completion;
  return nullptr;
}

void Program::bind_destructor_and_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out) {
  ClassCompletion completion{cls, rec, out, class_completion_};
  struct RestoreCompletion {
    ClassCompletion*& current;
    ClassCompletion* outer;
    ~RestoreCompletion() { current = outer; }
  } restore{class_completion_, std::exchange(class_completion_, &completion)};

  // Preserve the presence of each written default during overload resolution. A selected call
  // replays an omitted argument using its declaration's scope. Keep the handles until both the
  // defaults and the destructor specification are bound, so a cached default still obeys scope.
  const ast::ExprInfo info{w_->types.builtin(sema::Builtin::dependent), ast::ValueCategory::prvalue,
                           sema::expr_flag::dependent};
  const NodeId expression = w_->ins.intern(ast::Dependent{info, {}}, 0);
  const auto calls = w_->ins.intern_seq(std::span<const u32>{});
  for (auto& member : rec.members) {
    auto written = member_default_tokens_.find(member.entity);
    if (member.is_template || written == member_default_tokens_.end()) continue;
    const auto decl = member_decls_.at(std::to_underlying(member.entity));
    const u32 item_begin = items_->items[index(decl.item)].begin;
    member.defaults.resize(written->second.size(), NodeId::none);
    for (std::size_t i = 0; i < written->second.size(); ++i) {
      if (!written->second[i]) continue;
      const auto [begin, end] = *written->second[i];
      member.defaults[i] = w_->ins.intern(ast::DefaultArg{info, {expression, 0}, decl.item,
                                                         begin - item_begin, end - item_begin, calls}, 0);
      completion.defaults.emplace(std::pair{member.entity, begin}, PendingMemberDefault{&member.defaults[i], i, {begin, end}});
    }
  }
  bind_member_defaults(cls, rec, out, &completion);
  resolve_destructor_exception(completion, nullptr, 0);
  for (auto& [key, pending] : completion.defaults) *pending.slot = pending.result;
}

std::optional<bool> Program::resolve_destructor_exception(ClassCompletion& completion, sema::Sema* caller, u32 at) {
  if (completion.destructor == ReplayState::active) {
    if (caller) caller->diag(at, Diag::recursive_noexcept);
    completion.destructor = ReplayState::finished;
    return std::nullopt;
  }
  if (completion.destructor == ReplayState::pending) {
    completion.destructor = ReplayState::active;
    bind_special_member_exception(completion.cls, completion.record, completion.out, completion.record.declared_destructor);
    completion.destructor = ReplayState::finished;
  }
  return completion.record.destructor_noexcept;
}

void Program::bind_special_member_exception(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out, EntityId entity) {
  auto tokens = special_member_exception_tokens_.extract(entity);
  if (tokens.empty()) return;
  ast::Builder b(w_->ins, &arena_);
  auto slot = special_member_exception_types_.try_emplace(entity, std::nullopt).first;
  const DeclId decl = member_decls_.at(std::to_underlying(entity));
  const u32 item = index(decl.item);
  const auto& it = items_->items[item];
  const auto& cell = item_cells_[item]->value;
  const auto d = w_->ins.seqs.at(cell.decls, ast::number_of(decl.ord));
  AtItem q(*this, item, false);
  q.completing_class = cls;
  q.special_member_exception = true;
  sema::Sema s(*w_, b, q, out.diags, decl.item, 0);
  s.current_class = cls;
  const bool destructor = (d.flags & sema::decl_flag::destructor) != 0;
  if (destructor) s.current_function = entity;
  s.this_type = w_->types.pointer(w_->types.class_type(cls));
  s.push_scope();
  for (const auto& param : w_->ins.seqs.elements(d.params))
    if (param.name != s.no_name())
      s.declare_local(param.name, param.type, sema::local_flag::parameter, 0, false);
  const auto [begin, end] = tokens.mapped();
  Parser p(*r_, it.begin, end, s);
  p.set_questions(q);
  p.seek(begin);
  Parser::Suffix spec;
  bool ok = true;
  bool nonthrowing = false;
  const auto before = s.diag_count();
  try {
    p.exception_specifier(spec, ok);
    nonthrowing = ok && p.noexcept_value(spec.noexcept_condition, ok);
  } catch (const TooDeep&) {
    return;
  }
  if (!ok || q.failed_class_completion_dependency || (destructor && s.has_error_since(before))) return;
  const auto signature = w_->types.rec(d.type);
  slot->second = w_->types.function(signature.inner, signature.params, signature.variadic,
                                    signature.linkage, signature.fn_cv, signature.fn_ref, nonthrowing);
  if (destructor) rec.destructor_noexcept = nonthrowing;
  out.demands.insert(out.demands.end(), s.demands.begin(), s.demands.end());
}

// An exception specification is a complete-class context ([class.mem.general]). Check the written
// expression with the template still unbound, before any specialization needs its value.
void Program::check_member_exception_patterns(EntityId cls, const sema::ClassRec& rec, Result<ast::ClassDef>& out) {
  ast::Builder b(w_->ins, &arena_);
  for (const auto& member : rec.members) {
    const auto specification = member_exceptions_.find(member.entity);
    if (specification == member_exceptions_.end() || !specification->second.condition_tokens) continue;
    const auto item = specification->second.item;
    const auto tokens = *specification->second.condition_tokens;
    if (member_exception_patterns_.contains(item)) continue;
    const auto written = member_decl(member.entity);
    const auto decl = written ? decl_sema(*written) : std::nullopt;
    if (!decl) continue;
    const u32 item_begin = items_->items[item].begin;
    AtItem source(*this, item, false);
    source.completing_class = cls;
    sema::Sema replay(*w_, b, source, out.diags, id(item), 0);
    replay.exception_type_traits = sema::Sema::ExceptionTypeTraits::checking_pattern;
    replay.current_class = cls;
    replay.current_function = member.entity;
    replay.this_type = this_type(*decl);
    replay.template_scopes = enclosing_heads(item);
    replay.template_scopes.push_back(w_->ins.seqs.elements(decl->template_params));
    replay.push_scope();
    const auto before = replay.diag_count();
    for (const auto& param : w_->ins.seqs.elements(decl->params))
      if (param.name != replay.no_name())
        replay.declare_local(param.name, param.type, sema::local_flag::parameter, 0, false);
    Parser parser(*r_, item_begin, tokens.second, replay);
    parser.set_questions(source);
    parser.seek(tokens.first);
    Parser::Suffix exception;
    bool ok = true;
    try {
      parser.exception_specifier(exception, ok);
      if (ok && exception.noexcept_condition && !replay.dependent(*exception.noexcept_condition))
        parser.noexcept_value(exception.noexcept_condition, ok);
    } catch (const TooDeep&) {
      ok = false;
    }
    if (!ok || !parser.at_end() || replay.has_error_since(before)) {
      member_exception_patterns_.emplace(item, std::nullopt);
      continue;
    }
    member_exception_patterns_.emplace(item, MemberExceptionPattern{
        std::move(replay.dependent_calls), std::move(replay.expansions),
        replay.exception_type_traits == sema::Sema::ExceptionTypeTraits::dependent});
    out.demands.insert(out.demands.end(), replay.demands.begin(), replay.demands.end());
  }
}

// A namespace-scope function template's deferred condition, analysed in its definition as a
// member template's is (`check_member_exception_patterns`): what does not depend on the template's
// parameters is checked here, and its dependent calls and pack expansions are kept for the
// specializations' replays ([temp.res], [temp.dep.candidate], [temp.variadic]).
void Program::check_function_exception_pattern(EntityId entity, std::vector<ast::TemplateParamSema> head,
                                               std::span<const ast::ParamSema> params, u32 item, sema::Sema& caller) {
  const auto specification = member_exceptions_.find(entity);
  if (specification == member_exceptions_.end() || !specification->second.condition_tokens) return;
  if (member_exception_patterns_.contains(item)) return;
  const auto tokens = *specification->second.condition_tokens;
  const u32 item_begin = items_->items[item].begin;
  AtItem source(*this, item, false);
  sema::Sema replay(*w_, caller.builder(), source, caller.diagnostics(), id(item), 0);
  replay.exception_type_traits = sema::Sema::ExceptionTypeTraits::checking_pattern;
  replay.current_function = entity;
  replay.template_scopes = enclosing_heads(item);
  replay.template_scopes.push_back(std::move(head));
  replay.push_scope();
  const auto before = replay.diag_count();
  for (const auto& param : params)
    if (param.name != replay.no_name())
      replay.declare_local(param.name, param.type, sema::local_flag::parameter, 0, false);
  Parser parser(*r_, item_begin, tokens.second, replay);
  parser.set_questions(source);
  parser.seek(tokens.first);
  Parser::Suffix exception;
  bool ok = true;
  try {
    parser.exception_specifier(exception, ok);
    if (ok && exception.noexcept_condition && !replay.dependent(*exception.noexcept_condition))
      parser.noexcept_value(exception.noexcept_condition, ok);
  } catch (const TooDeep&) {
    ok = false;
  }
  if (!ok || !parser.at_end() || replay.has_error_since(before)) {
    member_exception_patterns_.emplace(item, std::nullopt);
    return;
  }
  member_exception_patterns_.emplace(item, MemberExceptionPattern{
      std::move(replay.dependent_calls), std::move(replay.expansions),
      replay.exception_type_traits == sema::Sema::ExceptionTypeTraits::dependent});
  caller.demands.insert(caller.demands.end(), replay.demands.begin(), replay.demands.end());
}

// [class.mem.general]: validate the initializer with the full member set and its original
// lexical prefix. For a class template, [temp.res] checks the unbound pattern. [temp.inst]
// leaves dependent defaults for an initialization that selects them.
void Program::check_default_member_initializers(const sema::ClassRec& rec, Result<ast::ClassDef>& out) {
  ast::Builder builder(w_->ins, &arena_);
  for (const auto& field : rec.fields) {
    auto written = dmis_.find(std::to_underlying(field.entity));
    if (written == dmis_.end()) continue;
    Dmi& dmi = written->second;
    AtItem source(*this, dmi.item, false);
    source.checking_default_member = true;
    sema::Sema caller(*w_, builder, source, out.diags, id(dmi.item), 0);
    const std::size_t before = caller.diag_count();
    try {
      source.replay_default_member(caller, field.entity, EntityId::none);
    } catch (const TooDeep&) {
      // The parser already diagnosed excessive nesting.
    }
    // Implementation limits in a nested initializer belong to the initialization that uses it.
    const auto unsupported = sema::diag_id(Diag::not_supported);
    const auto discarded = std::remove_if(out.diags.begin() + before, out.diags.end(), [&](const auto& diagnostic) {
      return diagnostic.id == unsupported;
    });
    out.diags.erase(discarded, out.diags.end());
    dmi.invalid = caller.has_error_since(before);
    if (!dmi.invalid) out.demands.insert(out.demands.end(), caller.demands.begin(), caller.demands.end());
  }
}

NodeId Program::resolve_member_default(ClassCompletion& completion, EntityId function, PendingMemberDefault& pending,
                                      sema::Sema* caller, u32 at) {
  if (pending.state == ReplayState::active) {
    if (caller) caller->diag(at, Diag::recursive_noexcept);
    pending.state = ReplayState::finished;
    return NodeId::none;
  }
  if (pending.state == ReplayState::pending) {
    pending.state = ReplayState::active;
    const NodeId result = bind_member_default(completion.cls, function, pending.parameter, pending.tokens, completion.out);
    if (pending.state == ReplayState::active) pending.result = result;
    pending.state = ReplayState::finished;
  }
  return pending.result;
}

void Program::bind_member_defaults(EntityId cls, sema::ClassRec& rec, Result<ast::ClassDef>& out,
                                  ClassCompletion* completion) {
  for (sema::MemberDeclRec& member : rec.members) {
    auto tokens = member_default_tokens_.extract(member.entity);
    if (tokens.empty()) continue;
    member.defaults.resize(tokens.mapped().size(), NodeId::none);
    for (std::size_t i = 0; i < tokens.mapped().size(); ++i) {
      if (const auto& written = tokens.mapped()[i]) {
        if (completion && !member.is_template) {
          auto& pending = completion->defaults.at({member.entity, written->first});
          resolve_member_default(*completion, member.entity, pending, nullptr, 0);
        } else {
          member.defaults[i] = bind_member_default(cls, member.entity, i, *written, out);
        }
      }
    }
  }
}

NodeId Program::bind_member_default(EntityId cls, EntityId function, std::size_t parameter, std::pair<u32, u32> tokens,
                                  Result<ast::ClassDef>& out) {
  const auto decl = member_decls_.at(std::to_underlying(function));
  const u32 item = index(decl.item);
  const auto& it = items_->items[item];
  const auto& cell = item_cells_[item]->value;
  const auto params = w_->ins.seqs.elements(w_->ins.seqs.at(cell.decls, ast::number_of(decl.ord)).params);
  const auto [begin, end] = tokens;
  AtItem q(*this, item, false);
  q.completing_class = cls;
  q.class_default_argument_start = begin;
  ast::Builder b(w_->ins, &arena_);
  sema::Sema s(*w_, b, q, out.diags, decl.item, 0);
  s.current_class = cls;
  s.push_scope();
  for (const auto& param : std::span(params).first(parameter + 1))
    s.declare_local(param.name, param.type, sema::local_flag::parameter | sema::local_flag::default_argument_parameter, 0, false);
  Parser p(*r_, it.begin, end, s);
  p.set_questions(q);
  p.seek(begin);
  const auto before = s.diag_count();
  NodeId arg;
  try {
    arg = p.default_argument(params[parameter].type);
  } catch (const TooDeep&) {
    return NodeId::none;
  }
  if (q.failed_class_completion_dependency || arg == NodeId::none || !p.at_end()) {
    if (!q.failed_class_completion_dependency && s.diag_count() == before) {
      if (p.at_end()) s.diag(p.pos(), Diag::expected_expression);
      else s.diag(p.pos(), Diag::unexpected_token, p.kind());
    }
    return NodeId::none;
  }
  out.demands.insert(out.demands.end(), s.demands.begin(), s.demands.end());
  return b.commit(ast::Child{arg, 0}).id;
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
    q_.satisfying_atom = true;
    q_.instantiation_calls = p.world().ins.seqs.elements(a.calls);
  }

  // Satisfied or not. An empty result means the initializer already reported a hard error.
  std::optional<bool> run(std::vector<engine::Diagnostic>& hard) {
    auto& ins = p_->world().ins;
    s_.current_class = atom_.lookup_class;
    // A lambda in it belongs to no declaration: its closure type is this satisfaction's own.
    s_.satisfying_atom = true;
    s_.declaration_part = true;
    sema::Fp128 closures = ast::mix(ast::seed("ycxx.parse.atom-closure"), ins.items.fp(atom_.item));
    closures = ast::mix(ast::mix(closures, u64(atom_.begin)), u64(atom_.end));
    for (ConstId e : ins.seqs.elements(atom_.mapping))
      closures = e == ConstId::none ? ast::mix(closures, u64(0)) : ast::mix(closures, ins.values.fp(e));
    // The pack expansions its definition recorded, at the atom's own offsets.
    if (const auto* recorded = p_->expansions(k_, 0)) {
      for (sema::Expansion x : *recorded)
        if (x.begin >= atom_.begin && x.end <= atom_.end) expansions_.push_back({x.begin - atom_.begin, x.end - atom_.begin, x.packs});
      s_.expansions_override = &expansions_;
    }
    const EntityId around = atom_.lookup_class != EntityId::none ? atom_.lookup_class : p_->namespace_entity(p_->ns_of(k_));
    const sema::Sema::ClosureOwnerScope owner(s_, sema::Sema::ClosureOwner{around, ast::Linkage::internal, closures});
    for (auto& head : p_->enclosing_heads(k_)) s_.template_scopes.push_back(std::move(head));
    const ast::ItemSema& is = p_->item_sema(k_).value;
    if (ins.seqs.size(is.decls) > 0) {
      ast::DeclSema d = ins.seqs.at(is.decls, 0);
      if (ins.seqs.size(d.template_params) > 0) s_.template_scopes.push_back(ins.seqs.elements(d.template_params));
    }
    sema::Templates t = s_.templates();
    // Spelled in a member of a class template specialization (a member template's constraint or
    // explicit-specifier): the enclosing class templates' parameters have the specialization's
    // arguments, as when the member was instantiated ([temp.inst]).
    for (EntityId c = atom_.lookup_class; c != EntityId::none; c = p_->world().entities.rec(c).scope) {
      const auto kind = p_->world().entities.rec(c).kind;
      if (kind != sema::EntityKind::class_ && kind != sema::EntityKind::class_template) break;
      if (auto args = p_->class_args_.find(std::to_underlying(c)); args != p_->class_args_.end()) {
        s_.template_args = args->second;
        break;
      }
    }
    if (s_.template_args.levels.size() > atom_.depth) s_.template_args.levels[atom_.depth].clear();
    s_.template_args.levels.resize(atom_.depth + 1);
    for (ConstId e : ins.seqs.elements(atom_.mapping))
      s_.template_args.levels[atom_.depth].push_back(e == ConstId::none ? std::nullopt : std::optional(t.mapping_arg(e)));
    // The function's parameters, of their substituted types, for an atom that names them.
    if (atom_.fn_params && ins.seqs.size(is.decls) > 0) s_.push_scope();
    if (atom_.fn_params && ins.seqs.size(is.decls) > 0)
      for (const ast::ParamSema& prm : ins.seqs.elements(ins.seqs.at(is.decls, 0).params)) {
        if (s_.types().kind(prm.type) == sema::TypeKind::pack_expansion) {
          const std::array pattern{t.type_arg(prm.type)};
          auto expanded = t.substitute_args(pattern, s_.template_args);
          if (expanded && std::ranges::none_of(*expanded, [&](const ast::TemplateArg& arg) { return s_.dependent(arg); })) {
            s_.declare_param_pack(prm.name);
            for (const ast::TemplateArg& element : *expanded)
              s_.declare_pack_element(prm.name, element.type);
            continue;
          }
        }
        auto ty = t.substitute(prm.type, s_.template_args);
        s_.declare_local(prm.name, ty ? *ty : prm.type, sema::local_flag::parameter, 0, false);
      }
    // A value argument without a type of its own has its parameter's, substituted
    // ([temp.arg.nontype]). Verify enum conversions after a dependent mapping becomes concrete.
    for (const auto& head : s_.template_scopes) {
      if (head.empty() || head[0].depth != atom_.depth) continue;
      auto& mapped = s_.template_args.levels[atom_.depth];
      for (std::size_t i = 0; i < head.size() && i < mapped.size(); ++i) {
        auto& arg = mapped[i];
        if (!arg || arg->kind != ast::TemplateArgKind::value) continue;
        auto type = t.substitute(head[i].type, s_.template_args);
        if (!type || s_.types().is_dependent(*type)) continue;
        if (s_.types().is_enum(*type) || (ins.values.is<ast::VInt>(arg->value) && s_.types().is_enum(arg->type))) {
          if (!ins.values.is<ast::VInt>(arg->value)) return false;
          auto converted = t.converted_value(*arg, *type);
          if (!converted) return false;
          arg = *converted;
        } else if (arg->type == QualType{}) {
          arg->type = *type;
        }
      }
    }
    Ex e = ps_.expression();
    bool failed = !e.ok() || !ps_.at_end();
    for (const engine::Diagnostic& d : sink_) {
      if (sema::survives_substitution(d.id)) hard.push_back(d);
      failed = true;
    }
    if (q_.observed_type_trait_failure) return std::nullopt;
    if (failed) return false;
    QualType ty = sema::Types::unqualified(s_.types().canonical(s_.rvalue(e).type()));
    if (!s_.types().is(ty, sema::Builtin::bool_)) {
      s_.diag(0, Diag::constraint_not_bool, ty);
      hard.insert(hard.end(), sink_.begin(), sink_.end());
      return false;
    }
    sema::Evaluated v = sema::Evaluator(s_, q_).evaluate(s_.rvalue(e));
    if (v.type_trait_failure) return std::nullopt;
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
  std::vector<sema::Expansion> expansions_;
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
  auto r = AtomAnalysis(*this, w_->ins.values.get<ast::VAtom>(c), point).run(hard);
  if (!r || !hard.empty()) atom_errors_.insert(key);
  // A limit met in the atom's tokens may have been met already where they were first analysed.
  const std::vector<engine::Diagnostic>& seen = item_sema(index(w_->ins.values.get<ast::VAtom>(c).item)).diags;
  for (engine::Diagnostic& d : hard)
    if (!std::ranges::contains(end_diags_, d) && !std::ranges::contains(seen, d)) end_diags_.push_back(std::move(d));
  atoms_[key] = r.value_or(false);
  return atoms_[key];
}

bool Program::member_value(EntityId e, MemberDemand demand) {
  const u32 key = std::to_underlying(e);
  auto it = lazy_members_.find(key);
  if (it == lazy_members_.end() || it->second.state == LazyMember::State::running) return true;
  const auto demand_key = std::pair(e, demand);
  if (it->second.dependent_layout) {
    // Each demand has its own completeness context, even if another query in the group already
    // reached this member. One successful definition supplies its ordinary specialization identity.
    if (auto checked = member_layout_demands_.find(demand_key); checked != member_layout_demands_.end()) return checked->second;
  } else if (it->second.state == LazyMember::State::done) {
    return !member_failed(e);
  }
  it->second.state = LazyMember::State::running;  // a use in its own initializer finds no value
  LazyMember m = it->second;
  // [temp.inst]/16: initializers instantiated through each other are limited as classes are; past
  // the limit, reported once, nothing more is instantiated.
  if (depth_reported_ || members_running_ >= instantiation_limit) {
    if (!depth_reported_)
      end_diags_.push_back({engine::Loc{std::to_underlying(id(m.item)), 0}, sema::diag_id(sema::Diag::instantiation_depth),
                            {engine::diag_arg(instantiation_limit)}});
    depth_reported_ = true;
    lazy_members_.at(key).state = LazyMember::State::done;
    return false;
  }
  ++members_running_;
  const auto layout_demand = m.dependent_layout ? std::optional(demand) : std::nullopt;
  ItemAnalysis analysis(*this, m.item, m.inst, m.ordinal, layout_demand);
  Result<ast::ItemSema> r = analysis.run();
  --members_running_;
  if (r.diags.empty()) {
    if (!lazy_members_.at(key).initializer) {
      lazy_members_.at(key).initializer = member_initializers_.size();
      member_initializers_.push_back({e, m.item, r.value.closures, std::move(r.demands)});
    }
    for (const auto& closure : w_->ins.seqs.elements(r.value.closures))
      for (EntityId f : {closure.call, closure.invoker})
        if (f != EntityId::none) declaration_closures_.at(std::to_underlying(f)).lazy_member = e;
  }
  // What the template's definition diagnosed it says once, there.
  if (item_sema(m.item).diags.empty())
    for (engine::Diagnostic& d : r.diags)
      if (!std::ranges::contains(end_diags_, d)) end_diags_.push_back(std::move(d));
  lazy_members_.at(key).state = LazyMember::State::done;
  const bool ready = m.dependent_layout ? analysis.has_member_initial_value() : !member_failed(e);
  if (m.dependent_layout) member_layout_demands_.emplace(demand_key, ready);
  return ready;
}

bool Program::require_enum_definition(EntityId e, u32 point) {
  const auto found = scoped_enum_definitions_.find(e);
  if (found == scoped_enum_definitions_.end()) return true;
  auto& [item, state] = found->second;
  using State = ScopedEnumDefinition::State;
  if (state != State::pending) return state != State::failed;
  const auto& pattern = item_sema(item);
  if (!valid_member_declarations(pattern.diags) || too_deep(scoped_enum_depth_, item)) {
    state = State::failed;
    return false;
  }
  const EntityId owner = w_->entities.rec(e).scope;
  state = State::running;
  ++scoped_enum_depth_;
  const auto& args = class_args_.at(std::to_underlying(owner));
  auto result = ItemAnalysis(*this, item, ClassInstantiation{owner, args, point}).replay_scoped_enum(e);
  --scoped_enum_depth_;
  state = classes_.find_enum(e)->defined ? State::complete : State::failed;
  for (auto& diag : result.diags)
    if (!std::ranges::contains(pattern.diags, diag) && !std::ranges::contains(end_diags_, diag))
      end_diags_.push_back(std::move(diag));
  return state == State::complete;
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
                : std::unique_ptr<sema::Questions>(std::make_unique<AtItem>(p, item, !generic_ || !generic_->g.part))),
        s_(p.world(), b_, *q_, out_.diags, p.id(item), begin()),
        ps_(p.region(), it_->begin + begin(), generic_ && generic_->g.part ? it_->end : it_->body_end, s_), inst_(std::move(inst)) {
    ps_.set_questions(*q_);
    ps_.body = true;
    at_ = inst_ ? &static_cast<AtInstantiation&>(*q_).pattern_context() : &static_cast<AtItem&>(*q_);
    if (!inst_ && generic_) static_cast<AtItem&>(*q_).generic_at_end = true;
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
    if (generic_ && generic_->g.part) {
      // In a declaration part, which may still be being analysed: what surrounds the lambda is in
      // its record.
      const EntityId cls = generic_->g.part->lookup_class;
      const bool specialization = cls != EntityId::none && p_->world().entities.rec(cls).templ != EntityId::none;
      if (specialization && !p_->class_items_.contains(std::to_underlying(cls))) at_->instantiating = cls;
      // At the point of the use, unless the class the lambda is in is still being defined there.
      if (at_->instantiating != EntityId::none || !p_->defining_outer_class(k_)) at_->at_point = generic_->point;
      s_.current_class = cls;
      s_.declaration_part = true;
      return generic_body();
    }
    const ast::ItemSema& is = p_->item_sema(k_).value;
    std::optional<ast::DeclSema> f;
    if (p_->world().ins.seqs.size(is.decls) > 0) f = p_->completed_declaration(p_->world().ins.seqs.at(is.decls, 0));
    std::optional<ast::DeclSema> pattern = f;
    const bool function_template = pattern && p_->world().ins.seqs.size(pattern->template_params) > 0;
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
      if (function_template)
        s_.template_scopes.push_back(p_->world().ins.seqs.elements(pattern->template_params));
      if (inst_) s_.template_args = inst_->args;
      // A member function's body sees its class's members ([basic.scope.class]) and `this`; a
      // body is looked up in the namespace of what it defines ([basic.lookup.unqual]/9).
      EntityId scope = p_->world().entities.rec(f->entity).scope;
      s_.current_function = f->entity;
      s_.in_function_body = true;
      s_.checking_virtual_body = f->is_virtual;
      s_.variadic_function_body = signature.variadic;
      // Its closures' members are defined wherever it is: an inline function, a member defined in
      // its class, or a specialization ([dcl.inline], [temp.inst]).
      s_.function_inline = (f->flags & sema::decl_flag::inline_) || inst_ ||
                           (it_->parent != items::none && p_->items().items[it_->parent].kind == ItemKind::class_def);
      if (f->flags & sema::decl_flag::member) {
        s_.current_class = scope;
        s_.this_type = p_->this_type(*f);
        if (inst_ && p_->classes_instantiating_.contains(std::to_underlying(s_.current_class)))
          static_cast<AtInstantiation&>(*q_).instantiating_class = s_.current_class;
        scope = p_->world().entities.rec(scope).scope;
      } else if (f->flags & sema::decl_flag::friend_) {
        // [class.mem.general]: the body is a complete-class context even though the
        // function belongs to the namespace and has no implicit object parameter. A class
        // template's friend is instantiated in the specialization that defines it.
        const EntityId templ = p_->world().entities.rec(f->entity).templ;
        const auto friend_of = p_->member_patterns_.find(std::to_underlying(templ != EntityId::none ? templ : f->entity));
        s_.current_class = inst_ && friend_of != p_->member_patterns_.end() ? friend_of->second.spec
                                                                             : p_->class_entity(it_->parent);
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
      std::vector<u32> param_locals;
      for (const ast::ParamSema& prm : p_->world().ins.seqs.elements(f->params)) {
        if (prm.flags & sema::param_flag::pack_element) s_.declare_pack_element(prm.name, prm.type);
        else param_locals.push_back(s_.declare_local(prm.name, prm.type, sema::local_flag::parameter, 0, false));
      }
      bool ctor = f->flags & sema::decl_flag::constructor;
      ConstructorInitializers inits;
      if (ctor && ps_.is(lex::Tk::colon)) inits = mem_initializers();
      const bool defaulted = p_->defaulted_body(k_);
      if (defaulted && p_->defaulted_comparison(f->entity) &&
          s_.ins().names.spelling(p_->world().entities.rec(f->entity).name) == "operator<=>") {
        root = defaulted_three_way_body(*f, param_locals);
      } else if (defaulted && p_->defaulted_comparison(f->entity)) {
        // [class.compare.default]: the implicit definition of a defaulted comparison that is
        // not deleted, `return x == y;` as [class.eq] expands it.
        if (auto result = p_->defaulted_comparison_result(s_, *f, param_locals); result && result->ok()) {
          Ex ret = s_.node(ast::Return{sema::Sema::at(*result, 0)}, 0, 0);
          root = s_.node(ast::Compound{b_.list(std::vector<ast::Child>{sema::Sema::at(ret, 0)})}, 0, 0);
        }
      } else if (!ps_.is(lex::Tk::l_brace)) {
        // A failed class keeps its written comparison declaration without completing its body.
        const bool failed_comparison = defaulted && p_->failed_class_assertions_.contains(s_.current_class) &&
            s_.ins().names.spelling(p_->world().entities.rec(f->entity).name) == "operator==";
        if (!failed_comparison) s_.diag(ps_.pos(), sema::Diag::not_supported, ps_.kind());
      } else {
        ps_.dependent_decay_copy_body = function_template && !inst_;
        root = ps_.compound_statement();
        ps_.dependent_decay_copy_body = false;
        if (!ps_.at_end()) s_.diag(ps_.pos(), sema::Diag::unexpected_token, ps_.kind());
        s_.check_labels();  // labels have function scope ([stmt.label]/2)
        // No return statement deduces the placeholder as `return;` would ([dcl.type.auto.deduct]/2).
        if (root.ok() && s_.deduces_return() && !s_.return_deduced() && !s_.return_seen() && !templated_pattern())
          if (!s_.deduce_return(nullptr, ps_.pos() > 0 ? ps_.pos() - 1 : 0)) root = {};
      }
      s_.pop_scope();
      const bool dtor = f->flags & sema::decl_flag::destructor;
      if (root.ok() && (dtor || (ctor && !inits.delegation)) && !check_subobject_destructors(root.begin)) root = {};
      if (root.ok() && ctor) root = constructor_body(std::move(inits), root, signature.fn_noexcept);
      if (root.ok() && dtor) root = destructor_body(s_, s_.current_class, root);
    }
    std::vector<ast::LocalRec> locals(s_.locals().begin(), s_.locals().end());
    // A template's definition demands nothing; its instantiations do. What it keeps for them is
    // which calls were dependent ([temp.dep.candidate]) and its pack expansions (ast.md §6.4).
    bool templated = function_template || !p_->enclosing_heads(k_).empty();
    std::vector<sema::Expansion> none;
    std::vector<u32> no_calls;
    if (at_->named_pending_member && !s_.has_error_since(0))
      s_.diag(0, sema::Diag::incomplete_type, s_.types().class_type(s_.current_class));
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

  // [class.spaceship]/3: the implicit definition of a defaulted three-way comparison that is not
  // deleted. Each subobject's comparison `vi`, converted to R, is returned when `vi != 0`;
  // otherwise `static_cast<R>(std::strong_ordering::equal)`:
  //   { R v0 = x0 <=> y0; if (v0 != 0) return v0; …; return R(std::strong_ordering::equal); }
  Ex defaulted_three_way_body(const ast::DeclSema& f, std::span<const u32> params) {
    const auto operands = p_->comparison_operands(s_, f, params);
    if (!operands) return {};
    const auto parts = p_->defaulted_three_way(s_, operands->cls, operands->x, operands->y, s_.types().rec(f.type).inner);
    if (!parts) return {};
    // A deduced return type is the comparison category ([class.spaceship]/2).
    if (s_.deduces_return() && !s_.deduce_return(&parts->equal, 0)) return {};
    std::vector<ast::Child> statements;
    for (const auto& part : parts->subobjects) {
      if (!part.return_value.ok()) return {};
      const auto decl = b_.list(std::vector<ast::Child>{sema::Sema::at(part.variable, 0)});
      statements.push_back(sema::Sema::at(s_.node(ast::DeclStmt{decl}, 0, 0), 0));
      const Ex ret = s_.node(ast::Return{sema::Sema::at(part.return_value, 0)}, 0, 0);
      statements.push_back(sema::Sema::at(
          s_.node(ast::If{{}, sema::Sema::at(part.unequal, 0), sema::Sema::at(ret, 0), {}}, 0, 0), 0));
    }
    statements.push_back(sema::Sema::at(s_.node(ast::Return{sema::Sema::at(parts->equal, 0)}, 0, 0), 0));
    return s_.node(ast::Compound{b_.list(statements)}, 0, 0);
  }

  // [class.default.ctor]/4: the implicitly defined default constructor of a dynamic class
  // initializes as a user-written one with no ctor-initializer and an empty body would. Its
  // default member initializers are analysed here, in the complete class (#309).
  // The default constructor defaulted on its first declaration is defined the same way: its item is a
  // member of its class, whose head encloses it. The arguments of a specialization are bound.
  Result<ast::BodySema> implicit_default_constructor(const ast::DeclSema& ctor) {
    at_->own_class_complete = true;
    if (inst_) {
      for (auto& head : p_->enclosing_heads(k_)) s_.template_scopes.push_back(std::move(head));
      if (it_->kind == ItemKind::class_def)
        s_.template_scopes.push_back(p_->world().ins.seqs.elements(p_->world().ins.seqs.at(p_->item_sema(k_).value.decls, 0).template_params));
      s_.template_args = inst_->args;
    }
    s_.current_class = p_->world().entities.rec(ctor.entity).scope;
    s_.current_function = ctor.entity;
    s_.in_function_body = true;
    s_.function_inline = true;
    s_.this_type = p_->this_type(ctor);
    s_.push_scope();
    Ex body = s_.node(ast::Compound{b_.list({})}, 0, 0);
    Ex root = constructor_body({}, body, s_.types().rec(ctor.type).fn_noexcept);
    s_.pop_scope();
    if (s_.has_error_since(0)) root = {};  // a deleted constructor (`validate_dynamic_constructors`)
    std::vector<ast::LocalRec> locals(s_.locals().begin(), s_.locals().end());
    std::vector<sema::Expansion> none;
    out_.value = b_.commit(ast::BodySema{root.ok() ? root.id : NodeId::none, b_.seq(std::span<const ast::LocalRec>(locals)),
                                         expansion_list(b_, none), b_.seq(std::span<const u32>{}),
                                         s_.builtin(sema::Builtin::void_), 0,
                                         b_.seq(std::span<const ast::ClosureDef>(s_.closures))});
    out_.demands = std::move(s_.demands);
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
    if (!gi.g.expansions.empty() || gi.g.part) s_.expansions_override = &gi.g.expansions;
    s_.open_generic(gi.g, gi.spec, gi.spec_type, gi.args);
    ps_.seek(it_->begin + begin() + gi.g.body_begin);
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

  // A destructor's body ([class.dtor]): `body`, then its non-variant members destroyed, the last first,
  // then its direct bases, the last first (a `DestroyMember` of field `~0u`).
  static Ex destructor_body(sema::Sema& s, EntityId cls, Ex body) {
    const sema::ClassRec& rec = s.classes().rec(cls);
    if (rec.union_) return body;
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
  // [class.base.init], [class.dtor]: these bodies potentially invoke the non-variant subobject
  // destructors, including those requiring no runtime cleanup.
  bool check_subobject_destructors(u32 at) {
    const auto& record = s_.classes().rec(s_.current_class);
    if (record.union_) return true;  // the constructor checks its selected variant separately
    return std::ranges::all_of(record.bases, [&](const sema::BaseRec& base) {
      return s_.check_destructor_access(s_.types().class_type(base.cls), at, Diag::base_unusable_destructor,
                                        /*base_subobject=*/true);
    }) && std::ranges::all_of(record.fields, [&](const sema::FieldRec& field) {
      return s_.check_destructor_access(field.type, at);
    });
  }

  struct ConstructorInitializers {
    std::map<u32, Ex> subobjects;
    std::optional<Ex> delegation;  // engaged even when a delegating initializer failed
  };

  // Skip an initializer after an error, leaving the next comma or body brace.
  void skip_mem_initializer() {
    if (ps_.is(Tk::l_paren)) {
      for (u32 n = ps_.past_parentheses(0); n > 0; --n) ps_.bump();
    } else if (ps_.is(Tk::l_brace)) {
      u32 depth = 0;
      do {
        if (ps_.is(Tk::l_brace)) ++depth;
        if (ps_.is(Tk::r_brace)) --depth;
        ps_.bump();
      } while (depth && !ps_.at_end());
    }
  }

  // mem-initializer-list ([class.base.init]): `: m(args), n{…}`, each subobject's initializer by
  // its position among the class's members and bases, or a sole delegating initializer.
  ConstructorInitializers mem_initializers() {
    ConstructorInitializers r;
    const sema::ClassRec& rec = p_->classes_.rec(s_.current_class);
    QualType ct = s_.types().class_type(s_.current_class);
    bool seen_initializer = false;
    bool reported_mixed = false;
    ps_.bump();
    do {
      if (!ps_.is(Tk::identifier)) {
        s_.diag(ps_.pos(), Diag::expected_unqualified_id);
        if (r.delegation) r.delegation.emplace();
        return r;
      }
      const bool had_initializer = std::exchange(seen_initializer, true);
      u32 at = ps_.pos();
      NameId n = ps_.name_of(ps_.tok());
      ps_.bump();
      u32 field = 0;
      while (field < rec.fields.size() && rec.fields[field].name != n) ++field;
      std::optional<QualType> type;
      auto destination = sema::ConstructorDestination::object;
      if (field < rec.fields.size()) {
        type = rec.fields[field].type;
      } else {
        // [class.base.init]/2: a direct base or the constructor's class, ignoring parameters.
        auto named = s_.class_name(n, false, true, std::nullopt, /*search_locals=*/false);
        if (named && s_.types().is_class(*named)) {
          EntityId cls = s_.types().class_of(*named);
          auto base = std::ranges::find(rec.bases, cls, &sema::BaseRec::cls);
          if (cls == s_.current_class) {
            type = ct;
            destination = sema::ConstructorDestination::delegation;
          } else if (base != rec.bases.end()) {
            type = s_.types().class_type(base->cls);
            field = u32(rec.fields.size() + (base - rec.bases.begin()));
            if (const auto* record = p_->classes_.find(cls); rec.bases.size() == 1 && record && record->polymorphic)
              destination = sema::ConstructorDestination::polymorphic_base;
          }
        }
      }
      const bool delegates = destination == sema::ConstructorDestination::delegation;
      if (r.delegation || (delegates && had_initializer)) {
        if (!std::exchange(reported_mixed, true)) s_.diag(at, Diag::delegating_initializer_not_alone);
        r.delegation.emplace();
        skip_mem_initializer();
        continue;
      }
      if (!type) {
        s_.diag(at, Diag::no_member, n, ct);
        skip_mem_initializer();
        continue;
      }
      if (delegates) {
        r.delegation.emplace();
        if (!s_.template_scopes.empty() || inst_) {
          s_.diag(at, Diag::not_supported, Tk::colon);
          skip_mem_initializer();
          continue;
        }
      }
      QualType ft = *type;
      Ex init;
      if (ps_.is(Tk::l_brace)) {
        auto list = ps_.braced_init_list();
        init = destination == sema::ConstructorDestination::object
                   ? s_.list_init(ft, *list)
                   : s_.construct_from_list(ft, *list, sema::Construction::direct_list, destination);
      } else {
        u32 open = ps_.pos();
        if (!ps_.expect(Tk::l_paren)) return r;
        std::vector<Ex> args;
        if (!ps_.is(Tk::r_paren))
          do args.push_back(ps_.assignment_expression());
          while (ps_.accept(Tk::comma));
        if (!ps_.expect(Tk::r_paren)) return r;
        if (destination != sema::ConstructorDestination::object) {
          init = s_.construct(ft, std::move(args), open, ps_.pos(), sema::Construction::direct, destination);
        } else if (s_.types().is_class(ft)) {
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
      if (delegates) r.delegation = init;
      else {
        if (r.subobjects.contains(field)) s_.diag(at, Diag::redeclared_member, n);  // [class.base.init]/3
        r.subobjects[field] = init;
      }
    } while (ps_.accept(Tk::comma));
    return r;
  }

  // A delegating initializer precedes the body. Other constructors initialize their direct bases in declaration
  // order, from their mem-initializers (keyed after the members) or default-initialized (a
  // `MemberInit` of field `~0u`); then every member in declaration order, from its
  // mem-initializer, its default member initializer, or default-initialized; then the compound
  // statement.
  Ex constructor_body(ConstructorInitializers inits, Ex body, bool nonthrowing) {
    std::vector<ast::Child> stmts;
    const sema::ClassRec& rec = p_->classes_.rec(s_.current_class);
    if (inits.delegation) {
      const Ex& init = *inits.delegation;
      if (!init.ok()) return {};
      if (rec.union_ && !nonthrowing && std::ranges::any_of(rec.fields, [&](const sema::FieldRec& field) {
            return s_.destructor_of(field.type) != EntityId::none;
          })) {
        s_.diag(init.begin, Diag::union_constructor_unwind);
        return {};
      }
      Ex delegate = s_.node(ast::DelegatingInit{sema::Sema::at(init, init.begin)}, init.begin, init.end);
      stmts.push_back(sema::Sema::at(delegate, 0));
      stmts.push_back(sema::Sema::at(body, 0));
      return s_.node(ast::Compound{b_.list(stmts)}, 0, body.end);
    }
    if (rec.union_ && inits.subobjects.size() > 1) {
      s_.diag(body.begin, Diag::too_many_initializers, s_.types().class_type(s_.current_class));
      return {};
    }
    for (std::size_t i = 0; i < rec.bases.size(); ++i) {
      const EntityId base = rec.bases[i].cls;
      const QualType bt = s_.types().class_type(base);
      Ex init;
      if (auto it = inits.subobjects.find(u32(rec.fields.size() + i)); it != inits.subobjects.end()) {
        init = it->second;
      } else if (const auto* record = p_->classes_.find(base); record && record->polymorphic) {
        if (!record->dynamic) {
          s_.check_polymorphic_base_default_init(bt, 0);
          continue;
        }
        init = s_.construct(bt, {}, 0, 0, sema::Construction::default_init, sema::ConstructorDestination::polymorphic_base);
      } else {
        init = s_.default_init(bt, 0, 0);
      }
      if (!init.ok()) continue;
      Ex stmt = s_.node(ast::MemberInit{~0u, bt, {init.id, 0}}, init.begin, init.end);
      stmts.push_back(sema::Sema::at(stmt, 0));
    }
    for (u32 i = 0; i < rec.fields.size(); ++i) {
      const sema::FieldRec& f = rec.fields[i];
      Ex init;
      if (auto it = inits.subobjects.find(i); it != inits.subobjects.end()) init = it->second;
      else if (rec.union_ && (!inits.subobjects.empty() || !f.dmi)) continue;
      // The early scalar value may have been folded before all class members were visible.
      else if (auto d = p_->dmi(f.entity)) init = member_initializer(f, *d);
      else if (f.init != ConstId::none) init = s_.literal(sema::Types::unqualified(f.type), f.init, ast::ValueCategory::prvalue, 0, 0);
      else if (s_.types().is_reference(f.type)) s_.diag(0, Diag::reference_uninitialized);  // [class.base.init]/10
      else init = s_.default_init(f.type, 0, 0);
      if (!init.ok()) continue;
      if (rec.union_) {
        if (!s_.check_destructor_access(s_.types().innermost_array_element(f.type), init.begin)) return {};
        // [except.ctor]: if construction unwinds, the completed variant must be cleaned up.
        // Only the ordinary written bodies collected by the Linux pipeline get this proof.
        // Generated and template bodies retain the source diagnostic.
        const bool native_cleanup = s_.types().target().triple == ast::x86_64_linux.triple &&
                                    !inst_ && !it_->specialization && !templated_pattern() &&
                                    p_->world().entities.rec(s_.current_class).templ == EntityId::none &&
                                    (p_->in_global_scope(k_) || p_->namespace_class_friend(k_));
        if (!nonthrowing && s_.destructor_of(f.type) != EntityId::none && !native_cleanup) {
          s_.diag(init.begin, Diag::union_constructor_unwind);
          return {};
        }
      }
      stmts.push_back({s_.node(ast::MemberInit{i, f.type, {init.id, 0}}, 0, 0).id, 0});
    }
    stmts.push_back(sema::Sema::at(body, 0));
    return s_.node(ast::Compound{b_.list(stmts)}, 0, body.end);
  }

  // A default member initializer, analysed in this constructor (a complete-class context with
  // `this`, [class.mem.general]); the function's parameters are not in its scope.
  Ex member_initializer(const sema::FieldRec& f, const Program::Dmi& d) {
    if (d.invalid) return {};
    auto saved = s_.take_scopes();
    // `sub` rebinds the evaluator callbacks to itself; restore them before it goes away.
    auto evaluate = s_.constant_of;
    auto evaluate_immediate = s_.evaluate_immediate;
    const auto source = std::exchange(at_->initializer_source, d);
    Parser sub(p_->region(), d.begin, d.end, s_);
    sub.set_questions(*q_);
    Ex init = sub.member_initializer(f.type, d.braced, d.form);
    at_->initializer_source = source;
    s_.constant_of = std::move(evaluate);
    s_.evaluate_immediate = std::move(evaluate_immediate);
    s_.put_scopes(std::move(saved));
    return init;
  }

  // Where the part analysed begins in the item: the body's, or a generic lambda's declaration part.
  u32 begin() const { return generic_ && generic_->g.part ? generic_->g.part->begin : it_->body_begin - it_->begin; }

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

// The point a synthesized definition of class `cls` is analysed at: after its class, whose member
// declaration is `item`, or at the end of the unit for a specialization, whose arguments may be
// declared after its template ([temp.point]).
std::unique_ptr<sema::Questions> Program::synthesis_questions(EntityId cls, u32 item) {
  if (class_args_.contains(std::to_underlying(cls))) return std::make_unique<AtInstantiation>(*this, item);
  auto q = std::make_unique<AtItem>(*this, item, true);
  q->own_class_complete = true;
  return q;
}

// [class.copy.ctor]/7, [class.copy.assign]/2: whether the implicit copy constructor's or copy
// assignment operator's parameter is a reference to const.
bool Program::copy_takes_const(const sema::ClassRec& rec, bool assignment) const {
  const auto& types = w_->types;
  const auto takes_const = [&](QualType t) {
    t = types.canonical(types.innermost_array_element(t));
    if (!types.is_class(t)) return true;
    const auto* leaf = classes_.find(types.class_of(t));
    if (!leaf) return true;
    const QualType owner = sema::Types::unqualified(t);
    bool declared = false;
    for (const sema::MemberDeclRec& m : leaf->members) {
      const auto p = assignment ? assignment_parameter(m, owner) : copy_move_constructor_parameter(m, owner);
      if (!p || types.rec(*p).rvalue_ref) continue;
      if (!types.is_reference(*p) || sema::Types::is_const(types.canonical(types.referee(*p)))) return true;
      declared = true;
    }
    return !declared && (assignment || leaf->defaulted_copy_const);
  };
  return std::ranges::all_of(rec.bases, [&](const sema::BaseRec& b) { return takes_const(w_->types.class_type(b.cls)); }) &&
         std::ranges::all_of(rec.fields, [&](const sema::FieldRec& f) { return takes_const(f.type); });
}

// What the definition of `function`, a special member of `cls` of kind `kind`, is ([class.default.ctor],
// [class.copy.ctor], [class.copy.assign]): overload resolution selects each subobject's operation
// from the class's context. Deletion is the language's; `unsupported` is a limit of ycxx.
Program::Classified Program::classify_special_member(u32 item, EntityId cls, const sema::ClassRec& rec, EntityId function,
                                                     SpecialMember kind, bool const_source, std::optional<u32> point) {
  using enum sema::CopyState;
  using enum SpecialMember;
  auto& types = w_->types;
  const bool assignment = kind == copy_assignment || kind == move_assignment;
  const bool move = kind == move_constructor || kind == move_assignment;
  const bool initialization = kind == default_constructor;
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;
  AtItem q(*this, item, false);
  if (point) q.at_point = *point;
  else q.own_class_complete = true;  // its nested classes are, whatever the order of its members
  bool calls = false;
  sema::Sema sm(*w_, b, q, diags, id(item), 0);
  sm.current_class = cls;
  sm.current_function = function;
  sm.completing_bases = rec.bases;
  const NameId source_name = w_->ins.names.intern("other");
  const NameId target_name = w_->ins.names.intern("object");
  // An lvalue or xvalue of type `t`, an object the operation uses.
  const auto object = [&](NameId name, QualType t, bool xvalue) {
    sm.declare_local(name, types.lvalue_ref(t), sema::local_flag::parameter, 0, false);
    const Ex e = sm.name_expr(name, 0, 0);
    return xvalue ? sm.static_cast_to(e, types.rvalue_ref(t), 0) : e;
  };
  const auto failure = [&](std::size_t before) {
    constexpr std::array invalid{
        Diag::no_viable_constructor, Diag::ambiguous_constructor, Diag::deleted_constructor, Diag::no_viable_function,
        Diag::ambiguous_call, Diag::deleted_function, Diag::private_member, Diag::protected_member,
        Diag::inaccessible_base, Diag::const_member_assign, Diag::const_member_call, Diag::not_assignable,
        Diag::bind_drops_qualifiers, Diag::bind_to_rvalue, Diag::no_conversion, Diag::object_unusable_destructor,
        Diag::base_unusable_destructor, Diag::closure_assignment, Diag::abstract_object};
    const bool language = diags.size() > before &&
        std::ranges::all_of(std::span(diags).subspan(before), [&](const engine::Diagnostic& d) {
          return std::ranges::any_of(invalid, [&](Diag k) { return d.id == sema::diag_id(k); });
        });
    return language ? deleted : unsupported;
  };
  const auto inspect = [&](QualType declared, bool base, bool mutable_) -> sema::CopyState {
    QualType t = types.canonical(declared);
    while (types.is_array(t)) t = types.canonical(types.add_cv(types.rec(t).inner, sema::Types::cv(t)));
    const auto r = types.rec(t);
    if (r.kind == sema::TypeKind::reference)
      return assignment || initialization || (!move && r.rvalue_ref) ? deleted : trivial;
    if (types.is_dependent(t)) return unsupported;
    const bool is_const = sema::Types::is_const(t);
    if (!types.is_class(t)) {
      if ((assignment || initialization) && is_const) return deleted;
      if (r.kind == sema::TypeKind::builtin && !w_->target[r.builtin].size) return unsupported;
      return assignment && types.is_member_pointer(t) ? unsupported : trivial;
    }
    const auto* leaf = classes_.find(types.class_of(t));
    if (!leaf || !leaf->defined) return unsupported;
    if (kind == move_constructor && classes_.implicit_move_differs(types.class_of(t))) return unsupported;
    // A call of a trivial member of the subobject's class.
    const auto trivial_member = [&](EntityId f) {
      const auto m = std::ranges::find(leaf->members, f, &sema::MemberDeclRec::entity);
      if (m == leaf->members.end() || !m->trivial_call) return false;
      calls = true;
      return true;
    };
    const auto destination = base && leaf->polymorphic ? sema::ConstructorDestination::polymorphic_base
                                                       : sema::ConstructorDestination::object;
    const std::size_t before = diags.size();
    sm.push_scope();
    Ex result;
    if (initialization) {
      if (sm.default_deleted(t) || (is_const && !sm.const_default_constructible(t))) {
        sm.pop_scope();
        return deleted;
      }
      result = destination == sema::ConstructorDestination::object
                   ? sm.default_init(t, 0, 0)
                   : sm.construct(t, {}, 0, 0, sema::Construction::default_init, destination);
    } else {
      const QualType from = const_source && !mutable_ ? types.add_cv(t, sema::Cv::const_) : t;
      const Ex source = object(source_name, from, move);
      result = assignment ? sm.assign(sema::Sema::AsgOp::assign, object(target_name, t, false), source, 0)
                          : sm.construct(sema::Types::unqualified(t), {source}, 0, 0, sema::Construction::direct, destination);
    }
    sm.pop_scope();
    if (!result.ok() || sm.has_error_since(before)) return failure(before);
    // The constructor potentially invokes the subobject's destructor ([class.base.init]).
    if (!assignment && !sm.check_destructor_access(t, 0, Diag::object_unusable_destructor, base)) return deleted;
    if (assignment) {
      const auto assigned = sm.as<ast::Assign>(result);
      const auto call = sm.as<ast::AssignmentCall>(result);
      return (assigned && assigned->op == ast::AssignOp::trivial_class_copy) || (call && trivial_member(call->function))
                 ? trivial : nontrivial;
    }
    if (const auto call = sm.as<ast::CtorCall>(result); call && trivial_member(call->ctor)) return trivial;
    if (initialization) return sm.as<ast::DefaultInit>(result) ? trivial : nontrivial;
    const auto conversion = sm.as<ast::ImplicitConversion>(result);
    return conversion && conversion->conversion == ast::ConversionKind::lvalue_to_rvalue ? trivial : nontrivial;
  };

  auto state = rec.polymorphic ? nontrivial : trivial;
  for (const sema::BaseRec& base : rec.bases) state = std::max(state, inspect(types.class_type(base.cls), true, false));
  const auto initialized = [](const sema::FieldRec& f) { return f.dmi || f.init != ConstId::none; };
  if (rec.union_) {
    // A variant member with a nontrivial operation deletes it. A default constructor ignores the
    // others' when one has a default member initializer, and const members unless all are.
    if (initialization && std::ranges::any_of(rec.fields, initialized)) return {std::max(state, nontrivial)};
    bool all_const = !rec.fields.empty();
    for (const sema::FieldRec& f : rec.fields) {
      const QualType element = types.canonical(types.innermost_array_element(f.type));
      all_const &= sema::Types::is_const(element);
      if (initialization && !types.is_class(element) && !types.is_reference(element)) continue;
      const auto s = inspect(f.type, false, f.mutable_);
      state = std::max(state, s == nontrivial ? deleted : s);
    }
    return {initialization && all_const ? deleted : state};
  }
  for (const sema::FieldRec& f : rec.fields) {
    if (!initialization || !initialized(f)) {
      state = std::max(state, inspect(f.type, false, f.mutable_));
      continue;
    }
    state = std::max(state, nontrivial);
    if (types.is_class(types.innermost_array_element(f.type)) &&
        !sm.check_destructor_access(f.type, 0, Diag::object_unusable_destructor)) state = deleted;
  }
  return {state, calls};
}

// Whether a trivial defaulted member of `rec` declared as `decl` is called for its exception
// specification ([expr.unary.noexcept]): the written one may be potentially-throwing, or an
// operation its definition `calls` is.
bool Program::calls_trivial_member(const sema::ClassRec& rec, const ast::DeclSema& decl, bool calls) const {
  if (rec.union_) return false;
  return decl.has_written_noexcept ? !w_->types.rec(decl.type).fn_noexcept : calls;
}

// Each constructor of the completed class `cls` defaulted on its first declaration
// ([dcl.fct.def.default]), and the implicit copy constructor of a class with a subobject that has
// a defaulted copy constructor.
void Program::defaulted_constructors(u32 item, EntityId cls, sema::ClassRec& rec, bool implicit_copy,
                                     std::optional<u32> point) {
  using enum sema::CopyState;
  using enum SpecialMember;
  auto& types = w_->types;
  const QualType ct = types.class_type(cls);
  const bool dependent = types.is_dependent(ct);
  for (sema::MemberDeclRec& m : rec.members) {
    // An ineligible one is no candidate and is never defined ([special]/6).
    if (m.defaulted == ordinary || m.ineligible) continue;
    const auto function = types.rec(m.type);
    const bool initialization = function.params.empty();
    if (initialization) rec.defaulted_default_ctor = m.entity;
    if (dependent) {  // each specialization decides
      m.defaulted = nontrivial;
      continue;
    }
    const u32 member_item = index(member_decls_.at(std::to_underlying(m.entity)).item);
    SpecialMember kind = default_constructor;
    bool const_source = false;
    bool mismatch = false;  // [dcl.fct.def.default]: not the implicit constructor's type
    if (!initialization) {
      const QualType parameter = function.params.front();
      const QualType source = types.canonical(types.referee(parameter));
      kind = types.rec(parameter).rvalue_ref ? move_constructor : copy_constructor;
      const_source = sema::Types::is_const(source);
      mismatch = sema::Types::is_volatile(source) ||
                 (const_source && (kind == move_constructor || !copy_takes_const(rec, false)));
    }
    const Classified found = mismatch ? Classified{deleted}
                                      : classify_special_member(item, cls, rec, m.entity, kind, const_source, point);
    m.defaulted = found.state;
    m.is_deleted = m.defaulted == deleted;
    auto decl = member_declaration(m.entity);
    m.trivial_call = m.defaulted == trivial && decl && calls_trivial_member(rec, *decl, found.calls);
    if (initialization) rec.trivial_defaulted_default_ctor = m.defaulted == trivial;
    else if (kind == copy_constructor && rec.defaulted_copy != nontrivial) rec.defaulted_copy = m.defaulted;
    if (!decl || (m.defaulted != nontrivial && !m.trivial_call)) continue;
    decl->flags |= sema::decl_flag::definition | sema::decl_flag::inline_;
    defaulted_constructors_.insert_or_assign(m.entity, DefaultedConstructor{{member_item, *decl, kind}});
  }
  if (!implicit_copy || dependent) return;
  const auto depends_on_defaulting = [&](QualType t) {
    return classes_.defaulted_copy_of(t) != ordinary || volatile_class(t);
  };
  if (std::ranges::none_of(rec.bases, [&](const sema::BaseRec& b) { return depends_on_defaulting(types.class_type(b.cls)); }) &&
      std::ranges::none_of(rec.fields, [&](const sema::FieldRec& f) { return depends_on_defaulting(f.type); })) return;
  if (rec.closure) {  // its captures were copied where it is defined
    rec.defaulted_copy = trivial;
    for (const sema::FieldRec& f : rec.fields) {
      if (types.is_reference(f.type)) continue;
      const auto [captured_nontrivial, captured_deleted] = classes_.copy_of(f.type);
      rec.defaulted_copy = std::max(rec.defaulted_copy, captured_deleted ? deleted : captured_nontrivial ? nontrivial : trivial);
    }
  } else {
    rec.defaulted_copy_const = copy_takes_const(rec, false);
    rec.defaulted_copy =
        classify_special_member(item, cls, rec, EntityId::none, copy_constructor, rec.defaulted_copy_const, point).state;
  }
  rec.copy_deleted = rec.defaulted_copy == deleted;
}

// The definition of a defaulted constructor, analysed on first use.
const Result<ast::BodySema>& Program::defaulted_constructor_body(DefaultedConstructor& constructor) {
  if (constructor.body) return *constructor.body;
  const Synthesized& s = constructor.definition;
  const EntityId cls = w_->entities.rec(s.decl.entity).scope;
  if (s.kind == SpecialMember::default_constructor) {
    std::optional<Instantiation> instantiation;
    if (const auto args = class_args_.find(std::to_underlying(cls)); args != class_args_.end())
      instantiation = Instantiation{s.decl, args->second};
    auto body = BodyAnalysis(*this, s.item, std::move(instantiation)).implicit_default_constructor(s.decl);
    return constructor.body.emplace(std::move(body));
  }
  Result<ast::BodySema> out;
  ast::Builder b(w_->ins, &arena_);
  const auto q = synthesis_questions(cls, s.item);
  sema::Sema sm(*w_, b, *q, out.diags, id(s.item), 0);
  sm.current_class = cls;
  sm.current_function = s.decl.entity;
  sm.in_function_body = true;
  sm.this_type = this_type(s.decl);
  const Ex root = copy_constructor_root(sm, cls, s.decl, s.kind == SpecialMember::move_constructor);
  const std::vector<ast::LocalRec> locals(sm.locals().begin(), sm.locals().end());
  out.value = b.commit(ast::BodySema{root.ok() ? root.id : NodeId::none, b.seq(std::span<const ast::LocalRec>(locals)),
                                     b.seq(std::span<const ast::ExpansionRec>{}), b.seq(std::span<const u32>{}), QualType{}, 0,
                                     b.seq(std::span<const ast::ClosureDef>{})});
  out.demands = std::move(sm.demands);
  return constructor.body.emplace(std::move(out));
}

// [except.spec]: the implicit exception specification of a special member defaulted on its first
// declaration without a noexcept-specifier, or of an implicit copy assignment operator. It is
// non-throwing unless an expression of the definition is potentially-throwing.
std::optional<bool> Program::defaulted_nonthrowing(EntityId f) {
  const auto constructor = defaulted_constructors_.find(f);
  const auto assignment = copy_assignments_.find(f);
  if (constructor == defaulted_constructors_.end() && assignment == copy_assignments_.end()) return std::nullopt;
  const Synthesized& s = constructor != defaulted_constructors_.end() ? constructor->second.definition
                                                                      : assignment->second.definition;
  if (s.decl.has_written_noexcept) return std::nullopt;
  const auto [slot, fresh] = defaulted_nonthrowing_.try_emplace(f, std::nullopt);
  if (!fresh) return slot->second.value_or(false);
  NodeId root = NodeId::none;
  if (constructor != defaulted_constructors_.end()) {
    root = defaulted_constructor_body(constructor->second).value.tree;
  } else {
    require_copy_assignment(f);
    if (assignment->second.body) root = assignment->second.body->tree;
  }
  bool nonthrowing = false;
  if (root != NodeId::none) {
    ast::Builder b(w_->ins, &arena_);
    std::vector<engine::Diagnostic> diags;
    const EntityId cls = w_->entities.rec(f).scope;
    const auto q = synthesis_questions(cls, s.item);
    sema::Sema sm(*w_, b, *q, diags, id(s.item), 0);
    sm.current_class = cls;
    sm.current_function = f;
    const Ex definition{b.adopt(root), 0, 0};
    const auto literal = sm.as<ast::Literal>(sm.unary_noexcept(definition, 0, 0, /*result_destroyed=*/false));
    nonthrowing = literal && w_->values.rec(literal->value).bits && diags.empty();
  }
  defaulted_nonthrowing_[f] = nonthrowing;
  return nonthrowing;
}

std::optional<QualType> Program::copy_move_constructor_parameter(const sema::MemberDeclRec& member, QualType owner) const {
  if (!member.is_ctor || member.is_template) return std::nullopt;
  const auto& types = w_->types;
  const sema::TypeRec function = types.rec(member.type);
  if (function.params.empty()) return std::nullopt;
  const QualType parameter = function.params.front();
  if (!types.is_reference(parameter) ||
      types.canonical(sema::Types::unqualified(types.referee(parameter))) != types.canonical(owner)) return std::nullopt;

  // [class.copy.ctor]/1–3: parameters after the first must have default arguments.
  std::size_t required = function.params.size();
  if (auto deferred = member_default_tokens_.find(member.entity); deferred != member_default_tokens_.end()) {
    while (required > 1 && deferred->second[required - 1]) --required;
  } else {
    while (required > 1 && required - 1 < member.defaults.size() && member.defaults[required - 1] != NodeId::none) --required;
  }
  return required == 1 ? std::optional(parameter) : std::nullopt;
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

sema::TrivialCopyability Program::class_trivial_copyability(u32 item, EntityId cls, const sema::ClassRec& rec) {
  using enum sema::CopyOperation;
  const auto& head = items_->items[item];
  if (head.template_depth || head.specialization || head.unnamed_type_ordinal != items::none ||
      w_->entities.rec(cls).templ != EntityId::none || !rec.bases.empty()) return {};

  const auto& types = w_->types;
  bool deletes_defaulted_copy_ctor = false;
  bool deletes_defaulted_assignment = false;
  for (const auto& field : rec.fields) {
    QualType type = types.canonical(field.type);
    if (types.is_dependent(type)) return {};
    if (types.is_reference(type)) {
      if (rec.union_) return {};
      deletes_defaulted_copy_ctor |= types.rec(type).rvalue_ref;  // [class.copy.ctor]/10
      deletes_defaulted_assignment = true;                       // [class.copy.assign]/7
      continue;
    }
    while (types.is_array(type)) {
      const auto array = types.rec(type);
      if (array.bound_value == ConstId::none) return {};
      type = types.canonical(array.inner);
    }
    if (!types.is_scalar(type)) return {};
    deletes_defaulted_assignment |= sema::Types::is_const(type);
  }

  sema::TrivialCopyability facts;
  const auto defaulted = [](bool deleted) { return deleted ? ineligible : eligible_trivial; };
  const auto written = [&](sema::CopyOperation& operation, const sema::MemberDeclRec& member, bool would_delete) {
    if (operation != unknown || types.is_dependent(member.type)) return false;
    const auto decl = member_decl(member.entity);
    const auto first = decl ? decl_sema(*decl) : std::nullopt;
    if (!first || first->constraint != NodeId::none || first->constraints != ConstId::none) return false;
    // [dcl.fct.def.default]/5: defaulting after the in-class declaration is user-provided.
    if (member.is_deleted) operation = ineligible;
    else if (defaulted_member_definition(index(decl->item), *first)) operation = defaulted(would_delete);
    else operation = eligible_nontrivial;
    return true;
  };

  // [dcl.fct.def.default]: a parameter the implicit declaration cannot have deletes the defaulted one.
  const auto mismatched = [&](QualType parameter, bool move) {
    if (!types.is_reference(parameter)) return false;
    const QualType source = types.canonical(types.referee(parameter));
    return sema::Types::is_volatile(source) || (move && sema::Types::is_const(source));
  };
  const QualType owner = w_->types.class_type(cls);
  const bool union_dmi = rec.union_ && std::ranges::any_of(rec.fields, &sema::FieldRec::dmi);
  for (const auto& member : rec.members) {
    if (member.is_virtual) return {};
    if (member.is_dtor) {
      if (!written(facts.destructor, member, union_dmi)) return {};
      // In this domain a user-provided destructor is the nontrivial case of the
      // first-declaration check ([dcl.fct.def.default]).
      facts.destructor_user_provided = facts.destructor == eligible_nontrivial;
    } else if (const auto constructor_parameter = copy_move_constructor_parameter(member, owner)) {
      const bool move = types.rec(*constructor_parameter).rvalue_ref;
      if (!written(move ? facts.move_constructor : facts.copy_constructor, member,
                   mismatched(*constructor_parameter, move) || (!move && deletes_defaulted_copy_ctor)))
        return {};
    } else if (const auto assignment = assignment_parameter(member, owner)) {
      const bool move = types.rec(*assignment).rvalue_ref;
      if (!written(move ? facts.move_assignment : facts.copy_assignment, member,
                   deletes_defaulted_assignment || types.rec(member.type).fn_cv != sema::Cv::none ||
                       mismatched(*assignment, move))) return {};
    }
  }

  // Even deleted declarations affect the implicit declaration rules ([class.copy.ctor],
  // [class.copy.assign]). Defaulted moves can be trivial with an rvalue-reference field.
  const bool declares_move = facts.move_constructor != unknown || facts.move_assignment != unknown;
  const bool implicit_move = !declares_move && facts.copy_constructor == unknown && facts.copy_assignment == unknown &&
                             facts.destructor == unknown;
  if (facts.copy_constructor == unknown) facts.copy_constructor = defaulted(declares_move || deletes_defaulted_copy_ctor);
  if (facts.move_constructor == unknown) facts.move_constructor = defaulted(!implicit_move);
  if (facts.copy_assignment == unknown) facts.copy_assignment = defaulted(declares_move || deletes_defaulted_assignment);
  if (facts.move_assignment == unknown) facts.move_assignment = defaulted(!implicit_move || deletes_defaulted_assignment);
  if (facts.destructor == unknown) {
    facts.destructor = defaulted(union_dmi);
    facts.destructor_user_provided = false;  // implicitly declared
  }
  // [class.dtor]/7.2: for a union the defaulted destructor also needs a nondeleted trivial
  // constructor selected by default-initialization. Defer selection among written constructors.
  if (rec.union_ && facts.destructor == eligible_trivial &&
      std::ranges::any_of(rec.members, &sema::MemberDeclRec::is_ctor)) return {};
  return facts;
}

void Program::eligible_special_members(u32 item, EntityId cls, sema::ClassRec& rec, u32 point) {
  if (std::ranges::all_of(rec.members, [](const sema::MemberDeclRec& m) { return m.constraints == ConstId::none; })) return;
  auto& types = w_->types;
  const QualType ct = types.class_type(cls);
  if (types.is_dependent(ct)) return;
  enum class Kind : u8 { none, default_constructor, copy_constructor, move_constructor, copy_assignment, move_assignment, destructor };
  const auto kind_of = [&](const sema::MemberDeclRec& m) {
    if (!m.is_function || m.is_template || m.is_static) return Kind::none;
    if (m.is_dtor) return Kind::destructor;
    if (m.is_ctor) {
      if (const auto p = copy_move_constructor_parameter(m, ct))
        return types.rec(*p).rvalue_ref ? Kind::move_constructor : Kind::copy_constructor;
      // [class.default.ctor]/1: callable with no arguments.
      const bool defaults = !m.defaults.empty() && m.defaults.front() != NodeId::none;
      return types.rec(m.type).params.empty() || defaults ? Kind::default_constructor : Kind::none;
    }
    if (const auto p = assignment_parameter(m, ct)) return types.rec(*p).rvalue_ref ? Kind::move_assignment : Kind::copy_assignment;
    return Kind::none;
  };
  std::vector<Kind> kinds;
  for (const sema::MemberDeclRec& m : rec.members) kinds.push_back(kind_of(m));
  // Satisfied in the specialization, its arguments substituted when its members were declared.
  std::vector<bool> satisfied(rec.members.size(), true);
  bool hard_error = false;  // already reported where the constraint is
  for (std::size_t i = 0; i < rec.members.size(); ++i) {
    const sema::MemberDeclRec& m = rec.members[i];
    if (m.constraints == ConstId::none || kinds[i] == Kind::none) continue;
    satisfied[i] = this->satisfied(m.constraints, point).value_or(true);
    hard_error |= satisfaction_failed(m.constraints);
  }
  // Not eligible: unsatisfied, or another of its kind and parameters with satisfied constraints is
  // more constrained ([temp.constr.order]).
  sema::Templates t = templates();
  for (std::size_t i = 0; i < rec.members.size(); ++i) {
    sema::MemberDeclRec& m = rec.members[i];
    const Kind kind = kinds[i];
    if (kind == Kind::none) continue;
    if (!satisfied[i]) {
      m.ineligible = true;
      continue;
    }
    for (std::size_t j = 0; j < rec.members.size() && !m.ineligible; ++j) {
      const sema::MemberDeclRec& o = rec.members[j];
      if (j == i || !satisfied[j] || kinds[j] != kind || (m.constraints == ConstId::none && o.constraints == ConstId::none))
        continue;
      const auto a = types.rec(types.canonical(m.type));
      const auto b = types.rec(types.canonical(o.type));
      if (a.params != b.params || a.variadic != b.variadic || a.fn_cv != b.fn_cv || a.fn_ref != b.fn_ref) continue;
      m.ineligible = t.subsumes(o.constraints, m.constraints) && !t.subsumes(m.constraints, o.constraints);
    }
  }
  // [class.dtor]/4: overload resolution among the prospective destructors selects one.
  std::size_t declared = 0, eligible = 0;
  for (const sema::MemberDeclRec& m : rec.members)
    if (m.is_dtor) {
      ++declared;
      if (!m.ineligible) ++eligible;
    }
  if (declared == 0 || eligible == 1 || hard_error) return;
  // Reported at the first prospective destructor's declaration.
  engine::Loc at{std::to_underlying(id(item)), 0};
  for (const sema::MemberDeclRec& m : rec.members)
    if (const auto decl = m.is_dtor ? member_decl(m.entity) : std::nullopt) {
      at = {std::to_underlying(decl->item), 0};
      break;
    }
  end_diags_.push_back({at, sema::diag_id(eligible == 0 ? Diag::destructor_unselected : Diag::destructor_ambiguous),
                        {engine::DiagArg{std::to_underlying(ct), w_->types.fp(ct)}}});
}

bool Program::special_members(u32 item, EntityId cls, sema::ClassRec& rec, std::vector<engine::Diagnostic>* class_diags,
                              sema::Sema* closure_context, std::optional<u32> point) {
  sema::Types& types = w_->types;
  QualType ct = types.class_type(cls);
  bool user_copy = false;
  bool user_move = false;
  bool user_copy_assignment = false;
  bool user_move_assignment = false;
  const sema::MemberDeclRec* user_dtor = nullptr;
  const sema::ClassRec* base = rec.bases.size() == 1 ? classes_.find(rec.bases.front().cls) : nullptr;
  const bool overrides_destructor = base && base->virtual_destructor;
  rec.polymorphic = base && base->polymorphic;
  rec.virtual_destructor = overrides_destructor;
  for (const sema::MemberDeclRec& m : rec.members) {
    if (const auto parameter = assignment_parameter(m, ct)) {
      if (types.rec(*parameter).rvalue_ref) user_move_assignment = true;
      else user_copy_assignment = true;
    }
    if (m.is_virtual) rec.polymorphic = true;
    // The selected destructor, of the prospective ones ([class.dtor]/4).
    if (m.is_dtor && (!user_dtor || (user_dtor->ineligible && !m.ineligible))) {
      user_dtor = &m;
      rec.declared_destructor = m.entity;
      rec.virtual_destructor |= m.is_virtual;
    }
    if (!m.is_ctor) continue;
    rec.user_ctors = true;
    if (const auto parameter = copy_move_constructor_parameter(m, ct)) {
      (types.rec(*parameter).rvalue_ref ? user_move : user_copy) = true;
    }
  }
  ast::Builder b(w_->ins, &arena_);
  auto synthesize = [&](bool copy) {
    std::string cname(w_->ins.names.spelling(w_->entities.rec(cls).name));
    QualType void_ = types.builtin(sema::Builtin::void_);
    std::vector<QualType> ps;
    std::vector<ast::ParamSema> params;
    if (copy) {
      ps.push_back(types.lvalue_ref(rec.defaulted_copy_const ? types.add_cv(ct, sema::Cv::const_) : ct));
      params.push_back({EntityId::none, ps[0], w_->ins.names.intern("other"), NodeId::none, 0});
    }
    const bool virtual_destructor = !copy && rec.virtual_destructor;
    QualType type = types.function(void_, ps, false, LangLinkage::cxx, sema::Cv::none, ast::RefQual::none,
                                   virtual_destructor && rec.destructor_noexcept.value_or(false));
    ast::DeclSema d{};
    d.is_virtual = virtual_destructor;
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
  // Only a class definition or an instantiation can admit the class or its subobjects
  // (`complete_virtual_members`, after this); closures keep the virtual layout guard.
  rec.virtual_layout_unsupported = rec.layout_requires_virtual_support;
  const auto declaration = user_dtor ? destructor_declarations_.find(user_dtor->entity) : destructor_declarations_.end();
  const bool defaulted_destructor = !rec.virtual_destructor && declaration != destructor_declarations_.end() &&
                                    declaration->second.defaulted;
  const bool manual_union_destruction = rec.union_ && user_dtor && !defaulted_destructor;
  if (class_diags && rec.union_ && !user_dtor && std::ranges::any_of(subobjects, [&](QualType t) {
        return classes_.destructor_of(t) != EntityId::none;
      })) {
    // The implicit destructor of a nontrivial-member union has separate C++26 rules (#203).
    class_diags->push_back({{std::to_underlying(id(item)), 0}, sema::diag_id(Diag::not_supported),
                           {engine::diag_arg(std::to_underlying(lex::Tk::kw_union))}});
  }
  // [except.spec]: the absence of an explicit destructor specification takes the
  // specification of the destructor that would be implicitly declared.
  std::optional<bool> implicit_noexcept = true;
  bool dependent_destructor = false;
  rec.destructor_usable = !user_dtor || !user_dtor->is_deleted;
  const bool nontrivial_subobject_destructor = std::ranges::any_of(subobjects, [&](QualType t) {
    return classes_.destructor_of(t) != EntityId::none;
  });
  // [class.dtor]/7: a variant member with a nontrivial destructor deletes the defaulted one.
  if (rec.union_ && defaulted_destructor && nontrivial_subobject_destructor) rec.destructor_usable = false;
  for (std::size_t i = 0; i < subobjects.size(); ++i) {
    const QualType element = types.innermost_array_element(subobjects[i]);
    const sema::TypeRec type = types.rec(element);
    if ((type.kind != sema::TypeKind::class_ || rec.closure) &&
        type.kind != sema::TypeKind::pointer && type.kind != sema::TypeKind::reference &&
        type.kind != sema::TypeKind::member_pointer && types.is_dependent(rec.closure ? element : subobjects[i])) {
      if (implicit_noexcept != false) implicit_noexcept.reset();
      if (rec.closure) dependent_destructor = true;
      else if (!manual_union_destruction) rec.destructor_usable = false;
      continue;
    }
    if (type.kind != sema::TypeKind::class_) continue;
    const auto* record = classes_.find(type.entity);
    if (record && record->destructor_noexcept == false) implicit_noexcept = false;
    else if ((!record || !record->destructor_noexcept) && implicit_noexcept != false) implicit_noexcept.reset();
    if (manual_union_destruction) continue;  // no implicit variant destructor calls
    if (rec.closure && record && record->destructor_dependent) {
      dependent_destructor = true;
      continue;
    }
    if (!record || !record->destructor_usable) {
      rec.destructor_usable = false;
      continue;
    }
    const EntityId destructor = record->destructor_declaration();
    if (destructor == EntityId::none) continue;
    const auto access = record->access_of(destructor);
    // [class.access.general]: the closure has its enclosing context's access. A capture of
    // this class type does not meet [class.protected]'s derived-object rule.
    const bool accessible = access == ast::Access::public_ ||
                            (i < rec.bases.size() && access == ast::Access::protected_) ||
                            record->befriends(cls, w_->entities) ||
                            (user_dtor && record->befriends(user_dtor->entity, w_->entities)) ||
                            (closure_context && closure_context->class_rec(type.entity) &&
                             closure_context->accessible(type.entity, ast::Access::private_));
    rec.destructor_usable &= accessible;
  }
  // A concrete capture can make destruction unusable even with other captures still dependent.
  rec.destructor_dependent = dependent_destructor && rec.destructor_usable;
  rec.destructor_usable &= !dependent_destructor;
  // [class.mem.general]: the parenthesized specification is evaluated in the complete class.
  if (!defaulted_destructor || !special_member_exception_tokens_.contains(user_dtor->entity))
    rec.destructor_noexcept = declaration != destructor_declarations_.end() && declaration->second.nonthrowing
                                ? declaration->second.nonthrowing : implicit_noexcept;
  if (declaration != destructor_declarations_.end()) declaration->second.implicit_nonthrowing = implicit_noexcept;
  if (overrides_destructor && class_diags && !types.is_dependent(ct)) {
    const bool unusable = !rec.destructor_usable || !base->destructor_usable;
    const bool unknown = !base->destructor_noexcept || !rec.destructor_noexcept;
    if (unusable || unknown || (*base->destructor_noexcept && !*rec.destructor_noexcept)) {
      auto at = engine::Loc{std::to_underlying(id(item)), 0};
      if (declaration != destructor_declarations_.end())
        if (const auto member = member_decl(user_dtor->entity))
          at = {std::to_underlying(member->item), declaration->second.at};
      const auto diag = unusable ? Diag::virtual_unusable_destructor
                      : unknown ? Diag::virtual_destructor_noexcept : Diag::virtual_override_noexcept;
      class_diags->push_back({at, sema::diag_id(diag), {}});
    }
  }
  // [class.copy.ctor]/6: either user-declared move operation deletes the otherwise implicit copy.
  const bool deletes_implicit_copy = user_move || user_move_assignment;
  const bool implicit_copy = !user_copy && !deletes_implicit_copy;
  defaulted_constructors(item, cls, rec, implicit_copy, point);
  // [class.prop]: of the declared copy and move constructors that are not deleted, one is not trivial.
  bool copy_usable = false;
  for (const sema::MemberDeclRec& m : rec.members) {
    const auto parameter = m.is_ctor && !m.is_deleted && !m.ineligible ? copy_move_constructor_parameter(m, ct) : std::nullopt;
    if (!parameter) continue;
    const bool nontrivial = m.defaulted != sema::CopyState::trivial;
    if (types.rec(*parameter).rvalue_ref) {
      rec.move_usable = true;
      rec.move_nontrivial |= nontrivial;
    } else {
      copy_usable = true;
      if (nontrivial) rec.copy_ctor = m.entity;
    }
  }
  if (user_copy) rec.copy_deleted = !copy_usable;
  // [class.dtor]: a destructor defaulted on its first declaration is trivial when every subobject's
  // is, and needs no cleanup. Otherwise it and the implicit destructor destroy the subobjects.
  if (user_dtor && !defaulted_destructor) {
    rec.destructor = user_dtor->entity;
  } else if (!rec.union_ && nontrivial_subobject_destructor && !user_dtor) {
    rec.destructor = synthesize(false);
  } else if (!rec.union_ && nontrivial_subobject_destructor && !types.is_dependent(ct)) {
    rec.destructor = user_dtor->entity;
    if (auto d = member_declaration(user_dtor->entity); d && rec.destructor_usable) {
      d->flags |= sema::decl_flag::definition | sema::decl_flag::inline_;
      if (!d->has_written_noexcept && implicit_noexcept == false) {
        const auto f = types.rec(d->type);
        d->type = types.function(f.inner, f.params, f.variadic, f.linkage, f.fn_cv, f.fn_ref, false);
        special_member_exception_types_[user_dtor->entity] = d->type;
      }
      defaulted_destructors_.emplace(d->entity, Synthesized{item, *d, SpecialMember::destructor});
    }
  }
  // [class.copy.ctor]/10–11: the implicit copy constructor is deleted with a member's, and not
  // trivial when a member's is not.
  if (!user_copy && deletes_implicit_copy) rec.copy_deleted = true;
  // GCC's call rule asks whether a class subobject deletes it, however its copy is classified.
  if (implicit_copy)
    for (QualType t : subobjects)
      if (!types.is_reference(t)) rec.copy_deleted_by_class_subobject |= classes_.copy_of(t).second;
  if (implicit_copy && rec.defaulted_copy == sema::CopyState::ordinary) {
    bool nontrivial = false;
    for (QualType t : subobjects) {
      if (types.is_reference(t)) {
        rec.copy_deleted |= types.rec(t).rvalue_ref;
        continue;
      }
      auto [n, del] = classes_.copy_of(t);
      rec.copy_deleted = rec.copy_deleted || del;
      nontrivial = nontrivial || n;
    }
    if (!rec.copy_deleted && nontrivial && !rec.layout_requires_virtual_support) rec.copy_ctor = synthesize(true);
  } else if (implicit_copy && rec.defaulted_copy == sema::CopyState::nontrivial && !rec.layout_requires_virtual_support) {
    rec.copy_ctor = synthesize(true);
  }
  // [class.copy.ctor]/8, 10–11: implicit move requires no user-declared copy, move or destructor.
  const bool implicit_move = implicit_copy && !user_copy_assignment && !user_dtor;
  const auto volatile_field = [&](const sema::FieldRec& f) { return volatile_class(f.type); };
  const bool volatile_classes = !rec.closure && !types.is_dependent(ct) && std::ranges::any_of(rec.fields, volatile_field);
  // Overload resolution decides about a subobject with a defaulted move, which can be inaccessible
  // or not take a const member.
  const bool defaulted_moves = !rec.closure && !types.is_dependent(ct) && std::ranges::any_of(subobjects, [&](QualType t) {
    t = types.canonical(types.innermost_array_element(t));
    const auto* sub = types.is_class(t) ? classes_.find(types.class_of(t)) : nullptr;
    const auto* move = sub ? classes_.declared_move(types.class_of(t), *sub) : nullptr;
    return move && move->defaulted != sema::CopyState::ordinary;
  });
  if (implicit_move && (volatile_classes || defaulted_moves)) {
    const auto move = classify_special_member(item, cls, rec, EntityId::none, SpecialMember::move_constructor, false, point).state;
    rec.move_usable = move != sema::CopyState::deleted;
    rec.move_nontrivial = rec.move_usable && move != sema::CopyState::trivial;
  } else if (implicit_move) {
    rec.move_usable = !rec.copy_deleted || std::ranges::none_of(subobjects, [&](QualType t) {
                        return !types.is_reference(t) && classes_.unmovable(t);
                      });
    rec.move_nontrivial = rec.polymorphic || std::ranges::any_of(subobjects, [&](QualType t) {
      return !types.is_reference(t) && classes_.move_nontrivial(t);
    });
  }
  // The Darwin Clang layout rule uses a trivial class ([class.prop]/2: its
  // default constructor, copy and move operations and destructor trivial), and standard-layout
  // (/3: one access for all its data members, no reference members, and its data members all
  // in one class of the hierarchy), whose members and bases are too.
  auto pod_type = [&](QualType t) {
    while (types.is_array(t)) t = types.rec(t).inner;
    if (types.is_reference(t)) return false;
    const sema::ClassRec* m = types.is_class(t) ? classes_.find(types.class_of(t)) : nullptr;
    return !m || m->pod;
  };
  // [dcl.fct.def.default]/5: a special member defaulted or deleted on its first declaration is not
  // user-provided.
  const auto provided_constructor = [&](const sema::MemberDeclRec& m) {
    return m.is_ctor && !m.is_deleted && (m.is_template || m.defaulted == sema::CopyState::ordinary);
  };
  const auto provided_assignment = [&](bool move) {
    return std::ranges::any_of(rec.members, [&](const sema::MemberDeclRec& m) {
      // An ineligible one does not count ([special]/6), as Clang and GCC lay classes out.
      const auto parameter = m.explicit_defaulted_assignment || m.is_deleted || m.ineligible ? std::nullopt : assignment_parameter(m, ct);
      return parameter && types.rec(*parameter).rvalue_ref == move;
    });
  };
  bool one_access = std::ranges::adjacent_find(rec.fields, [&](const sema::FieldRec& a, const sema::FieldRec& b) {
                      return rec.access_of(a.entity) != rec.access_of(b.entity);
                    }) == rec.fields.end();
  bool data_in_bases = std::ranges::any_of(rec.bases, [&](const sema::BaseRec& b) {
    const sema::ClassRec* r = classes_.find(b.cls);
    return r && (!r->fields.empty() || !r->pod);
  });
  // Clang on arm64 Darwin takes C++11's definition ([class.prop]). Its trivial class has a trivial
  // default constructor, even a deleted one. The other modes and targets take the Itanium ABI's,
  // C++03's, whose class has only public members and no base classes (docs/compatibility.toml).
  const bool modern_pod = compatibility_ == ast::Compatibility::clang &&
                          w_->target.operating_system == ast::OperatingSystem::darwin;
  const bool pod_definition = modern_pod
      ? (!rec.user_ctors || std::ranges::any_of(rec.members, [&](const sema::MemberDeclRec& m) {
           return m.is_ctor && !m.is_template && !m.ineligible && types.rec(m.type).params.empty() &&
                  (m.is_deleted || m.defaulted == sema::CopyState::trivial);
         })) && !rec.copy_nontrivial() && !(user_move && rec.move_usable && rec.move_nontrivial) &&
        !provided_assignment(true) && one_access && (rec.fields.empty() || !data_in_bases)
      : std::ranges::none_of(rec.members, provided_constructor) && rec.bases.empty() &&
        std::ranges::all_of(rec.fields, [&](const sema::FieldRec& f) { return rec.access_of(f.entity) == ast::Access::public_; });
  // No operation of a volatile class member is trivial for Apple's definition, even one that is deleted.
  const bool pod_fields = !modern_pod || std::ranges::none_of(rec.fields, volatile_field);
  rec.pod = pod_fields && !rec.layout_requires_virtual_support && (rec.destructor == EntityId::none || (user_dtor && user_dtor->is_deleted)) &&
            pod_definition &&
            !provided_assignment(false) && std::ranges::none_of(rec.fields, &sema::FieldRec::dmi) &&
            std::ranges::all_of(rec.fields, [&](const sema::FieldRec& f) { return pod_type(f.type); }) &&
            std::ranges::all_of(rec.bases, [&](const sema::BaseRec& b) { return pod_type(types.class_type(b.cls)); });
  defaulted_assignments(item, cls, rec, point);
  if (!user_copy_assignment) declare_copy_assignment(item, cls, rec, deletes_implicit_copy, point);
  return implicit_move;
}

void Program::prepare_move_constructor(u32 item, EntityId cls, sema::ClassRec& rec) {
  const auto& owner = w_->entities.rec(cls);
  if (!rec.copy_deleted || !rec.move_usable || !rec.move_nontrivial || rec.union_ ||
      rec.layout_requires_virtual_support || rec.unapplied_layout || !rec.bases.empty() ||
      owner.templ != EntityId::none || !is_namespace(outermost_class_scope(cls)) || items_->items[item].template_depth)
    return;
  // [class.copy.ctor]: a deleted or inaccessible subobject destructor deletes the move.
  if (!rec.destructor_usable) return;

  auto& types = w_->types;
  const QualType type = types.class_type(cls);
  const QualType parameter = types.rvalue_ref(type);
  const NameId other = w_->ins.names.intern("other");
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;
  AtItem q(*this, item, false);
  q.completing_class = cls;
  sema::Sema sm(*w_, b, q, diags, id(item), 0);
  sm.current_class = cls;
  sm.this_type = types.pointer(type);
  sm.push_scope();
  sm.declare_local(other, parameter, sema::local_flag::parameter, 0, false);
  const Ex source = sm.static_cast_to(sm.name_expr(other, 0, 0), parameter, 0);

  std::vector<ast::Child> statements;
  for (u32 i = 0; i < rec.fields.size(); ++i) {
    const auto& field = rec.fields[i];
    const bool class_field = types.is_class(field.type);
    if (!class_field && !types.is_scalar(field.type)) return;
    // [class.copy.ctor]: direct-initialize each field from the corresponding source xvalue.
    Ex member = sm.member(source, false, field.name, 0, 0, 0);
    Ex init = class_field ? sm.construct(field.type, {member}, 0, 0) : sm.convert(member, field.type, 0);
    if (!init.ok()) return;
    if (class_field) {
      const auto nonthrowing = sm.as<ast::Literal>(sm.unary_noexcept(init, 0, 0));
      if (!nonthrowing || !w_->values.rec(nonthrowing->value).bits) return;
    }
    statements.push_back({sm.node(ast::MemberInit{i, field.type, {init.id, 0}}, 0, 0).id, 0});
  }
  if (sm.has_error_since(0)) return;

  statements.push_back({sm.node(ast::Compound{b.list({})}, 0, 0).id, 0});
  const Ex root = sm.node(ast::Compound{b.list(statements)}, 0, 0);
  sm.pop_scope();
  const std::vector<ast::LocalRec> locals(sm.locals().begin(), sm.locals().end());
  const ast::BodySema body = b.commit(ast::BodySema{
      root.id, b.seq(std::span<const ast::LocalRec>(locals)), b.seq(std::span<const ast::ExpansionRec>{}),
      b.seq(std::span<const ast::u32>{}), QualType{}, 0, b.seq(std::span<const ast::ClosureDef>{})});

  ast::DeclSema decl{};
  decl.kind = ast::DeclKind::function;
  decl.type = types.function(types.builtin(sema::Builtin::void_), std::span(&parameter, 1), false,
                             LangLinkage::cxx, sema::Cv::none, ast::RefQual::none, true);
  decl.linkage = ast::Linkage::external;
  decl.storage = ast::Storage::none;
  decl.access = ast::Access::public_;
  decl.flags = sema::decl_flag::member | sema::decl_flag::implicit | sema::decl_flag::definition |
               sema::decl_flag::inline_ | sema::decl_flag::constructor;
  decl.attributes = b.seq(std::span<const NodeId>{});
  decl.annotations = b.seq(std::span<const ast::AnnotationSema>{});
  decl.constraint = NodeId::none;
  decl.constraints = ast::ConstId::none;
  decl.contracts = NodeId::none;
  const ast::ParamSema param{EntityId::none, parameter, other, NodeId::none, 0};
  decl.params = b.seq(std::span(&param, 1));
  decl.template_params = b.seq(std::span<const ast::TemplateParamSema>{});
  decl.entity = w_->entities.entity({sema::EntityKind::constructor, owner.name, decl.type, LangLinkage::cxx,
                                     ast::Linkage::external, {}, cls});
  sema::MemberDeclRec member{};
  member.name = owner.name;
  member.type = decl.type;
  member.entity = decl.entity;
  member.is_function = true;
  member.is_ctor = true;
  rec.members.push_back(std::move(member));
  move_constructors_.try_emplace(decl.entity, PendingMoveConstructor{
      {item, b.commit(decl), SpecialMember::move_constructor}, body, std::move(sm.demands)});
}

// Each assignment operator of the completed class `cls` defaulted on its first declaration
// ([dcl.fct.def.default], [class.copy.assign]).
void Program::defaulted_assignments(u32 item, EntityId cls, sema::ClassRec& rec, std::optional<u32> point) {
  using sema::CopyAssignment;
  using enum SpecialMember;
  auto& types = w_->types;
  const QualType ct = types.class_type(cls);
  if (types.is_dependent(ct)) return;
  for (sema::MemberDeclRec& m : rec.members) {
    const auto parameter = m.explicit_defaulted_assignment && !m.ineligible ? assignment_parameter(m, ct) : std::nullopt;
    if (!parameter) continue;
    const auto function = types.rec(m.type);
    const QualType source = types.canonical(types.referee(*parameter));
    const auto kind = types.rec(*parameter).rvalue_ref ? move_assignment : copy_assignment;
    const bool const_source = sema::Types::is_const(source);
    const u32 member_item = index(member_decls_.at(std::to_underlying(m.entity)).item);
    // Not the implicit operator's type ([dcl.fct.def.default]).
    const bool mismatch = sema::Types::is_volatile(source) || function.fn_cv != sema::Cv::none ||
                          (const_source && (kind == move_assignment || !copy_takes_const(rec, true)));
    const bool const_member = std::ranges::any_of(rec.fields, [&](const sema::FieldRec& f) {
      const QualType t = types.canonical(types.innermost_array_element(f.type));
      return !types.is_class(t) && !types.is_reference(t) && sema::Types::is_const(t);
    });
    const Classified found = mismatch ? Classified{sema::CopyState::deleted}
                                      : classify_special_member(item, cls, rec, m.entity, kind, const_source, point);
    switch (found.state) {
    case sema::CopyState::trivial: m.copy_assignment = CopyAssignment::trivial; break;
    case sema::CopyState::nontrivial: m.copy_assignment = CopyAssignment::memberwise; break;
    case sema::CopyState::deleted: m.copy_assignment = const_member ? CopyAssignment::const_member : CopyAssignment::deleted; break;
    case sema::CopyState::ordinary:
    case sema::CopyState::unsupported: m.copy_assignment = CopyAssignment::unsupported; break;
    }
    auto d = member_declaration(m.entity);
    if (!d || (m.copy_assignment != CopyAssignment::trivial && m.copy_assignment != CopyAssignment::memberwise)) continue;
    ast::ParamSema param{EntityId::none, *parameter, w_->ins.names.intern("other"), NodeId::none, 0};
    d->params = w_->ins.intern_seq(std::span<const ast::ParamSema>(&param, 1));
    d->flags |= sema::decl_flag::definition | sema::decl_flag::inline_;
    m.trivial_call = m.copy_assignment == CopyAssignment::trivial && calls_trivial_member(rec, *d, found.calls);
    PendingCopyAssignment pending{{member_item, *d, kind}, m.copy_assignment};
    pending.calls = found.calls && !rec.union_;
    copy_assignments_.insert_or_assign(m.entity, std::move(pending));
  }
}

void Program::declare_copy_assignment(u32 item, EntityId cls, sema::ClassRec& rec, bool user_move, std::optional<u32> point) {
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
  // [class.copy.assign]/7: an operator of a volatile class member is selected like any other.
  if (!rec.closure && std::ranges::any_of(rec.fields, [&](const sema::FieldRec& f) { return volatile_class(f.type); })) {
    const auto found = classify_special_member(item, cls, rec, EntityId::none, SpecialMember::copy_assignment,
                                               const_parameter, point).state;
    deleted |= found == sema::CopyState::deleted;
    trivial &= found == sema::CopyState::trivial;
    memberwise &= found == sema::CopyState::trivial || found == sema::CopyState::nontrivial;
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
  const auto q = synthesis_questions(cls, s.item);
  sema::Sema sm(*w_, b, *q, diags, id(s.item), 0);
  sm.current_class = cls;
  sm.current_function = f;
  sm.this_type = this_type(s.decl);
  const auto param = w_->ins.seqs.at(s.decl.params, 0);
  sm.push_scope();
  sm.declare_local(param.name, param.type, sema::local_flag::parameter, 0, false);
  const Ex self = sm.this_expr(0, 0);
  const Ex destination = sm.unary(ast::UnaryOp::deref, self, 0, 0, 0);
  const bool move = s.kind == SpecialMember::move_assignment;
  Ex source = sm.name_expr(param.name, 0, 0);
  if (move) source = sm.static_cast_to(source, param.type, 0);
  std::vector<ast::Child> statements;
  const auto append = [&](Ex expr) {
    if (!expr.ok()) return false;
    if (w_->types.is_class(expr.type()) && !expr.glvalue())
      expr = sm.materialize(expr, sema::Types::unqualified(expr.type()), false);
    statements.push_back({sm.node(ast::ExprStmt{{expr.id, 0}}, 0, 0).id, 0});
    return true;
  };

  bool ok = true;
  if (copy.state == CopyAssignment::trivial && !copy.calls) {
    const auto info = destination.info;
    ok = append(sm.node(ast::Assign{info, ast::AssignOp::trivial_class_copy,
                                   {destination.id, 0}, {source.id, 0}, info.type}, 0, 0, info));
  } else {
    // [class.copy.assign]/12: the direct bases, then the members; an array element by element, or
    // as a whole when its elements' assignment is trivial.
    const auto element_of = [&](Ex array, sema::u64 i, bool xvalue, QualType t) {
      const QualType index = w_->types.builtin(sema::Builtin::int_);
      const Ex at = sm.literal(index, w_->values.integer(index, i), ast::ValueCategory::prvalue, 0, 0);
      const Ex element = sm.subscript(array, at, 0, 0);
      return xvalue ? sm.static_cast_to(element, w_->types.rvalue_ref(element.ok() ? element.type() : t), 0) : element;
    };
    const auto assign = [&](this const auto& self_, Ex to, Ex from, QualType t) -> bool {
      if (!w_->types.is_array(t)) return append(sm.assign(sema::Sema::AsgOp::assign, to, from, 0));
      Ex first = to, source_first = from;
      QualType element = w_->types.canonical(t);
      while (w_->types.is_array(element)) {
        element = w_->types.canonical(w_->types.rec(element).inner);
        first = element_of(first, 0, false, element);
        source_first = element_of(source_first, 0, move, element);
      }
      const Ex probe = sm.assign(sema::Sema::AsgOp::assign, first, source_first, 0);
      if (!probe.ok()) return false;
      const auto assigned = sm.as<ast::Assign>(probe);
      if (!w_->types.is_class(element) || (assigned && assigned->op == ast::AssignOp::trivial_class_copy)) {
        const ast::ExprInfo info{t, ast::ValueCategory::lvalue, 0};
        return append(sm.node(ast::Assign{info, ast::AssignOp::trivial_class_copy, {to.id, 0}, {from.id, 0}, t}, 0, 0, info));
      }
      const sema::TypeRec array = w_->types.rec(w_->types.canonical(t));
      for (sema::u64 i = 0; i < array.bound; ++i)
        if (!self_(element_of(to, i, false, array.inner), element_of(from, i, move, array.inner), array.inner)) return false;
      return true;
    };
    const auto& record = classes_.rec(cls);
    ok = std::ranges::all_of(record.bases, [&](const sema::BaseRec& base) {
      return assign(sm.to_base(destination, base.cls, 0), sm.to_base(source, base.cls, 0), w_->types.class_type(base.cls));
    }) && std::ranges::all_of(record.fields, [&](const sema::FieldRec& field) {
      return assign(sm.member(self, true, field.name, 0, 0, 0), sm.member(source, false, field.name, 0, 0, 0), field.type);
    });
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
  copy.demands = std::move(sm.demands);
  sm.pop_scope();
  return copy.state;
}

// The copy of the object `src` designates as an initializer of type `t` ([class.copy.ctor]/14):
// a class through its copy constructor; an array whose elements' copy is not trivial element by
// element, else as a whole.
// [class.compare.default]: the expanded list of subobjects of `x` and `y`, objects of class `cls`
// (the direct bases in declaration order, then the non-static data members, an array's elements
// in increasing index order), given pairwise to `each` until it returns false. False when the
// class has a reference or variant member, which makes a defaulted comparison deleted.
bool Program::comparison_subobjects(sema::Sema& sm, EntityId cls, Ex x, Ex y, std::function_ref<bool(Ex, Ex)> each) {
  const sema::ClassRec& rec = classes_.rec(cls);
  if (rec.union_) return false;
  bool more = true;
  auto pair = [&](this const auto& self, Ex a, Ex b, QualType t) -> void {
    if (!more) return;
    if (w_->types.is_array(t)) {
      const sema::TypeRec r = w_->types.rec(t);
      const QualType index = w_->types.builtin(sema::Builtin::int_);
      for (sema::u64 i = 0; more && i < r.bound; ++i) {
        auto at = [&] { return sm.literal(index, w_->values.integer(index, i), ast::ValueCategory::prvalue, 0, 0); };
        self(sm.subscript(a, at(), 0, 0), sm.subscript(b, at(), 0, 0), r.inner);
      }
      return;
    }
    more = each(a, b);
  };
  for (const sema::FieldRec& f : rec.fields)
    if (w_->types.is_reference(f.type) || (w_->ins.names.spelling(f.name).empty() && !f.bit_width))
      return false;  // an anonymous union's variant members
  for (const sema::BaseRec& base : rec.bases)
    pair(sm.to_base(x, base.cls, 0), sm.to_base(y, base.cls, 0), w_->types.class_type(base.cls));
  for (const sema::FieldRec& f : rec.fields)
    if (!w_->ins.names.spelling(f.name).empty())  // an unnamed bit-field is not a member ([class.bit])
      pair(sm.member(x, false, f.name, 0, 0, 0), sm.member(y, false, f.name, 0, 0, 0), f.type);
  return true;
}

// [class.eq]: `x == y` for the defaulted equality operator of class `cls`. The subobjects are
// compared pairwise, each `xi == yi` contextually converted to `bool`, joined by `&&` so the first
// `false` decides; `true` with no subobjects. Nullopt when the operator is deleted
// ([class.compare.default]: a reference or variant member); a comparison that is not usable
// leaves its diagnostics in `sm` ([class.eq]).
std::optional<Ex> Program::defaulted_equality(sema::Sema& sm, EntityId cls, Ex x, Ex y) {
  using BinOp = sema::Sema::BinOp;
  std::optional<Ex> all;
  bool ok = true;
  if (!comparison_subobjects(sm, cls, x, y, [&](Ex a, Ex b) {
        Ex c = sm.to_bool(sm.binary(BinOp::eq, a, b, 0), 0);
        if (c.ok()) all = all ? sm.binary(BinOp::logical_and, *all, c, 0) : c;
        ok = c.ok() && all->ok();
        return ok;
      }) || !ok)
    return std::nullopt;
  return all ? *all : sm.bool_literal(true, 0, 0);
}

// [class.spaceship]: with a declared return type of `auto`, R is the common comparison category
// of the subobjects' `xi <=> yi` (/2), `std::strong_ordering` with none; the operator is deleted
// when one is not a comparison category type. Each result is converted to R (/1). Nullopt when
// deleted or not usable, with diagnostics for a comparison that fails analysis.
// (Synthesizing a comparison from `==` and `<` when `<=>` has no viable candidate is #1197.)
std::optional<Program::DefaultedThreeWay> Program::defaulted_three_way(sema::Sema& sm, EntityId cls, Ex x, Ex y,
                                                                       QualType declared) {
  using Ordering = sema::Sema::Ordering;
  std::vector<Ex> compared;
  bool ok = true;
  if (!comparison_subobjects(sm, cls, x, y, [&](Ex a, Ex b) {
        const Ex c = sm.binary(sema::Sema::BinOp::spaceship, a, b, 0);
        ok = c.ok();
        if (ok) compared.push_back(c);
        return ok;
      }) || !ok)
    return std::nullopt;
  DefaultedThreeWay r{declared, {}, {}};
  if (w_->types.has_placeholder(declared)) {
    Ordering common = Ordering::strong;
    for (const Ex& c : compared) {
      const auto k = sm.ordering_of(c.type());
      if (!k) return std::nullopt;
      common = std::min(common, *k);
    }
    const auto category = sm.comparison_category(common, 0u);
    if (!category) return std::nullopt;
    r.result = category->type;
  }
  const QualType result = w_->types.canonical(sema::Types::unqualified(r.result));
  // The synthesized three-way comparison of type R ([class.spaceship]/1): `static_cast<R>(c)`.
  auto synthesized = [&](Ex c) {
    if (w_->types.canonical(sema::Types::unqualified(c.type())) == result) return sm.convert(c, r.result, 0);
    return sm.static_cast_to(c, r.result, 0, true);
  };
  const QualType int_type = w_->types.builtin(sema::Builtin::int_);
  const bool compare_with_equality = sm.compatibility() == ast::Compatibility::gcc;
  for (const Ex& c : compared) {
    const std::size_t before = sm.diag_count();
    const Ex initializer = synthesized(c);
    if (!initializer.ok()) {
      // [class.spaceship]/1.2, /2.2: a standard category has no conversion to a scalar
      // builtin type. This failure makes the synthesized comparison undefined. Keep errors
      // for unsupported casts, including conversions between comparison categories (#339).
      if (w_->types.builtin_of(result) && sm.ordering_of(c.type()) && sm.diag_count() == before + 1 &&
          sm.diagnostics().back().id == sema::diag_id(Diag::no_conversion))
        sm.diagnostics().resize(before);
      return std::nullopt;
    }
    // [class.spaceship]/3: test `vi != 0` with `vi` naming the initialized local in both
    // completion and the body. GCC uses the negation of the contextual bool value of `vi == 0`.
    const u32 local = sm.declare_local(sm.no_name(), r.result, 0, 0, false);
    const Ex variable = sm.node(ast::LocalVar{local, sema::Sema::at(initializer, 0), sm.destructor_of(r.result)}, 0, 0);
    sm.set_local_decl(local, variable.id);
    sm.set_initialized(local);
    const Ex zero = sm.literal(int_type, w_->values.integer(int_type, 0), ast::ValueCategory::prvalue, 0, 1);
    const auto op = compare_with_equality ? sema::Sema::BinOp::eq : sema::Sema::BinOp::ne;
    Ex unequal = sm.to_bool(sm.binary(op, sm.local_ref(local, 0, 0), zero, 0), 0);
    if (compare_with_equality && unequal.ok()) unequal = sm.unary(ast::UnaryOp::not_, unequal, 0, 0, 0);
    if (!unequal.ok()) return std::nullopt;
    r.subobjects.push_back({initializer, variable, unequal, {}, local});
  }
  // `static_cast<R>(std::strong_ordering::equal)`: for a comparison category R, the value its
  // `equivalent` (or `equal`) member names ([cmp.categories]).
  const auto own = sm.ordering_of(r.result);
  const auto category = sm.comparison_category(own ? *own : Ordering::strong, 0u);
  if (!category) return std::nullopt;
  const EntityId equal = category->values[1];
  r.equal = synthesized(sm.static_member(w_->types.class_of(category->type), equal, w_->entities.rec(equal).type, 0, 0));
  if (!r.equal.ok()) return std::nullopt;
  // [class.spaceship]/3, [except.spec]: initializing the return value can throw. Resolve it
  // after forming the synthesized comparisons, which determine implicit deletion (/2.2).
  for (auto& part : r.subobjects) {
    part.return_value = sm.init_explicit_return(sm.local_ref(part.local, 0, 0), r.result);
    if (!part.return_value.ok()) break;
  }
  return r;
}

std::optional<Program::ComparisonOperands> Program::comparison_operands(sema::Sema& sm, const ast::DeclSema& f,
                                                                       std::span<const u32> params) {
  const sema::TypeRec fn = w_->types.rec(f.type);
  auto parameter = [&](std::size_t i) {
    const ast::ExprInfo info{w_->types.referee(fn.params[i]), ast::ValueCategory::lvalue, 0};
    return sm.node(ast::LocalRef{info, params[i]}, 0, 0, info);
  };
  const bool member = f.flags & sema::decl_flag::member;
  if (params.size() != (member ? 1u : 2u)) return std::nullopt;
  const EntityId cls = member ? w_->entities.rec(f.entity).scope : w_->types.class_of(w_->types.referee(fn.params[0]));
  Ex x = member ? sm.unary(ast::UnaryOp::deref, sm.this_expr(0, 0), 0, 0, 0) : parameter(0);
  return ComparisonOperands{cls, x, parameter(member ? 0 : 1)};
}

std::optional<Ex> Program::defaulted_comparison_result(sema::Sema& sm, const ast::DeclSema& f, std::span<const u32> params) {
  const auto operands = comparison_operands(sm, f, params);
  if (!operands) return std::nullopt;
  return defaulted_equality(sm, operands->cls, operands->x, operands->y);
}

// [class.compare.default]: once its class is complete, whether each defaulted comparison operator
// function declared in it (a member or a friend) is deleted, and its implicit exception
// specification ([except.spec]): non-throwing unless an expression of the implicit definition
// is potentially-throwing.
void Program::complete_defaulted_comparisons(EntityId cls, sema::ClassRec& rec) {
  auto complete = [&](EntityId f, DeclId decl, sema::MemberDeclRec* member) {
    const u32 item = index(decl.item);
    const std::string_view name = w_->ins.names.spelling(w_->entities.rec(f).name);
    if (!defaulted_body(item) || (name != "operator==" && name != "operator<=>")) return;
    const bool three_way = name == "operator<=>";
    const ast::ItemSema& is = item_sema(item).value;
    if (ast::number_of(decl.ord) >= w_->ins.seqs.size(is.decls)) return;
    const ast::DeclSema d = w_->ins.seqs.at(is.decls, ast::number_of(decl.ord));
    if (d.kind != ast::DeclKind::function || d.entity != f) return;
    ast::Builder b(w_->ins, &arena_);
    std::vector<engine::Diagnostic> diags;
    AtItem q(*this, item, true);
    q.completing_class = cls;
    q.own_class_complete = true;
    sema::Sema sm(*w_, b, q, diags, id(item), 0);
    sm.current_class = cls;
    sm.current_function = f;
    sm.this_type = this_type(d);
    sm.in_function_body = true;
    sm.push_scope();
    std::vector<u32> params;
    for (const ast::ParamSema& p : w_->ins.seqs.elements(d.params))
      params.push_back(sm.declare_local(p.name, p.type, sema::local_flag::parameter, 0, false));
    // `==` is one expression; `<=>` initializes, tests and returns subobject comparison results
    // or the equal result ([class.spaceship]).
    std::vector<Ex> results;
    std::optional<QualType> result_type;
    bool invalid_return = false;
    if (three_way) {
      if (const auto operands = comparison_operands(sm, d, params))
        if (const auto parts = defaulted_three_way(sm, operands->cls, operands->x, operands->y, w_->types.rec(d.type).inner)) {
          for (const auto& part : parts->subobjects) {
            results.push_back(part.initializer);
            results.push_back(part.unequal);
            results.push_back(part.return_value);
            invalid_return |= !part.return_value.ok();
          }
          results.push_back(parts->equal);
          result_type = parts->result;
        }
    } else if (const std::optional<Ex> equal = defaulted_comparison_result(sm, d, params)) {
      results.push_back(*equal);
    }
    const bool usable = !results.empty() && std::ranges::all_of(results, [](const Ex& e) { return e.ok(); });
    // Deleted: a reference or variant member (no result and nothing diagnosed), or a subobject
    // comparison that finds no viable, an ambiguous, a deleted or an inaccessible function. Any
    // other diagnostic is ycxx's own limitation: the operator stays declared, and analysing its
    // body reports it. Failure to initialize the return value is diagnosed in the body too.
    const bool deleted = !invalid_return && (diags.empty() ? !usable : std::ranges::all_of(diags, [](const engine::Diagnostic& g) {
      return std::ranges::any_of(std::array{Diag::invalid_operands, Diag::ambiguous_call, Diag::deleted_function,
                                            Diag::private_member, Diag::protected_member, Diag::inaccessible_base,
                                            Diag::ambiguous_base, Diag::ambiguous_member},
                                 [&](Diag k) { return g.id == sema::diag_id(k); });
    }));
    QualType type = d.type;
    const sema::TypeRec fn = w_->types.rec(d.type);
    // [class.spaceship]/2: a deduced return type is the common comparison category.
    if (!deleted && diags.empty() && result_type)
      type = w_->types.function(*result_type, fn.params, fn.variadic, fn.linkage, fn.fn_cv, fn.fn_ref, fn.fn_noexcept);
    if (!deleted && diags.empty() && !d.has_written_noexcept) {
      const QualType boolean = w_->types.builtin(sema::Builtin::bool_);
      const bool nonthrowing = std::ranges::all_of(results, [&](const Ex& e) {
        const auto literal = sm.as<ast::Literal>(sm.unary_noexcept(e, 0, 0));
        return literal && literal->value == w_->values.integer(boolean, 1);
      });
      if (diags.empty() && nonthrowing) {
        const sema::TypeRec t = w_->types.rec(type);
        type = w_->types.function(t.inner, t.params, t.variadic, t.linkage, t.fn_cv, t.fn_ref, true);
      }
    }
    sm.pop_scope();
    defaulted_comparisons_.insert_or_assign(f, DefaultedComparison{deleted, type, decl});
    if (member) {
      member->is_deleted = deleted;
      if (three_way) member->type = type;  // the deduced return type, for calls
    }
  };
  for (sema::MemberDeclRec& m : rec.members)
    if (m.is_function && !m.is_static && !m.is_template)
      if (const auto decl = member_decl(m.entity)) complete(m.entity, *decl, &m);
  for (const ast::MemberRec& f : rec.adl_friends) complete(f.entity, f.decl, nullptr);
}

// The initialization of a subobject of type `t` from the corresponding glvalue `src` of the source
// object ([class.copy.ctor]/14): an xvalue for a move.
Ex Program::copy_from(sema::Sema& sm, Ex src, QualType t, bool move) {
  if (w_->types.is_class(t)) return sm.construct(t, {src}, 0, 0);
  if (!w_->types.is_array(t)) return sm.convert(src, t, 0);
  // An array as a whole when the operation its elements select is trivial.
  const QualType element_type = w_->types.canonical(w_->types.innermost_array_element(t));
  bool whole = true;
  if (w_->types.is_class(element_type)) {
    const QualType zero = w_->types.builtin(sema::Builtin::int_);
    Ex first = src;
    for (QualType a = t; w_->types.is_array(a); a = w_->types.canonical(w_->types.rec(a).inner)) {
      first = sm.subscript(first, sm.literal(zero, w_->values.integer(zero, 0), ast::ValueCategory::prvalue, 0, 0), 0, 0);
      if (move && first.ok()) first = sm.static_cast_to(first, w_->types.rvalue_ref(first.type()), 0);
    }
    const Ex probe = sm.construct(sema::Types::unqualified(element_type), {first}, 0, 0);
    if (!probe.ok()) return probe;
    const auto conversion = sm.as<ast::ImplicitConversion>(probe);
    whole = conversion && conversion->conversion == ast::ConversionKind::lvalue_to_rvalue;
  }
  if (whole) return sm.convert_node(src, ast::ConversionKind::lvalue_to_rvalue, sema::Types::unqualified(t));
  sema::TypeRec r = w_->types.rec(t);
  std::vector<Ex> elems;
  QualType index = w_->types.builtin(sema::Builtin::int_);
  for (sema::u64 i = 0; i < r.bound; ++i) {
    Ex at = sm.literal(index, w_->values.integer(index, i), ast::ValueCategory::prvalue, 0, 0);
    Ex element = sm.subscript(src, at, 0, 0);
    if (move) element = sm.static_cast_to(element, w_->types.rvalue_ref(r.inner), 0);
    elems.push_back(copy_from(sm, element, r.inner, move));
  }
  return sm.list_of(t, std::move(elems), 0, 0);
}

// [class.copy.ctor]/14: the definition of a copy or move constructor initializes each direct base
// and each member from the other object's, in declaration order. No root when one cannot be formed.
Ex Program::copy_constructor_root(sema::Sema& sm, EntityId cls, const ast::DeclSema& decl, bool move) {
  const sema::ClassRec& rec = classes_.rec(cls);
  const NameId other = w_->ins.names.intern("other");
  const QualType parameter = w_->ins.seqs.at(decl.params, 0).type;
  sm.push_scope();
  sm.declare_local(other, parameter, sema::local_flag::parameter, 0, false);
  const auto source = [&] {
    const Ex name = sm.name_expr(other, 0, 0);
    return move ? sm.static_cast_to(name, parameter, 0) : name;
  };
  std::vector<ast::Child> stmts;
  bool failed = false;
  for (const sema::BaseRec& base : rec.bases) {
    const QualType bt = w_->types.class_type(base.cls);
    const Ex from = sm.to_base(source(), base.cls, 0);
    // A dynamic base, abstract or not, is copied by its own copy constructor.
    const auto* record = classes_.find(base.cls);
    const Ex init = record && record->dynamic
                        ? sm.construct(bt, {from}, 0, 0, sema::Construction::direct, sema::ConstructorDestination::polymorphic_base)
                        : copy_from(sm, from, bt, move);
    failed |= !init.ok();
    stmts.push_back({sm.node(ast::MemberInit{~0u, bt, {init.id, 0}}, 0, 0).id, 0});
  }
  for (u32 i = 0; i < rec.fields.size(); ++i) {
    const sema::FieldRec& f = rec.fields[i];
    Ex from = sm.member(source(), false, f.name, 0, 0, 0);
    // An rvalue reference member binds to what the other's refers to.
    if (w_->types.is_reference(f.type) && w_->types.rec(w_->types.canonical(f.type)).rvalue_ref)
      from = sm.static_cast_to(from, f.type, 0);
    const Ex init = copy_from(sm, from, f.type, move);
    failed |= !init.ok();
    stmts.push_back({sm.node(ast::MemberInit{i, f.type, {init.id, 0}}, 0, 0).id, 0});
  }
  const Ex body = sm.node(ast::Compound{sm.builder().list({})}, 0, 0);
  stmts.push_back({body.id, 0});
  const Ex root = failed || sm.has_error_since(0) ? Ex{} : sm.node(ast::Compound{sm.builder().list(stmts)}, 0, 0);
  sm.pop_scope();
  return root;
}

const ast::BodySema& Program::synthesized_body(std::size_t i) {
  if (auto it = synth_bodies_.find(i); it != synth_bodies_.end()) return it->second;
  const Synthesized& s = synth_[i];
  if (s.kind == SpecialMember::copy_assignment || s.kind == SpecialMember::move_assignment)
    return *copy_assignments_.at(s.decl.entity).body;
  if (s.kind == SpecialMember::default_constructor) {
    std::optional<Instantiation> inst;
    if (const auto args = class_args_.find(std::to_underlying(w_->entities.rec(s.decl.entity).scope)); args != class_args_.end())
      inst = Instantiation{s.decl, args->second};
    Result<ast::BodySema> result = BodyAnalysis(*this, s.item, inst).implicit_default_constructor(s.decl);
    if (inst) synth_results_[i] = {std::move(result.demands), std::move(result.diags)};
    return synth_bodies_[i] = result.value;
  }
  const bool specialization = class_patterns_.contains(std::to_underlying(w_->entities.rec(s.decl.entity).scope));
  return synth_bodies_[i] = synthesize_body(s, specialization ? std::optional(end()) : std::nullopt);
}

// The body of an implicit copy constructor or destructor; a specialization's as of `point`.
ast::BodySema Program::synthesize_body(const Synthesized& s, std::optional<u32> point) {
  ast::Builder b(w_->ins, &arena_);
  std::vector<engine::Diagnostic> diags;  // none: the class was analysed
  AtItem q(*this, s.item, true);
  q.own_class_complete = true;
  sema::Sema sm(*w_, b, q, diags, id(s.item), 0);
  EntityId cls = w_->entities.rec(s.decl.entity).scope;
  q.at_point = point;
  sm.current_class = cls;
  sm.this_type = this_type(s.decl);
  Ex root;
  if (s.kind == SpecialMember::copy_constructor) {
    // A copy that cannot be formed leaves no body: code generation reports it.
    root = copy_constructor_root(sm, cls, s.decl, /*move=*/false);
  } else {
    Ex body = sm.node(ast::Compound{b.list({})}, 0, 0);
    root = BodyAnalysis::destructor_body(sm, cls, body);
  }
  std::vector<ast::LocalRec> locals(sm.locals().begin(), sm.locals().end());
  return b.commit(ast::BodySema{root.id, b.seq(std::span<const ast::LocalRec>(locals)),
                                                   b.seq(std::span<const ast::ExpansionRec>{}), b.seq(std::span<const u32>{}),
                                                   QualType{}, 0, b.seq(std::span<const ast::ClosureDef>{})});
}

const Result<ast::BodySema>& Program::analyze_body(u32 item) {
  if (!body_cells_[item]) body_cells_.at(item) = BodyAnalysis(*this, item).run();
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

std::optional<sema::BaseRec> Program::instantiate_base(EntityId specialization, const ClassBasePattern& pattern, u32 item, u32 point,
                                                       std::span<const ast::TemplateParamSema> params, const sema::Bindings& args) {
  const QualType type = pattern.base.type;
  const bool dependent = w_->types.is_dependent(type);
  if (!dependent && !pattern.replay) return sema::BaseRec{w_->types.class_of(type), pattern.base.access};

  std::vector<engine::Diagnostic> sink;
  ast::Builder b(w_->ins, &arena_);
  AtItem q(*this, item, false);
  q.at_point = point;
  sema::Sema s(*w_, b, q, sink, id(item), 0);
  const bool member_class = named_nested_class(item);
  if (member_class) {
    // The base-clause is in the owner's scope. The owner can still be declaring
    // members if a later by-value field is what requires this class to be complete.
    q.instantiating = w_->entities.rec(specialization).scope;
    s.current_class = q.instantiating;
  }
  std::optional<QualType> substituted;
  if (pattern.replay) {
    // An alias can discard an argument whose written type must still substitute ([temp.alias]).
    const items::Item& it = items_->items[item];
    Parser ps(*r_, it.begin, it.begin + pattern.end, s);
    ps.set_questions(q);
    q.type_trait_demand = point;
    ps.seek(it.begin + pattern.begin);
    s.template_scopes.emplace_back(params.begin(), params.end());
    s.template_args = args;
    try {
      DeclSpec spec = ps.decl_specifiers();
      if (spec.ok && spec.has_type && ps.at_end()) substituted = spec.type;
    } catch (const TooDeep&) {
      // The parser has already diagnosed the resource limit.
    }
  } else {
    substituted = s.templates().substitute(type, args);
  }
  if (!substituted && !s.has_error_since(0) && !q.observed_type_trait_failure)
    s.diag(pattern.begin, Diag::base_substitution, type);
  // A nondependent base's completeness was checked at the definition, even if its spelling
  // has dependent arguments whose validity must be checked here.
  std::optional<EntityId> base;
  if (substituted && !s.has_error_since(0) && !q.observed_type_trait_failure)
    base = dependent ? complete_base_class(s, *substituted, pattern.begin, /*allow_polymorphic=*/true) : std::optional(w_->types.class_of(type));
  if (q.observed_class_assertion_failure) failed_class_assertions_.insert(specialization);
  else if (q.observed_type_trait_failure) failed_type_trait_instantiations_.insert(specialization);
  else if (member_class && !base && s.has_error_since(0)) failed_member_class_instantiations_.insert(specialization);
  for (engine::Diagnostic& d : sink) {
    if (sema::is_type_trait_failure(d.id)) failed_type_trait_instantiations_.insert(specialization);
    if (!std::ranges::contains(end_diags_, d)) end_diags_.push_back(std::move(d));
  }
  if (!base) return std::nullopt;
  return sema::BaseRec{*base, pattern.base.access, dependent};
}

const sema::ClassRec* Program::instantiate_class(EntityId spec, u32 point) {
  const u32 key = std::to_underlying(spec);
  // This unit's instantiation: its member records, member patterns and diagnostics are the
  // unit's, even when another unit published the class's record first (#156).
  // Instantiated already: complete at points after its pattern's definition, wherever it was
  // first needed.
  if (classes_instantiated_.contains(key)) return point > last_member(class_patterns_.at(key)) ? classes_.find(spec) : nullptr;
  const sema::EntityRec rec = w_->entities.rec(spec);
  if (failed_class_assertions_.contains(spec) || failed_type_trait_instantiations_.contains(spec)) return nullptr;
  if (w_->types.is_dependent(w_->types.class_type(spec)) || classes_instantiating_.contains(key) || partial_matching_.contains(key))
    return nullptr;
  // Its pattern: the partial specialization that matches it, else the template's definition.
  std::optional<Chosen> partial = choose_partial(spec, point);
  if (partial && (partial->ambiguous || !partial->defined)) {
    if (partial->ambiguous) ambiguous_partial(spec, point);
    return nullptr;
  }
  ensure_declared(rec.templ, point);
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
  return replay_class(spec, k, point, params, args);
}

// [temp.inst]/3: instantiating the enclosing class supplies the member declaration. Only
// a demand for completeness replays its definition with the enclosing arguments bound.
const sema::ClassRec* Program::instantiate_member_class(EntityId cls, u32 point) {
  const u32 key = std::to_underlying(cls);
  if (failed_class_assertions_.contains(cls) || failed_type_trait_instantiations_.contains(cls) ||
      failed_member_class_instantiations_.contains(cls) || classes_instantiating_.contains(key)) return nullptr;
  const u32 k = class_patterns_.at(key);
  if (point <= last_member(k)) return nullptr;
  if (classes_instantiated_.contains(key)) return classes_.find(cls);
  if (too_deep(classes_instantiating_.size(), k)) return nullptr;
  const EntityId owner = w_->entities.rec(cls).scope;
  const auto& args = class_args_.at(std::to_underlying(owner));
  class_def(k);
  const auto heads = enclosing_heads(k);
  return replay_class(cls, k, point, heads.front(), args);
}

const sema::ClassRec* Program::replay_class(EntityId spec, u32 k, u32 point,
                                          std::span<const ast::TemplateParamSema> params, const sema::Bindings& args) {
  const u32 key = std::to_underlying(spec);
  classes_instantiating_.insert(key);
  std::vector<sema::BaseRec> bases;
  if (auto p = class_base_patterns_.find(k); p != class_base_patterns_.end()) {
    auto base = instantiate_base(spec, p->second, k, point, params, args);
    if (!base) {
      classes_instantiating_.erase(key);
      return nullptr;
    }
    bases.push_back(*base);
    if (!base->dependent_in_pattern) set_bases(spec, bases);
  }
  if (!bases.empty()) instantiating_bases_.insert_or_assign(spec, bases.front().cls);
  class_args_.emplace(key, args);
  class_patterns_.emplace(key, k);
  class_points_.emplace(key, point);
  // Each member item's declaration part, again, with the arguments bound: the members of `spec`,
  // asked at the point of instantiation ([temp.point]).
  bool member_declarations_valid = true;
  bool can_complete = true;
  ClassContent c = class_content(k, [&](u32 m) -> const ast::ItemSema& {
    auto cell_key = std::pair(m, key);
    const auto& written = item_sema(m);
    member_declarations_valid &= valid_member_declarations(written.diags);
    const bool assertion = class_assertion(m);
    can_complete &= !items_with_failed_assertion_dependencies_.contains(m);
    if (assertion) can_complete &= valid_class_assertion(written);
    if (!class_member_cells_.contains(cell_key)) {
      ItemAnalysis analysis(*this, m, ClassInstantiation{spec, args, point});
      Result<ast::ItemSema> cell = analysis.run();
      can_complete &= !analysis.class_assertion_failure();
      member_declarations_valid &= valid_member_declarations(cell.diags);
      // What the definition diagnosed it says once, there.
      if (assertion) {
        std::erase_if(cell.diags, [&](const auto& diag) { return std::ranges::contains(written.diags, diag); });
      } else if (!written.diags.empty()) {
        cell.diags.clear();
      }
      class_member_cells_.emplace(cell_key, std::move(cell));
      class_member_order_.push_back(cell_key);
      // A later member can name a static member function whose return type only its own
      // instantiation tells ([dcl.spec.auto.general]/11); other members wait for the whole class.
      for (const ast::DeclSema& d : w_->ins.seqs.elements(class_member_sema(m, spec).decls))
        if (d.entity != EntityId::none && d.kind == ast::DeclKind::function && (d.flags & sema::decl_flag::static_) &&
            !(d.flags & (sema::decl_flag::friend_ | sema::decl_flag::template_)) && w_->types.has_placeholder(d.type))
          member_patterns_.emplace(std::to_underlying(d.entity), MemberOf{m, spec});
    }
    const auto& member = class_member_cells_.at(cell_key);
    member_declarations_valid &= valid_member_declarations(member.diags);
    if (assertion) can_complete &= valid_class_assertion(member);
    return member.value;
  });
  classes_instantiating_.erase(key);
  instantiating_bases_.erase(spec);
  if (!can_complete) {
    failed_class_assertions_.insert(spec);
    return nullptr;
  }
  if (named_nested_class(k) && !member_declarations_valid) {
    failed_member_class_instantiations_.insert(spec);
    return nullptr;
  }
  classes_instantiated_.insert(key);
  for (const ast::MemberRec& m : c.members)
    if (const auto kind = w_->entities.rec(m.entity).kind; kind != sema::EntityKind::data_member && kind != sema::EntityKind::class_)
      member_patterns_.emplace(std::to_underlying(m.entity), MemberOf{index(m.decl.item), spec});
  // A friend function the class defines is instantiated from its item as members are
  // ([temp.inst]/4); the first specialization to define it does.
  for (const ast::MemberRec& f : c.adl_friends) {
    const u32 m = index(f.decl.item);
    auto [prior, fresh] = member_patterns_.emplace(std::to_underlying(f.entity), MemberOf{m, spec});
    // Two specializations that define one function redefine it ([basic.def.odr]/1, [temp.inst]/3).
    if (!fresh && prior->second.spec != spec) {
      u32 offset = 0;
      for (const items::DeclName& n : items_->items[m].names)
        if (n.ordinal == ast::number_of(f.decl.ord)) offset = n.offset;
      const ast::NameId name = w_->entities.rec(f.entity).name;
      end_diags_.push_back({engine::Loc{std::to_underlying(id(m)), offset}, sema::diag_id(sema::Diag::redefinition),
                            {{std::to_underlying(name), w_->ins.names.fp(name)}}});
    }
  }
  // Built whole, then published once (#156); the first publication wins.
  sema::ClassRec built;
  built.member_declarations_valid = member_declarations_valid;
  built.union_ = class_rec_is_union(k);
  built.unapplied_layout = has_unapplied_class_layout(k, c);
  built.bases = std::move(bases);
  built.fields = std::move(c.fields);
  built.members = std::move(c.statics);
  built.types = std::move(c.nested);
  built.alias_templates = std::move(c.alias_templates);
  for (const ast::MemberRec& m : c.members) built.access.push_back({m.entity, m.access});
  built.friends = std::move(c.friends);
  built.adl_friends = std::move(c.adl_friends);
  if (const auto head = class_heads_.find(k); head != class_heads_.end()) built.is_final = head->second.is_final;
  std::vector<engine::Diagnostic> diags;
  eligible_special_members(k, spec, built, point);
  special_members(k, spec, built, built.union_ || !built.bases.empty() ? &diags : nullptr, nullptr, point);
  for (engine::Diagnostic& d : diags)
    if (built.union_ || !std::ranges::contains(end_diags_, d)) end_diags_.push_back(std::move(d));
  pure_final_overriders(built, spec);
  complete_virtual_members(k, spec, built, c.members, member_declarations_valid);
  built.defined = true;
  return &classes_.publish(spec, std::move(built), [&](sema::ClassRec& fresh) { validate_dynamic_constructors(fresh); });
}

// The item that defines, outside its class, the member of a class template specialization that
// `spec` is: the definition of the current instantiation's member declared at the same place.
std::optional<u32> Program::out_of_class_definition(EntityId spec, MemberOf of) {
  const Result<ast::ItemSema>& cell = class_member_cells_.at(std::pair(of.item, std::to_underlying(of.spec)));
  std::vector<ast::DeclSema> inst = w_->ins.seqs.elements(cell.value.decls);
  std::vector<ast::DeclSema> pattern = w_->ins.seqs.elements(item_sema(of.item).value.decls);
  for (std::size_t i = 0; i < inst.size() && i < pattern.size(); ++i) {
    if (inst[i].entity != spec) continue;
    ensure_declared(pattern[i].entity, end());
    auto def = definitions_.find(std::to_underlying(pattern[i].entity));
    if (def != definitions_.end()) return def->second;
  }
  return std::nullopt;
}

const Program::VarInstance* Program::variable_instance(EntityId v, u32 point,
                                                       std::optional<QualType> potential_constant_type) {
  const u32 key = std::to_underlying(v);
  const sema::EntityRec& rec = w_->entities.rec(v);
  if (potential_constant_type &&
      ((rec.scope != EntityId::none && w_->entities.rec(rec.scope).kind != sema::EntityKind::namespace_) ||
       std::ranges::any_of(rec.args, [&](const ast::TemplateArg& arg) { return w_->arg_dependent(arg); })))
    return nullptr;
  auto qualifies = [&](const ast::DeclSema& pattern) {
    if (!potential_constant_type || (pattern.flags & sema::decl_flag::constexpr_)) return true;
    const QualType type = w_->types.canonical(*potential_constant_type);
    return w_->types.is_reference(type) ||
           (sema::Types::is_const(type) && !sema::Types::is_volatile(type) &&
            (w_->types.is_integral(type) || w_->types.is_enum(type)));
  };
  if (auto it = var_index_.find(key); it != var_index_.end()) {
    const VarInstance& cached = var_instances_[it->second];
    if (cached.item >= point) return nullptr;
    if (potential_constant_type && !qualifies(w_->ins.seqs.at(item_sema(cached.item).value.decls, 0))) return nullptr;
    return &cached;
  }
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
  ensure_declared(rec.templ, point);
  auto t = var_templates_.find(std::to_underlying(rec.templ));
  if (!partial && (t == var_templates_.end() || t->second >= point)) return nullptr;
  if (potential_constant_type && partial && !partial->defined) return nullptr;
  const u32 k = partial ? partial->item : t->second;
  // [temp.inst]/16: nested instantiations are limited, as functions' are.
  if (too_deep(var_instantiating_.size(), k)) return nullptr;
  ast::DeclSema tmpl = w_->ins.seqs.at(item_sema(k).value.decls, 0);
  if (!qualifies(tmpl)) return nullptr;
  std::vector<ast::TemplateParamSema> head = w_->ins.seqs.elements(tmpl.template_params);
  sema::Bindings args = partial ? partial->args : sema::Bindings::of(head.empty() ? 0 : head[0].depth, rec.args);
  var_instantiating_.insert(key);
  ItemAnalysis analysis(*this, k, VarInstantiation{v, std::move(args), point, partial ? std::nullopt : std::optional(tmpl)});
  Result<ast::ItemSema> cell = analysis.run();
  var_instantiating_.erase(key);
  // What the definition diagnosed it says once, there.
  const auto& pattern_diags = item_sema(k).diags;
  if (!pattern_diags.empty()) cell.diags.clear();
  const bool diagnosed_failure = analysis.variable_instantiation_failure() ||
                                  !valid_member_declarations(cell.diags) || !valid_member_declarations(pattern_diags);
  if (analysis.type_trait_failure() || std::ranges::any_of(cell.diags, [](const auto& d) {
        return sema::is_type_trait_failure(d.id);
      })) failed_type_trait_instantiations_.insert(v);
  var_index_.emplace(key, var_instances_.size());
  var_instances_.push_back({v, k, std::move(cell), diagnosed_failure});
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
  if (member != member_patterns_.end() && defaulted_special_member(spec)) return nullptr;  // synthesized
  if (member == member_patterns_.end()) ensure_declared(rec.templ, point);
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
  if (!member_body_ready(k)) return nullptr;
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
  // A friend template of a class template specialization: that specialization's (#259).
  auto outer = class_args_.find(std::to_underlying(w_->entities.rec(rec.templ).scope));
  if (auto friend_of = member_patterns_.find(std::to_underlying(rec.templ)); outer == class_args_.end() && friend_of != member_patterns_.end())
    outer = class_args_.find(std::to_underlying(friend_of->second.spec));
  if (outer != class_args_.end()) {
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
  instantiating_.insert(key);
  struct Instantiating {
    std::set<u32>& active;
    u32 key;
    ~Instantiating() { active.erase(key); }
  } instantiating{instantiating_, key};
  AtItem parameter_questions(*this, k, false);
  parameter_questions.at_point = point;
  std::vector<engine::Diagnostic> parameter_diagnostics;
  ast::Builder parameter_builder(w_->ins, &arena_);
  sema::Sema parameter_sema(*w_, parameter_builder, parameter_questions, parameter_diagnostics, id(k), 0);
  sema::Templates t = templates();
  t.call_constant = [&parameter_sema](EntityId templ, std::span<const ast::TemplateArg> args) {
    return parameter_sema.evaluate_dependent_call(templ, args, 0);
  };
  std::vector<ast::ParamSema> params;
  // A parameter's type is adjusted again after substitution ([dcl.fct]/5): an array or function
  // becomes a pointer; the body's object keeps its cv-qualifiers. A function parameter pack is a
  // parameter for each element ([temp.variadic]/8).
  sema::Types& ty = w_->types;
  auto adjusted = [&](ast::ParamSema p) {
    p.type = adjusted_parameter_type(ty, p.type);
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
    in.body = BodyAnalysis(*this, k, Instantiation{decl, std::move(args)}).run();
    if (in.body.value.deduced) in.decl.type = in.body.value.deduced_type;
  }
  in.body.diags.append_range(std::move(parameter_diagnostics));
  instance_index_.emplace(key, instances_.size());
  instances_.push_back(std::move(in));
  instance_order_.push_back(&instances_.back());
  return &instances_.back();
}

// The entities a tree names, in tree order.
void Program::references(NodeId n, std::vector<EntityId>& out) const {
  if (n == NodeId::none || w_->ins.ast.is<ast::ConstantProbe>(n)) return;
  ast::dispatch<ast::Nodes>(ast::kind_of(n), [&]<class T>(std::type_identity<T>) {
    T v = w_->ins.ast.get<T>(n);
    ast::for_each_ref(v, [&]<class H>(H& h) {
      if constexpr (std::is_same_v<H, EntityId>) {
        if (h != EntityId::none) out.push_back(h);
      } else if constexpr (std::is_same_v<H, NodeId>) {
        references(h, out);
      } else if constexpr (std::is_same_v<H, ast::ChildList>) {
        for (ast::Child c : w_->ins.ast.children(h)) references(c.id, out);
      } else if constexpr (std::is_same_v<H, ConstId>) {
        references(h, out);
      }
    });
  });
}

void Program::references(ConstId value, std::vector<EntityId>& out) const {
  if (value == ConstId::none) return;
  const auto& values = w_->ins.values;
  if (values.is<ast::VLvalue>(value)) {
    const ConstId base = values.get<ast::VLvalue>(value).base;
    if (values.is<ast::VEntity>(base)) out.push_back(values.get<ast::VEntity>(base).entity);
  } else if (values.is<ast::VAggregate>(value)) {
    for (ConstId element : w_->ins.seqs.elements(values.get<ast::VAggregate>(value).elems))
      references(element, out);
  }
}

void Program::part_references(NodeId root, ast::SeqId<ast::ClosureDef> closures, std::vector<EntityId>& out) const {
  references(root, out);
  for (const auto& closure : w_->ins.seqs.elements(closures)) {
    if (!sema::Sema::closure_has_code(*w_, closure)) continue;
    references(closure.body, out);
    references(closure.invoker_body, out);
  }
}

// A potentially-constant variable named by a surviving expression needs its definition for
// constant evaluation ([temp.inst]/7, [expr.const]), even when the expression is read at run
// time. Visit committed ordinary namespace items and bodies before moving to the next source
// item so a later constant evaluation cannot choose their specialization's context instead.
void Program::instantiate_variable_demands(std::span<const sema::Demand> demands, u32 point) {
  struct Pending {
    sema::Demand demand;
    u32 depth;
  };
  std::vector<Pending> work;
  auto enqueue = [&](std::span<const sema::Demand> ds, u32 depth) {
    for (const sema::Demand& d : ds)
      if (d.variable_type) work.push_back({d, depth});
  };
  enqueue(demands, 1);
  std::set<EntityId> seen;
  for (std::size_t i = 0; i < work.size(); ++i) {
    const auto [d, depth] = work[i];
    // Leave depth-limit diagnostics to the END worklist, with its original demand chain.
    if (depth > instantiation_limit || !seen.insert(d.spec).second) continue;
    const auto* inst = variable_instance(d.spec, point, d.variable_type);
    if (!inst || !inst->cell.diags.empty()) continue;
    // The initializer's demands inherit the outer point. Their own source locations are kept
    // for the END worklist and diagnostics in the initializer still refer to its own tokens.
    enqueue(inst->cell.demands, depth + 1);
  }
}

void Program::instantiate_all() {
  // [temp.expl.spec]/7: a specialization is explicitly specialized before its first use that
  // would instantiate it: before any earlier cell demanded it, or instantiated the class.
  specializations_before(end());
  // The demands of every cell, in item order, then those of each instance as it is made: the END
  // worklist of §B6.2, deterministic.
  std::vector<sema::Demand> work;
  // What a body requires, besides what its cell demanded: every member function of a class
  // template specialization it names, a constructor or destructor it runs included
  // ([temp.inst]/4), found by walking its tree. A synthesized member emitted only on use is walked
  // where something uses it.
  std::map<EntityId, std::size_t> unused_synthesized;
  std::set<EntityId> used_special_members;
  std::set<EntityId> tables;
  std::vector<std::size_t> promoted_initializers;
  std::function<void(std::span<const EntityId>, ItemId)> require;
  auto required_members = [&](NodeId root, ItemId at) {
    std::vector<EntityId> refs;
    references(root, refs);
    // A constructor can destroy the members it has initialized ([except.ctor]).
    if (root != NodeId::none && w_->ins.ast.is<ast::Compound>(root))
      for (ast::Child c : w_->ins.ast.children(w_->ins.ast.get<ast::Compound>(root).stmts))
        if (w_->ins.ast.is<ast::MemberInit>(c.id))
          if (const EntityId d = classes_.destructor_of(w_->ins.ast.get<ast::MemberInit>(c.id).type); d != EntityId::none)
            refs.push_back(d);
    require(refs, at);
  };
  // A dynamic class's table names the definitions in its slots: emitting a constructor or a
  // destructor that installs it requires those of its class template specializations ([temp.inst]).
  auto require_table = [&](EntityId cls, ItemId at) {
    const auto* owner = classes_.find(cls);
    if (!owner || !owner->dynamic || !tables.insert(cls).second) return;
    std::vector<EntityId> slots;
    for (const sema::VirtualSlot& slot : owner->dynamic->slots)
      if (!slot.pure) slots.push_back(slot.function);
    require(slots, at);
  };
  auto special_member = [&](EntityId e) {
    const auto kind = w_->entities.rec(e).kind;
    return kind == sema::EntityKind::constructor || kind == sema::EntityKind::destructor;
  };
  auto require_installed = [&](EntityId f, ItemId at) {
    if (special_member(f)) require_table(w_->entities.rec(f).scope, at);
  };
  auto walk_one = [&](std::size_t i) {
    require_installed(synth_[i].decl.entity, id(synth_[i].item));
    const auto [body, closures] = [&] {
      const ast::BodySema& b = synthesized_body(i);
      return std::pair(b.tree, b.closures);
    }();
    if (const auto result = synth_results_.find(i); result != synth_results_.end()) {
      for (const engine::Diagnostic& d : result->second.second)
        if (!std::ranges::contains(end_diags_, d)) end_diags_.push_back(d);
      work.insert(work.end(), result->second.first.begin(), result->second.first.end());
    }
    const ItemId at = id(synth_[i].item);
    required_members(body, at);
    // The lambdas of default member initializers that a constructor runs.
    for (const auto& closure : w_->ins.seqs.elements(closures)) {
      if (!sema::Sema::closure_has_code(*w_, closure)) continue;
      required_members(closure.body, at);
      required_members(closure.invoker_body, at);
    }
  };
  require = [&](std::span<const EntityId> refs, ItemId at) {
    for (EntityId e : refs) {
      // The definition of a nontrivial defaulted destructor that something uses.
      if (auto destructor = defaulted_destructors_.extract(e); !destructor.empty()) synth_.push_back(destructor.mapped());
      if (const auto unused = unused_synthesized.extract(e); !unused.empty()) walk_one(unused.mapped());
      if (may_have_virtual_layouts_ && special_member(e)) used_special_members.insert(e);
      if (auto assign = copy_assignments_.find(e); assign != copy_assignments_.end() && assign->second.body &&
          !assign->second.emitted) {
        assign->second.emitted = true;
        synth_.push_back(assign->second.definition);
        work.insert(work.end(), assign->second.demands.begin(), assign->second.demands.end());
      }
      if (auto constructor = defaulted_constructors_.find(e);
          constructor != defaulted_constructors_.end() && !constructor->second.emitted) {
        constructor->second.emitted = true;
        const auto& body = defaulted_constructor_body(constructor->second);
        // A default member initializer that is ill-formed or that ycxx does not support.
        for (const engine::Diagnostic& d : body.diags)
          if (!std::ranges::contains(end_diags_, d)) end_diags_.push_back(d);
        synth_bodies_.emplace(synth_.size(), body.value);
        synth_.push_back(constructor->second.definition);
        work.insert(work.end(), body.demands.begin(), body.demands.end());
      }
      if (auto move = move_constructors_.extract(e); !move.empty()) {
        auto& pending = move.mapped();
        synth_bodies_.emplace(synth_.size(), pending.body);
        synth_.push_back(pending.definition);
        work.insert(work.end(), pending.demands.begin(), pending.demands.end());
      }
      if (const auto i = member_initializer_index(e); i && member_initializers_[*i].type_only) {
        member_initializers_[*i].type_only = false;
        // This initializer was already evaluated. Emitting its storage has no new semantic point.
        retain_member(e);
        promoted_initializers.push_back(*i);
      }
      // A closure's callable address can survive folding even when its owner's storage does not.
      if (const auto c = declaration_closures_.find(std::to_underlying(e));
          c != declaration_closures_.end() && c->second.lazy_member != EntityId::none)
        if (const auto i = member_initializer_index(c->second.lazy_member); i && !member_initializers_[*i].emits_closures()) {
          member_initializers_[*i].native_closure_use = true;
          promoted_initializers.push_back(*i);
        }
      if (!defaulted_special_member(e) &&
          (member_patterns_.contains(std::to_underlying(e)) || type_only_instances_.contains(e)))
        work.push_back({e, ast::Loc{at, 0}});
    }
  };
  const auto required_part = [&](NodeId root, ast::SeqId<ast::ClosureDef> closures, ItemId at) {
    std::vector<EntityId> refs;
    part_references(root, closures, refs);
    require(refs, at);
  };
  auto require_value = [&](ConstId value, ItemId at) {
    if (value == ConstId::none) return;
    std::vector<EntityId> refs;
    references(value, refs);
    require(refs, at);
  };
  // A constant initializer can name a function in the emitted data instead of in a body.
  auto require_static_values = [&](const ast::ItemSema& part, ItemId at) {
    if (part.tree == NodeId::none || !w_->ins.ast.is<ast::DeclGroup>(part.tree)) return;
    for (const ast::Child child : w_->ins.ast.children(w_->ins.ast.get<ast::DeclGroup>(part.tree).declarators)) {
      const auto declarator = w_->ins.ast.get<ast::Declarator>(child.id);
      if (declarator.value == ConstId::none) continue;
      const auto declaration = w_->ins.seqs.at(part.decls, declarator.decl);
      if (declaration.kind == ast::DeclKind::variable && (declaration.flags & sema::decl_flag::definition))
        require_value(declarator.value, at);
    }
  };
  // The items with cells, in item order: a group's Program has few (#947).
  std::set<u32> analysed;
  item_cells_.add_items(analysed);
  class_cells_.add_items(analysed);
  body_cells_.add_items(analysed);
  for (u32 k : analysed) {
    if (item_cells_[k]) {
      work.insert(work.end(), item_cells_[k]->demands.begin(), item_cells_[k]->demands.end());
      // A namespace-scope variable's initializer, and its destructor ([basic.start.term]), are code
      // of the unit's initialization function.
      if (!templated_body(k) && in_class_[k]) {
        const auto& part = item_cells_[k]->value;
        required_part(NodeId::none, part.closures, id(k));
        require_static_values(part, id(k));
      }
      if (!templated_body(k) && !in_class_[k]) {
        const auto& part = item_cells_[k]->value;
        required_part(part.tree, part.closures, id(k));
        for (const ast::DeclSema& d : w_->ins.seqs.elements(item_cells_[k]->value.decls))
          if (d.kind == ast::DeclKind::variable && d.entity != EntityId::none && (d.flags & sema::decl_flag::definition)) {
            QualType t = d.type;
            while (w_->types.is_array(t)) t = w_->types.rec(t).inner;
            EntityId dtor = w_->types.is_class(t) ? classes_.destructor_of(t) : EntityId::none;
            if (dtor != EntityId::none) require(std::array{dtor}, id(k));
          }
      }
    }
    if (class_cells_[k])
      work.insert(work.end(), class_cells_[k]->demands.begin(), class_cells_[k]->demands.end());
    if (body_cells_[k]) {
      work.insert(work.end(), body_cells_[k]->demands.begin(), body_cells_[k]->demands.end());
      const auto& part = body_cells_[k]->value;
      if (!templated_body(k)) {
        required_part(part.tree, part.closures, id(k));
        if (w_->ins.seqs.size(item_cells_[k]->value.decls) > 0)
          if (const EntityId f = w_->ins.seqs.at(item_cells_[k]->value.decls, 0).entity; f != EntityId::none)
            require_installed(f, id(k));
      }
    }
  }
  // A key function defined here owns its class's table (Itanium C++ ABI §5.2.3).
  const auto classes = classes_.classes();
  for (EntityId c : std::vector<EntityId>(classes.begin(), classes.end())) {
    const auto& record = classes_.rec(c);
    if (!record.dynamic || record.dynamic->key_function == EntityId::none ||
        !function_defined_before(record.dynamic->key_function, end())) continue;
    if (const auto at = member_decl(record.dynamic->key_function)) require_table(c, at->item);
  }
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
    if (!used) {
      used = std::ranges::any_of(class_member_order_, [&](const auto& key) {
        const auto& cell = class_member_cells_.at(key);
        return class_points_.at(key.second) < k && cell.diags.empty() &&
               std::ranges::any_of(cell.demands, [&](const sema::Demand& d) { return std::to_underlying(d.spec) == e; });
      });
    }
    // Cleanup references in ordinary bodies and variable definitions are first collected above.
    // A template body added later to the worklist can be instantiated after its source item.
    if (!used && w_->entities.rec(EntityId(e)).kind == sema::EntityKind::destructor)
      used = std::ranges::any_of(work, [&](const sema::Demand& demand) {
        return std::to_underlying(demand.spec) == e && index(demand.at.item) < k;
      });
    if (!used) continue;
    ast::NameId name = w_->entities.rec(EntityId(e)).name;
    auto at = spec_names_.find(e);
    end_diags_.push_back({engine::Loc{std::to_underlying(id(k)), at == spec_names_.end() ? 0 : at->second},
                          sema::diag_id(sema::Diag::specialized_after_use),
                          {{std::to_underlying(name), w_->ins.names.fp(name)}}});
  }
  // The synthesized special members' bodies, those of classes instantiated as the worklist runs
  // included.
  std::size_t walked = 0;
  auto walk_synthesized = [&] {
    for (; walked < synth_.size(); ++walked) {
      EntityId cls = w_->entities.rec(synth_[walked].decl.entity).scope;
      if (w_->types.is_dependent(w_->types.class_type(cls))) continue;
      const EntityId member = synth_[walked].decl.entity;
      if (emitted_on_use(member) && !used_special_members.contains(member)) unused_synthesized.emplace(member, walked);
      else walk_one(walked);
    }
  };
  walk_synthesized();
  std::vector<u32> depth(work.size(), 1);
  const u32 limit = instantiation_limit;
  std::set<u32> seen;
  std::size_t walked_members = 0;
  std::size_t walked_initializers = 0;
  std::size_t walked_promotions = 0;
  std::size_t walked_member_values = 0;
  std::size_t walked_instances = 0;
  std::size_t walked_variable_values = 0;
  std::set<EntityId> included_initializers;
  auto require_initializer = [&](const MemberInitializer& initializer) {
    if (!included_initializers.insert(initializer.member).second) return;
    work.append_range(initializer.demands);
    required_part(NodeId::none, initializer.closures, id(initializer.item));
  };
  for (std::size_t i = 0; ; ++i) {
    // To a fixed point: a closure's body can need a synthesized member, whose body can instantiate
    // a class with closures of its own.
    do {
      for (; walked_initializers < member_initializers_.size(); ++walked_initializers) {
        const auto& initializer = member_initializers_[walked_initializers];
        if (initializer.emits_closures() && w_->ins.seqs.size(initializer.closures) != 0)
          require_initializer(initializer);
      }
      depth.resize(work.size(), 1);
      // Class-specialization replay can add member cells while processing this worklist.
      for (; walked_members < class_member_order_.size(); ++walked_members) {
        const auto key = class_member_order_[walked_members];
        const auto& cell = class_member_cells_.at(key);
        if (!cell.diags.empty()) continue;
        const auto& part = cell.value;
        required_part(NodeId::none, part.closures, id(key.first));
        require_static_values(part, id(key.first));
        depth.resize(work.size(), 1);
        for (sema::Demand demand : cell.demands) {
          if (!demand.variable_type) continue;
          const auto point = class_points_.at(key.second);
          if (point < end()) demand.at = ast::Loc{id(point), 0};
          work.push_back(demand);
          depth.push_back(1);
        }
      }
      // A class static can retain a function address after replaying the initializer
      // separately from its declaration.
      for (; walked_member_values < used_members_.size(); ++walked_member_values) {
        const EntityId member = used_members_[walked_member_values];
        const auto initializer = member_initializer_index(member);
        if (!initializer || member_initializers_[*initializer].type_only) continue;
        const auto& m = member_initializers_[*initializer];
        require_initializer(m);
        require_value(member_init(member), id(m.item));
      }
      for (; walked_promotions < promoted_initializers.size(); ++walked_promotions) {
        const auto& initializer = member_initializers_[promoted_initializers[walked_promotions]];
        require_initializer(initializer);
        if (!initializer.type_only) require_value(member_init(initializer.member), id(initializer.item));
      }
      depth.resize(work.size(), 1);
      if (i == work.size() && has_type_only_instances()) {
        // Other checked instances still follow the ordinary emission rules. Walk their
        // definitions if they can promote an instance checked during deduction.
        for (; walked_instances < instance_order_.size(); ++walked_instances) {
          const Instance& in = *instance_order_[walked_instances];
          if (emits_instance(in.spec) && !seen.contains(std::to_underlying(in.spec)))
            work.push_back({in.spec, {id(in.item), 0}});
        }
        for (; walked_variable_values < var_instances_.size(); ++walked_variable_values) {
          const VarInstance& v = var_instances_[walked_variable_values];
          if (emits_instance(v.spec) && !seen.contains(std::to_underlying(v.spec)))
            work.push_back({v.spec, {id(v.item), 0}});
        }
        depth.resize(work.size(), 1);
      }
      walk_synthesized();
      depth.resize(work.size(), 1);
    } while (walked_initializers < member_initializers_.size() || walked_members < class_member_order_.size() ||
             walked_member_values < used_members_.size() || walked_promotions < promoted_initializers.size());
    if (i == work.size()) break;
    if (!seen.insert(std::to_underlying(work[i].spec)).second) continue;
    type_only_instances_.erase(work[i].spec);
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
      if (v->cell.diags.empty()) {
        work.append_range(v->cell.demands);
        required_part(NodeId::none, v->cell.value.closures, id(v->item));
        require_static_values(v->cell.value, id(v->item));
        walk_synthesized();
        depth.resize(work.size(), depth[i] + 1);
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
    require_installed(in->spec, id(in->item));
    walk_synthesized();
    depth.resize(work.size(), depth[i] + 1);
  }
  check_type_only_instances(seen);
}

void Program::mark_type_only_instances(std::size_t first_function, std::size_t first_variable, std::size_t first_member) {
  for (const Instance* in : std::span(instance_order_).subspan(first_function))
    type_only_instances_.insert(in->spec);
  for (std::size_t v = first_variable; v < var_instances_.size(); ++v)
    type_only_instances_.insert(var_instances_[v].spec);
  for (std::size_t m = first_member; m < member_initializers_.size(); ++m)
    member_initializers_[m].type_only = true;
}

// Expressions in an instantiated body have their own evaluation contexts. Check the
// definitions they require, including a called member with a written return type.
// The checked definitions have no native roots of their own ([dcl.spec.auto.general], [temp.inst]).
void Program::check_type_only_instances(const std::set<u32>& native) {
  if (!has_type_only_instances()) return;
  struct Pending {
    sema::Demand demand;
    u32 depth;
  };
  std::vector<Pending> work;
  std::set<EntityId> checked;
  std::size_t walked_functions = 0;
  std::size_t walked_variables = 0;
  std::size_t walked_members = 0;
  for (std::size_t i = 0; ; ++i) {
    for (; walked_functions < instance_order_.size(); ++walked_functions) {
      const Instance& in = *instance_order_[walked_functions];
      if (!emits_instance(in.spec)) work.push_back({{in.spec, {id(in.item), 0}}, 1});
    }
    for (; walked_variables < var_instances_.size(); ++walked_variables) {
      const VarInstance& v = var_instances_[walked_variables];
      if (!emits_instance(v.spec)) work.push_back({{v.spec, {id(v.item), 0}}, 1});
    }
    for (; walked_members < member_initializers_.size(); ++walked_members) {
      const auto& initializer = member_initializers_[walked_members];
      if (initializer.type_only) work.push_back({{initializer.member, {id(initializer.item), 0}}, 1});
    }
    if (i == work.size()) break;
    const auto [d, depth] = work[i];
    if (native.contains(std::to_underlying(d.spec)) || !checked.insert(d.spec).second) continue;
    end_at_ = engine::Loc{std::to_underlying(d.at.item), d.at.offset};
    if (too_deep(depth - 1, index(d.at.item), d.at.offset)) break;
    const std::size_t first_fn = instance_order_.size();
    const std::size_t first_var = var_instances_.size();
    const std::size_t first_member = member_initializers_.size();
    auto enqueue = [&](std::span<const sema::Demand> demands, NodeId tree, ast::SeqId<ast::ClosureDef> closures,
                       u32 item, ConstId value = ConstId::none) {
      for (const auto& next : demands) work.push_back({next, depth + 1});
      std::vector<EntityId> refs;
      part_references(tree, closures, refs);
      references(value, refs);
      for (EntityId ref : refs)
        if (member_patterns_.contains(std::to_underlying(ref))) work.push_back({{ref, {id(item), 0}}, depth + 1});
    };
    if (const auto m = member_initializer_index(d.spec)) {
      const auto& initializer = member_initializers_[*m];
      enqueue(initializer.demands, NodeId::none, initializer.closures, initializer.item, member_init(d.spec));
    } else if (const VarInstance* v = variable_instance(d.spec, end())) {
      if (v->cell.diags.empty()) enqueue(v->cell.demands, v->cell.value.tree, v->cell.value.closures, v->item);
    } else if (const Instance* in = instance(d.spec, end())) {
      if (!in->failed && in->body.diags.empty())
        enqueue(in->body.demands, in->body.value.tree, in->body.value.closures, in->item);
    }
    mark_type_only_instances(first_fn, first_var, first_member);
  }
}

interp::Callee Program::function_ir(EntityId f, u32 item, bool* diagnosed_failure) {
  if (sema::Sema::closure_member(*w_, f)) return closure_ir(f, item);
  interp::Callee c;
  c.is_constexpr = is_constexpr(f, item);
  if (auto constructor = dynamic_synthesized_.find(f); constructor != dynamic_synthesized_.end()) {
    if (!c.is_constexpr) return c;
    const std::size_t index = constructor->second;
    const auto& s = synth_[index];
    const u32 key = std::to_underlying(f);
    if (!instance_irs_.contains(key)) {
      const auto& body = synthesized_body(index);
      std::optional<irgen::FunctionIr> ir;
      if (body.tree != NodeId::none) {
        irgen::FunctionInput input{f, s.decl, body, id(s.item), 0};
        input.this_type = this_type(s.decl);
        // L1 constant evaluation runs the checked initialization of the subobjects.
        // Native IR generation separately supplies the constructor's ABI table.
        AtItem q(*this, s.item, true);
        q.own_class_complete = true;
        if (class_patterns_.contains(std::to_underlying(w_->entities.rec(f).scope))) q.at_point = end();
        sema::EvalFacts facts(*w_, q);
        if (auto r = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*r);
      }
      instance_irs_.emplace(key, std::move(ir));
    }
    const auto& ir = instance_irs_.at(key);
    if (!ir) c.unsupported = true;
    else {
      c.body = &ir->body;
      c.default_constructs = w_->entities.rec(f).scope;
    }
    return c;
  }
  const auto constructor = defaulted_constructors_.find(f);
  if (auto copy = copy_assignments_.find(f); copy != copy_assignments_.end() || constructor != defaulted_constructors_.end()) {
    if (!c.is_constexpr) return c;
    const auto& s = copy != copy_assignments_.end() ? copy->second.definition : constructor->second.definition;
    const auto* body = copy != copy_assignments_.end() ? (copy->second.body ? &*copy->second.body : nullptr)
                                                       : &defaulted_constructor_body(constructor->second).value;
    if (!body) return c;
    const u32 key = std::to_underlying(f);
    if (!instance_irs_.contains(key)) {
      std::optional<irgen::FunctionIr> ir;
      if (body->tree != NodeId::none) {
        irgen::FunctionInput input{f, s.decl, *body, id(s.item), 0};
        input.this_type = this_type(s.decl);
        const auto q = synthesis_questions(w_->entities.rec(f).scope, s.item);
        sema::EvalFacts facts(*w_, *q);
        if (auto r = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*r);
      }
      instance_irs_.emplace(key, std::move(ir));
    }
    const auto& ir = instance_irs_.at(key);
    if (!ir) c.unsupported = true;
    else c.body = &ir->body;
    return c;
  }
  // A specialization needed for constant evaluation uses the same instantiated body as
  // native lowering ([temp.inst]/8).
  const u32 key = std::to_underlying(f);
  const auto& entity = w_->entities.rec(f);
  const EntityId templ = entity.templ;
  const bool member_instance = member_patterns_.contains(key);
  const bool specialized_destructor = member_instance && entity.kind == sema::EntityKind::destructor &&
                                      explicitly_specialized(f, item);
  const bool class_member = member_instance && !specialized_destructor;
  if (class_member || (templ != EntityId::none && !explicitly_specialized(f, item))) {
    if (!class_member) {
      ensure_declared(templ, item);
      auto def = definitions_.find(std::to_underlying(templ));
      if (def == definitions_.end() || def->second >= item) return c;
    }
    const Instance* in = instance(f, item);
    if (!in || in->item >= item) return c;
    if (in->failed || !in->body.diags.empty() || in->body.value.tree == NodeId::none) {
      c.unsupported = true;
      if (diagnosed_failure)
        *diagnosed_failure = !valid_member_declarations(in->body.diags) ||
                             (in->failed && body_cells_[in->item] &&
                              !valid_member_declarations(body_cells_[in->item]->diags));
      return c;
    }
    if (!instance_irs_.contains(key)) {
      const items::Item& it = items_->items[in->item];
      irgen::FunctionInput input{f, in->decl, in->body.value, id(in->item), it.body_begin - it.begin};
      input.this_type = this_type(in->decl);
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
  ensure_declared(f, item);
  auto def = definitions_.find(std::to_underlying(f));
  if (def == definitions_.end() || def->second >= item || !friend_body_ready(def->second) || !member_body_ready(def->second))
    return c;
  if (specialized_destructor && def->second < explicit_specs_.at(key)) return c;
  u32 k = def->second;
  if (!irs_.contains(k)) {
    // A body with errors has no L1: the evaluation stops at the call.
    const Result<ast::BodySema>& body = analyze_body(k);
    std::optional<irgen::FunctionIr> ir;
    const ast::ItemSema& is = item_sema(k).value;
    if (body.diags.empty() && body.value.tree != NodeId::none && w_->ins.seqs.size(is.decls) > 0) {
      ast::DeclSema d = completed_declaration(w_->ins.seqs.at(is.decls, 0));
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
  if (!ir) {
    c.unsupported = true;
    if (diagnosed_failure && body_cells_[k]) *diagnosed_failure = !valid_member_declarations(body_cells_[k]->diags);
  } else c.body = &ir->body;
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
    irgen::FunctionInput input{f, in.decl, in.body.value, id(in.item), in.body_offset(itm), false, {}, this_type(in.decl)};
    AtItem q(*this, in.item, !in.part);
    if (in.part && !defining_outer_class(in.item)) q.at_point = end();
    sema::EvalFacts facts(*w_, q);
    std::optional<irgen::FunctionIr> ir;
    if (auto x = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*x);
    auto& slot = closure_irs_[key] = std::move(ir);
    if (!slot) r.unsupported = true;
    else r.body = &slot->body;
    return r;
  }
  const auto define = [&](const ast::ClosureDef& c, u32 k, u32 begin, bool in_body) {
    if (c.flags & sema::closure_flag::polymorphic_result) {
      r.unsupported = true;
      return r;
    }
    sema::Sema::ClosureMember m =
        sema::Sema::closure_member_def(*w_, c, f, c.call == f ? c.body : c.invoker_body, w_->ins.seqs.elements(c.locals));
    irgen::FunctionInput input{f, m.decl, m.body, id(k), begin + c.body_begin, false, {}, m.this_type};
    AtItem q(*this, k, in_body);
    // What it calls may be declared after it, as a template's instantiation may call it
    // ([temp.point]); the evaluation asks whether it is defined.
    if (!in_body && !defining_outer_class(k)) q.at_point = end();
    sema::EvalFacts facts(*w_, q);
    std::optional<irgen::FunctionIr> ir;
    if (auto x = irgen::function_ir(w_->ins, input, facts)) ir = std::move(*x);
    auto& slot = closure_irs_[key] = std::move(ir);
    if (!slot) r.unsupported = true;
    else r.body = &slot->body;
    return r;
  };
  // A lambda of a declaration part: the part's analysis kept it.
  if (auto it = declaration_closures_.find(key); it != declaration_closures_.end()) {
    const DeclarationClosure& c = it->second;
    if (c.item >= item) return r;
    return define(c.def, c.item, c.begin, false);
  }
  // The function whose body has the lambda: past the operators of enclosing lambdas.
  EntityId fn = w_->entities.rec(w_->entities.rec(f).scope).scope;
  while (sema::Sema::closure_member(*w_, fn)) fn = w_->entities.rec(w_->entities.rec(fn).scope).scope;
  if (fn == EntityId::none || w_->entities.rec(fn).kind != sema::EntityKind::function) {
    r.unsupported = true;
    return r;
  }
  const ast::BodySema* body = nullptr;
  u32 k = 0;
  if (const sema::EntityRec& fr = w_->entities.rec(fn); (fr.templ != EntityId::none && !explicitly_specialized(fn, item)) ||
                                                         member_patterns_.contains(std::to_underlying(fn))) {
    const Instance* in = instance(fn, item);
    if (!in || in->failed) return r;
    body = &in->body.value;
    k = in->item;
  } else {
    ensure_declared(fn, item);
    auto def = definitions_.find(std::to_underlying(fn));
    if (def == definitions_.end() || def->second >= item) return r;
    k = def->second;
    body = &analyze_body(k).value;
  }
  for (const ast::ClosureDef& c : w_->ins.seqs.elements(body->closures)) {
    if (c.call != f && c.invoker != f) continue;
    const items::Item& it = items_->items[k];
    return define(c, k, it.body_begin - it.begin, true);
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
  if (known && known->part && known->erroneous) return {std::nullopt, true};  // its definition's errors say it
  if (known && known->part) {  // in a declaration part, analysed or being analysed here
    g = *known;
    k = index(known->part->item);
  } else if (known) {
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
      ensure_declared(fn, point);
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
  const items::Item& source = items_->items[*k];
  // Reported at the lambda's body, which nests ([temp.inst]/16).
  const u32 part_begin = g->part ? g->part->begin : source.body_begin - source.begin;
  if (too_deep(instantiating_.size(), *k, part_begin + g->body_begin))
    return {std::nullopt, true};
  instantiating_.insert(key);
  Instance in{spec, *k, {}, {}, false, enclosing};
  if (g->part) in.part = part_begin;
  in.body = BodyAnalysis(*this, *k, enclosing, GenericInst{*g, spec, sr.type, sr.args, point}).run();
  if (g->part) declaration_closures(in.body.value.closures, *k, part_begin);
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
    ensure_declared(templ, item);
    auto def = definitions_.find(std::to_underlying(templ));
    if (def == definitions_.end() || def->second >= item) return std::nullopt;
    const Instance* in = instance(f, item);
    if (!in || in->failed || !in->body.value.deduced) return std::nullopt;
    return in->body.value.deduced_type;
  }
  // A member function of a class template specialization, instantiated likewise.
  if (member_patterns_.contains(std::to_underlying(f))) {
    const std::size_t first_fn = instance_order_.size();
    const std::size_t first_var = var_instances_.size();
    const std::size_t first_member = member_initializers_.size();
    const Instance* in = instance(f, item);
    if (!in || in->failed || !in->body.value.deduced) return std::nullopt;
    // Instantiation for deduction alone does not supply a native use. An evaluated
    // reference to this static member will take the ordinary END worklist path.
    constexpr u16 static_member = sema::decl_flag::member | sema::decl_flag::static_;
    if ((in->decl.flags & static_member) == static_member &&
        !(in->decl.flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)))
      mark_type_only_instances(first_fn, first_var, first_member);
    return in->body.value.deduced_type;
  }
  ensure_declared(f, item);
  auto def = definitions_.find(std::to_underlying(f));
  if (def == definitions_.end() || def->second >= item || !friend_body_ready(def->second) || !member_body_ready(def->second))
    return std::nullopt;
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
