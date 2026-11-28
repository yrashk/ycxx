// Module implementation unit for ycxx.parse.
// Kept out of parse.cppm so routine edits do not regenerate the ycxx.parse BMI.
module ycxx.parse;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pp;
import ycxx.items;
import ycxx.ir;
import ycxx.irgen;
import ycxx.interp;
import ycxx.sema;
import ycxx.lex;

namespace ycxx::parse {

auto Program::is_complete(EntityId c, u32 item, bool body, bool own) -> bool {
  // A local class is complete past its definition, which its block's analysis completed at its
  // `}` ([class.local]); within it, as any class is.
  if (const auto* entity = w_->entities.find(c); entity && entity->local_class) {
    const auto it = class_items_.find(std::to_underlying(c));
    if (it == class_items_.end() || item < it->second || item > last_member(it->second)) return classes_.is_defined(c);
  }
  // So is a class nested in one, outside the local class ([class.local]/3: also a local class).
  if (const auto it = class_items_.find(std::to_underlying(c)); it != class_items_.end())
    if (const u32 root = local_class_item(it->second); root != items::none && root != it->second &&
                                                       (item < root || item > last_member(root))) {
      if (!classes_.is_defined(c) && std::ranges::find(running_, class_def_key(it->second)) == running_.end())
        class_def(it->second);
      return classes_.is_defined(c);
    }
  // Elsewhere, a local class's item is where the block that defines it is: in a function body, a
  // complete-class context of the classes around it ([class.mem.general]/7).
  {
    const auto def = class_items_.find(std::to_underlying(c));
    for (u32 l = local_class_item(item); l != items::none; l = local_class_item(item)) {
      if (def != class_items_.end() && def->second >= l && def->second <= last_member(l)) break;  // within it
      item = items_->items[l].local_owner;
      body = body || items_->items[item].kind == ItemKind::function;
      own = false;
    }
  }
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
  if (templ != EntityId::none) specializations_before(c, item);
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
  // Member lookup and classification of defaulted special members can ask during class definition.
  if (std::ranges::find(running_, class_def_key(d)) != running_.end()) return false;
  class_def(d);
  return classes_.is_defined(c);
}

auto Program::qualified_decls(std::string_view name, const std::string& ns, u32 item, bool self, LookupNames filter, bool direct_only) -> std::vector<DeclId> {
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

auto Program::declared_in(std::string_view name, const std::string& path, u32 item, bool self, bool inhabit, bool bound_functions, bool hide_function_tags) -> std::vector<DeclId> {
  std::vector<DeclId> r;
  std::vector<std::string> binding_scopes;
  auto add = [&](DeclId declaration, const std::string& ns) {
    r.push_back(declaration);
    if (hide_function_tags) binding_scopes.push_back(ns);
  };
  ask(inhabit ? Probe::declared_in_inhabit : Probe::declared_in, name, path);
  // The index's entries that pass the probe, without visiting the name's other declarations: a
  // namespace such as `std` is reopened in each header (#1653).
  const Index::Scoped& scoped = inhabit ? ix_->by_scope : ix_->by_visible_scope;
  const auto entries = scoped.find({name, path});
  const auto passing = entries == scoped.end() ? std::span<const std::pair<u32, u32>>{} : std::span(entries->second);
  for (auto [j, k] : passing) {
    if (j >= item + (self ? 1 : 0)) break;
    const items::Item& it = items_->items[j];
    const items::DeclName& n = it.names[k];
    const std::string& ns = name_ns(j, k);
    if (in_class_[j] && !n.member_elaborated_candidate) continue;
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

auto Program::declared(const ast::DeclSema& d, u32 item, bool replayed_class) -> void {
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

auto Program::choose_partial(EntityId spec, u32 point) -> std::optional<Chosen> {
  specializations_before(spec, point);
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

auto Program::satisfied(ConstId c, u32 point) -> std::optional<bool> {
  if (c == ConstId::none) return true;
  if (w_->ins.values.is<ast::VConceptParameter>(c)) {
    auto t = templates();
    if (t.constraint_dependent(c)) return std::nullopt;
    auto resolved = t.substitute_constraint(c, {});
    return resolved ? satisfied(*resolved, point) : std::optional(false);
  }
  if (w_->ins.values.is<ast::VFoldConstraint>(c)) {
    const auto fold = w_->ins.values.get<ast::VFoldConstraint>(c);
    if (w_->ins.seqs.size(fold.packs) == w_->ins.seqs.size(fold.bound))
      return fold_satisfied(c, fold, point);
    if (fold.expression == ConstId::none) return std::nullopt;
    return w_->ins.values.is<ast::VAtom>(fold.expression) ? atom_satisfied(c, point) : satisfied(fold.expression, point);
  }
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

auto Program::specialization_type(EntityId v, u32 point) -> std::optional<QualType> {
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

auto AtItem::members_before(u32 cls_item, u32 limit, EntityId owner, ast::NameId name, bool type_only, std::vector<ast::DeclSema>* functions) -> std::optional<ast::DeclSema> {
  std::optional<ast::DeclSema> r;
  std::vector<ast::DeclSema> introduced;  // by earlier using-declarations ([namespace.udecl])
  for (u32 m : p_->items().items[cls_item].members) {
    if (m == limit) break;
    if (!type_only && p_->items().items[m].kind != ItemKind::access_spec &&
        p_->region().tokens[p_->items().items[m].begin].kind == lex::Tk::kw_using)
      if (const auto u = p_->member_usings_.find({m, std::to_underlying(owner)}); u != p_->member_usings_.end())
        for (const sema::MemberDeclRec& f : u->second)
          if (f.name == name) introduced.push_back(introduced_declaration(f));
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
  // An earlier member function with the same parameter-type-list and qualifiers hides one
  // (/11); member function templates are compared by their types alone.
  const auto own_corresponding = [&](const ast::DeclSema& f) {
    return functions && std::ranges::any_of(*functions, [&](const ast::DeclSema& d) {
      return ((d.flags & sema::decl_flag::template_) != 0) == ((f.flags & sema::decl_flag::template_) != 0) &&
             (!(d.flags & sema::decl_flag::template_) || p_->equivalent_heads(d.entity, f.entity)) &&
             d.constraint == NodeId::none && d.constraints == ast::ConstId::none &&
             f.constraint == NodeId::none && f.constraints == ast::ConstId::none &&
             p_->corresponding_signatures(d.type, f.type);
    });
  };
  for (const ast::DeclSema& f : introduced) {
    if (own_corresponding(f)) continue;
    if (!r || sema::Sema::is_type(r->kind)) r = f;
    if (functions) functions->push_back(f);
  }
  return r;
}

auto AtItem::introduced_declaration(const sema::MemberDeclRec& f) -> ast::DeclSema {
  if (f.is_template)
    if (auto t = template_decl(f.entity)) return *t;
  auto& ins = p_->world().ins;
  std::vector<ast::ParamSema> params;
  const sema::TypeRec type = p_->world().types.rec(f.type);
  for (std::size_t i = 0; i < type.params.size(); ++i)
    params.push_back({EntityId::none, type.params[i], ast::NameId{}, i < f.defaults.size() ? f.defaults[i] : NodeId::none, 0});
  ast::DeclSema d{};
  d.entity = f.entity;
  d.type = f.type;
  d.kind = ast::DeclKind::function;
  d.linkage = ast::Linkage::external;
  d.storage = ast::Storage::none;
  d.access = ast::Access::none;
  d.flags = sema::decl_flag::member | (f.is_static ? sema::decl_flag::static_ : 0) |
            (f.is_deleted ? sema::decl_flag::deleted : 0) | (f.is_template ? sema::decl_flag::template_ : 0);
  d.attributes = ins.intern_seq(std::span<const NodeId>{});
  d.constraint = NodeId::none;
  d.constraints = ast::ConstId::none;
  d.contracts = NodeId::none;
  d.params = ins.intern_seq(std::span<const ast::ParamSema>(params));
  d.template_params = ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  d.is_virtual = f.is_virtual;
  d.is_pure = f.is_pure;
  return d;
}

auto ItemAnalysis::analyse() -> void {
  // A local class its block never defined (an error came first, and said so) declares nothing.
  if (const u32 l = p_->local_class_item(k_); l != items::none && !p_->local_class(l)) return;
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
    if (member && local_member_restricted()) break;
    if (member) member_declaration();
    else declaration();
    if (member && p_->local_class_item(k_) != items::none)
      for (const ast::DeclSema& d : decls_)
        if (d.kind == ast::DeclKind::variable && d.storage == ast::Storage::static_) {
          s_.diag(0, Diag::local_class_static_data_member);  // [class.static.data]
          bad();
          break;
        }
    break;
  case ItemKind::function:
    if (member && local_member_restricted()) break;
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

auto ItemAnalysis::declaration() -> void {
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
      const std::size_t closures = s_.closures.size();
      const u32 closure_aliases = s_.closure_alias_uses;
      ps_.namespace_alias_type = true;
      auto t = [&] {
        const Parser::GccSignatureScope spelling{ps_};
        return ps_.type_id();
      }();
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
      // [temp.alias]/5: a lambda's closure type is each specialization's own, read again, also
      // where it is another alias template's.
      pattern.closures = s_.closures.size() != closures || s_.closure_alias_uses != closure_aliases;
      if (pattern.closures && pattern.kind == Kind::none) pattern.kind = Kind::unrepresented;
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
    if (!it_->deduction_guide.empty()) return deduction_guide();
    if (ps_.is(Tk::kw_template) || Parser::is_class_key(key) ||
        ps_.is(Tk::kw_using) || ps_.is(Tk::kw_concept) || (it_->kind != ItemKind::function && !ps_.starts_declaration())) {
      s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());
      return bad();
    }
    template_ = true;
  }
  if (!it_->deduction_guide.empty()) return deduction_guide();
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
  ps_.deduced_class_ok = !template_ && !explicit_;
  DeclSpec spec = template_ ? ps_.function_template_decl_specifiers(return_alias_checks) : ps_.decl_specifiers();
  ps_.deduced_class_ok = false;
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
    parse::Declarator d = template_ ? ps_.function_template_declarator(spec.type, spec.is_typedef)
                                    : ps_.declarator(spec.type, false, spec.is_typedef);
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

auto ItemAnalysis::member_using_declarator(EntityId ce) -> bool {
  sema::Types& types = s_.types();
  if (ps_.is(Tk::kw_typename)) {  // a member type (#1425)
    s_.diag(ps_.pos(), Diag::not_supported, Tk::kw_typename);
    return false;
  }
  const sema::Sema::ClassPrefixScope prefix(s_, ce);
  u32 next = 0;
  auto sc = ps_.nested_name(0, next);
  if (!sc || !(ps_.lookup_id_ahead(next) || ps_.kind(next) == Tk::kw_operator)) {
    s_.diag(ps_.pos(), Diag::not_a_namespace, ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : s_.no_name());
    return false;
  }
  for (u32 j = 0; j < next; ++j) ps_.bump();
  const u32 at = ps_.pos();
  std::optional<NameId> name;
  std::optional<QualType> conversion;
  if (ps_.conversion_id_ahead()) {
    ps_.bump();
    conversion = ps_.conversion_type_id();
    if (!conversion) return false;
    name = s_.ins().names.intern(lex::conversion_function_name);
  } else if (ps_.is(Tk::kw_operator) && !ps_.literal_operator_ahead()) {
    name = ps_.operator_function_name();
  } else {
    name = ps_.lookup_id();
  }
  if (!name) return false;
  if (sc->ns) {
    s_.diag(at, Diag::using_namespace_member, *name);
    return false;
  }
  if (sc->type && (types.is_dependent(*sc->type) || (conversion && types.is_dependent(*conversion)))) {
    p_->dependent_member_usings_[std::to_underlying(ce)].insert(*name);
    return true;
  }
  if (!sc->type || !types.is_class(*sc->type)) {
    s_.diag(at, Diag::using_not_base, sc->type ? *sc->type : s_.builtin(sema::Builtin::void_), types.class_type(ce));
    return false;
  }
  const EntityId b = types.class_of(*sc->type);
  // A direct base, or a base of one: the class is not complete yet, so its base-clause's.
  std::vector<EntityId> direct;
  for (const sema::BaseRec& x : q_.defining_base_records(ce)) direct.push_back(x.cls);
  if (const auto own = p_->instantiating_bases_.find(ce); own != p_->instantiating_bases_.end() &&
                                                           !std::ranges::contains(direct, own->second))
    direct.push_back(own->second);
  const bool is_base = std::ranges::any_of(direct, [&](EntityId d) {
    int paths = 0;
    return d == b || s_.base_path(d, b, paths).has_value();
  });
  if (b == ce || !is_base) {
    s_.diag(at, Diag::using_not_base, *sc->type, types.class_type(ce));
    return false;
  }
  // An inheriting constructor (#308) names the base's constructors.
  if (*name == s_.world().entities.rec(b).name) {
    s_.diag(at, Diag::not_supported, Tk::kw_using);
    return false;
  }
  if (!s_.require_complete(*sc->type, at)) return false;
  const auto found = s_.find_member(b, *name);
  if (!found) {
    s_.diag(at, Diag::no_member, *name, *sc->type);
    return false;
  }
  if (found->ambiguous) {
    s_.diag(at, Diag::ambiguous_member, *name, *sc->type);
    return false;
  }
  // A data member, member type or enumerator (#1425).
  if (!found->member || !found->member->is_function) {
    s_.diag(at, Diag::not_supported, Tk::kw_using);
    return false;
  }
  std::vector<const sema::MemberDeclRec*> functions = s_.member_functions(found->owner, *name);
  if (conversion)
    std::erase_if(functions, [&](const sema::MemberDeclRec* f) {
      return types.canonical(types.rec(f->type).inner) != types.canonical(*conversion);
    });
  if (functions.empty()) {
    s_.diag(at, Diag::no_member, *name, *sc->type);
    return false;
  }
  // [namespace.udecl]/14: each declaration it names is accessible here.
  for (const sema::MemberDeclRec* f : functions)
    if (!s_.check_access(found->owner, f->entity, *name, at, b)) return false;
  // [namespace.udecl]/10, [class.mem.general]/6: a member using-declarator is not repeated.
  std::vector<sema::MemberDeclRec>& introduced = p_->member_usings_[{k_, std::to_underlying(ce)}];
  for (u32 m : p_->items().items[it_->parent].members) {
    if (m == k_) break;
    if (p_->items().items[m].kind == ItemKind::access_spec ||
        p_->region().tokens[p_->items().items[m].begin].kind != Tk::kw_using)
      continue;
    // A specialization's members are analysed in order; a class's earlier ones on demand.
    if (!p_->class_patterns_.contains(std::to_underlying(ce)) && !p_->running(m)) p_->item_sema(m);
    const auto earlier = p_->member_usings_.find({m, std::to_underlying(ce)});
    if (earlier == p_->member_usings_.end()) continue;
    for (const sema::MemberDeclRec* f : functions)
      if (std::ranges::contains(earlier->second, f->entity, &sema::MemberDeclRec::entity)) {
        s_.diag(at, Diag::redeclared_member, *name);
        return false;
      }
  }
  for (const sema::MemberDeclRec* f : functions)
    if (!std::ranges::contains(introduced, f->entity, &sema::MemberDeclRec::entity)) introduced.push_back(*f);
  return true;
}

auto ItemAnalysis::using_declarator() -> bool {
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

auto ItemAnalysis::merge_defaults(std::vector<ast::ParamSema>& params, std::span<const ast::DeclSema> prior, EntityId f, const parse::Declarator& d) -> bool {
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

auto ItemAnalysis::declare_alias(NameId name, u32 at, QualType type) -> bool {
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

auto ItemAnalysis::partial_arguments(const ast::DeclSema& tmpl, std::span<const ast::TemplateArg> args, u32 at) -> std::optional<std::vector<ast::TemplateArg>> {
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
    // GCC accepts a direct permutation of type parameters even though it does not specialize
    // the primary. An unchanged argument list was already checked above.
    const bool gcc_permutation = s_.compatibility() == ast::Compatibility::gcc &&
        full->size() == head.size() && head.size() == params.size() &&
        std::ranges::all_of(params, [](const auto& p) { return p.kind == ast::TemplateArgKind::type && !p.pack; }) &&
        std::ranges::all_of(*full, [&](const auto& a) {
          if (a.kind != ast::TemplateArgKind::type) return false;
          const auto r = s_.types().rec(a.type);
          return r.kind == sema::TypeKind::template_param && r.index < head.size() &&
                 head[r.index].kind == a.kind && head[r.index].depth == r.depth && !head[r.index].pack &&
                 sema::Types::cv(s_.types().canonical(a.type)) == sema::Cv::none;
        });
    if (!gcc_permutation) {
      s_.diag(at, Diag::partial_not_more_specialized, name);
      return std::nullopt;
    }
  }
  return full;
}

auto ItemAnalysis::partial_class_definition(Tk key, NameId name, u32 name_at) -> void {
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

auto ItemAnalysis::explicit_class_definition(Tk key, NameId name, u32 name_at) -> void {
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

auto ItemAnalysis::alias_template(NameId name, u32 at, QualType type) -> bool {
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

auto ItemAnalysis::deduction_guide() -> void {
  if (explicit_ || two_heads_) {
    s_.diag(0, Diag::not_supported, Tk::kw_template);
    return bad();
  }
  sema::Types& types = s_.types();
  bool is_explicit = false;
  ConstId explicit_condition = ConstId::none;
  if (ps_.is(Tk::kw_explicit)) {
    ps_.bump();
    // A value-dependent condition waits for deduction ([over.match.funcs.general]/8).
    auto spec = ps_.explicit_specifier(/*want_atom=*/template_);
    if (!spec) return bad();
    is_explicit = spec->value.value_or(false);
    explicit_condition = spec->atom;
  }
  if (!ps_.is(Tk::identifier)) {
    s_.diag(ps_.pos(), Diag::expected_unqualified_id);
    return bad();
  }
  const NameId name = ps_.name_of(ps_.tok());
  const u32 name_at = ps_.pos();
  const EntityId templ = s_.deducible_template(name);
  if (templ == EntityId::none) {
    s_.diag(name_at, Diag::deduction_guide_template, name);
    return bad();
  }
  // [temp.deduct.guide]/3: in the class template's own target scope.
  if (s_.world().entities.rec(templ).scope != ns_entity()) {
    s_.diag(name_at, Diag::deduction_guide_scope, name);
    return bad();
  }
  ps_.bump();
  bool ok = true;
  auto fs = ps_.function_suffix(ok);
  if (!ok) return bad();
  if (fs.cv != sema::Cv::none) {
    s_.diag(name_at, Diag::cv_on_non_member);
    return bad();
  }
  if (!ps_.expect(Tk::arrow)) return bad();
  const u32 result_at = ps_.pos();
  auto result = ps_.type_id();
  if (!result) return bad();
  // [temp.deduct.guide]/3: the simple-template-id names the class template.
  if (!types.is_class(*result) || s_.world().entities.rec(types.class_of(*result)).templ != templ) {
    s_.diag(result_at, Diag::deduction_guide_result, name);
    return bad();
  }
  if (template_) trailing_requires_clause(fs.params);
  if (!ps_.expect(Tk::semi)) return bad();
  std::vector<QualType> ps;
  std::vector<std::uint8_t> defaulted;
  for (const parse::Param& p : fs.params) {
    ps.push_back(p.type);
    defaulted.push_back(p.default_arg != NodeId::none);
  }
  ast::Interners& ins = s_.ins();
  ast::DeclSema sem{};
  sem.entity = EntityId::none;
  sem.kind = ast::DeclKind::function;
  sem.type = types.function(*result, ps, fs.variadic, LangLinkage::cxx);
  sem.linkage = ast::Linkage::none;
  sem.storage = ast::Storage::none;
  sem.access = ast::Access::none;
  sem.flags = template_ ? sema::decl_flag::template_ : u16(0);
  sem.attributes = ins.intern_seq(std::span<const NodeId>{});
  sem.constraint = NodeId::none;
  sem.constraints = template_ ? associated_constraints() : ConstId::none;
  sem.contracts = NodeId::none;
  sem.params = ins.intern_seq(std::span<const ast::ParamSema>{});
  sem.template_params = template_ ? ins.intern_seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()))
                                  : ins.intern_seq(std::span<const ast::TemplateParamSema>{});
  sem.is_explicit = is_explicit;
  sem.explicit_condition = explicit_condition;
  p_->deduction_guides_.insert_or_assign(k_, Program::DeductionGuide{templ, {sem, std::move(defaulted)}});
}

auto ItemAnalysis::member_requires_clause(const std::vector<parse::Param>& params, std::optional<QualType> object, bool defer_layout) -> bool {
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
  const auto saved_this = std::exchange(s_.this_type, object);
  const bool saved_layout = std::exchange(ps_.defer_member_constraint_layout, defer_layout);
  trailing_requires_clause(params);
  ps_.defer_member_constraint_layout = saved_layout;
  s_.this_type = saved_this;
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

auto ItemAnalysis::concept_definition() -> void {
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

auto ItemAnalysis::class_template_decl(NameId name, bool definition) -> ast::DeclSema {
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

auto ItemAnalysis::read_alignment(std::vector<ast::AnnotationSema>& annotations, bool field) -> std::optional<Program::ClassAlignment> {
  sema::Sema::ImmediateReferenceScope references(s_, /*allowed=*/true);
  sema::Sema::ClassAlignmentOperand operand_context(s_);
  const bool last_nonzero = !field && s_.compatibility() == ast::Compatibility::gcc;
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
    if (last_nonzero && bits.lo == 0) alignment.zeros.push_back(at);
    if (last_nonzero ? bits.lo != 0 : bits.lo > alignment.value) {
      alignment.value = u8(bits.lo);
      alignment.at = at;
    }
  }
  return alignment;
}

auto ItemAnalysis::aligned_class_declaration() -> void {
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
  if (!check_class_alignment_redeclaration(std::to_underlying(sem.entity), *alignment)) return bad();
  if (!class_declared(sem, key, at)) return bad();
  p_->class_alignments_.emplace(k_, std::move(*alignment));
}

auto ItemAnalysis::check_class_alignment_redeclaration(u32 cls, const Program::ClassAlignment& alignment) -> bool {
  const auto mode = s_.compatibility();
  const auto definition = p_->class_items_.find(cls);
  const bool defined = definition != p_->class_items_.end() && definition->second < k_;
  if (mode == ast::Compatibility::gcc) {
    if (defined) {
      const u32 first = ps_.index() - ps_.pos();
      for (const auto& specifier : alignment.specifiers) s_.diag(specifier.first - first, Diag::class_alignment_ignored);
    } else {
      for (u32 at : alignment.zeros) s_.diag(at, Diag::class_alignment_zero);
    }
    return true;
  }
  const bool clang = mode == ast::Compatibility::clang;
  if (defined) {
    const auto written = p_->class_alignments_.find(definition->second);
    if (written == p_->class_alignments_.end()) {
      s_.diag(alignment.at, Diag::class_alignment_missing);
      return false;
    }
    if (clang && !alignment.value) return true;
    p_->class_def(definition->second);
    const auto& prior = written->second;
    if (!prior.natural && (prior.value || alignment.value)) {
      s_.diag(alignment.at, Diag::class_alignment_uncertain_layout);
      return false;
    }
    if (!clang && alignment.value && alignment.value < prior.natural) {
      s_.diag(alignment.at, Diag::class_alignment_weak);
      return false;
    }
    if (prior.natural && alignment.effective(prior.natural) != prior.effective(prior.natural)) {
      s_.diag(alignment.at, Diag::class_alignment_mismatch);
      return false;
    }
  } else if (clang) {
    const auto declarations = p_->decls_.find(cls);
    if (declarations != p_->decls_.end()) {
      for (const auto& [item, previous] : declarations->second) {
        if (item >= k_) continue;
        const auto written = p_->class_alignments_.find(item);
        if (written != p_->class_alignments_.end() && written->second.value != alignment.value) {
          s_.diag(alignment.at, Diag::class_alignment_mismatch);
          return false;
        }
      }
    }
  }
  return true;
}

auto ItemAnalysis::class_definition() -> void {
  if (!supported_definition_attributes(after_definition_attributes())) return bad();
  const bool local = it_->local_owner != items::none;  // a local class ([class.local])
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
  const bool unnamed_local = local && !ps_.is(Tk::identifier);
  if (!ps_.is(Tk::identifier) && !has_named && !unnamed_local) {
    s_.diag(ps_.pos(), Diag::not_supported, ps_.kind());  // unnamed classes, attributes
    return bad();
  }
  const u32 head_index = ps_.index();
  NameId name = ps_.is(Tk::identifier) ? ps_.name_of(ps_.tok()) : unnamed_local ? s_.no_name() : named.first;
  u32 name_at = ps_.is(Tk::identifier) || unnamed_local ? ps_.pos() : named.second;
  if (ps_.is(Tk::identifier)) ps_.bump();
  // `class O::I { … }`: the definition of a class declared in class O ([class.pre]/3).
  EntityId qualified_owner = EntityId::none;
  if (ps_.is(Tk::coloncolon) && !nested && !local && !template_ && !explicit_ && !td && !alignment) {
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
  if (local) {
    // Its entity is the one its block's analysis gave it; its declarators are that block's.
    const auto* lc = p_->local_class(k_);
    if (!lc) return bad();
    ast::DeclSema sem = class_decl(lc->entity, true);
    sem.linkage = s_.world().entities.rec(lc->entity).linkage;
    sem.is_final = is_final;
    sem.annotations = b_.seq(std::span<const ast::AnnotationSema>(annotations));
    if (!class_declared(sem, key, name_at)) return bad();
    s_.own.push_back({name, sem});
    if (!bases.empty()) p_->set_bases(lc->entity, std::move(bases));
    if (base_pattern) p_->class_base_patterns_.emplace(k_, *base_pattern);
    return;
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
  if (alignment) {
    for (u32 at : alignment->zeros) s_.diag(at, Diag::class_alignment_zero);
    p_->class_alignments_.emplace(k_, std::move(*alignment));
  }
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

auto ItemAnalysis::member_class_qualifier(NameId& name, u32& name_at) -> EntityId {
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

auto ItemAnalysis::member_class_definition(Tk key, NameId name, u32 name_at, EntityId entity, EntityId owner, bool is_final, std::vector<sema::BaseRec>& bases, std::vector<ast::AnnotationSema>& annotations, std::vector<ast::AnnotationSema>& leading_annotations, u32 at, bool td) -> void {
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

auto ItemAnalysis::unnamed_union_member() -> void {
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

auto ItemAnalysis::base_clause(Tk key, NameId name, std::vector<sema::BaseRec>& bases, std::optional<Program::ClassBasePattern>& pattern, EntityId self, bool replay) -> bool {
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

auto ItemAnalysis::enum_redeclaration_ok(NameId name, u32 at, bool scoped, std::optional<QualType> fixed, bool definition) -> bool {
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

auto ItemAnalysis::enum_declaration(EntityId cls) -> void {
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
  if (scoped && inst_ && replay == EntityId::none && s_.compatibility() != ast::Compatibility::gcc) {
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
  using sema::Builtin;
  const bool clang = s_.compatibility() == ast::Compatibility::clang;
  constexpr Builtin promotions[] = {Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long,
                                    Builtin::long_long, Builtin::unsigned_long_long,
                                    Builtin::int128, Builtin::unsigned_int128};
  std::optional<EnumValue> next = EnumValue{};
  bool increment_overflow = false;
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
        } else {
          const auto sh = sema::shape(s_.world().target, *types.builtin_of(v.type()));
          const auto bits = ast::int_bits(s_.ins(), *c);
          value = EnumValue{bits, sh.is_signed && ast::is_negative(bits)};
          if (fixed) {
            Ex lit = s_.literal(v.type(), *c, ast::ValueCategory::prvalue, x.begin, x.end);
            if (s_.narrows(lit, *fixed)) s_.diag(x.begin, Diag::narrowing, v.type(), *fixed);
          } else {
            prev_type = initializer_type(x.type());
          }
        }
      }
    } else if (increment_overflow) {
      no_enum_type(en_at);  // [dcl.enum]/5: the increment exceeds even the widest supported integer.
    } else if (!value) {
      if (!fixed) prev_type = s_.builtin(sema::Builtin::dependent);
    } else if (!values.empty()) {
      // An incremented value outside the previous type widens to an integral type that holds it.
      if (!fixed && !holds(prev_type, *value)) {
        const auto candidates = std::span(promotions).first(clang ? 6 : std::size(promotions));
        const auto next_type = std::ranges::find_if(candidates, [&](Builtin b) { return holds(s_.builtin(b), *value); });
        if (next_type != candidates.end()) {
          prev_type = s_.builtin(*next_type);
        } else if (clang && value->bits == ast::Bits128{0, 1}) {
          const auto written = s_.world().values.integer(s_.builtin(Builtin::unsigned_int128), value->bits);
          s_.diag(en_at, Diag::enum_increment_truncation, written);
          value = EnumValue{};
        } else {
          no_enum_type(en_at);
        }
      }
      if (fixed && !holds(*fixed, *value)) {
        const auto written = s_.builtin(value->negative ? Builtin::int128 : Builtin::unsigned_int128);
        s_.diag(en_at, Diag::narrowing, written, *fixed);
      }
    }
    // Increment a mathematical integer represented in up to 128 bits.
    next.reset();
    increment_overflow = false;
    if (value) {
      const auto added = ast::wide::add(value->bits, {1, 0});
      increment_overflow = !value->negative && ast::wide::is_zero(added);
      if (!increment_overflow) next = EnumValue{added, value->negative && !ast::wide::is_zero(added)};
    }
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
    const ConstId c = value ? s_.world().values.integer(prev_type, value->bits) : ConstId::none;
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
    const bool negative = std::ranges::any_of(values, [](const auto& v) { return v->negative; });
    const auto all_fit = [&](Builtin type) {
      return std::ranges::all_of(values, [&](const auto& v) { return holds(s_.builtin(type), *v); });
    };
    const Builtin ordinary[] = {negative ? Builtin::int_ : Builtin::unsigned_int,
                                negative ? Builtin::long_ : Builtin::unsigned_long};
    const auto underlying = std::ranges::find_if(ordinary, all_fit);
    if (underlying != std::end(ordinary)) {
      er.underlying = s_.builtin(*underlying);
    } else if (clang) {
      s_.diag(name_at, Diag::enum_range_truncation);
      er.underlying = s_.builtin(negative ? Builtin::long_long : Builtin::unsigned_long_long);
    } else {
      const Builtin extended = negative ? Builtin::int128 : Builtin::unsigned_int128;
      if (!all_fit(extended)) no_enum_type(name_at);
      er.underlying = s_.builtin(extended);
    }
    // [conv.prom]/3: use the first standard type that holds the range, then the
    // smallest suitable extended type, preferring a signed type of the same rank.
    const auto candidates = std::span(promotions).first(clang ? 6 : std::size(promotions));
    const auto promotion = std::ranges::find_if(candidates, all_fit);
    if (promotion != candidates.end()) er.promotion = s_.builtin(*promotion);
    else er.promotion = clang ? s_.builtin(Builtin::long_long) : *er.underlying;
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
      xr.value = s_.world().values.integer(*er.underlying, values[i]->bits);
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

auto ItemAnalysis::enum_declarators(EntityId cls, QualType t, u32 at, bool td) -> void {
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

auto ItemAnalysis::member_declaration() -> void {
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
  // A using-declaration ([namespace.udecl]) naming members of a base class.
  if (ps_.is(Tk::kw_using) && ps_.kind(1) != Tk::kw_namespace && ps_.kind(1) != Tk::kw_enum) {
    ps_.bump();
    do {
      if (!member_using_declarator(ce)) {
        bad();
        ps_.skip_to_semi();
        return;
      }
    } while (ps_.accept(Tk::comma));
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
      auto t = [&] {
        const Parser::GccSignatureScope spelling{ps_};
        return ps_.type_id();
      }();
      ps_.member_alias_type = false;
      if (!t || !member_alias(c, n, at, *t, /*alias_template=*/true)) return bad();
      if (!ps_.expect(Tk::semi)) bad();
      return;
    }
  }
  // A member deduction-guide for a member class template ([temp.deduct.guide]/3) declares no
  // member. Deduction through a qualified template-name does not read it yet (#1706).
  if (!it_->deduction_guide.empty()) {
    ps_.skip_to_semi();
    return;
  }
  // `class-key identifier ;` declares a member class ([class.nest], [dcl.type.elab]/3).
  if (!member_template_ && Parser::is_class_key(ps_.kind()) &&
      ps_.kind(ps_.past_attributes(1)) == Tk::identifier && ps_.kind(ps_.past_attributes(1) + 1) == Tk::semi)
    return member_class_declaration(ce);
  // Defaults wait for the complete class ([class.mem.general]): an ordinary class's, and a class
  // template's, in its definition and in each specialization, which instantiates them when a
  // call uses them ([temp.inst]/12). A templated class's member templates' too, whose
  // exception specifications also wait; an ordinary class's member templates' bind at once,
  // as the exception specifications that may use them are (#1607). A local class's in a
  // templated function bind at once.
  if ((!member_template_ || inst_ || !p_->enclosing_heads(k_).empty()) &&
      (s_.template_scopes.empty() ? !inst_ : p_->local_class_item(k_) == items::none))
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
    parse::Parser::ExplicitSpec explicit_spec{false};
    auto flags = constructor_flags(ctor_at, dtor, dtor ? &virtual_at : nullptr, ctor ? &explicit_at : nullptr,
                                   ctor ? &explicit_spec : nullptr);
    if (!flags) return bad();
    return special_member(c, ctor, *flags, virtual_at, explicit_spec);
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
  const bool bare_explicit = ps_.is(Tk::kw_explicit) && ps_.kind(1) != Tk::l_paren;
  if (!bare_explicit && !ps_.conversion_id_ahead() &&
      !Parser::starts_decl_specifier(ps_.kind()) && !ps_.starts_declaration()) {
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
  DeclSpec spec = [&] {
    const Parser::GccSignatureScope spelling{ps_, member_template_};
    return ps_.decl_specifiers(/*in_type_id=*/false, nullptr, /*allow_conversion_id=*/true, /*class_member=*/true);
  }();
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
    if (spec.explicit_at) {
      s_.diag(*spec.explicit_at, Diag::explicit_placement);
      return bad();
    }
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

auto ItemAnalysis::member_declarators(const DeclSpec& spec, const ast::DeclSema& c, bool fields_only, const std::optional<Program::ClassAlignment>& alignment) -> void {
  sema::Types& types = s_.types();
  const std::span<const std::pair<u32, u32>> applied = alignment ? std::span(alignment->specifiers)
                                                                : std::span<const std::pair<u32, u32>>{};
  if (alignment) p_->field_alignment_specifiers_.try_emplace(k_, alignment->specifiers);
  const bool shared_overlap = layout_attribute(p_->region(), it_->begin, ps_.index(),
                                               LayoutAttributes::leading_affecting_layout, applied, "no_unique_address");
  do {
    const bool conversion = ps_.conversion_id_ahead();
    if (spec.explicit_at) {
      if (!conversion) {
        s_.diag(*spec.explicit_at, Diag::explicit_placement);
        return bad();
      }
      if (member_template_ || !s_.template_scopes.empty() || inst_) {
        s_.diag(*spec.explicit_at, Diag::not_supported, Tk::kw_explicit);
        return bad();
      }
    }
    if (conversion && spec.has_type) {
      s_.diag(spec.begin, Diag::conversion_decl_type);
      return bad();
    }
    if (!conversion && !spec.has_type) {
      s_.diag(ps_.pos(), Diag::missing_type_specifier);
      return bad();
    }
    using enum Parser::NamedExceptionSpec;
    auto exception = class_member;
    if (p_->defaulted_body(k_)) exception = defaulted_assignment;
    else if (member_template_) exception = member_template;
    auto array_bounds = Parser::MemberArrayBounds::none;
    if (!spec.is_static && !member_template_ && !conversion && s_.templated())
      array_bounds = p_->named_nested_class(it_->parent) ? Parser::MemberArrayBounds::nested_layout
                                                       : Parser::MemberArrayBounds::sizeof_type;
    parse::Declarator d = [&] {
      const Parser::GccSignatureScope spelling{ps_, member_template_};
      return ps_.declarator(spec.type, /*abstract=*/false, /*typedef_=*/false, conversion, exception,
                            /*defer_ref_check=*/true, /*reject_parameter_defaults=*/false, array_bounds);
    }();
    if (d.ok && d.function && member_template_ && s_.compatibility() == ast::Compatibility::gcc)
      d.type = ps_.retain_gcc_signature_transforms(d.type);
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
      // [dcl.fct]: a non-static member of a class at namespace scope, a class template, its
      // nested classes and member templates included, keeps its ref-qualifier through
      // instantiation; the implicit object parameter carries it ([over.match.funcs]/4-5).
      EntityId enclosing = s_.world().entities.rec(c.entity).scope;
      while (enclosing != EntityId::none && (s_.world().entities.rec(enclosing).kind == sema::EntityKind::class_ ||
                                             s_.world().entities.rec(enclosing).kind == sema::EntityKind::class_template))
        enclosing = s_.world().entities.rec(enclosing).scope;
      const bool ordinary_member = !spec.is_static && p_->is_namespace(enclosing);
      if (!is_virtual && !conversion && !ordinary_member && !ps_.check_named_ref_qualifier(d)) return bad();
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
    u8 wide_bit_width = 0;
    if (ps_.is(Tk::colon)) {
      auto width = bit_field(spec, d, c.entity);
      if (!width) return bad();
      const auto size = s_.world().target[*types.builtin_of(d.type)].size;
      bit_width = u8(std::min(int(*width), size * 8));
      if (*width != bit_width) wide_bit_width = *width;
    }
    if (types.is_function(d.type) || spec.is_static) {
      if (it_->kind == ItemKind::class_def || p_->items().items[it_->parent].unnamed_type_ordinal != items::none) {
        s_.diag(d.name_at, Diag::not_supported, Tk::kw_union);
        return bad();
      }
      if (spec.explicit_at && it_->kind == ItemKind::function &&
          !finish_member_definition_declarator()) return bad();
      if (!member_function_or_static(spec, d, c, is_virtual, final_at.has_value(), overridden)) return bad();
      if (it_->kind == ItemKind::function) return;  // its body follows the declaration part
      continue;
    }
    if (spec.is_inline || spec.is_constexpr || spec.is_consteval) {
      // Inline or constexpr non-static data members are ill-formed.
      s_.diag(spec.begin, Diag::not_supported, Tk::kw_constexpr);
      return bad();
    }
    // [temp.pre]/2: a variable template in a class is a static data member template. (An
    // operator's name here is a function declarator that did not parse as one.)
    if (member_template_ && !s_.ins().names.spelling(d.name).starts_with("operator")) {
      s_.diag(d.name_at, Diag::data_member_template, d.name);
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
    sem.wide_bit_width = wide_bit_width;
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
        written->type = d.type;
        p_->set_dmi(sem.entity, *written);
      } else {
        const std::size_t n = s_.diag_count();
        init = ps_.member_initializer(d.type, braced, form);
        // A value that observed constant evaluation is not kept: most uses run at run time.
        auto v = init.ok() && types.is_scalar(d.type) ? constant_initializer(init, false) : std::nullopt;
        s_.truncate_diags(n);
        if (v) value = *v;
        else init = {};
        p_->set_dmi(sem.entity, {.item = k_, .begin = it_->begin + from, .end = it_->begin + ps_.pos(),
                                .type = d.type, .braced = braced, .form = form});
      }
    }
    s_.own.push_back({d.name, sem});
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), sema::Sema::at(init, d.begin), value}, d.begin, ps_.pos()));
    decls_.push_back(sem);
  } while (ps_.accept(Tk::comma));
  if (!ps_.expect(Tk::semi)) bad();
}

auto ItemAnalysis::bit_field(const DeclSpec& spec, const parse::Declarator& d, EntityId owner) -> std::optional<u8> {
  const items::Item& record = p_->items().items[it_->parent];
  // The first slice covers a non-template record without bases or layout attributes.
  auto attributed = [&](u32 begin, u32 end) {
    return layout_attribute(p_->region(), begin, end, LayoutAttributes::any);
  };
  const auto type = s_.types().builtin_of(d.type);
  const auto size = type ? s_.world().target[*type].size : 0;
  if (d.name_at != d.begin || d.template_args || member_template_ || spec.is_static || !q_.defining_bases(owner).empty() ||
      p_->class_rec_is_union(it_->parent) || record.template_depth != 0 || record.specialization ||
      sema::Types::cv(s_.types().canonical(d.type)) != sema::Cv::none ||
      !type || !ast::is_integral(*type) || *type == sema::Builtin::bool_ || size == 0 || size > 8 ||
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
  if (width == 0 || width > 255) {
    s_.diag(at, Diag::bit_field_width);
    return std::nullopt;
  }
  if (ps_.is(Tk::equal) || ps_.is(Tk::l_brace)) {
    s_.diag(ps_.pos(), Diag::bit_field_initializer);
    return std::nullopt;
  }
  if (width > u64(size * 8)) {
    if (s_.compatibility() == ast::Compatibility::gcc)
      s_.diag(d.name_at, Diag::bit_field_excess_width, s_.world().entities.rec(owner).name, d.name);
    else if (s_.compatibility() == ast::Compatibility::clang)
      s_.diag(d.name_at, Diag::bit_field_truncated_width, d.name, width, size * 8);
  }
  return u8(width);
}

auto ItemAnalysis::friend_declaration() -> void {
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

auto ItemAnalysis::friend_class_template() -> void {
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

}  // namespace ycxx::parse
