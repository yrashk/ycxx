// Module implementation unit for ycxx.sema (Sema member function definitions).
// Kept out of actions.cppm so routine edits do not regenerate the ycxx.sema BMI.
module ycxx.sema;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir;
import ycxx.interp;
import ycxx.lex;

namespace ycxx::sema {

auto Sema::class_name(NameId name, bool elaborated, bool aliases, std::optional<u32> type_at, bool search_locals, std::optional<EntityId>* qualifier_namespace) -> std::optional<QualType> {
  // Innermost block first: a block-scope typedef-name, block using-declaration, or a local that hides the type.
  for (std::size_t d = search_locals ? scopes_.size() : 0; d-- > 0;) {
    if (!elaborated) {
      if (std::ranges::any_of(scopes_[d], [&](const auto& p) { return p.first == name; })) return std::nullopt;
      if (std::ranges::any_of(block_aliases_, [&](const BlockAlias& a) { return a.depth == d && a.name == name; }))
        return std::nullopt;
      for (const BlockUsing& u : block_usings_)
        if (u.depth == d && u.name == name &&
            std::ranges::any_of(u.decls, [](const ast::DeclSema& decl) { return !is_type(decl.kind); }))
          return std::nullopt;
    }
    for (auto t = block_types_.rbegin(); t != block_types_.rend(); ++t)
      if (t->depth == d && t->name == name) return aliases ? std::optional(t->type) : std::nullopt;
    for (auto u = block_usings_.rbegin(); u != block_usings_.rend(); ++u) {
      if (u->depth != d || u->name != name || !u->type_index) continue;
      const ast::DeclSema& decl = u->decls[*u->type_index];
      if (decl.flags & decl_flag::template_) return std::nullopt;
      if (decl.kind == ast::DeclKind::type_alias && !aliases) return std::nullopt;
      return decl.type;
    }
  }
  // A template parameter hides what the enclosing scopes declare ([basic.scope.temp]).
  // For a local class, its enclosing blocks are inside the template's scope and are checked first.
  if (!outer())
    if (const ast::TemplateParamSema* p = template_param(name)) {
      if (elaborated || p->kind != ast::TemplateArgKind::type) return std::nullopt;
      if (auto a = bound_arg(*p)) return a->type;
      return p->type;
    }
  // One the class's scope hides ([temp.local]/7): what it declares, a type or not.
  if (!outer() && declared_template_param(name)) {
    for (EntityId cls = current_class; cls != EntityId::none; cls = enclosing_class(cls)) {
      const ClassRec* cr = class_rec(cls);
      if ((!cr || !cr->defined) && cls != current_class)
        if (auto m = q_->enclosing_earlier_member(cls, name, elaborated))
          return is_type(m->kind) && !(m->flags & decl_flag::template_) ? std::optional(m->type) : std::nullopt;
      auto hit = class_body_type(cls, cr && cr->defined ? *cr : ClassRec{}, name, type_at,
                                 ClassBodyLookup::before_template_parameter, elaborated);
      if (hit.found) return hit.type;
    }
    return std::nullopt;
  }
  if (!elaborated && earlier_nested_enumerator(name)) return std::nullopt;
  // In a class template, its name without arguments is the injected-class-name: the current
  // instantiation, or in an instantiation the specialization ([temp.local]/1).
  if (current_class != EntityId::none) {
    const EntityRec& c = w_->entities.rec(current_class);
    if (c.templ != EntityId::none && c.name == name) return types().class_type(current_class);
    if (qualifier_namespace && type_at) {
      const auto before = diag_count();
      const Scope scope{std::nullopt, types().class_type(current_class)};
      if (auto type = enum_path_member(scope, name, *type_at, ClassBodyLookup::enum_qualifier)) return type;
      if (has_error_since(before)) return std::nullopt;
    }
    // A type nested in the class whose scope this is ([basic.scope.class]).
    if (const ClassRec* cr = class_rec(current_class); cr && cr->defined) {
      if (!elaborated && current_function != EntityId::none && !scopes_.empty() && !templated() &&
          c.kind == EntityKind::class_ && c.templ == EntityId::none && (c.name == name || !cr->bases.empty())) {
        auto hit = class_body_type(current_class, *cr, name, type_at);
        if (hit.found) return hit.type;
      }
      if (!elaborated && unscoped_member_enumerator(current_class, name, /*unqualified=*/true)) return std::nullopt;
      if (!elaborated && !types().is_dependent(types().class_type(current_class)))
        if (auto found = find_member(current_class, name, /*unqualified=*/true);
            found && !found->ambiguous &&
            (found->nested_type == EntityId::none || found->enumerator != EntityId::none))
          return std::nullopt;
      if (c.name == name && enclosing_class(current_class) != EntityId::none) return types().class_type(current_class);
      if (auto r = nested_member(current_class, name, true)) return r->first.type;
    } else {
      const auto prefix_bases = class_prefix_bases(current_class);
      if (!elaborated && !prefix_bases.empty() &&
          (name == static_initializer_name_ || own_initializer_static(name))) return std::nullopt;
      if (!elaborated && q_->dependent_member_using(current_class, name)) return std::nullopt;
      if (auto m = q_->earlier_member(name, elaborated)) {
        if (m->kind == ast::DeclKind::type_alias && (m->flags & decl_flag::template_))
          return std::nullopt;  // a member alias template, not a type
        if (m->kind == ast::DeclKind::enum_ || m->kind == ast::DeclKind::type_alias || m->kind == ast::DeclKind::class_)
          return m->type;
        if (!elaborated && (m->kind == ast::DeclKind::data_member || m->kind == ast::DeclKind::variable ||
                            m->kind == ast::DeclKind::enumerator ||
                            (m->kind == ast::DeclKind::function && c.name != name)))
          return std::nullopt;
      }
      // The injected-class-name is declared in the class's own scope ([class.pre]). A nested
      // class's is the only declaration of its name lookup reaches from here ([class.nest]).
      if ((!prefix_bases.empty() || enclosing_class(current_class) != EntityId::none) && c.name == name)
        return types().class_type(current_class);
      if (!elaborated && !prefix_bases.empty()) {
        // A base declaration hides a namespace type. Merge base types by the types they name.
        auto hit = class_body_type(current_class, ClassRec{}, name, type_at, ClassBodyLookup::class_prefix);
        if (hit.found) return hit.type;
      } else if (!elaborated && !templated() && c.kind == EntityKind::class_ && c.templ == EntityId::none) {
        auto hit = class_body_type(current_class, ClassRec{}, name, type_at);
        if (hit.found) return hit.type;
      }
      // In the class being defined, a type of its (complete) bases.
      for (EntityId b : q_->defining_bases(current_class))
        if (auto r = nested_member(b, name)) return r->first.type;
    }
    // Then the classes the class is nested in, innermost first ([class.nest], [basic.lookup.unqual]/2).
    for (EntityId k = enclosing_class(current_class); k != EntityId::none; k = enclosing_class(k)) {
      if (const ClassRec* cr = class_rec(k); cr && cr->defined) {
        // A data member, function or enumerator hides a class of the same name ([basic.scope.hiding]).
        if (auto f = direct_member(k, *cr, name);
            !elaborated && f && (f->nested_type == EntityId::none || f->enumerator != EntityId::none))
          return std::nullopt;
        auto hit = class_body_type(k, *cr, name, type_at, ClassBodyLookup::class_prefix, elaborated);
        if (hit.found) return hit.type;
      } else {
        auto hit = enclosing_prefix_type(k, name, type_at, elaborated);
        if (hit.found) return hit.type;
      }
    }
  }
  // Past a local class, the block enclosing it ([class.local]), then its template parameters.
  if (bool hidden = false; outer()) {
    if (auto t = outer_block_class_name(name, elaborated, aliases, hidden)) return t;
    if (hidden) return std::nullopt;
    if (const ast::TemplateParamSema* p = template_param(name)) {
      if (elaborated || p->kind != ast::TemplateArgKind::type) return std::nullopt;
      if (auto a = bound_arg(*p)) return a->type;
      return p->type;
    }
  }
  // BOOTSTRAP: -Wmaybe-uninitialized: a value and a flag, not std::optional. At -O3 GCC 16.2
  // reports the optional's payload as possibly uninitialized after the `!found` checks (#748).
  // Without the bug: `std::optional<ast::DeclSema> found;`, `found = s;`, `found->kind`.
  ast::DeclSema found{};
  bool has_found = false;
  const auto take = [&](const ast::DeclSema& s) { found = s, has_found = true; };
  for (auto p = own.rbegin(); p != own.rend() && !has_found; ++p)
    if (p->name == name) take(p->sema);
  if (qualifier_namespace && type_at && !has_found) {
    const auto before = diag_count();
    if (auto scope = qualifier_scope(name, *type_at, q_->qualifier_decls(name, std::nullopt, directives()))) {
      *qualifier_namespace = scope->ns;
      return scope->type;
    }
    if (!has_error_since(before) && member_introduction && member_introduction->name == name)
      return member_introduction->sema.type;
    return std::nullopt;
  }
  if (!has_found)
    for (DeclId d : q_->visible_decls(name, directives())) {
      ast::DeclSema s = q_->decl_sema(d);
      if (s.entity == EntityId::none || is_special(s)) continue;
      if (is_type(s.kind) || !elaborated) take(s);
      if (!elaborated && !is_type(s.kind)) break;
    }
  if (member_introduction && member_introduction->name == name &&
      (!has_found || elaborated || w_->entities.rec(found.entity).scope !=
                                       w_->entities.rec(member_introduction->sema.entity).scope))
    take(member_introduction->sema);
  if (!has_found && !elaborated && aliases) return builtin_type_name(name);
  if (!has_found || !is_type(found.kind)) return std::nullopt;
  if (found.kind == ast::DeclKind::type_alias && !aliases) return std::nullopt;
  // A class template's name needs its arguments outside the template ([temp.names]).
  if (found.flags & decl_flag::template_) return std::nullopt;
  return found.type;
}

auto Sema::enclosing_prefix_value(EntityId cls, NameId name, u32 begin, u32 end, u32 name_at, bool unqualified, bool callee, std::optional<u32> address_at) -> std::optional<Ex> {
  std::vector<ast::DeclSema> functions;
  if (auto m = q_->enclosing_earlier_member(cls, name, /*type_only=*/false, &functions)) {
    if (m->kind == ast::DeclKind::enumerator) return enumerator(m->entity, m->type, begin, end);
    if (m->kind == ast::DeclKind::variable) return static_member(cls, m->entity, m->type, begin, end);
    if (m->kind == ast::DeclKind::function) {
      const auto is_static = [](const ast::DeclSema& d) { return (d.flags & decl_flag::static_) != 0; };
      if (!functions.empty() && std::ranges::all_of(functions, is_static))
        return decls_expr(std::move(functions), true, name, begin, end, callee);
      if (std::ranges::any_of(functions, is_static))
        return bind_prefix_member_functions(cls, name, functions, begin, end);
    }
    if (m->kind == ast::DeclKind::data_member && unevaluated)
      return unevaluated_data_member(m->entity, m->type, m->bit_width, begin, end);
    diag(begin, is_type(m->kind) ? Diag::type_as_expression : Diag::member_without_object, name);
    return Ex{NodeId::none, begin, end};
  }
  if (w_->entities.rec(cls).name == name) {
    diag(name_at, Diag::type_as_expression, name);
    return Ex{NodeId::none, begin, end};
  }
  const auto bases = class_prefix_bases(cls);
  if (bases.empty()) return std::nullopt;
  return class_member(cls, name, begin, end, name_at, address_at, unqualified, /*check_all_bases=*/true, bases);
}

auto Sema::class_body_type(EntityId naming, const ClassRec& root, NameId name, std::optional<u32> type_at, ClassBodyLookup lookup, bool type_only) -> ClassBodyType {
  const bool qualified_class = lookup == ClassBodyLookup::qualified_class;
  const bool qualified_enum = lookup == ClassBodyLookup::qualified_enum;
  const bool enum_lookup = qualified_enum || lookup == ClassBodyLookup::enum_qualifier;
  const bool qualified = lookup == ClassBodyLookup::qualified || qualified_class || enum_lookup;
  const bool unqualified = lookup == ClassBodyLookup::unqualified;
  const bool before_parameter = lookup == ClassBodyLookup::before_template_parameter;
  const bool class_prefix = lookup == ClassBodyLookup::class_prefix;
  struct Edge {
    EntityId derived;
    ast::Access access;
  };
  struct Hit {
    std::optional<QualType> type;
    EntityId owner;
    EntityId declaration;
    ast::Access access;
    bool injected;
    std::vector<Edge> path;
  };
  std::vector<Hit> hits;
  std::vector<Edge> path;
  auto collect = [&](this auto&& visit, EntityId cls, const ClassRec& cr) -> void {
    const EntityRec& c = w_->entities.rec(cls);
    if (c.kind == EntityKind::class_ && c.name == name &&
        (c.templ == EntityId::none || class_prefix || qualified_class || (before_parameter && cls != naming))) {
      hits.push_back({types().class_type(cls), cls, cls, ast::Access::public_, true, path});
      return;
    }
    if (auto member = direct_member(cls, cr, name)) {
      // A member alias template is found and is no type without its arguments.
      if (std::ranges::contains(cr.alias_templates, name, &NestedType::name)) {
        hits.push_back({std::nullopt, cls, EntityId::none, ast::Access::public_, false, path});
        return;
      }
      auto nested = std::ranges::find(cr.types, name, &NestedType::name);
      if (nested != cr.types.end() &&
          (type_only || (member->nested_type != EntityId::none && member->enumerator == EntityId::none))) {
        if (w_->entities.rec(nested->entity).kind == EntityKind::class_template) {
          hits.push_back({std::nullopt, cls, EntityId::none, ast::Access::public_, false, path});
          return;
        }
        hits.push_back({nested->type, cls, nested->entity, cr.access_of(nested->entity), false, path});
        return;
      }
      if (!type_only) {
        hits.push_back({std::nullopt, cls, EntityId::none, ast::Access::public_, false, path});
        return;
      }
    }
    std::span<const BaseRec> bases = cr.bases;
    if (cls == naming && !cr.defined) {
      if (class_prefix) bases = class_prefix_bases(naming);
      else bases = q_->defining_base_records(cls);
    }
    for (const BaseRec& b : bases) {
      // [temp.dep.general]: the naming class's dependent bases are not searched. A base of a
      // nondependent base is nondependent, whatever it was in that base's own pattern.
      if ((before_parameter || qualified_class) && cls == naming && b.dependent_in_pattern) continue;
      if (const ClassRec* base = class_rec(b.cls)) {
        if (type_at) path.push_back({cls, b.access});
        visit(b.cls, *base);
        if (type_at) path.pop_back();
      }
    }
  };
  collect(naming, root);
  const bool injected = std::ranges::any_of(hits, &Hit::injected);
  const bool inherited_alias = (unqualified || class_prefix) && std::ranges::any_of(hits, [&](const Hit& h) {
    return h.type && h.owner != naming && w_->entities.rec(h.declaration).kind == EntityKind::type_alias;
  });
  // For an ordinary incomplete class, handle inherited aliases and keep the legacy lookup
  // for other types. The caller already considered its own preceding declarations.
  if (hits.empty() || (lookup == ClassBodyLookup::qualified && !injected) ||
      (unqualified && !root.defined && !inherited_alias)) return {};
  // [class.qual]: in ordinary lookup the naming class's own injected name denotes its constructor.
  if (qualified && !type_only && hits.front().injected && hits.front().owner == naming)
    return {true, std::nullopt};
  auto first = std::ranges::find_if(hits, [](const Hit& h) { return h.type.has_value(); });
  if (first == hits.end()) return {true, std::nullopt};
  // Merge equivalent designated types before checking the declarations and base paths that
  // can supply the selected type ([class.member.lookup], [class.access]).
  if (class_prefix || (type_at && (injected || inherited_alias || enum_lookup || qualified_class || before_parameter))) {
    QualType selected = types().canonical(*first->type);
    const bool gcc_alias = compatibility() == ast::Compatibility::gcc && inherited_alias &&
                           first->declaration != EntityId::none &&
                           w_->entities.rec(first->declaration).kind == EntityKind::type_alias;
    if (std::ranges::any_of(hits, [&](const Hit& h) {
          return !h.type || types().canonical(*h.type) != selected ||
                 (gcc_alias && h.declaration != first->declaration);
        })) {
      if (type_at) diag(*type_at, Diag::ambiguous_member, name, types().class_type(naming));
      if (enum_lookup || qualified_class || class_prefix) return {true, std::nullopt};
    } else if (type_at) {
      if (qualified_enum || qualified_class) {
        const auto is_alias = [&](const Hit& hit) {
          return hit.declaration != EntityId::none &&
                 w_->entities.rec(hit.declaration).kind == EntityKind::type_alias;
        };
        if (std::ranges::all_of(hits, is_alias)) {
          diag(*type_at, Diag::elaborated_typedef, name);
          return {true, std::nullopt};
        }
        if (std::ranges::any_of(hits, is_alias)) {
          diag(*type_at, Diag::ambiguous_member, name, types().class_type(naming));
          return {true, std::nullopt};
        }
      }
      const bool clang_elaborated = type_only && (lookup == ClassBodyLookup::qualified || qualified_class) &&
                                    compatibility() == ast::Compatibility::clang;
      auto allowed = [this, naming, before_parameter, clang_elaborated](const Hit& h) {
        return accessible(h.owner, h.access, naming) &&
               ((clang_elaborated && h.injected) || std::ranges::all_of(h.path, [&](const Edge& edge) {
                  return accessible(edge.derived, edge.access, before_parameter ? naming : EntityId::none);
                }));
      };
      if (!std::ranges::any_of(hits, allowed)) {
        QualType type = *first->type;
        if (qualified && defer_type_access) {
          deferred_injected_access_.push_back([this, at = *type_at, type, naming, name, first_owner = first->owner,
                                               first_access = first->access, qualified_class, allowed,
                                               hits = std::move(hits)] {
            if (!std::ranges::any_of(hits, allowed)) {
              if (qualified_class && !accessible(first_owner, first_access, naming))
                diag(at, first_access == ast::Access::protected_ ? Diag::protected_member : Diag::private_member,
                     name, types().class_type(first_owner));
              else
                diag(at, Diag::inaccessible_base, type, types().class_type(naming));
            }
          });
          return {true, type};
        }
        if ((before_parameter || inherited_alias || qualified_class) && !accessible(first->owner, first->access, naming))
          diag(*type_at, first->access == ast::Access::protected_ ? Diag::protected_member : Diag::private_member,
               name, types().class_type(first->owner));
        else
          diag(*type_at, Diag::inaccessible_base, inherited_alias ? types().class_type(first->owner) : type,
               types().class_type(naming));
      }
    }
  }
  return {true, first->type};
}

auto Sema::named_template(NameId name, ast::DeclKind kind, std::optional<u32> at, bool initializer) -> std::optional<ast::DeclSema> {
  // A static data member template of the class, a base or an enclosing class ([temp.mem]); any
  // other member hides the namespace's templates ([basic.scope.class]).
  if (kind == ast::DeclKind::variable)
    if (auto member = unqualified_member_variable_template(name)) return member;
  for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
    for (auto p = s->rbegin(); p != s->rend(); ++p)
      if (p->first == name) return std::nullopt;
  if (template_param(name)) return std::nullopt;
  const EntityId injected = kind == ast::DeclKind::class_ ? injected_class_template(name) : EntityId::none;
  // A block-scope using-declaration is found before the class's members ([basic.lookup.unqual]).
  if (injected == EntityId::none && names_member(name) && !std::ranges::contains(block_usings_, name, &BlockUsing::name))
    return std::nullopt;
  std::optional<ast::DeclSema> found;
  // Block-scope using-declarations, innermost first ([namespace.udecl]).
  for (auto u = block_usings_.rbegin(); u != block_usings_.rend() && !found; ++u)
    if (u->name == name)
      for (const ast::DeclSema& d : u->decls)
        if (d.kind == kind && (d.flags & decl_flag::template_)) found = d;
  for (auto p = own.rbegin(); p != own.rend() && !found; ++p)
    if (p->name == name) found = p->sema;
  std::vector<ast::DeclSema> decls;
  for (DeclId d : q_->visible_decls(name, directives())) {
    ast::DeclSema s = q_->decl_sema(d);
    if (s.entity != EntityId::none && !is_special(s)) decls.push_back(s);
  }
  if (!found && (at || initializer)) {
    auto matching = std::find_if(decls.rbegin(), decls.rend(), [&](const ast::DeclSema& d) {
      return d.kind == kind && (d.flags & decl_flag::template_);
    });
    if (!initializer && at && matching != decls.rend() && std::ranges::any_of(decls, [&](const ast::DeclSema& d) {
          return d.entity != matching->entity && d.kind != ast::DeclKind::class_ && d.kind != ast::DeclKind::enum_;
        })) {
      diag(*at, Diag::ambiguous_name, name);
      return std::nullopt;
    }
    if (matching != decls.rend()) found = *matching;
  }
  if (!found && !decls.empty()) found = decls.back();
  if (injected != EntityId::none && (!found || found->entity != injected)) {
    // The template ordinary lookup outside the class would not find (a member template).
    found = q_->template_decl(injected);
    if (!found) return std::nullopt;
  }
  if (!found || found->kind != kind || !(found->flags & decl_flag::template_)) return std::nullopt;
  // The deferred argument retains no entity. Ordinary lookup at the initializer must
  // determine one before replay. A tag from another namespace can make the name ambiguous.
  if (initializer && at && std::ranges::any_of(decls, [&](const auto& d) { return d.entity != found->entity; })) {
    diag(*at, Diag::ambiguous_name, name);
    return std::nullopt;
  }
  return with_template_defaults(*found, decls);
}

auto Sema::canonical_clang_class_argument(TemplateArg arg) -> TemplateArg {
  auto canonical_args = [&](ast::SeqId<TemplateArg> sequence) {
    auto elements = ins().seqs.elements(sequence);
    bool changed = false;
    for (auto& element : elements) {
      const auto canonical = canonical_clang_class_argument(element);
      changed |= canonical != element;
      element = canonical;
    }
    return changed ? ins().intern_seq(std::span<const TemplateArg>(elements)) : sequence;
  };
  if (arg.kind == TemplateArgKind::pack) arg.elems = canonical_args(arg.elems);
  if (arg.kind != TemplateArgKind::value) return arg;
  if (ins().values.is<ast::VDepVariable>(arg.value)) {
    auto variable = ins().values.get<ast::VDepVariable>(arg.value);
    variable.qualifier = {};
    variable.global = 0;
    variable.args = canonical_args(variable.args);
    variable.written_args = canonical_args(variable.written_args);
    arg.value = ins().intern_value(variable);
  } else if (ins().values.is<ast::VDepOp>(arg.value)) {
    auto operation = ins().values.get<ast::VDepOp>(arg.value);
    auto operands = canonical_args(operation.operands);
    if (operands != operation.operands) {
      operation.operands = operands;
      arg.value = ins().intern_value(operation);
    }
  }
  return arg;
}

auto Sema::class_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 at, bool directly_named, bool* alias_constraint_failed, bool replay_partial_alias, bool record_spelling) -> std::optional<QualType> {
  auto full = template_arguments(tmpl, args, at);
  if (!full) return std::nullopt;
  if (tmpl.kind == ast::DeclKind::type_alias) {
    std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
    auto bindings = Bindings::of(params.empty() ? 0 : params[0].depth, *full);
    if (alias_constraint_failed && namespace_scoped(tmpl.entity) &&
        !check_named_type_constraints(tmpl, args.size(), *full, bindings, at)) {
      *alias_constraint_failed = true;
      return std::nullopt;
    }
    const auto before = diag_count();
    const auto diagnosed = diagnosed_constant_calls;
    const auto use = replaying_type_default ? AliasSubstitution::type_default : AliasSubstitution::ordinary;
    auto t = substitute_alias(tmpl, *full, bindings, at, use, replay_partial_alias);
    // A producer body or the nesting limit may have been diagnosed in another cell
    // ([temp.inst]/16). Keep that diagnostic as the cause of the failed alias.
    if (!t && !has_error_since(before) && diagnosed == diagnosed_constant_calls && !q_->instantiation_gave_up())
      diag(at, Diag::template_args_mismatch, w_->entities.rec(tmpl.entity).name);
    return t;
  }
  // [temp.names]/9: naming a non-dependent specialization checks the primary's
  // constraints even when the use does not require a complete class.
  if (directly_named && tmpl.kind == ast::DeclKind::class_ && tmpl.constraints != ConstId::none &&
      w_->entities.rec(tmpl.entity).kind == EntityKind::class_template &&
      args.size() == full->size() &&
      std::ranges::all_of(*full, [&](const TemplateArg& arg) {
        return (arg.kind == ast::TemplateArgKind::type || arg.kind == ast::TemplateArgKind::value) && !dependent(arg);
      })) {
    const auto before = diag_count();
    if (!constraints_satisfied(tmpl, *full)) {
      if (!has_error_since(before)) diag(at, Diag::constraints_not_satisfied, w_->entities.rec(tmpl.entity).name);
      return std::nullopt;
    }
  }
  if (compatibility() == ast::Compatibility::clang) {
    auto canonical = *full;
    for (auto& argument : canonical) argument = canonical_clang_class_argument(argument);
    EntityId specialization = class_specialization(tmpl.entity, canonical);
    if (directly_named && record_spelling && canonical != *full)
      template_type_spellings.push_back({specialization, ins().intern_seq(std::span<const TemplateArg>(*full)), loc_base_ + at});
    return types().class_type(specialization);
  }
  return types().class_type(class_specialization(tmpl.entity, *full));
}

auto Sema::substitute_alias(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, const Bindings& bindings, u32 at, AliasSubstitution use, bool replay_partial_alias) -> std::optional<QualType> {
  const bool substituting_default = use == AliasSubstitution::type_default;
  if (substituting_default && tmpl.constraints != ConstId::none &&
      std::ranges::none_of(args, [&](const TemplateArg& a) { return dependent(a); })) {
    auto t = templates();
    bool hard_error = false;
    auto sat = named_constraint_satisfaction(t, tmpl.constraints, bindings, /*bind_lazily=*/true, hard_error);
    if (hard_error || !sat.value_or(true)) return std::nullopt;
  }
  const auto pattern = q_->alias_call_pattern(tmpl.entity);
  if (pattern.closures) ++closure_alias_uses;
  if (pattern.kind != AliasCallPattern::Kind::none || pattern.rank_checks || pattern.qualified_type_arguments) {
    // Replay checks obligations that become concrete now. The enclosing alias, member or
    // signature retains checks whose arguments still depend on its own parameters.
    const bool partial_type_arguments = pattern.qualified_type_arguments &&
        (replay_partial_alias || use == AliasSubstitution::signature) &&
        std::ranges::any_of(args, [&](const TemplateArg& a) { return !dependent(a); });
    if ((pattern.class_result && replay_partial_alias) || partial_type_arguments ||
        std::ranges::none_of(args, [&](const TemplateArg& a) { return dependent(a); })) {
      const auto before = diag_count();
      const bool saved = std::exchange(replaying_type_default, substituting_default);
      auto type = q_->replay_alias_calls(*this, tmpl, bindings);
      replaying_type_default = saved;
      if (!type && use != AliasSubstitution::ordinary) drop_failures(before);
      return type;
    }
    // Until signatures can retain the replay, do not form a type that can lose a call check.
    if (pattern.kind == AliasCallPattern::Kind::may_erase) return std::nullopt;
    // The expansion holds a placeholder for an operand it cannot represent, which every such
    // alias shares. The alias-id keeps its identity ([temp.alias]/2) and is read again once its
    // arguments are known.
    if (pattern.kind == AliasCallPattern::Kind::unrepresented) {
      auto expanded = templates_for_use(at).substitute(tmpl.type, bindings);
      if (!expanded) return std::nullopt;
      return types().deferred_alias(tmpl.entity, args, *expanded, substituting_default);
    }
  }
  if (compatibility() == ast::Compatibility::gcc) {
    // A dependent alias still carries the written transformations of its concrete
    // result into a template signature. An already-bound alias uses the folded type.
    const bool dependent_alias = std::ranges::any_of(args, [&](const TemplateArg& a) { return dependent(a); });
    if (!types().is_dependent(tmpl.type) && dependent_alias) return tmpl.type;
    auto substitution = templates_for_use(at);
    substitution.retain_gcc_concrete_transforms = dependent_alias;
    auto result = substitution.substitute(tmpl.type, bindings);
    if (result && ins().types.is<ast::SWrittenTransform>(*result)) return types().canonical(*result);
    return result;
  }
  return templates_for_use(at).substitute(tmpl.type, bindings);
}

auto Sema::variable_template_id(const ast::DeclSema& tmpl, std::span<const TemplateArg> args, u32 begin, u32 end) -> Ex {
  // A static data member template of the current instantiation is value-dependent: each
  // specialization of the class names its own ([temp.dep.constexpr]/2.3, [temp.inst]/3).
  if (const EntityId owner = w_->entities.rec(tmpl.entity).scope;
      owner != EntityId::none && w_->entities.rec(owner).kind == EntityKind::class_ &&
      types().is_dependent(types().class_type(owner)))
    return dependent_variable_template_id(tmpl, args, begin, end);
  auto full = template_arguments(tmpl, args, begin);
  if (!full) return {NodeId::none, begin, end};
  std::vector<ast::TemplateParamSema> params = ins().seqs.elements(tmpl.template_params);
  const Bindings bindings = Bindings::of(params.empty() ? 0 : params[0].depth, *full);
  const EntityRec& entity = w_->entities.rec(tmpl.entity);
  const NameId name = entity.name;
  if (entity.kind == EntityKind::variable_template &&
      !check_named_type_constraints(tmpl, args.size(), *full, bindings, begin))
    return {NodeId::none, begin, end};
  auto t = templates().substitute(tmpl.type, bindings);
  if (!t) {
    diag(begin, Diag::template_args_mismatch, name);
    return {NodeId::none, begin, end};
  }
  EntityId spec = variable_specialization(tmpl.entity, *full);
  if (auto st = q_->specialization_type(spec)) t = *st;
  if (!unevaluated) demands.push_back({spec, ast::Loc{item_, loc_base_ + begin}, *t});
  ExprInfo info{types().referee(*t), ValueCategory::lvalue, 0};
  return node(ast::DeclRef{info, spec}, begin, end, info);
}

auto Sema::unresolved_default_source(const Ex& expression) -> std::optional<Diag> {
  if (!default_source_observer_ || (!default_source_observer_->line &&
      default_source_observer_->descriptor_type == EntityId::none)) return std::nullopt;
  const auto root = b_->commit(ast::Child{expression.id, 0});
  const auto& nodes = ins().ast;
  // A literal through parentheses or contextual conversion to bool cannot observe the line.
  auto literal_truth = [&](NodeId id) -> std::optional<bool> {
    while (true) {
      if (nodes.is<ast::Paren>(id)) {
        id = nodes.get<ast::Paren>(id).operand.id;
      } else if (nodes.is<ast::ImplicitConversion>(id)) {
        const auto conversion = nodes.get<ast::ImplicitConversion>(id);
        if (conversion.conversion != ast::ConversionKind::boolean) return std::nullopt;
        id = conversion.operand.id;
      } else {
        break;
      }
    }
    if (!nodes.is<ast::Literal>(id)) return std::nullopt;
    const auto value = nodes.get<ast::Literal>(id).value;
    if (!ins().values.is<ast::VInt>(value)) return std::nullopt;
    return ast::int_bits(ins(), value) != ast::Bits128{};
  };
  std::optional<Diag> observed;
  auto visit = [&](this const auto& self, const ast::Red& n) -> void {
    if (observed || n.is<ast::ConstantProbe>()) return;
    if (n.is<ast::SourceLine>()) {
      observed = Diag::default_arg_source_line_immediate;
      return;
    }
    if (n.is<ast::SourceLocation>()) {
      observed = Diag::default_arg_source_location_immediate;
      return;
    }
    if (n.is<ast::Conditional>()) {
      const auto conditional = n.get<ast::Conditional>();
      if (const auto truth = literal_truth(conditional.condition.id)) {
        const auto arm = *truth ? conditional.then_ : conditional.else_;
        self(ast::Red(nodes, arm.id));
        return;
      }
    }
    if (n.is<ast::OrderedBinary>()) {
      const auto binary = n.get<ast::OrderedBinary>();
      if ((binary.op == ast::OrderedOp::logical_and && literal_truth(binary.lhs.id) == false) ||
          (binary.op == ast::OrderedOp::logical_or && literal_truth(binary.lhs.id) == true))
        return;
    }
    n.for_each_child(self);
  };
  visit(ast::Red(nodes, root.id));
  return observed;
}

auto Sema::placeholder_concept(NameId name, u32 at) -> std::optional<EntityId> {
  auto not_concept = [&]() -> std::optional<EntityId> {
    diag(at, Diag::not_a_concept, name);
    return std::nullopt;
  };
  auto select = [&](std::span<const ast::DeclSema> decls) -> std::optional<EntityId> {
    auto candidate = std::ranges::find(decls, ast::DeclKind::concept_, &ast::DeclSema::kind);
    if (candidate == decls.end()) return not_concept();
    if (std::ranges::any_of(decls, [&](const ast::DeclSema& d) { return d.entity != candidate->entity; })) {
      diag(at, Diag::ambiguous_name, name);
      return std::nullopt;
    }
    if (ins().seqs.size(candidate->template_params) == 0 ||
        ins().seqs.at(candidate->template_params, 0).kind != ast::TemplateArgKind::type) {
      diag(at, Diag::not_a_type_concept, name);
      return std::nullopt;
    }
    return candidate->entity;
  };
  for (std::size_t depth = scopes_.size(); depth-- > 0;) {
    auto names_here = [=](const auto& binding) { return binding.depth == depth && binding.name == name; };
    if (std::ranges::any_of(scopes_[depth], [&](const auto& local) { return local.first == name; }) ||
        std::ranges::any_of(block_types_, names_here) || std::ranges::any_of(block_aliases_, names_here))
      return not_concept();
    for (const auto& [scope, e] : using_enums_) {
      if (scope != depth) continue;
      if (const EnumRec* record = enum_rec(e); record &&
          std::ranges::any_of(record->enumerators, [&](const EnumeratorRec& x) { return x.name == name; }))
        return not_concept();
    }
    std::vector<ast::DeclSema> from_usings;
    for (const BlockUsing& u : block_usings_)
      if (names_here(u)) from_usings.insert(from_usings.end(), u.decls.begin(), u.decls.end());
    if (!from_usings.empty()) return select(from_usings);
  }
  if (template_param(name) || (namespace_lambda && namespace_lambda->name == name)) return not_concept();
  for (EntityId c = current_class; c != EntityId::none;) {
    const EntityRec& record = w_->entities.rec(c);
    if (record.kind != EntityKind::class_ && record.kind != EntityKind::class_template) break;
    if (record.name == name || find_member(c, name, /*unqualified=*/true) ||
        (c == current_class && q_->earlier_member(name)))
      return not_concept();
    c = record.scope;
  }
  for (auto d = own.rbegin(); d != own.rend(); ++d)
    if (d->name == name) return select(std::span{&d->sema, 1});
  std::vector<ast::DeclSema> decls;
  for (DeclId id : q_->visible_decls(name, directives())) {
    auto d = q_->decl_sema(id);
    if (d.entity != EntityId::none && !is_special(d)) decls.push_back(d);
  }
  return select(decls);
}

auto Sema::atom(u32 begin, u32 end, std::span<const NameId> names, std::span<const PackRef> fold_packs) -> ConstId {
  const bool fn_params = std::ranges::any_of(names, [&](NameId n) { return local_named(n); });
  u16 depth = 0;
  std::vector<ConstId> mapping;
  if (!template_scopes.empty() && !template_scopes.back().empty()) {
    const std::vector<ast::TemplateParamSema>& head = template_scopes.back();
    depth = head[0].depth;
    Templates t = templates();
    for (const ast::TemplateParamSema& p : head) {
      if (!fn_params && !std::ranges::contains(names, p.name)) {
        mapping.push_back(ConstId::none);
        continue;
      }
      // Bound in an instantiation (a nested-requirement of a requires-expression being
      // re-analysed): its argument; otherwise the parameter itself.
      auto bound = template_args.at(p.depth, p.index);
      TemplateArg a = bound ? *bound
                      : p.kind == TemplateArgKind::type ? t.type_arg(p.type)
                      : p.kind == TemplateArgKind::template_ ? TemplateArg{
                            TemplateArgKind::template_, p.type, ConstId::none, {}, EntityId::none, p.concept_template}
                      : t.value_arg(p.type, ins().intern_value(ast::VDepParam{p.depth, p.index}));
      if (!bound && p.pack &&
          !std::ranges::contains(fold_packs, PackRef{p.depth, p.index, {}})) {
        const std::array expansion{t.expansion_of(a)};
        a = t.pack(expansion);
      }
      mapping.push_back(t.mapping_value(a));
    }
  }
  std::vector<u32> calls;
  for (u32 c : dependent_calls)
    if (c >= begin && c < end) calls.push_back(c - begin);
  return ins().intern_value(ast::VAtom{item_, loc_base_ + begin, loc_base_ + end, ins().intern_seq(std::span<const ConstId>(mapping)),
                                       ins().intern_seq(std::span<const u32>(calls)), current_class, atom_lambda_scope, depth,
                                       fn_params});
}

auto Sema::check_placeholder_constraint(QualType placeholder, QualType deduced, u32 at) -> bool {
  const TypeRec p = types().rec(placeholder);
  const NameId name = w_->entities.rec(p.entity).name;
  auto unresolved = [&] {
    diag(at, Diag::placeholder_constraint_unresolved, name);
    return false;
  };
  if (types().is_dependent(deduced)) return unresolved();
  auto c = q_->template_decl(p.entity);
  if (!c) return unresolved();
  // BOOTSTRAP: -Warray-bounds: reserved and appended, as `parameters` in `member_call` (#748).
  // Without the bug: `std::vector<TemplateArg> args{templates().type_arg(deduced)};` and
  // `args.insert(args.end(), tail.begin(), tail.end());`
  const auto tail = ins().seqs.elements(p.args);
  std::vector<TemplateArg> args;
  args.reserve(1 + tail.size());
  args.push_back(templates().type_arg(deduced));
  args.append_range(tail);
  const std::size_t before = diag_count();
  auto nf = concept_constraint(*c, args, at);
  if (!nf) return has_error_since(before) ? false : unresolved();
  auto sat = q_->satisfied(*nf);
  if (q_->satisfaction_failed(*nf)) return false;
  if (!sat) return unresolved();
  if (!*sat) diag(at, Diag::constraints_not_satisfied, name);
  return *sat;
}

auto Sema::pack_index_type(NameId name, u32 at, ConstId index, u32 index_at, ConstId written) -> std::optional<QualType> {
  const ast::TemplateParamSema* p = template_param(name);
  if (!p || p->kind != ast::TemplateArgKind::type || !p->pack) {
    diag(at, Diag::not_a_pack, name);
    return std::nullopt;
  }
  const bool dependent = ins().values.is_dependent(index);
  if (!dependent) {
    const auto b = types().builtin_of(ins().values.get<ast::VInt>(index).type);
    ast::Bits128 v = ast::int_bits(ins(), index);
    if (v.hi != 0 || (b && ast::is_signed(w_->target, *b) && ast::is_negative(v))) {
      diag(index_at, Diag::pack_index_not_constant);
      return std::nullopt;
    }
  }
  auto a = template_args.at(p->depth, p->index);
  if (!a) return types().pack_index(p->type, index, written);
  if (a->kind != ast::TemplateArgKind::pack) {
    diag(index_at, Diag::not_supported, lex::Tk::l_square);
    return std::nullopt;
  }
  if (dependent) return types().pack_index(p->type, index, a->elems, written);
  std::vector<TemplateArg> es = ins().seqs.elements(a->elems);
  auto r = templates().select_element(es, index);
  if (!r) diag(index_at, Diag::pack_index_range, ast::int_bits(ins(), index).lo, es.size());
  return r;
}

auto Sema::trivial_copyability_expr(QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType canonical = ts.canonical(operand);
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, const auto&... args) -> Ex {
    diag_at(loc, d, args...);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(canonical)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::is_trivially_copyable, canonical)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    return fail(Diag::trivial_copyability_dependent);
  }
  const QualType element = ts.innermost_array_element(canonical);
  if (ts.is_scalar(element)) return bool_literal(true, begin, end);
  if (ts.is_reference(element) || ts.is_function(element) || ts.is_void(element))
    return bool_literal(false, begin, end);
  if (ts.is_class(element)) {
    const EntityId cls = ts.class_of(element);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
      if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
      return fail(Diag::trivial_copyability_incomplete, element);
    }
    if (const ClassRec* record = classes().find(cls))
      if (auto fact = record->trivially_copyable()) return bool_literal(*fact, begin, end);
  }
  return fail(Diag::trivial_copyability_unsupported, operand);
}

auto Sema::standard_layout_expr(QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType type = ts.canonical(operand);
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, QualType reported) -> Ex {
    diag_at(loc, d, reported);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(type)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::is_standard_layout, type)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    return fail(Diag::standard_layout_unsupported, operand);
  }
  const QualType element = ts.innermost_array_element(type);
  if (ts.is_reference(element) || ts.is_function(element) || ts.is_void(element))
    return bool_literal(false, begin, end);
  if (ts.is_enum(element)) {
    const EntityId e = ts.enum_of(element);
    if (auto fixed = q_->defining_enum_is_fixed(e)) {
      if (!*fixed) return fail(Diag::standard_layout_incomplete, element);
    } else if (const EnumRec* record = classes().find_enum(e)) {
      if (!record->fixed && !record->defined) return fail(Diag::standard_layout_incomplete, element);
    } else {
      return fail(Diag::standard_layout_unsupported, operand);
    }
    return bool_literal(true, begin, end);
  }
  if (ts.is_class(element)) {
    const EntityId cls = ts.class_of(element);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
      if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
      return fail(Diag::standard_layout_incomplete, element);
    }
    if (auto result = classes().standard_layout(cls)) return bool_literal(*result, begin, end);
  } else if (ts.is_scalar(element) || ts.is_floating(element)) {
    return bool_literal(true, begin, end);
  }
  return fail(Diag::standard_layout_unsupported, operand);
}

auto Sema::implicit_lifetime_expr(QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType type = ts.canonical(operand);
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, QualType reported) -> Ex {
    diag_at(loc, d, reported);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(type)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::is_implicit_lifetime, type)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    return fail(Diag::implicit_lifetime_unsupported, operand);
  }
  if (ts.is_array(type)) return bool_literal(true, begin, end);
  if (ts.is_reference(type) || ts.is_function(type) || ts.is_void(type)) return bool_literal(false, begin, end);
  if (ts.is_enum(type)) {
    const EntityId e = ts.enum_of(type);
    if (auto fixed = q_->defining_enum_is_fixed(e)) {
      if (!*fixed) return fail(Diag::implicit_lifetime_incomplete, type);
    } else if (const EnumRec* record = classes().find_enum(e)) {
      if (!record->fixed && !record->defined) return fail(Diag::implicit_lifetime_incomplete, type);
    } else {
      return fail(Diag::implicit_lifetime_unsupported, operand);
    }
    return bool_literal(true, begin, end);
  }
  if (ts.is_scalar(type) || ts.is_floating(type)) return bool_literal(true, begin, end);
  if (ts.is_class(type)) {
    const EntityId cls = ts.class_of(type);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
      if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
      return fail(Diag::implicit_lifetime_incomplete, type);
    }
    if (const ClassRec* record = classes().find(cls))
      if (auto fact = record->implicit_lifetime()) return bool_literal(*fact, begin, end);
  }
  return fail(Diag::implicit_lifetime_unsupported, operand);
}

auto Sema::unique_object_representations_expr(QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType type = ts.canonical(operand);
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, QualType reported) -> Ex {
    diag_at(loc, d, reported);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(type)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::has_unique_object_representations, type)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    return fail(Diag::unique_object_representations_unsupported, operand);
  }
  const QualType element = ts.innermost_array_element(type);
  if (ts.is_reference(element) || ts.is_function(element) || ts.is_void(element))
    return bool_literal(false, begin, end);
  if (ts.is_class(element)) {
    const EntityId cls = ts.class_of(element);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
      if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
      return fail(Diag::unique_object_representations_incomplete, element);
    }
  } else if (ts.is_enum(element)) {
    const EntityId e = ts.enum_of(element);
    if (auto fixed = q_->defining_enum_is_fixed(e)) {
      if (!*fixed) return fail(Diag::unique_object_representations_incomplete, element);
    } else if (const EnumRec* record = classes().find_enum(e); record && !record->fixed && !record->defined) {
      return fail(Diag::unique_object_representations_incomplete, element);
    }
  } else if (auto b = ts.builtin_of(element)) {
    using enum Builtin;
    // Each bit pattern of a narrow character type represents a distinct value ([basic.fundamental]).
    if (*b == char_ || *b == signed_char || *b == unsigned_char || *b == char8_t_)
      return bool_literal(true, begin, end);
    if (ast::is_standard_floating(*b)) {
      const ast::FloatFormat format = w_->target[*b].format;
      // These formats encode positive and negative zero differently.
      if (format == ast::FloatFormat::binary32 || format == ast::FloatFormat::binary64 ||
          format == ast::FloatFormat::x87_extended)
        return bool_literal(false, begin, end);
    }
  }
  return fail(Diag::unique_object_representations_unsupported, operand);
}

auto Sema::destructibility_expr(ast::DepOp op, NameId name, QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  QualType element = ts.canonical(operand);
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, const auto&... args) -> Ex {
    diag_at(loc, d, args...);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(element)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(op, element)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    return fail(Diag::destructibility_dependent, name);
  }
  // References are destructible even when the referent is incomplete. An unknown-bound
  // array gives false; only a bounded array queries its ultimate element ([meta.unary.prop]).
  if (ts.is_reference(element)) return bool_literal(true, begin, end);
  while (ts.is_array(element)) {
    const TypeRec array = ts.rec(element);
    if (array.bound_value == ConstId::none) return bool_literal(false, begin, end);
    element = ts.canonical(array.inner);
  }
  if (ts.is_function(element) || ts.is_void(element)) return bool_literal(false, begin, end);
  if (!ts.is_enum(element) && ts.is_scalar(element)) return bool_literal(true, begin, end);
  if (ts.is_class(element)) {
    const EntityId cls = ts.class_of(element);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) {
      if (q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
      return fail(Diag::destructibility_incomplete, element, name);
    }
  }
  return fail(Diag::destructibility_unsupported, name, operand);
}

auto Sema::class_is_empty(EntityId cls) -> std::optional<bool> {
  const Classes& records = classes();
  std::vector<EntityId> pending{cls};
  std::set<EntityId> visited;
  bool known = true;
  while (!pending.empty()) {
    const EntityId current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) continue;
    const ClassRec* record = records.find(current);
    if (!record || !record->defined) {
      known = false;
      continue;
    }
    if (record->polymorphic) return false;
    for (const FieldRec& field : record->fields) {
      if (!field.no_unique_address || !types().is_class(field.type)) return false;
      // [intro.object]: a nonzero subobject proves this overlapping member has
      // nonzero size. An otherwise empty member needs a layout fact.
      known = false;
      pending.push_back(types().class_of(field.type));
    }
    for (const BaseRec& base : record->bases) pending.push_back(base.cls);
  }
  return known ? std::optional(true) : std::nullopt;
}

auto Sema::empty_expr(QualType operand, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType type = Types::unqualified(ts.canonical(operand));
  const engine::Loc loc = type_trait_loc(begin);
  if (ts.is_dependent(type)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::is_empty, type)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    // A template body outside the trait's replay scopes can still re-parse it.
    return dependent_node({}, begin, end, builtin(Builtin::bool_));
  }
  if (!ts.is_class(type) || is_union_type(type) == true) return bool_literal(false, begin, end);
  const EntityId cls = ts.class_of(type);
  // Owning replays check a concrete spelling at its source. A re-parsed
  // template body keeps the completeness context it used for this trait.
  bool complete;
  if (auto demand = q_->type_trait_at_demand(cls, loc)) complete = *demand;
  else if (q_->in_type_trait_replay()) complete = q_->is_complete_at_source(cls);
  else complete = q_->is_complete(cls);
  if (!complete) {
    if (!q_->type_trait_instantiation_failed(cls)) diag_at(loc, Diag::empty_incomplete, operand);
    return {NodeId::none, begin, end};
  }
  if (auto result = class_is_empty(cls)) return bool_literal(*result, begin, end);
  diag_at(loc, Diag::empty_unsupported, operand);
  return {NodeId::none, begin, end};
}

auto Sema::aggregate_trait_expr(QualType type, u32 begin, u32 end) -> Ex {
  type = types().canonical(type);
  const engine::Loc loc = type_trait_loc(begin);
  if (types().is_dependent(type)) {
    if (q_->in_type_trait_pattern())
      if (auto value = templates().unary_trait_value(ast::DepOp::is_aggregate, type)) {
        q_->record_dependent_type_trait_operand(loc);
        return literal(builtin(Builtin::bool_), *value, ValueCategory::prvalue, begin, end);
      }
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    return dependent_node({}, begin, end, builtin(Builtin::bool_));
  }
  if (!types().is_class(type)) return bool_literal(types().is_array(type), begin, end);
  const EntityId cls = types().class_of(type);
  if (auto demand = q_->type_trait_at_demand(cls, loc)) {
    if (!*demand) {
      if (!q_->type_trait_instantiation_failed(cls)) diag_at(loc, Diag::virtual_class_trait_incomplete, type);
      return {NodeId::none, begin, end};
    }
  } else if (!require_complete(type, begin)) {
    return {NodeId::none, begin, end};
  }
  const ClassRec* record = classes().find(cls);
  if (!record) return {NodeId::none, begin, end};
  return bool_literal(is_aggregate_class(*record), begin, end);
}

auto Sema::written_move_nothrow(QualType owner, bool constructing) -> std::optional<bool> {
  Types& ts = types();
  const EntityId cls = ts.class_of(owner);
  const ClassRec* record = classes().find(cls);
  if (!record || !record->defined || !record->member_declarations_valid || record->union_ ||
      record->polymorphic || !record->bases.empty() || w_->entities.rec(cls).templ != EntityId::none)
    return std::nullopt;

  const MemberDeclRec* move = nullptr;
  for (const MemberDeclRec& member : record->members) {
    const bool assignment = member.is_function && ins().names.spelling(member.name) == "operator=";
    if (constructing ? !member.is_ctor : !assignment) continue;
    const TypeRec function = ts.rec(member.type);
    if (constructing && !function.variadic && function.params.empty()) continue;
    if (function.variadic || function.params.size() != 1 || member.is_static || member.is_template ||
        function.fn_cv != Cv::none)
      return std::nullopt;

    const TypeRec parameter = ts.rec(function.params.front());
    if (parameter.kind != TypeKind::reference || Types::unqualified(ts.canonical(parameter.inner)) != owner)
      return std::nullopt;
    if (!parameter.rvalue_ref) {
      // A ref-qualified copy assignment could change the ordering of the object bindings.
      if (function.fn_ref != ast::RefQual::none) return std::nullopt;
      continue;
    }
    if (move || ts.canonical(parameter.inner) != owner || function.fn_ref == ast::RefQual::rvalue ||
        (!constructing && (member.explicit_defaulted_assignment || ts.canonical(function.inner) != ts.lvalue_ref(owner))))
      return std::nullopt;
    move = &member;
  }
  if (!move) return std::nullopt;
  // The traits check access as if from a context unrelated to the class.
  if (move->is_deleted || record->access_of(move->entity) != ast::Access::public_) return false;
  const auto declaration = q_->written_member(move->entity);
  if (!declaration || q_->deferred_exception(move->entity) || q_->unresolved_special_member_exception(move->entity))
    return std::nullopt;
  if (!ts.rec(declaration->type).fn_noexcept) return false;
  if (!constructing) return true;
  const EntityId destructor = record->destructor_declaration();
  if (record->access_of(destructor) != ast::Access::public_) return false;
  const auto written_destructor = std::ranges::find(record->members, destructor, &MemberDeclRec::entity);
  if (written_destructor != record->members.end() && written_destructor->is_deleted) return false;
  if (!record->destructor_usable || q_->unresolved_special_member_exception(destructor)) return std::nullopt;
  return record->destructor_noexcept;
}

auto Sema::assignability_expr(ast::DepOp op, NameId name, QualType left, QualType right, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType lhs = ts.canonical(left), rhs = ts.canonical(right);
  const engine::Loc loc = type_trait_loc(begin);
  if (ts.is_dependent(lhs) || ts.is_dependent(rhs)) {
    if (auto pattern = replayed_binary_type_trait(op, lhs, rhs, begin, end)) return *pattern;
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    if (defer_exception_type_trait(op)) {
      q_->record_dependent_type_trait_operand(loc);
      return dependent_node({}, begin, end, builtin(Builtin::bool_));
    }
    diag_at(loc, Diag::assignability_dependent, name);
    return {NodeId::none, begin, end};
  }
  const QualType destination = ts.canonical(ts.referee(lhs));
  const bool lvalue = ts.is_reference(lhs) && !ts.rec(lhs).rvalue_ref;
  if (op == ast::DepOp::is_nothrow_assignable && ts.is_class(destination) && Types::cv(destination) == Cv::none &&
      lvalue && ts.is_reference(rhs) && ts.rec(rhs).rvalue_ref && ts.canonical(ts.referee(rhs)) == destination) {
    const EntityId cls = ts.class_of(destination);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (demand ? *demand : q_->is_complete_at_source(cls)) {
      if (auto answer = written_move_nothrow(destination, /*constructing=*/false)) return bool_literal(*answer, begin, end);
    } else if (q_->type_trait_instantiation_failed(cls)) {
      return {NodeId::none, begin, end};
    }
  }
  if (!ts.is_arithmetic(destination) || !ts.is_arithmetic(ts.referee(rhs))) {
    diag_at(loc, Diag::assignability_unsupported, name, left, right);
    return {NodeId::none, begin, end};
  }
  return bool_literal(lvalue && !Types::is_const(destination), begin, end);
}

auto Sema::base_path(EntityId derived, EntityId base, bool ordinary_layouts) -> BasePath {
  struct Path {
    EntityId cls;
    bool public_;
  };
  std::vector<Path> pending{{derived, true}};
  std::map<EntityId, unsigned> arrivals;
  bool found = false, unique_public = true;
  while (!pending.empty()) {
    const auto [cls, public_] = pending.back();
    pending.pop_back();
    auto& count = arrivals[cls];
    if (count == 2) continue;
    ++count;
    const ClassRec* record = classes().find(cls);
    if (!record || !record->defined ||
        (ordinary_layouts && (record->union_ || record->polymorphic || record->layout_requires_virtual_support)))
      return BasePath::unknown;
    if (cls == base) {
      unique_public = unique_public && !found && public_;
      found = true;
    }
    for (const BaseRec& edge : record->bases)
      pending.push_back({edge.cls, public_ && edge.access == ast::Access::public_});
  }
  return found && unique_public ? BasePath::unique_public : BasePath::none;
}

auto Sema::object_pointer_conversion(QualType from, QualType to, engine::Loc loc) -> PointerConversion {
  using enum PointerConversion;
  const Types& ts = types();
  from = ts.canonical(from);
  to = ts.canonical(to);
  // A null pointer value converts to any pointer type; to `bool` only by direct-initialization
  // ([conv.ptr]/1, [conv.bool]).
  if (ts.is(from, Builtin::nullptr_t)) {
    if (ts.is_pointer(to) || ts.is(to, Builtin::nullptr_t)) return yes;
    return ts.is_arithmetic(to) || ts.is_enum(to) ? no : unmodelled;
  }
  if (ts.is(to, Builtin::bool_)) return yes;  // [conv.bool]
  if (ts.is_arithmetic(to) || ts.is_enum(to) || ts.is(to, Builtin::nullptr_t) || ts.is_member_pointer(to))
    return no;
  if (!ts.is_pointer(to)) return unmodelled;
  // Similar pointers whose levels add cv-qualifiers ([conv.qual]), or a pointer to a noexcept
  // function to one to the same function without it ([conv.fctptr]).
  if (qualification_convertible(from, to) || function_indirection_convertible(from, to)) return yes;
  const QualType source = ts.canonical(ts.pointee(from)), target = ts.canonical(ts.pointee(to));
  // Only object pointers convert to `cv void*` or a base ([conv.ptr]).
  if (ts.is_function(source) || ts.is_function(target)) return no;
  // Every conversion here adds cv-qualifiers to the pointee, never removes them ([conv.qual]).
  // An array's are its elements' ([basic.type.qualifier]/3).
  if (!cv_within(object_cv(source), object_cv(target))) return no;
  const QualType s = object_unqualified(source), t = object_unqualified(target);
  if (ts.is_void(t)) return yes;  // an object type to `cv void*`
  if (!ts.is_class(s) || !ts.is_class(t)) {
    // Pointers that differ below their pointer levels are unrelated, unless an array of unknown
    // bound or a member pointer there makes them similar ([conv.qual]).
    return similar_types(from, to).has_value() ? no : unmodelled;
  }
  // Derived* to Base* needs a complete Derived whose Base is accessible and unambiguous
  // ([conv.ptr]/3); without one, the pointer types are unrelated.
  const EntityId derived = ts.class_of(s), base = ts.class_of(t);
  const auto demand = q_->type_trait_at_demand(derived, loc);
  if (!(demand ? *demand : q_->is_complete_at_source(derived)))
    return q_->type_trait_instantiation_failed(derived) ? failed : no;
  // Virtual functions do not change which bases a class has or the access to them.
  switch (base_path(derived, base, false)) {
    case BasePath::unique_public: return yes;
    case BasePath::none: return no;
    case BasePath::unknown: return unmodelled;
  }
  std::unreachable();
}

auto Sema::conversion_function_initialization(const Ex& object, QualType target, bool direct) -> std::optional<PointerConversion> {
  using enum PointerConversion;
  const Types& ts = types();
  const EntityId cls = ts.class_of(object.type());
  const ClassRec* record = class_rec(cls);
  if (!record) return unmodelled;
  for (const BaseRec& edge : record->bases)
    if (declares_conversion_functions(edge.cls)) return unmodelled;
  struct Viable {
    const MemberDeclRec* function;
    Ics object, result;
  };
  std::vector<Viable> viable;
  const bool class_target = ts.is_class(target);
  const Cv cv = object_cv(object.type());
  const bool object_rvalue = object.cat() != ValueCategory::lvalue;
  for (const MemberDeclRec* candidate : member_functions(cls, ins().names.intern(lex::conversion_function_name))) {
    if (candidate->is_template) return unmodelled;
    if (candidate->is_explicit && (!direct || class_target)) continue;
    const TypeRec fn = ts.rec(candidate->type);
    const Ics object_rank = implicit_object_rank(fn, cv, object_rvalue);
    if (!object_rank.viable) continue;
    // A reference result is an lvalue, or an xvalue for an rvalue reference to an object
    // ([expr.call]/14).
    const QualType produced = ts.canonical(ts.referee(fn.inner));
    Ics result_rank{true, 0, 0};
    if (class_target) {
      if (!ts.is_class(produced)) continue;
      if (!same_unqualified(produced, target)) {
        const int depth = base_depth(produced, target);
        if (!depth) continue;
        result_rank = {true, 2, 0};
        result_rank.base_depth = depth;
      }
    } else {
      const bool reference = ts.is_reference(fn.inner);
      const ValueCategory category = !reference ? ValueCategory::prvalue
                                     : ts.rec(fn.inner).rvalue_ref && !ts.is_function(produced) ? ValueCategory::xvalue
                                                                                                : ValueCategory::lvalue;
      const QualType result_type = reference || ts.is_class(produced) ? produced : Types::unqualified(produced);
      result_rank = ics(opaque_operand(result_type, category, object.begin), target);
      if (!result_rank.viable || (candidate->is_explicit && result_rank.rank != 0)) continue;
    }
    viable.push_back({candidate, object_rank, result_rank});
  }
  if (viable.empty()) return std::nullopt;
  const auto better = [](const Viable& a, const Viable& b) {
    if (a.object < b.object) return true;
    if (b.object < a.object) return false;
    return a.result < b.result;
  };
  const Viable* best = nullptr;
  for (const Viable& v : viable)
    if (std::ranges::all_of(viable, [&](const Viable& other) { return &other == &v || better(v, other); })) best = &v;
  if (!best) return no;  // ambiguous ([over.match.best.general]/3)
  if (best->function->is_deleted || record->access_of(best->function->entity) != ast::Access::public_) return no;
  return yes;
}

auto Sema::user_convertibility(QualType from, QualType to, engine::Loc loc, u32 at_) -> std::optional<PointerConversion> {
  using enum PointerConversion;
  const Types& ts = types();
  const QualType from_object = ts.canonical(ts.referee(from)), to_object = ts.canonical(ts.referee(to));
  const bool from_class = ts.is_class(from_object), to_class = ts.is_class(to_object);
  if (!from_class && !to_class) return std::nullopt;
  for (QualType operand : {from_object, to_object}) {
    if (!ts.is_class(operand)) continue;
    if (is_union_type(operand) != false) return std::nullopt;
    const EntityId cls = ts.class_of(operand);
    const auto demand = q_->type_trait_at_demand(cls, loc);
    if (!(demand ? *demand : q_->is_complete_at_source(cls))) return std::nullopt;
  }
  if (from_class && to_class) {
    const EntityId f = ts.class_of(from_object), t = ts.class_of(to_object);
    if (f == t || reaches_base(f, t) != false) return std::nullopt;
  }
  const bool reference = ts.is_reference(to);
  if (!reference && (ts.is_array(to) || ts.is_function(to))) return std::nullopt;
  const Ex source = declval_operand(from, at_);
  const bool conversion_functions = from_class && declares_conversion_functions(ts.class_of(from_object));
  // [dcl.init.ref]/5 through a conversion function ([over.match.ref]) is not modelled.
  if (reference && conversion_functions) return unmodelled;
  if (reference) return trial_initialization([&] { return bind_reference(source, to, at_); });
  const QualType target = Types::unqualified(to);
  // A closure's conversion function to a pointer to function is not a recorded member
  // ([expr.prim.lambda.closure]/9); `ics` finds it as the invoker. Its other uses, such as a
  // following boolean conversion, are not modelled, nor is a class target, whose converting
  // constructor would take the invoker as a second user-defined conversion ([over.best.ics]/4, #1718).
  if (const ClassRec* record = from_class ? class_rec(ts.class_of(from_object)) : nullptr; record && record->closure) {
    if (!to_class &&
        (closure_invoker(from_object, target) || generic_invoker(from_object, target, at_, /*demand_definition=*/false)))
      return yes;
    return unmodelled;
  }
  const auto converted = [&] { return trial_initialization([&] { return convert(source, target, at_); }); };
  if (!conversion_functions) return to_class ? converted() : no;
  const auto by_function = conversion_function_initialization(source, target, /*direct=*/false);
  if (!to_class) return by_function.value_or(no);
  if (by_function == unmodelled) return unmodelled;
  // Converting constructors compete with the conversion functions in one overload
  // resolution ([over.match.copy]/1); comparing the two kinds is not modelled.
  const PointerConversion by_constructor = converted();
  if (!by_function) return by_constructor;
  if (by_constructor == no) return by_function;
  return unmodelled;
}

auto Sema::convertibility_expr(ast::DepOp op, NameId name, QualType source, QualType target, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType from = ts.canonical(source), to = ts.canonical(target);
  const engine::Loc loc = type_trait_loc(begin);
  if (ts.is_dependent(from) || ts.is_dependent(to)) {
    if (auto pattern = replayed_binary_type_trait(op, from, to, begin, end)) {
      if (op == ast::DepOp::is_convertible) q_->record_dependent_type_trait_operand(loc);
      return *pattern;
    }
    if (auto at_satisfaction = trait_at_satisfaction(loc, begin, end)) return *at_satisfaction;
    diag_at(loc, Diag::convertibility_dependent, name);
    return {NodeId::none, begin, end};
  }
  const auto unsupported = [&]() -> Ex {
    diag_at(loc, Diag::convertibility_unsupported, name, source, target);
    return {NodeId::none, begin, end};
  };
  const bool from_void = ts.is_void(from), to_void = ts.is_void(to);
  const QualType from_object = ts.canonical(ts.referee(from)), to_object = ts.canonical(ts.referee(to));
  const bool source_lvalue = ts.is_reference(from) && !ts.rec(from).rvalue_ref;
  const bool target_reference = ts.is_reference(to);
  const bool rvalue_target = target_reference && ts.rec(to).rvalue_ref;
  const auto answer = [&](PointerConversion c) -> Ex {
    switch (c) {
      case PointerConversion::no: return bool_literal(false, begin, end);
      case PointerConversion::yes: return bool_literal(true, begin, end);
      case PointerConversion::failed: return {NodeId::none, begin, end};
      case PointerConversion::unmodelled: return unsupported();
    }
    std::unreachable();
  };
  // Only `void` converts to `void` ([meta.rel]).
  if (from_void || to_void) return bool_literal(from_void && to_void, begin, end);
  if (auto user = user_convertibility(from, to, loc, begin)) {
    // Whether the selected constructor or conversion function throws is not modelled.
    if (op == ast::DepOp::is_nothrow_convertible && *user == PointerConversion::yes) return unsupported();
    return answer(*user);
  }
  // A pointer or `std::nullptr_t` source converts by a standard conversion only, which does not
  // throw ([conv.ptr], [conv.qual], [conv.bool]).
  if (!target_reference && (ts.is_pointer(from_object) || ts.is(from_object, Builtin::nullptr_t)))
    return answer(object_pointer_conversion(from_object, to, loc));
  if (op == ast::DepOp::is_convertible && ts.is_class(from_object) && ts.is_class(to_object)) {
    const EntityId derived = ts.class_of(from_object), base = ts.class_of(to_object);
    if (derived == base || is_union_type(from_object) != false || is_union_type(to_object) != false)
      return unsupported();
    for (QualType operand : {from_object, to_object}) {
      const EntityId cls = ts.class_of(operand);
      const auto demand = q_->type_trait_at_demand(cls, loc);
      if (demand ? *demand : q_->is_complete_at_source(cls)) continue;
      if (!q_->type_trait_instantiation_failed(cls)) diag_at(loc, Diag::convertibility_incomplete, operand, name);
      return {NodeId::none, begin, end};
    }
    const ClassRec* base_record = unique_public_base(derived, base);
    if (!base_record) return unsupported();
    // The classes are reference-related, so an invalid direct binding cannot fall back to
    // a user-defined conversion or a converted temporary ([dcl.init.ref]).
    if (target_reference) {
      const bool binds_rvalue = rvalue_target || Types::cv(to_object) == Cv::const_;
      return bool_literal(cv_within(Types::cv(from_object), Types::cv(to_object)) &&
                              (source_lvalue ? !rvalue_target : binds_rvalue),
                          begin, end);
    }
    if (supports_base_value_conversion(*base_record, from_object, source_lvalue))
      return bool_literal(true, begin, end);
    return unsupported();
  }
  const auto scalar = [&](QualType t) {
    return ts.is_arithmetic(t) || ts.is_pointer(t) || ts.is(t, Builtin::nullptr_t);
  };
  if (!scalar(from_object) || !scalar(to_object)) return unsupported();
  // These conversions and bindings do not throw either.
  if (!target_reference) return answer(scalar_conversion(from_object, to_object, loc));
  return answer(scalar_reference_binding(from_object, source_lvalue, to, loc));
}

auto Sema::reference_temporary_expr(ast::DepOp op, NameId name, QualType target, QualType source, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType to = ts.canonical(target), from = ts.canonical(source);
  const engine::Loc loc = type_trait_loc(begin);
  if (ts.is_dependent(to) || ts.is_dependent(from)) {
    // Completeness is then judged at the point of instantiation, as for `__is_base_of`.
    q_->record_dependent_type_trait_operand(loc);
    if (auto pattern = replayed_binary_type_trait(op, to, from, begin, end)) return *pattern;
    return dependent_node({}, begin, end, builtin(Builtin::bool_));
  }
  if (!ts.is_reference(to) || ts.is_void(from)) return bool_literal(false, begin, end);
  const QualType to_object = ts.canonical(ts.referee(to));
  const bool prvalue = !ts.is_reference(from);
  const QualType from_object = ts.canonical(prvalue ? from : ts.referee(from));
  const bool binds_rvalue = ts.rec(to).rvalue_ref || Types::cv(to_object) == Cv::const_;
  if (ts.is_arithmetic(from_object) && ts.is_arithmetic(to_object)) {
    // A prvalue of the same type is materialized; another type is converted to a temporary.
    const bool converted = Types::unqualified(from_object) != Types::unqualified(to_object);
    return bool_literal(binds_rvalue && (converted || prvalue), begin, end);
  }
  if (ts.is_class(from_object) && ts.is_class(to_object)) {
    const EntityId f = ts.class_of(from_object), t = ts.class_of(to_object);
    // The reference-related query needs both class definitions ([dcl.init.ref]).
    for (EntityId c : f == t ? std::vector{f} : std::vector{f, t}) {
      const auto demand = q_->type_trait_at_demand(c, loc);
      if (demand ? *demand : q_->is_complete_at_source(c)) continue;
      if (!q_->type_trait_instantiation_failed(c))
        diag_at(loc, Diag::convertibility_incomplete, c == f ? from_object : to_object, name);
      return {NodeId::none, begin, end};
    }
    const std::optional<bool> related = f == t ? std::optional(true) : reaches_base(f, t);
    // Reference-related: a class prvalue is materialized and bound when the cv-qualifiers allow
    // and, for a base, the derived-to-base conversion is valid: an inaccessible or ambiguous
    // base makes the binding ill-formed, with no fallback ([dcl.init.ref]/5).
    if (related == true) {
      if (f != t && !unique_public_base(f, t)) return bool_literal(false, begin, end);
      return bool_literal(binds_rvalue && prvalue && cv_within(Types::cv(from_object), Types::cv(to_object)), begin, end);
    }
  }
  diag_at(loc, Diag::convertibility_unsupported, name, target, source);
  return {NodeId::none, begin, end};
}

auto Sema::base_of_expr(QualType base_type, QualType derived_type, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const QualType base = Types::unqualified(ts.canonical(base_type));
  const QualType derived = Types::unqualified(ts.canonical(derived_type));
  const engine::Loc loc = type_trait_loc(begin);
  const auto fail = [&](Diag d, const auto&... args) -> Ex {
    diag_at(loc, d, args...);
    return {NodeId::none, begin, end};
  };
  if (ts.is_dependent(base) || ts.is_dependent(derived)) {
    if (auto deferred = dependent_class_relation(ast::DepOp::is_base_of, base, derived, begin, end)) return *deferred;
    return fail(Diag::base_of_dependent);
  }
  if (!ts.is_class(base) || !ts.is_class(derived)) return bool_literal(false, begin, end);
  const auto base_union = is_union_type(base), derived_union = is_union_type(derived);
  if (base_union == true || derived_union == true) return bool_literal(false, begin, end);
  if (!base_union || !derived_union) return fail(Diag::base_of_unsupported, base_type, derived_type);
  const EntityId b = ts.class_of(base), d = ts.class_of(derived);
  if (b == d) return bool_literal(true, begin, end);

  const auto complete_at_demand = q_->type_trait_at_demand(d, loc);
  if (!(complete_at_demand ? *complete_at_demand : q_->is_complete_at_source(d))) {
    if (q_->type_trait_instantiation_failed(d)) return {NodeId::none, begin, end};
    return fail(Diag::base_of_incomplete, derived_type);
  }
  if (auto result = reaches_base(d, b)) return bool_literal(*result, begin, end);
  return fail(Diag::base_of_unsupported, base_type, derived_type);
}

auto Sema::layout_compatible(QualType first, QualType second) -> std::optional<bool> {
  const Types& ts = types();
  const QualType a = object_unqualified(ts.canonical(first)), b = object_unqualified(ts.canonical(second));
  if (a == b) return true;
  if (ts.is_enum(a) && ts.is_enum(b)) {
    const auto x = enum_underlying(ts.enum_of(a)), y = enum_underlying(ts.enum_of(b));
    if (!x || !y) return std::nullopt;
    return Types::unqualified(*x) == Types::unqualified(*y);
  }
  if (!ts.is_class(a) || !ts.is_class(b)) return false;
  const EntityId x = ts.class_of(a), y = ts.class_of(b);
  const ClassRec* rx = classes().find(x);
  const ClassRec* ry = classes().find(y);
  if (!rx || !ry || !rx->defined || !ry->defined) return std::nullopt;
  if (rx->union_ != ry->union_) return false;
  for (EntityId c : {x, y}) {
    const auto standard = classes().standard_layout(c);
    if (standard != true) return standard;
  }
  const std::vector<FieldRec>* mx = standard_layout_members(x);
  const std::vector<FieldRec>* my = standard_layout_members(y);
  if (!mx || !my) return std::nullopt;
  // Copies: deciding a member's type can read other class records.
  const std::vector<FieldRec> fx = *mx, fy = *my;
  if (fx.size() != fy.size()) return false;
  if (rx->unapplied_layout || ry->unapplied_layout || q_->class_layout_uncertain(x) || q_->class_layout_uncertain(y))
    return std::nullopt;
  if (rx->union_) {
    const auto overlapping = [](const FieldRec& f) { return f.no_unique_address; };
    if (std::ranges::any_of(fx, overlapping) || std::ranges::any_of(fy, overlapping)) return std::nullopt;
    // Unions pair their members in any order. Layout compatibility is an equivalence, so any
    // compatible partner will do; an undecided pair leaves the answer open only when no
    // partner is found.
    std::vector<bool> paired(fy.size(), false);
    for (const FieldRec& f : fx) {
      bool found = false, undecided = false;
      for (std::size_t i = 0; i < fy.size() && !found; ++i) {
        if (paired[i]) continue;
        const auto same = layout_compatible(f.type, fy[i].type);
        if (!same) undecided = true;
        else if (*same) paired[i] = found = true;
      }
      if (!found) return undecided ? std::nullopt : std::optional(false);
    }
    return true;
  }
  // Structs: the common initial sequence must be all members of both.
  for (std::size_t i = 0; i < fx.size(); ++i) {
    const FieldRec &f = fx[i], &g = fy[i];
    if (f.no_unique_address || g.no_unique_address) {
      // [class.mem.general] ends the common initial sequence at an overlapping member.
      // GCC continues when both members carry the attribute.
      if (compatibility() != ast::Compatibility::gcc || f.no_unique_address != g.no_unique_address) return false;
    }
    if (f.bit_width != g.bit_width || f.wide_bit_width != g.wide_bit_width) return false;
    const auto same = layout_compatible(f.type, g.type);
    if (same != true) return same;
    if (compatibility() == ast::Compatibility::clang) {
      if (f.written_alignment != g.written_alignment) return false;
      continue;
    }
    const auto fa = layout_of(f.type, true), ga = layout_of(g.type, true);
    if (!fa || !ga) return std::nullopt;
    if (std::max(*fa, u64(f.explicit_alignment)) != std::max(*ga, u64(g.explicit_alignment))) return false;
  }
  return true;
}

auto Sema::layout_compatible_expr(NameId name, QualType first, QualType second, u32 begin, u32 end) -> Ex {
  const Types& ts = types();
  const engine::Loc loc = type_trait_loc(begin);
  if (ts.is_dependent(first) || ts.is_dependent(second)) {
    if (auto deferred = dependent_class_relation(ast::DepOp::is_layout_compatible, first, second, begin, end))
      return *deferred;
    diag_at(loc, Diag::class_relation_dependent, name);
    return {NodeId::none, begin, end};
  }
  const auto unsupported = [&]() -> Ex {
    diag_at(loc, Diag::class_relation_unsupported, name, first, second);
    return {NodeId::none, begin, end};
  };
  for (QualType operand : {first, second}) {
    const QualType type = ts.canonical(operand);
    if (ts.is_array(type) && ts.rec(type).bound_value == ConstId::none) continue;
    const QualType element = ts.innermost_array_element(type);
    bool complete = true;
    if (ts.is_class(element)) {
      const EntityId cls = ts.class_of(element);
      const auto demand = q_->type_trait_at_demand(cls, loc);
      complete = demand ? *demand : q_->is_complete_at_source(cls);
      if (!complete && q_->type_trait_instantiation_failed(cls)) return {NodeId::none, begin, end};
    } else if (ts.is_enum(element)) {
      const EntityId e = ts.enum_of(element);
      if (auto fixed = q_->defining_enum_is_fixed(e)) complete = *fixed;
      else if (const EnumRec* record = classes().find_enum(e)) complete = record->fixed || record->defined;
      else return unsupported();
    }
    if (!complete) {
      diag_at(loc, Diag::class_relation_incomplete, element, name);
      return {NodeId::none, begin, end};
    }
  }
  if (auto result = layout_compatible(first, second)) return bool_literal(*result, begin, end);
  return unsupported();
}

auto Sema::trait_expr(lex::TypeTrait t, NameId name, std::span<const QualType> args, u32 begin, u32 end) -> Ex {
  using enum lex::TypeTrait;
  if (t == is_trivially_copyable) return trivial_copyability_expr(args[0], begin, end);
  if (t == is_standard_layout) return standard_layout_expr(args[0], begin, end);
  if (t == is_empty) return empty_expr(args[0], begin, end);
  if (t == is_aggregate) return aggregate_trait_expr(args[0], begin, end);
  QualType b = builtin(Builtin::bool_);
  auto structural_trait = [&](std::optional<ConstId> v) {
    if (v) return literal(b, *v, ValueCategory::prvalue, begin, end);
    ExprInfo info{b, ValueCategory::prvalue, expr_flag::dependent | expr_flag::unrepresented_value};
    return node(ast::Dependent{info, b_->list({})}, begin, end, info);
  };
  std::optional<ast::DepOp> unary_op;
  switch (t) {
  case array_rank: {
    const QualType size = builtin(w_->target.size_type);
    if (auto value = templates().array_rank_value(args[0]))
      return literal(size, *value, ValueCategory::prvalue, begin, end);
    ExprInfo info{size, ValueCategory::prvalue, expr_flag::dependent | expr_flag::unrepresented_value};
    return node(ast::Dependent{info, b_->list({})}, begin, end, info);
  }
  case is_same: return structural_trait(templates().is_same_value(args[0], args[1]));
  case is_trivial: return trivial_or_pod_expr(ast::DepOp::is_trivial, name, args[0], begin, end);
  case is_pod: return trivial_or_pod_expr(ast::DepOp::is_pod, name, args[0], begin, end);
  case is_implicit_lifetime: return implicit_lifetime_expr(args[0], begin, end);
  case has_unique_object_representations: return unique_object_representations_expr(args[0], begin, end);
  case is_destructible: return destructibility_expr(ast::DepOp::is_destructible, name, args[0], begin, end);
  case is_nothrow_destructible:
    return destructibility_expr(ast::DepOp::is_nothrow_destructible, name, args[0], begin, end);
  case is_trivially_destructible:
    return destructibility_expr(ast::DepOp::is_trivially_destructible, name, args[0], begin, end);
  case is_base_of: return base_of_expr(args[0], args[1], begin, end);
  case is_virtual_base_of: return virtual_base_of_expr(name, args[0], args[1], begin, end);
  case is_pointer_interconvertible_base_of:
    return pointer_interconvertible_base_of_expr(name, args[0], args[1], begin, end);
  case is_layout_compatible: return layout_compatible_expr(name, args[0], args[1], begin, end);
  case reference_converts_from_temporary:
    return reference_temporary_expr(ast::DepOp::reference_converts_from_temporary, name, args[0], args[1], begin, end);
  case reference_constructs_from_temporary:
    return reference_temporary_expr(ast::DepOp::reference_constructs_from_temporary, name, args[0], args[1], begin, end);
  case is_assignable: return assignability_expr(ast::DepOp::is_assignable, name, args[0], args[1], begin, end);
  case is_nothrow_assignable:
    return assignability_expr(ast::DepOp::is_nothrow_assignable, name, args[0], args[1], begin, end);
  case is_trivially_assignable:
    return assignability_expr(ast::DepOp::is_trivially_assignable, name, args[0], args[1], begin, end);
  case is_convertible: return convertibility_expr(ast::DepOp::is_convertible, name, args[0], args[1], begin, end);
  case is_nothrow_convertible:
    return convertibility_expr(ast::DepOp::is_nothrow_convertible, name, args[0], args[1], begin, end);
  case is_reference: return structural_trait(templates().unary_trait_value(ast::DepOp::is_reference, args[0]));
  case is_const: unary_op = ast::DepOp::is_const; break;
  case is_volatile: unary_op = ast::DepOp::is_volatile; break;
  case is_array: unary_op = ast::DepOp::is_array; break;
  case is_bounded_array: unary_op = ast::DepOp::is_bounded_array; break;
  case is_unbounded_array: unary_op = ast::DepOp::is_unbounded_array; break;
  case is_pointer: unary_op = ast::DepOp::is_pointer; break;
  case is_function: unary_op = ast::DepOp::is_function; break;
  case is_enum: unary_op = ast::DepOp::is_enum; break;
  case is_scoped_enum: unary_op = ast::DepOp::is_scoped_enum; break;
  case is_polymorphic: return class_property_trait_expr(ast::DepOp::is_polymorphic, args[0], begin, end);
  case is_abstract: return class_property_trait_expr(ast::DepOp::is_abstract, args[0], begin, end);
  case is_final: return class_property_trait_expr(ast::DepOp::is_final, args[0], begin, end);
  case has_virtual_destructor:
    return class_property_trait_expr(ast::DepOp::has_virtual_destructor, args[0], begin, end);
  case is_union: unary_op = ast::DepOp::is_union; break;
  case is_class: unary_op = ast::DepOp::is_class; break;
  case is_member_pointer: unary_op = ast::DepOp::is_member_pointer; break;
  case is_member_object_pointer: unary_op = ast::DepOp::is_member_object_pointer; break;
  case is_member_function_pointer: unary_op = ast::DepOp::is_member_function_pointer; break;
  case is_object: unary_op = ast::DepOp::is_object; break;
  default: break;
  }
  if (unary_op) {
    if (auto v = templates().unary_trait_value(*unary_op, args[0]))
      return literal(b, *v, ValueCategory::prvalue, begin, end);
    if (!types().is_dependent(args[0])) {
      diag(begin, Diag::trait_not_applicable, args[0]);
      return {NodeId::none, begin, end};
    }
  }
  // The generic dependent node lets a template body re-parse the operand.
  if (std::ranges::any_of(args, [&](QualType a) { return types().is_dependent(a); })) return dependent_node({}, begin, end, b);
  const QualType c = types().canonical(args[0]);
  bool v = false;
  switch (t) {
  case is_void: v = types().is_void(c); break;
  case is_integral: v = types().is_integral(c); break;
  case is_floating_point: v = types().is_floating(c); break;
  case is_arithmetic: v = types().is_integral(c) || types().is_floating(c); break;
  case is_lvalue_reference: v = types().is_reference(c) && !types().rec(c).rvalue_ref; break;
  case is_rvalue_reference: v = types().is_reference(c) && types().rec(c).rvalue_ref; break;
  default: break;
  }
  return bool_literal(v, begin, end);
}

auto Sema::scope_named(NameId n, std::optional<u32> enum_qualifier_at, bool* ambiguous_namespace, bool search_block_import_types) -> std::optional<Scope> {
  // A type template parameter as a nested-name-specifier names a dependent type
  // ([temp.dep.type]); in an instantiation, its argument.
  if (const ast::TemplateParamSema* p = template_param(n)) {
    if (p->kind != ast::TemplateArgKind::type) return std::nullopt;
    // A partially replayed alias can bind this parameter to a type that still needs substitution.
    if (auto a = bound_arg(*p)) {
      if (!types().is_class(a->type) && !types().is_enum(a->type)) {
        if (!replaying_alias_type_arguments || !types().is_dependent(a->type)) return std::nullopt;
        // These type forms cannot name a scope even when their components are dependent.
        switch (types().kind(a->type)) {
        case TypeKind::pointer: case TypeKind::member_pointer: case TypeKind::reference:
        case TypeKind::array: case TypeKind::function: return std::nullopt;
        default: break;
        }
      }
      return Scope{std::nullopt, a->type};
    }
    return Scope{std::nullopt, p->type};
  }
  if (search_block_import_types) {
    // A qualifier sees aliases and types introduced by using-declarations in the nearest block.
    // [basic.lookup.qual] ignores local values when looking up a qualifier.
    for (std::size_t depth = scopes_.size(); depth-- > 0;) {
      for (const BlockAlias& alias : block_aliases_ | std::views::reverse)
        if (alias.depth == depth && alias.name == n) return Scope{alias.ns, std::nullopt};
      for (const BlockType& alias : block_types_ | std::views::reverse)
        if (alias.depth == depth && alias.name == n) return Scope{std::nullopt, alias.type};
      for (const BlockUsing& used : block_usings_)
        if (used.depth == depth && used.name == n && used.type_index) {
          const ast::DeclSema& decl = used.decls[*used.type_index];
          // A bare template name hides an outer type but needs arguments to qualify a name.
          if (decl.flags & decl_flag::template_) return std::nullopt;
          return Scope{std::nullopt, decl.type};
        }
    }
  } else {
    // A using-directive or namespace alias keeps its namespace lookup path ([basic.lookup.udir]).
    for (const BlockAlias& alias : block_aliases_ | std::views::reverse)
      if (alias.name == n) return Scope{alias.ns, std::nullopt};
  }
  // A namespace alias of the blocks around a local class ([class.local]).
  for (const OuterBlock* b = outer(); b; b = b->outer.get())
    for (const OuterBlock::Name& nm : b->names)
      if (nm.name == n && nm.kind == OuterBlock::Name::Kind::namespace_alias) return Scope{nm.ns, std::nullopt};
  if (search_block_import_types) {
    if (enum_qualifier_at) {
      std::optional<EntityId> ns;
      if (auto t = class_name(n, true, true, enum_qualifier_at, /*search_locals=*/false, &ns))
        return Scope{std::nullopt, t};
      if (ns) return Scope{ns, std::nullopt};
      return std::nullopt;
    }
    if (auto t = class_name(n, /*elaborated=*/true, /*aliases=*/true, std::nullopt, /*search_locals=*/false))
      return Scope{std::nullopt, t};
  }
  if (auto ns = q_->namespace_named(n, directives())) {
    if (ambiguous_namespace)
      *ambiguous_namespace = competing_qualifier(*ns, q_->qualifier_decls(n, std::nullopt, directives()));
    return Scope{ns, std::nullopt};
  }
  return std::nullopt;
}

auto Sema::scope_member(const Scope& s, NameId n, bool assume_dependent_type, bool* ambiguous_namespace) -> std::optional<Scope> {
  // A member type of the current instantiation is found where it is declared ([temp.dep.type]).
  // A type-only context can assume a dependent name denotes a type until instantiation
  // ([temp.res.general]).
  if (s.type && types().is_class(*s.type)) {
    const auto cls = types().class_of(*s.type);
    if (!types().is_dependent(*s.type) || cls == current_class || encloses_current_class(cls))
      if (auto t = member_type(cls, n)) return Scope{std::nullopt, *t};
  }
  if (assume_dependent_type && s.type && types().is_dependent(*s.type))
    return Scope{std::nullopt, types().dep_name(*s.type, n)};
  if (s.ns) {
    if (auto t = direct_member_introduction(*s.ns, n)) return Scope{std::nullopt, t};
    auto members = q_->qualifier_decls(n, s.ns, {});
    for (const ast::DeclSema& d : members) {
      if (d.kind == ast::DeclKind::namespace_) {
        if (ambiguous_namespace)
          *ambiguous_namespace = competing_qualifier(d.entity, members);
        return Scope{d.entity, std::nullopt};
      }
      if (is_type(d.kind)) return Scope{std::nullopt, d.type};
    }
    if (*s.ns == EntityId::none && members.empty())
      if (auto t = builtin_type_name(n)) return Scope{std::nullopt, *t};
  }
  return std::nullopt;
}

auto Sema::namespace_conversion_type(const Scope& s, NameId name, u32 at) -> std::optional<QualType> {
  if (auto t = direct_member_introduction(*s.ns, name)) return t;
  auto found = q_->namespace_members(*s.ns, name);
  if (found.empty()) return scope_type(s, name);
  std::erase_if(found, [&](const ast::DeclSema& tag) {
    if (tag.kind != ast::DeclKind::class_ && tag.kind != ast::DeclKind::enum_) return false;
    const auto scope = w_->entities.rec(tag.entity).scope;
    return std::ranges::any_of(q_->namespace_members(scope, name), [&](const ast::DeclSema& d) {
      return d.kind != ast::DeclKind::class_ && d.kind != ast::DeclKind::enum_ &&
             w_->entities.rec(d.entity).scope == scope;
    });
  });
  if (found.empty()) return std::nullopt;
  const auto& decl = found.front();
  if (std::ranges::any_of(found, [&](const ast::DeclSema& d) {
        if (d.entity == decl.entity) return false;
        return ((d.flags | decl.flags) & decl_flag::template_) || !is_type(d.kind) || !is_type(decl.kind) ||
               types().canonical(d.type) != types().canonical(decl.type);
      })) {
    diag(at, Diag::ambiguous_name, name);
    return std::nullopt;
  }
  if (!is_type(decl.kind) || (decl.flags & decl_flag::template_)) return std::nullopt;
  return decl.type;
}

auto Sema::using_declaration(const Scope& s, NameId n, u32 at_) -> void {
  std::vector<ast::DeclSema> found = s.ns ? q_->namespace_members(*s.ns, n) : std::vector<ast::DeclSema>{};
  // [namespace.udecl], [basic.scope.scope]: a local value can hide a class or enumeration
  // name. Type aliases and type templates conflict with the value.
  if (!scopes_.empty() && std::ranges::any_of(scopes_.back(), [&](const auto& p) { return p.first == n; }) &&
      (found.empty() || !std::ranges::all_of(found, is_non_template_tag))) {
    diag(at_, Diag::redeclared_local, n);
    return;
  }
  if (found.empty()) {
    diag(at_, Diag::no_namespace_member, n);
    return;
  }
  // [namespace.udecl]: redeclarations of one template and non-template declarations of
  // the same canonical type each agree. A template and an ordinary type conflict.
  const auto same_type = [&](const ast::DeclSema& a, const ast::DeclSema& b) {
    return a.entity == b.entity ||
           (!((a.flags | b.flags) & decl_flag::template_) &&
            types().canonical(a.type) == types().canonical(b.type));
  };
  std::optional<std::size_t> type_index;
  for (std::size_t i = 0; i < found.size(); ++i) {
    const ast::DeclSema& decl = found[i];
    if (!is_type(decl.kind)) continue;
    if (!type_index) type_index = i;
    else if (!same_type(found[*type_index], decl)) {
      diag(at_, Diag::ambiguous_name, n);
      return;
    }
  }
  const std::size_t depth = scopes_.size() - 1;
  if (type_index &&
      (std::ranges::any_of(block_usings_, [&](const BlockUsing& earlier) {
         return earlier.depth == depth && earlier.name == n && earlier.type_index &&
                !same_type(found[*type_index], earlier.decls[*earlier.type_index]);
       }) || std::ranges::any_of(block_types_, [&](const BlockType& alias) {
         return alias.depth == depth && alias.name == n && conflicts_with_imported_alias(found, alias.type);
       }))) {
    diag(at_, Diag::conflicting_declaration, n);
    return;
  }
  block_usings_.push_back({depth, n, std::move(found), type_index});
}

auto Sema::explicit_destroy(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at) -> Ex {
  const u32 begin = object.begin;
  const Ex fail{NodeId::none, begin, end};
  if (!object.ok()) return fail;
  const QualType void_type = builtin(Builtin::void_);
  if (dependent(object)) return dependent_node({object}, begin, end, void_type);
  QualType type = object.type();
  if (arrow) {
    object = rvalue(object);
    if (!object.ok()) return fail;
    if (!types().is_pointer(object.type())) {
      diag(op_at, Diag::arrow_of_non_pointer, object.type());
      return fail;
    }
    type = types().pointee(object.type());
  }
  if (!types().is_class(type)) {
    diag(name_at, Diag::not_supported, lex::Tk::tilde);  // scalar pseudo-destructors
    return fail;
  }
  if (!require_complete(type, op_at)) return fail;
  const EntityId owner = types().class_of(type);
  if (w_->entities.rec(owner).name != name) {
    const auto named = class_name(name, false, true, name_at);
    if (named && types().is_dependent(*named)) return dependent_node({object}, begin, end, void_type);
    if (!named || !same_unqualified(*named, type)) {
      int paths = 0;
      if (named && types().is_class(*named) && base_path(owner, types().class_of(*named), paths))
        diag(name_at, Diag::not_supported, lex::Tk::tilde);
      else
        diag(name_at, Diag::destructor_type_mismatch, name, type);
      return fail;
    }
  }
  const ClassRec* record = class_rec(owner);
  if (!record || record->virtual_destructor) {
    diag(name_at, Diag::not_supported, lex::Tk::tilde);
    return fail;
  }
  if (!record->destructor_usable) {
    diag(name_at, Diag::object_unusable_destructor, type);
    return fail;
  }
  if (!check_destructor_access(type, name_at) || !require_supported_virtual_layout(type, name_at)) return fail;
  if (!arrow) {
    if (!object.glvalue()) object = materialize(object, type, false);
    if (!object.ok()) return fail;
    const ExprInfo pointer{types().pointer(object.type()), ValueCategory::prvalue, 0};
    object = node(ast::Unary{pointer, ast::UnaryOp::address_of, at(object, begin)}, begin, object.end, pointer);
  }
  const ExprInfo info{void_type, ValueCategory::prvalue, 0};
  return node(ast::ExplicitDestroy{info, record->destructor, at(object, begin)}, begin, end, info);
}

auto Sema::member(Ex object, bool arrow, NameId name, u32 end, u32 op_at, u32 name_at, std::optional<NameId> qualifier, bool retain_class_parameter) -> Ex {
  u32 begin = object.begin;
  Ex fail{NodeId::none, begin, end};
  if (!object.ok()) return fail;
  QualType ct = object.type();
  std::optional<ConstId> parameter;
  bool class_parameter = false;
  if (retain_class_parameter && !arrow && !qualifier && types().is_class(ct) && !types().is_dependent(ct)) {
    Ex base = object;
    while (auto p = as<ast::Paren>(base)) base.id = p->operand.id;
    if (auto lit = as<ast::Literal>(base); lit && ins().values.is<ast::VDepParam>(lit->value) &&
        ins().values.get<ast::VDepParam>(lit->value).depth != dependent_depth) {
      parameter = lit->value;
    }
    auto decl = as<ast::DeclRef>(base);
    class_parameter = parameter || (decl && w_->entities.rec(decl->entity).kind == EntityKind::template_parameter_object);
  }
  // A member of the current instantiation or of an unknown specialization is looked up when the
  // template is instantiated ([temp.dep.expr]/5).
  if (dependent(object) && !parameter)
    return dependent_node({object}, begin, end, std::nullopt, ValueCategory::lvalue);
  if (arrow) {
    object = rvalue(object);
    if (!types().is_pointer(object.type()) || !types().is_class(types().pointee(object.type()))) {
      diag(op_at, Diag::arrow_of_non_pointer, object.type());
      return fail;
    }
    ct = types().pointee(object.type());
  } else if (!types().is_class(ct)) {
    diag(op_at, Diag::member_of_non_class, ct);
    return fail;
  } else if (!object.glvalue()) {
    object = materialize(object, ct, false);  // [expr.ref]: a class prvalue is materialized
    if (!object.ok()) return fail;
  }
  if (!require_complete(ct, op_at)) return fail;
  EntityId c = types().class_of(ct);
  if (qualifier) {
    const ClassRec* record = class_rec(c);
    if (*qualifier != w_->entities.rec(c).name || ((!record || !record->dynamic) && !virtual_consumer(c))) {
      diag(name_at, Diag::not_supported, lex::Tk::coloncolon);
      return fail;
    }
  }
  auto found = find_member(c, name);
  if (!found) {
    diag(name_at, Diag::no_member, name, Types::unqualified(ct));
    return fail;
  }
  if (parameter && !found->field)
    return dependent_node({object}, begin, end, std::nullopt, ValueCategory::lvalue);
  if (found->ambiguous) {
    diag(name_at, Diag::ambiguous_member, name, Types::unqualified(ct));
    return fail;
  }
  if (found->nested_type != EntityId::none) {
    if (found->enumerator == EntityId::none) {
      diag(name_at, Diag::type_as_expression, name);
      return fail;
    }
    if (!check_access(found->owner, found->nested_type, name, name_at, c)) return fail;
    Ex value = enumerator(found->enumerator, types().enum_type(found->nested_type), name_at, end);
    if (!value.ok()) return fail;
    Ex discarded = convert_node(object, ast::ConversionKind::to_void, builtin(Builtin::void_));
    return node(ast::OrderedBinary{value.info, ast::OrderedOp::comma, at(discarded, begin), at(value, begin)},
                begin, end, value.info);
  }
  EntityId naming = c;
  // A member of a base: named through the base class subobject ([class.member.lookup]); a
  // static member needs no object, so not the conversion either.
  if (found->owner != c && found->member && found->member->is_static) {
    c = found->owner;
  } else if (found->owner != c) {
    object = to_base(object, found->owner, name_at);
    if (!object.ok()) return fail;
    ct = arrow ? types().pointee(object.type()) : object.type();
    c = found->owner;
  }
  if (found->member && found->member->is_function)
    return bound_member(c, name, object, arrow, begin, end, name_at, naming, qualifier.has_value(),
                        /*explicit_receiver=*/true);
  if (!check_access(c, found->member ? found->member->entity : found->field->entity, name, name_at, naming)) return fail;
  if (found->member) {
    // A static data member: the object expression is evaluated, then discarded ([expr.ref]).
    const MemberDeclRec& m = *found->member;
    if (m.is_template) {
      diag(name_at, Diag::variable_template_without_arguments, name);
      return fail;
    }
    ExprInfo info{m.type, ValueCategory::lvalue, 0};
    Ex ref = node(ast::DeclRef{info, m.entity}, name_at, end, info);
    Ex discarded = convert_node(object, ast::ConversionKind::to_void, builtin(Builtin::void_));
    return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(discarded, begin), at(ref, begin)}, begin, end, info);
  }
  u32 index = found->index;
  const FieldRec& f = *found->field;
  if (unevaluated == 0 && !(checking_virtual_body && c == current_class) &&
      !(has_peer_virtual_layout(ct) && (arrow || peer_object_lvalue(object))) &&
      !(has_peer_automatic_layout(ct) && default_constructed_peer_object(object)) &&
      !require_supported_virtual_layout(ct, name_at)) return fail;
  // [expr.ref]/7.2: the member's type, with the object's cv-qualifiers added; the result is an
  // lvalue if the object is (or is reached through a pointer), else an xvalue.
  // A reference member gives the object it refers to, an lvalue.
  bool ref = types().is_reference(f.type);
  QualType t = field_access_type(f, ct);
  if (f.bit_width && Types::is_volatile(t) && unevaluated == 0) {
    diag(name_at, Diag::volatile_bit_field);
    return fail;
  }
  ValueCategory cat = ref || arrow || object.cat() == ValueCategory::lvalue ? ValueCategory::lvalue : ValueCategory::xvalue;
  ExprInfo info{t, cat, class_parameter ? expr_flag::structural_projection : u8{0}, f.bit_width};
  if (parameter) {
    info.flags |= expr_flag::dependent;
    ConstId value = ins().intern_value(ast::VDepField{c, f.entity, types().canonical(f.type), *parameter});
    return node(ast::Literal{info, value}, begin, end, info);
  }
  return node(ast::Member{info, f.entity, index, arrow ? 1u : 0u, ref ? 1u : 0u, at(object, begin)}, begin, end, info);
}

auto Sema::can_parenthesize_aggregate(QualType t, std::span<const Ex> args, bool allow_base_copy) -> bool {
  const auto arithmetic_record = [&](QualType type, const ClassRec& record) {
    return is_aggregate(type) && !record.union_ && record.destructor_usable &&
           record.destructor == EntityId::none &&
           std::ranges::all_of(record.fields, [&](const FieldRec& f) {
             return record.access_of(f.entity) == ast::Access::public_ &&
                    f.bit_width == 0 && types().is_arithmetic(f.type);
           });
  };
  const ClassRec* c = class_rec(types().class_of(t));
  if (!c || !arithmetic_record(t, *c)) return false;
  // A sole class expression normally uses the class conversion path, including base adjustment.
  if (c->bases.empty())
    return args.size() != 1 || (args.front().ok() && !types().is_class(args.front().type()));
  if (!allow_base_copy || c->bases.size() != 1 || c->unapplied_layout || c->layout_requires_virtual_support ||
      c->fields.empty() || args.empty() || args.size() - 1 > c->fields.size()) return false;

  // Copy the one direct nonvirtual base from its exact type.
  const BaseRec& edge = c->bases.front();
  if (edge.access != ast::Access::public_ || edge.dependent_in_pattern) return false;
  const QualType base_type = types().class_type(edge.cls);
  const Ex& first = args.front();
  if (!first.ok() || types().is_dependent(base_type) || !same_unqualified(first.type(), base_type)) return false;
  if (std::ranges::any_of(std::span(c->fields).subspan(args.size() - 1), &FieldRec::dmi) &&
      (unevaluated || templated() || !ordinary_default_member_owner(types().class_of(t)))) return false;
  const QualType source = types().canonical(first.type());
  if (Types::is_volatile(source)) return false;
  const ClassRec* base = class_rec(edge.cls);
  if (!base || !base->bases.empty() || base->unapplied_layout || !arithmetic_record(base_type, *base) ||
      base->trivial_copyability.copy_constructor != CopyOperation::eligible_trivial ||
      base->trivial_copyability.destructor != CopyOperation::eligible_trivial ||
      base->copy_deleted || base->copy_nontrivial() ||
      std::ranges::any_of(base->fields, [&](const FieldRec& f) {
        return Types::is_volatile(types().canonical(f.type));
      })) return false;
  if (args.size() == 1) {
    // A conversion function could make the derived copy or move constructor viable.
    const NameId conversion = ins().names.intern(lex::conversion_function_name);
    if (!member_functions(edge.cls, conversion).empty()) return false;
  }
  if (first.cat() == ValueCategory::lvalue) return !Types::is_const(source) || base->defaulted_copy_const;
  return first.cat() == ValueCategory::prvalue && Types::cv(source) == Cv::none &&
         explicit_aggregate_prvalue(first) && base->move_usable && !base->move_nontrivial &&
         base->trivial_copyability.move_constructor == CopyOperation::eligible_trivial;
}

auto Sema::element_default(const Element& e, bool value, bool replay, u32 begin, u32 end, EntityId required_base) -> Ex {
  const bool fallback = !replay && required_base == EntityId::none;
  if (e.default_member != EntityId::none && (!fallback || e.init == ConstId::none)) {
    if (auto init = q_->replay_default_member(*this, e.default_member, required_base, fallback)) {
      if (*init == NodeId::none) return {NodeId::none, begin, end};
      if (fallback) return {*init, begin, end, {Types::unqualified(e.type), ValueCategory::prvalue, 0}};
      ExprInfo info{e.type, ValueCategory::prvalue, 0};
      // A named declaration in a template is also a newly supported replay.
      if ((!replay_default_members_ || templated()) && required_base == EntityId::none &&
          !unevaluated && !constant_reference_context) {
        const Ex operand{*init, begin, end, info};
        const Ex proof = runtime_default_supported(operand);
        if (!proof.ok()) return {NodeId::none, begin, end};
        const auto result = as<ast::Literal>(proof);
        if (!result || !w_->values.rec(result->value).bits) {
          diag(begin, Diag::aggregate_default_unwind);
          return {NodeId::none, begin, end};
        }
      }
      return node(ast::DefaultMemberInit{info, {*init, 0}}, begin, end, info);
    }
    if (required_base != EntityId::none) {
      diag(begin, Diag::not_supported, lex::Tk::equal);
      return {NodeId::none, begin, end};
    }
  }
  if (e.init != ConstId::none) return literal(Types::unqualified(e.type), e.init, ValueCategory::prvalue, begin, end);
  if (e.default_member != EntityId::none) {
    diag(begin, Diag::not_supported, lex::Tk::equal);  // a non-constant default member initializer in an aggregate (#118)
    return {NodeId::none, begin, end};
  }
  return value ? value_init(e.type, begin, end) : default_init(e.type, begin, end);
}

auto Sema::list_init(QualType t, const Braced& b, Construction form, bool extend_reference) -> Ex {
  Ex fail{NodeId::none, b.begin, b.end};
  if (types().is_dependent(t) || dependent(b)) {
    std::vector<Ex> ops;
    for (const Braced::Clause& c : b.clauses)
      if (!c.list) ops.push_back(c.expr);
    return dependent_node(ops, b.begin, b.end, types().is_dependent(t) ? std::nullopt : std::optional(Types::unqualified(t)));
  }
  if (types().is_void(t) && b.clauses.empty()) return void_prvalue(b.begin, b.end);  // [expr.type.conv]/2
  if (types().is_reference(t) && types().is_class(types().referee(t)))
    return class_reference_list_init(t, b, extend_reference);
  if (!require_complete(t, b.begin) || !require_concrete_object(t, b.begin) ||
      !require_supported_virtual_layout(t, b.begin)) return fail;
  // [dcl.init.string]/1: a character array from a string literal in braces.
  if (b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
    if (auto r = string_init(t, b.clauses[0].expr)) return *r;
  // [dcl.init.general]: an initializer-list or a designated-initializer-list, not a mix.
  if (std::ranges::any_of(b.clauses, &Braced::Clause::designated) &&
      !std::ranges::all_of(b.clauses, &Braced::Clause::designated)) {
    diag(b.begin, Diag::mixed_designators);
    return fail;
  }
  // [dcl.init.list]: an aggregate class from one element of its own class. A same-type
  // prvalue also initializes its result object directly ([dcl.init.general]).
  if (types().is_class(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated &&
      initializes(t, b.clauses[0].expr) && (is_aggregate(t) || !b.clauses[0].expr.glvalue()))
    return convert(b.clauses[0].expr, t, b.clauses[0].begin);
  // [dcl.init.list]/3.7: a class with constructors: `{}` value-initializes it, else the clauses
  // are a constructor's arguments, without narrowing.
  if (has_user_ctors(t)) {
    if (b.clauses.empty() && form == Construction::direct_list) return value_init(t, b.begin, b.end);
    return construct_from_list(t, b, form);
  }
  // A reference from one element ([dcl.init.list]/3.10).
  if (types().is_reference(t) && b.clauses.size() == 1 && !b.clauses[0].list && !b.clauses[0].designated)
    return list_element(t, b.clauses[0].expr);
  if (is_aggregate(t)) {
    std::size_t next = 0;
    Ex r = aggregate(t, b, next, b.begin, b.end);
    if (r.ok() && next < b.clauses.size()) {
      diag(b.clauses[next].begin, Diag::too_many_initializers, Types::unqualified(t));
      return fail;
    }
    return r;
  }
  // A scalar: `{}` value-initializes it, `{e}` copy-initializes it from `e`, without narrowing.
  if (b.clauses.empty()) return value_init(t, b.begin, b.end);
  if (b.clauses.size() > 1) {
    diag(b.clauses[1].begin, Diag::too_many_initializers, Types::unqualified(t));
    return fail;
  }
  const Braced::Clause& c = b.clauses[0];
  if (c.list || c.designated) {
    diag(c.begin, Diag::braces_for_scalar, Types::unqualified(t));
    return fail;
  }
  // [dcl.init.list]/3.8: an enumeration with a fixed underlying type from one integer, without
  // narrowing.
  if (types().is_enum(t) && c.expr.ok() && !types().is_enum(c.expr.type())) {
    const EnumRec* er = enum_rec(types().enum_of(t));
    if (er && er->fixed) {
      Ex v = list_element(*er->underlying, c.expr);
      if (!v.ok()) return fail;
      return convert_node(v, ast::ConversionKind::integral, Types::unqualified(t));
    }
  }
  return list_element(t, c.expr);
}

auto Sema::aggregate(QualType t, const Braced& b, std::size_t& next, u32 begin, u32 end) -> Ex {
  Ex fail{NodeId::none, begin, end};
  std::vector<Element> elems = elements(t);
  std::vector<Ex> out;
  const bool replay = can_replay_default_members(t);
  const bool union_ = is_union(t);
  if (union_ && !elems.empty() && next == b.clauses.size() && compatibility() == ast::Compatibility::gcc) {
    const ClassRec& record = *class_rec(types().class_of(t));
    const QualType first = types().innermost_array_element(elems.front().type);
    const ClassRec* variant = types().is_class(first) ? class_rec(types().class_of(first)) : nullptr;
    // Preserve the simple zero initializer. A first class variant would otherwise run
    // a constructor or require access to a destructor that GCC does not select.
    if (variant && (has_member_inits(elems.front().type) || variant->destructor_declaration() != EntityId::none) &&
        std::ranges::none_of(record.fields, [](const FieldRec& field) { return field.dmi || field.init != ConstId::none; }))
      return value_init(t, begin, end);
  }
  // [dcl.init.aggr]: the caller can potentially destroy each initialized class element.
  // The existing union check applies to the selected variant. Other aggregates also check
  // a written trivial destructor, whose access is independent of its runtime cleanup.
  for (const Element& element : elems) {
    const QualType type = types().innermost_array_element(element.type);
    const ClassRec* record = types().is_class(type) && !types().is_dependent(type)
                               ? class_rec(types().class_of(type)) : nullptr;
    if ((union_ || (compatibility() != ast::Compatibility::gcc && record &&
                   record->declared_destructor != EntityId::none && record->destructor == EntityId::none)) &&
        !check_destructor_access(type, begin)) return fail;
  }
  const auto omitted = [&](const Element& element, u32 at) {
    // [dcl.init.aggr]: copy-list-initialize an omitted union element and the
    // first variant of an empty union list in the absence of a default member initializer.
    if (element.init == ConstId::none && element.default_member == EntityId::none &&
        (union_ || (is_union(element.type) && is_aggregate(element.type))))
      return list_init(element.type, Braced{at, at, {}}, Construction::copy_list);
    return element_default(element, true, replay, at, at);
  };
  bool elided = begin != b.begin || end != b.end;
  for (std::size_t i = 0; i < elems.size(); ++i) {
    if (next == b.clauses.size()) {
      out.push_back(omitted(elems[i], end));
      if (!out.back().ok()) return fail;
      continue;
    }
    const Braced::Clause& c = b.clauses[next];
    if (c.designated) {
      // [dcl.init.aggr]/3.1: designators name members in declaration order; the members
      // skipped are initialized as if there were no initializer.
      std::size_t j = i;
      while (j < elems.size() && elems[j].name != c.designator) ++j;
      if (j == elems.size() && union_ && !elided) {
        diag(c.begin, Diag::not_supported, lex::Tk::period);  // a union's member other than its first
        return fail;
      }
      if (elided || !types().is_class(t) || j == elems.size()) {
        diag(c.begin, Diag::bad_designator, c.designator, Types::unqualified(t));
        return fail;
      }
      for (; i < j; ++i) {
        out.push_back(omitted(elems[i], c.begin));
        if (!out.back().ok()) return fail;
      }
    }
    QualType et = elems[i].type;
    if (c.list) {
      ++next;
      out.push_back(list_init(et, *c.list, Construction::copy_list));
    } else if (auto str = string_element(et, c.expr)) {
      // [dcl.init.string]: a character array element from a string literal, not elided into.
      ++next;
      out.push_back(*str);
    } else if (is_aggregate(et) && !c.designated && !initializes(et, c.expr)) {
      // Brace elision: the clause begins the sub-aggregate's own elements.
      if (!require_complete(et, c.begin)) return fail;
      out.push_back(aggregate(et, b, next, c.begin, c.begin));
      out.back().end = next > 0 ? b.clauses[next - 1].expr.end : c.begin;
    } else {
      ++next;
      out.push_back(list_element(et, c.expr));
    }
    if (!out.back().ok()) return fail;
  }
  return list_of(t, std::move(out), begin, end);
}

auto Sema::parenthesized_aggregate(QualType t, std::span<const Ex> args, u32 begin, u32 end) -> Ex {
  const std::vector<Element> elems = elements(t);
  if (args.size() > elems.size()) {
    diag(args[elems.size()].begin, Diag::too_many_initializers, Types::unqualified(t));
    return {NodeId::none, begin, end};
  }
  std::vector<Ex> out;
  out.reserve(elems.size());
  const bool replay = replay_default_members_ && can_replay_default_members(t);
  // The derived fallback has proved that its first element is the supplied exact base.
  const EntityId required_base = !elems.empty() && types().is_class(elems.front().type)
      ? types().class_of(elems.front().type) : EntityId::none;
  for (std::size_t i = 0; i < elems.size(); ++i) {
    Ex init;
    if (i < args.size()) {
      Ex source = args[i];
      if (i == 0 && types().is_class(elems[i].type) && source.cat() == ValueCategory::prvalue) {
        const Ex* list = explicit_aggregate_prvalue(source);
        if (!list) {
          diag(begin, Diag::no_viable_constructor, Types::unqualified(t));
          return {NodeId::none, begin, end};
        }
        // The base is a potentially overlapping subobject ([intro.object]). The gate
        // proved a trivial implicit move. Initialize a complete temporary and copy its
        // value into the base before evaluating the following field initializer.
        source = materialize(*list, Types::unqualified(elems[i].type), false);
      }
      init = convert(source, elems[i].type, args[i].begin);
    } else {
      init = element_default(elems[i], true, replay, end, end, required_base);
    }
    if (!init.ok()) return {NodeId::none, begin, end};
    out.push_back(init);
  }
  return list_of(t, std::move(out), begin, end);
}

auto Sema::narrows(const Ex& from, QualType to) -> bool {
  if (!from.ok() || dependent(from) || types().is_dependent(to)) return false;
  auto a = types().builtin_of(from.type()), b = types().builtin_of(to);
  if (types().is_pointer(from.type()) && b == Builtin::bool_) return true;
  // Check an unscoped enumeration's narrowing against its underlying integer type.
  if (b && ast::is_floating(*b) && is_unscoped_enum(from.type())) {
    QualType u = underlying_of(from.type());
    return narrows_floating(convert_node(from, ast::ConversionKind::integral, u), *types().builtin_of(u), *b);
  }
  if (a && b && (ast::is_floating(*a) || ast::is_floating(*b))) return narrows_floating(from, *a, *b);
  if (!a || !b || !ast::is_integral(*a) || !ast::is_integral(*b) || *a == *b) return false;
  IntShape sa = shape(w_->target, *a), sb = shape(w_->target, *b);
  if (*b == Builtin::bool_) sb = {1, false};
  if (*a == Builtin::bool_) sa = {1, false};
  // A bit-field narrower than its type holds only the values of its width ([dcl.init.list]/7.4).
  if (from.info.bit_width && from.info.bit_width < sa.bits) sa.bits = from.info.bit_width;
  if (ast::represents(w_->target, *b, sa.bits, sa.is_signed) || (*b == Builtin::bool_ && *a == Builtin::bool_)) return false;
  auto v = constant_of ? constant_of(from) : std::nullopt;
  if (!v || !ins().values.is<ast::VInt>(*v)) return true;
  // The constant, read in the source type, must lie in the target type's range: reduced to the
  // target's width it keeps its bits and its sign.
  ast::Bits128 x = ast::wrap(ast::int_bits(ins(), *v), sa.bits, sa.is_signed);
  ast::Bits128 y = ast::wrap(x, sb.bits, sb.is_signed);
  return y != x || (sa.is_signed && ast::is_negative(x)) != (sb.is_signed && ast::is_negative(y));
}

auto Sema::template_param_id(const ast::TemplateParamSema& p, std::span<const TemplateArg> args, u32 at) -> std::optional<QualType> {
  // The arguments fit the parameter's own head, whose defaults complete them, whatever template
  // it is bound to ([temp.arg]/4, [temp.arg.template]); a pack parameter's arguments grouped, as
  // Itanium mangles them (`T_IclE`, `T_IcJEE`).
  std::vector<ast::TemplateParamSema> head = ins().seqs.elements(p.params);
  Templates t = templates();
  std::vector<std::optional<TemplateArg>> out(head.size());
  std::optional<std::vector<TemplateArg>> full;
  if (t.explicit_into(head, args, out)) full = t.complete(ast::DeclSema{}, head, out);
  if (!full) {
    diag(at, Diag::template_args_mismatch, p.name);
    return std::nullopt;
  }
  if (auto a = bound_arg(p); a && a->kind == ast::TemplateArgKind::template_) {
    if (a->templ != EntityId::none) {
      auto d = q_->template_decl(a->templ);
      if (!d) return std::nullopt;
      return class_template_id(*d, t.flatten(*full), at, /*directly_named=*/false);
    }
    TypeRec r = types().rec(a->type);
    // Bound to `Q::template M` with Q dependent: `typename Q::template M<args>`.
    if (r.kind == TypeKind::dep_name) return dependent_member_template_id(r.inner, r.name, *full);
    return types().dep_spec(r.depth, r.index, r.pack, *full);
  }
  return types().dep_spec(p.depth, p.index, p.pack != 0, *full);
}

auto Sema::template_name_arg(std::optional<EntityId> ns, NameId name, std::optional<u32> at) -> std::optional<TemplateArg> {
  if (!ns)
    if (const ast::TemplateParamSema* p = template_param(name)) {
      if (p->kind != ast::TemplateArgKind::template_) return std::nullopt;
      if (auto a = bound_arg(*p)) return a;
      return TemplateArg{TemplateArgKind::template_, p->type, ConstId::none, {}, EntityId::none, p->concept_template};
    }
  // In a class template, its injected-class-name is the template as a template argument and the
  // class (the current instantiation, or the specialization) as a type one ([temp.local]/1).
  if (!ns && current_class != EntityId::none) {
    const EntityRec& c = w_->entities.rec(current_class);
    if (c.templ != EntityId::none && c.name == name && !block_type_template(name).found)
      return TemplateArg{ast::TemplateArgKind::template_, types().class_type(current_class), ConstId::none, {}, c.templ};
  }
  auto t = ns ? namespace_type_template(*ns, name) : type_template(name);
  if (!t) {
    const auto c = ns ? namespace_template(*ns, name, ast::DeclKind::concept_) : concept_named(name);
    if (!c) return std::nullopt;
    return TemplateArg{TemplateArgKind::template_, QualType{}, ConstId::none, {}, c->entity, true};
  }
  if (!ns && at) unqualified_type_template_access(*t, name, *at);
  return TemplateArg{ast::TemplateArgKind::template_, QualType{}, ConstId::none, {},
                     templates().template_argument_entity(*t)};
}

auto Sema::templates() -> Templates {
  Templates t(*w_, classes());
  t.compatibility = compatibility();
  t.class_definition = [this](EntityId c) { return class_rec(c); };
  t.defining_enum_is_scoped = [this](EntityId e) { return q_->defining_enum_is_scoped(e); };
  t.member_type = [this](EntityId c, NameId n) { return member_type(c, n); };
  t.member_template = [this](EntityId c, NameId n) -> std::optional<EntityId> {
    auto d = member_alias_template(c, n);
    return d ? std::optional(d->entity) : std::nullopt;
  };
  t.member_constant = [this](QualType qualifier, NameId name) {
    return qualified_member_constant(qualifier, name, 0);
  };
  t.plain_class_template_type = [this](EntityId c) { return q_->plain_class_template_type(c); };
  t.template_decl = [this](EntityId tmpl) { return q_->template_decl(tmpl); };
  t.variable_constant = [this](EntityId spec) { return q_->constant_value(spec); };
  t.concept_constant = [this](EntityId entity, std::span<const TemplateArg> args) {
    return evaluate_dependent_concept(entity, args, 0);
  };
  t.call_constant = [this](EntityId templ, std::span<const TemplateArg> args) {
    return evaluate_dependent_call(templ, args, 0);
  };
  t.alias_type = [this](const ast::DeclSema& alias, std::span<const TemplateArg> args, const Bindings& b,
                        AliasSubstitution use) {
    return substitute_alias(alias, args, b, 0, use);
  };
  t.variable_global = [this](EntityId spec) { return q_->global(spec); };
  t.variable_specialization_type = [this](EntityId spec) { return q_->specialization_type(spec); };
  return t;
}

auto Sema::is_template_name(NameId name) -> bool {
  for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s)
    for (auto p = s->rbegin(); p != s->rend(); ++p)
      if (p->first == name) return false;
  if (template_param(name)) return false;
  // A member template of the class whose member is being analysed ([temp.mem]).
  // A static data member template of the class, a base or an enclosing class ([temp.mem]).
  if (unqualified_member_variable_template(name)) return true;
  // Of a complete class, the member its lookup finds, perhaps in a base ([class.member.lookup]).
  if (current_class != EntityId::none && class_rec(current_class))
    if (auto member = find_member(current_class, name, /*unqualified=*/true);
        member && !member->ambiguous && member->member && member->member->is_template)
      return true;
  if (names_member(name)) {
    if (class_has_member_template(current_class, name)) return true;
    const auto bases = class_prefix_bases(current_class);
    if (!bases.empty() && !static_initializer_has_own_name(name) && !q_->earlier_member(name))
      if (auto member = find_base_member(bases, name, /*unqualified=*/true);
          member && !member->ambiguous && member->member && (member->member->is_function || member->member->is_template))
        return class_has_member_template(member->owner, name);
    return false;
  }
  for (const BlockUsing& u : block_usings_)
    if (u.name == name) return std::ranges::any_of(u.decls, [](const ast::DeclSema& d) { return (d.flags & decl_flag::template_) != 0; });
  for (const OwnDecl& o : own)
    if (o.name == name) return (o.sema.flags & decl_flag::template_) != 0;
  for (DeclId d : q_->visible_decls(name, directives()))
    if (q_->decl_sema(d).flags & decl_flag::template_) return true;
  return false;
}

auto Sema::dependent_immediate_template_call(Ex callee, std::span<const TemplateArg> args, u32 end, bool allow_class_result) -> std::optional<Ex> {
  if (!types().is(callee.type(), Builtin::overload_set)) return std::nullopt;
  auto t = templates();
  if (!std::ranges::all_of(args, [&](const TemplateArg& a) {
        return a.kind == TemplateArgKind::type && t.structural_operand(a.type);
      })) return std::nullopt;
  auto found = overloads_.find(std::to_underlying(callee.id));
  if (found == overloads_.end()) return std::nullopt;
  const ast::DeclSema* selected = nullptr;
  for (const ast::DeclSema& d : found->second) {
    if (!(d.flags & decl_flag::template_)) continue;
    if (selected || !(d.flags & decl_flag::consteval_) || (d.flags & decl_flag::member))
      return std::nullopt;
    selected = &d;
  }
  if (!selected) return std::nullopt;
  auto fn = types().rec(selected->type);
  if (!fn.params.empty() || fn.variadic) return std::nullopt;
  const auto params = ins().seqs.elements(selected->template_params);
  if (!std::ranges::all_of(params, [](const ast::TemplateParamSema& p) { return p.kind == TemplateArgKind::type; }))
    return std::nullopt;
  const bool trailing_pack = !params.empty() && params.back().pack;
  const auto fixed = params.size() - (trailing_pack ? 1 : 0);
  if (args.size() < fixed || (!trailing_pack && args.size() != fixed) ||
      std::ranges::any_of(args.first(fixed), [&](const TemplateArg& a) { return t.is_expansion(a); }))
    return std::nullopt;
  if (types().is_class(fn.inner)) {
    // Ordinary alias replay checks the selected specialization even if the alias erases the call.
    // Check the result profile before a completeness query can instantiate a class template.
    const EntityId result = types().class_of(fn.inner);
    if (!allow_class_result || found->second.size() != 1 ||
        params.size() != 2 || params.front().pack || params.back().pack ||
        !t.structural_class_parameter(fn.inner) || !q_->is_complete(result) || q_->class_layout_uncertain(result))
      return std::nullopt;
  } else if (!types().is_integral(fn.inner) && !types().is_enum(fn.inner)) {
    return std::nullopt;
  }
  return dependent_call_value(selected->entity, fn.inner, args, callee.begin, end);
}

auto Sema::dependent_immediate_call(Ex callee, std::span<const Ex> args, u32 end, bool template_default) -> std::optional<Ex> {
  auto reference = as<ast::DeclRef>(callee);
  if (!reference || !any_dependent(args)) return std::nullopt;
  std::vector<TemplateArg> operands;
  operands.reserve(args.size());
  for (Ex arg : args) {
    if (!arg.ok() || !types().is_integral(arg.type())) return std::nullopt;
    while (auto paren = as<ast::Paren>(arg)) arg.id = paren->operand.id;
    auto value = as<ast::Literal>(arg);
    if (!value || (!ins().values.is<ast::VInt>(value->value) && !ins().values.is<ast::VDepParam>(value->value)))
      return std::nullopt;
    if (ins().values.is<ast::VDepParam>(value->value) &&
        ins().values.get<ast::VDepParam>(value->value).depth == dependent_depth)
      return std::nullopt;
    operands.push_back(TemplateArg{TemplateArgKind::value, types().canonical(Types::unqualified(arg.type())), value->value});
  }
  EntityId function = reference->entity;
  QualType type = callee.type();
  if (template_default && types().is(type, Builtin::overload_set)) {
    // The operands are value-dependent but have fixed integral types ([temp.dep.general]).
    // Rank the ordinary candidates here and keep the selected declaration through substitution.
    const auto& declarations = overloads_.at(std::to_underlying(callee.id));
    if (std::ranges::any_of(declarations, [](const ast::DeclSema& d) { return d.flags & decl_flag::template_; }))
      return std::nullopt;
    std::vector<Candidate> candidates;
    candidates.reserve(declarations.size());
    for (const ast::DeclSema& declaration : declarations)
      candidates.push_back(non_template_call_candidate(declaration, false));
    auto selected = resolve(candidates, args, callee.begin, w_->entities.rec(function).name, std::nullopt);
    if (!selected) return Ex{NodeId::none, callee.begin, end};
    const auto& declaration = declarations[*selected];
    if (!(declaration.flags & decl_flag::consteval_)) return std::nullopt;
    function = declaration.entity;
    type = declaration.type;
  } else {
    if (!types().is_function(type) || !is_consteval(function)) return std::nullopt;
    if (deleted_.contains(std::to_underlying(function))) {
      if (!template_default) return std::nullopt;
      diag(callee.begin, Diag::deleted_function, w_->entities.rec(function).name);
      return Ex{NodeId::none, callee.begin, end};
    }
  }
  const auto& entity = w_->entities.rec(function);
  const auto fn = types().rec(type);
  if (!types().is_integral(fn.inner) && (template_default || !types().is_enum(fn.inner))) return std::nullopt;
  if (template_default) {
    if (!namespace_scoped(function)) return std::nullopt;
  } else if (!unqualified_name(callee) || entity.linkage != ast::Linkage::external || entity.lang != LangLinkage::cxx) {
    return std::nullopt;
  }
  if (fn.variadic || fn.params.size() != args.size() ||
      !std::ranges::all_of(fn.params, [&](QualType parameter) { return types().is_integral(parameter); }))
    return std::nullopt;
  return dependent_call_value(function, fn.inner, operands, callee.begin, end);
}

auto Sema::evaluate_dependent_call(EntityId function, std::span<const TemplateArg> args, u32 at, bool diagnose) -> std::optional<TemplateArg> {
  auto decl = q_->replay_function_decl(function);
  if (!decl || !(decl->flags & decl_flag::consteval_)) return std::nullopt;
  Ex callee = decls_expr({*decl}, false, w_->entities.rec(function).name, at, at);
  std::vector<Ex> operands;
  if (decl->flags & decl_flag::template_) {
    explicit_template_args(callee, std::vector<TemplateArg>(args.begin(), args.end()));
  } else {
    operands.reserve(args.size());
    for (const TemplateArg& arg : args)
      operands.push_back(literal(arg.type, arg.value, ValueCategory::prvalue, at, at));
  }
  const auto before = diag_count();
  Ex invocation = call(callee, std::move(operands), at, at);
  if (!invocation.ok()) {
    // The synthesized call is in the immediate context of template substitution.
    if (!diagnose) drop_failures(before);
    return std::nullopt;
  }
  auto evaluation = q_->constant_call(*this, invocation);
  if (!evaluation.value) {
    if (diagnose && !evaluation.diagnosed) diag(at, Diag::not_constant, evaluation.reason);
    return std::nullopt;
  }
  return TemplateArg{TemplateArgKind::value, invocation.type(), *evaluation.value};
}

auto Sema::captured(u32 ctx, u32 key, NameId name, u32 begin, u32 end) -> Ex {
  const ast::LocalRec* local = nullptr;
  if (!(key & capture_bit)) local = ctx == lambdas_.size() ? &locals_[key] : &lambdas_[ctx].outer_locals[key];
  if (local && types().has_placeholder(local->type)) {
    diag(begin, Diag::auto_used_in_initializer, name);
    return {NodeId::none, begin, end};
  }
  if (ctx == lambdas_.size()) return source_expr(ctx, key, begin, end);
  // A function parameter pack of a template's definition: its uses are expanded, and captured,
  // in each instantiation ([temp.variadic], [expr.prim.lambda.capture]/17).
  if (local && types().kind(local->type) == TypeKind::pack_expansion) {
    if (pattern_packs_.empty()) {
      diag(begin, Diag::unexpanded_pack, name);
      return {NodeId::none, begin, end};
    }
    note_pack({0, 0, name});
    return dependent_node({}, begin, end);
  }
  // A static local is no local entity, so it is named, never captured
  // ([expr.prim.lambda.capture]/7, [basic.pre]/7).
  if (local && local->entity != EntityId::none) {
    ExprInfo info{types().referee(local->type), ValueCategory::lvalue, 0};
    return node(ast::DeclRef{info, local->entity}, begin, end, info);
  }
  // In an unevaluated operand naming it is no odr-use: no capture ([expr.prim.lambda.capture]/8),
  // only its type, and for `decltype` its declared type.
  if (unevaluated > 0) {
    QualType declared = local ? local_declared_type(*local) : lambdas_[ctx - 1].fields[key & ~capture_bit].type;
    ExprInfo info{types().referee(declared), ValueCategory::lvalue, 0};
    Ex e = node(ast::Dependent{info, b_->list(std::vector<Child>{})}, begin, end, info);
    declared_types_[std::to_underlying(e.id)] = declared;
    return e;
  }
  // A constant the innermost lambda cannot capture (no capture-default, not captured
  // explicitly) is named for its value, which is no odr-use ([basic.def.odr]/5); one it can is
  // captured, so that it is an lvalue where one is needed.
  if (!(key & capture_bit)) {
    const LambdaFrame& owner = lambdas_[ctx];
    const ast::LocalRec& l = owner.outer_locals[key];
    const LambdaFrame& inner = lambdas_.back();
    bool capturable = inner.default_capture != 0 ||
                      std::ranges::any_of(inner.captures, [&](const Capture& c) { return c.ctx == ctx && c.key == key; });
    if (!capturable && owner.outer_values[key] && !types().is_reference(l.type) && types().is_scalar(l.type))
      return literal(Types::unqualified(l.type), *owner.outer_values[key], ValueCategory::prvalue, begin, end);
  }
  std::optional<u32> field;
  for (u32 j = ctx; j < lambdas_.size(); ++j) {
    field = capture_in(j, ctx, key, field, name, begin);
    if (!field) return {NodeId::none, begin, end};
  }
  return field_expr(u32(lambdas_.size() - 1), *field, begin, end);
}

auto Sema::add_capture(LambdaFrame& f, u32 ctx, u32 key, NameId name, bool by_ref, Ex src) -> std::optional<u32> {
  // The entity's own type, not the type `src` has through an enclosing lambda's const operator:
  // a copy keeps the entity's cv-qualifiers ([expr.prim.lambda.capture]/10).
  QualType ent = source_type(ctx, key);
  QualType t = by_ref ? types().lvalue_ref(ent) : ent;
  // Array captures that need element construction or access checks are handled in #353
  // ([expr.prim.lambda.capture]/15).
  if (!by_ref && types().is_array(t) && (classes().copy_of(t).first || destructor_of(t) != EntityId::none ||
                                       classes().defaulted_copy_of(t) != CopyState::ordinary)) {
    diag(src.begin, Diag::not_supported, lex::Tk::l_square);
    return std::nullopt;
  }
  u32 field = u32(f.fields.size());
  EntityId e = w_->entities.entity({EntityKind::data_member, name, t, LangLinkage::cxx, ast::Linkage::none, {}, f.closure});
  f.fields.push_back({name, t, e, ConstId::none, false});
  if (!(key & capture_bit)) {
    const std::vector<std::optional<ConstId>>& values = ctx == lambdas_.size() ? local_values_ : lambdas_[ctx].outer_values;
    if (key < values.size() && values[key] && types().is_scalar(ent)) f.const_fields[field] = *values[key];
  }
  // An array is copied element by element ([expr.prim.lambda.capture]/15): its member from the
  // array's value, as a class object is copied.
  f.inits.push_back(by_ref                ? bind_reference(src, t, src.begin)
                    : types().is_array(t) ? convert_node(src, ast::ConversionKind::lvalue_to_rvalue, t)
                                          : convert(src, Types::unqualified(t), src.begin));
  f.captures.push_back({ctx, key, field});
  return field;
}

auto Sema::open_lambda(LambdaSpec spec) -> bool {
  // The enclosing entity: the function, the enclosing lambda's operator(), or what owns the
  // closures of the declaration being analysed (docs/design/closures.md, decision 3).
  EntityId scope = lambdas_.empty() ? current_function : lambdas_.back().call;
  const ClosureOwner* owner = scope == EntityId::none && closure_owner ? &*closure_owner : nullptr;
  // A lambda in an unevaluated operand outside every function body and every declaration that
  // owns its closures (a `decltype` in an alias-declaration or a function's declarator,
  // [expr.prim.lambda.general]/3): a closure type no definition can name elsewhere.
  std::optional<ClosureOwner> unevaluated_owner;
  if (scope == EntityId::none && !owner && unevaluated > 0) owner = &unevaluated_owner.emplace(unevaluated_closure_owner());
  // [expr.prim.lambda.capture]/3: only a block scope or a default member initializer gives a
  // capture-default something to capture.
  if (owner && spec.default_capture) {
    diag(spec.begin, Diag::nonlocal_lambda_capture_default);
    return false;
  }
  if (owner) scope = owner->scope;
  if (scope == EntityId::none && !owner) {
    diag(spec.begin, Diag::not_supported, lex::Tk::l_square);  // a lambda outside a function body
    return false;
  }
  std::vector<QualType> ps;
  for (const LambdaParam& p : spec.params) ps.push_back(p.type);
  QualType declared = spec.trailing ? *spec.trailing : types().placeholder(false);
  QualType call_type = types().function(declared, ps, spec.variadic, LangLinkage::cxx,
                                        spec.mutable_ || spec.static_ ? Cv::none : Cv::const_,
                                        ast::RefQual::none, spec.nonthrowing);
  // The closure's identity: its enclosing entity, signature and ordinal among the closures of
  // that entity and signature completed before it (docs/design/closures.md, decision 2).
  const auto signature = [&](Fp128 discriminator) {
    Fp128 sig = ast::seed("ycxx.sema.closure");
    if (discriminator != Fp128{}) sig = ast::mix(sig, discriminator);
    for (QualType p : types().rec(call_type).params) sig = ast::mix(sig, types().fp(types().canonical(p)));
    sig = ast::mix(sig, u64(spec.variadic));
    // The <lambda-sig> records explicit template parameters in addition to the parameter types.
    for (const ast::TemplateParamSema& h : spec.head)
      if (h.name != invented_name()) {
        sig = ast::mix(sig, (u64(std::to_underlying(h.kind)) + 1) * 4 + u64(h.pack));
        if (h.type_constraint != ConstId::none) sig = ast::mix(sig, ins().values.fp(h.type_constraint));
      }
    if (spec.head_constraint.tokens != Fp128{}) sig = ast::mix(sig, spec.head_constraint.tokens);
    return sig;
  };
  const Fp128 abi_signature = signature({});
  const Fp128 sig = owner && owner->discriminator != Fp128{} ? signature(owner->discriminator) : abi_signature;
  u32 ordinal = 0;
  for (const auto& [sc, sg] : completed_closures_)
    if (sc == scope && sg == sig) ++ordinal;
  ast::Linkage linkage = owner ? owner->linkage : closure_linkage(scope);
  // Its record's type, which names it (the <lambda-sig>), has a generic lambda's parameters at
  // depth 0, as the innermost template's are mangled.
  EntityRec cr{EntityKind::class_, no_name(), rebased(call_type, spec.head), LangLinkage::cxx, linkage, {}, scope};
  cr.head = ast::mix(sig, u64(ordinal));
  // The symbol's ordinal is based on the written signature. The semantic record
  // also depends on the normalized constraints in this translation unit and mode.
  if (spec.constraints != ConstId::none) cr.head = ast::mix(cr.head, ins().values.fp(spec.constraints));
  for (const auto& clause : {spec.head_constraint, spec.trailing_constraint}) {
    if (clause.tokens != Fp128{}) cr.head = ast::mix(cr.head, clause.tokens);
    if (clause.expression != ConstId::none) cr.head = ast::mix(cr.head, ins().values.fp(clause.expression));
  }
  if (spec.static_) cr.head = ast::mix(cr.head, ast::seed("ycxx.sema.static-closure"));
  cr.ordinal = ordinal;
  cr.static_closure = spec.static_;
  // In a template's definition, its own template-head (pushed already) aside.
  cr.closure_pattern = closure_patterns ||
                       (declaration_part && (in_generic_definition() || (template_scopes.size() > (spec.head.empty() ? 0u : 1u) &&
                                                                         !instantiating() && !satisfying_atom)));
  for (const ast::TemplateParamSema& h : spec.head)
    if (h.name != invented_name()) cr.closure_head.push_back(h);
  const u16 depth = spec.head.empty() ? 0 : spec.head.front().depth;
  cr.lambda_head_constraint = spec.head_constraint;
  cr.lambda_trailing_constraint = spec.trailing_constraint;
  cr.lambda_constraint_depth = depth;
  cr.lambda_signature = abi_signature;
  LambdaFrame f;
  f.closure = w_->entities.entity(std::move(cr));
  EntityRec call{EntityKind::function, ins().names.intern("operator()"), call_type, LangLinkage::cxx, linkage, {}, f.closure};
  call.lambda_constraints = spec.constraints;
  call.lambda_trailing_constraint = spec.trailing_constraint;
  call.lambda_constraint_depth = depth;
  f.call = w_->entities.entity(std::move(call));
  f.call_type = call_type;
  if (spec.noexcept_dependent) dependent_noexcept_calls_.insert(std::to_underlying(f.call));
  f.sig = sig;
  f.scope = scope;
  f.begin = spec.begin;
  f.default_capture = spec.default_capture;
  f.has_capture = spec.has_capture;
  f.mutable_ = spec.mutable_;
  f.static_ = spec.static_;
  f.ordinal = ordinal;
  f.head = spec.head;
  f.diags_at = diag_count();
  QualType ct = types().class_type(f.closure);
  f.this_type = types().pointer(spec.mutable_ ? ct : types().add_cv(ct, Cv::const_));
  // [expr.prim.lambda.capture]/5: nor a template parameter's.
  for (const CaptureSpec& c : spec.captures)
    if (c.name != no_name() && std::ranges::any_of(spec.head, [&](const ast::TemplateParamSema& h) { return h.name == c.name; })) {
      diag(c.at, Diag::redeclared_local, c.name);
      return false;
    }
  // [expr.prim.lambda.capture]/5: a simple capture's name is not a parameter's.
  for (const LambdaParam& p : spec.params)
    if (p.name != no_name() && std::ranges::any_of(spec.captures, [&](const CaptureSpec& c) { return c.name == p.name; })) {
      diag(p.at, Diag::redeclared_local, p.name);
      return false;
    }
  for (CaptureSpec& c : spec.captures) {
    using K = CaptureSpec::Kind;
    if (c.pack) {
      // A pack capture: each element of an instantiation's function parameter pack; in a
      // template's definition, nothing yet.
      auto it = std::ranges::find(param_packs_, c.name, &ParamPack::name);
      if (it == param_packs_.end()) {
        auto src = capture_source(c.name);
        if (!src || src->second & capture_bit || types().kind(source_type(src->first, src->second)) != TypeKind::pack_expansion) {
          diag(c.at, Diag::cannot_capture, c.name);
          return false;
        }
        continue;
      }
      for (u32 l : it->locals) {
        Ex src = captured(it->ctx, l, c.name, c.at, c.at + 1);
        if (!src.ok() || !add_capture(f, it->ctx, l, c.name, c.kind == K::ref, src)) return false;
      }
      continue;
    }
    if (!c.init.ok() && c.kind != K::this_ && c.kind != K::star_this) return false;
    bool twice = std::ranges::any_of(f.fields, [&](const FieldRec& x) { return x.name == c.name && c.name != no_name(); });
    if (twice || ((c.kind == K::this_ || c.kind == K::star_this) && f.this_field)) {
      diag(c.at, Diag::capture_twice, c.name);
      return false;
    }
    switch (c.kind) {
    case K::copy:
    case K::ref:
      // A variable with static storage duration is not captured ([expr.prim.lambda.capture]/5).
      if (!(c.source->second & capture_bit) &&
          (c.source->first == lambdas_.size() ? locals_[c.source->second] : lambdas_[c.source->first].outer_locals[c.source->second])
                  .entity != EntityId::none) {
        diag(c.at, Diag::cannot_capture, c.name);
        return false;
      }
      if (!add_capture(f, c.source->first, c.source->second, c.name, c.kind == K::ref, c.init)) return false;
      break;
    case K::this_:
    case K::star_this: {
      Ex t = this_expr(c.at, c.at);
      if (!t.ok()) return false;
      add_this(f, t, c.kind == K::star_this);
      break;
    }
    case K::init_copy:
    case K::init_ref: {
      // As `auto name = init;` or `auto& name = init;` ([expr.prim.lambda.capture]/6).
      QualType p = types().placeholder(false);
      if (c.kind == K::init_ref) p = types().lvalue_ref(p);
      auto t = deduce_placeholder(p, c.init, c.init.begin);
      if (!t) return false;
      u32 field = u32(f.fields.size());
      EntityId e = w_->entities.entity({EntityKind::data_member, c.name, *t, LangLinkage::cxx, ast::Linkage::none, {}, f.closure});
      f.fields.push_back({c.name, *t, e, ConstId::none, false});
      f.inits.push_back(types().is_reference(*t) ? bind_reference(c.init, *t, c.init.begin) : convert(c.init, *t, c.init.begin));
      f.init_captures.push_back({c.name, field});
      break;
    }
    }
  }
  // The enclosing context's state, until the body ends.
  f.outer_locals = std::exchange(locals_, {});
  f.outer_values = std::exchange(local_values_, {});
  f.return_type = return_type_;
  f.declared_return = declared_return_;
  f.return_deduced = return_deduced_;
  f.return_seen = return_seen_;
  f.no_return_deduction = std::exchange(no_return_deduction, 0);
  f.loop_depth = std::exchange(loop_depth, 0);
  set_return_type(declared);
  f.scope_base = scopes_.size();
  lambdas_.push_back(std::move(f));
  push_scope();
  guard_scope();
  for (const LambdaParam& p : spec.params) declare_local(p.name, p.type, local_flag::parameter, p.at, false);
  for (const auto& [n, field] : lambdas_.back().init_captures) scopes_.back().push_back({n, capture_bit | field});
  return true;
}

auto Sema::close_lambda(Ex body, u32 body_begin, u32 end) -> Ex {
  LambdaFrame& f = lambdas_.back();
  // No return statement deduces `void` ([dcl.type.auto.deduct]/2), unless a template's
  // definition left the type to its instantiations.
  const bool generic = !f.head.empty();
  if (body.ok() && deduces_return() && !return_deduced_ && !return_seen_ && !in_pattern())
    if (!deduce_return(nullptr, end > 0 ? end - 1 : 0)) body = {};
  // A generic lambda's operator keeps its declared type: each specialization deduces its own.
  QualType call_type = f.call_type;
  if (generic) {
  } else if (return_deduced_) {
    call_type = with_return(f.call_type, return_type_);
  } else if (deduces_return()) {
    call_type = with_return(f.call_type, builtin(Builtin::dependent));
  }
  std::vector<ast::LocalRec> locals = std::move(locals_);
  std::size_t nparams = types().rec(call_type).params.size();
  pop_scope();
  LambdaFrame fr = std::move(lambdas_.back());
  lambdas_.pop_back();
  locals_ = std::move(fr.outer_locals);
  local_values_ = std::move(fr.outer_values);
  return_type_ = fr.return_type;
  declared_return_ = fr.declared_return;
  return_deduced_ = fr.return_deduced;
  return_seen_ = fr.return_seen;
  no_return_deduction = fr.no_return_deduction;
  loop_depth = fr.loop_depth;
  completed_closures_.push_back({fr.scope, fr.sig});
  closure_calls_[std::to_underlying(fr.call)] = call_type;
  const auto signature = types().rec(call_type);
  if (!body.ok() || !(fr.polymorphic_result ? require_supported_virtual_parameters(signature, fr.begin)
                                            : require_supported_virtual_signature(signature, fr.begin)))
    return {NodeId::none, fr.begin, end};
  // The closure type ([expr.prim.lambda.closure]): its captures' fields, its function call
  // operator, and for a lambda without a lambda-capture the static invoker its conversion to a
  // pointer to function returns (/8–9).
  ClassRec rec;
  rec.defined = true;
  rec.closure = true;
  rec.default_deleted = fr.has_capture;
  rec.fields = fr.fields;
  for (FieldRec& fd : rec.fields) fd.name = no_name();  // named by its lambda's body only
  rec.members.push_back({ins().names.intern("operator()"), fr.call, call_type, true, fr.static_,
                         false, false, false, generic, fr.head, {}});
  // A generic lambda forms each invoker after a pointer type supplies its template arguments.
  bool captureless = !fr.has_capture && !generic && !types().is_dependent(call_type);
  EntityId invoker = EntityId::none;
  NodeId invoker_body = NodeId::none;
  if (captureless && !fr.static_) {
    TypeRec cr = types().rec(call_type);
    // A pointer to noexcept function when the operator is non-throwing (/8).
    QualType it = types().function(cr.inner, cr.params, cr.variadic, LangLinkage::cxx, Cv::none, ast::RefQual::none,
                                   cr.fn_noexcept);
    bool simple = std::ranges::none_of(cr.params, [&](QualType p) { return types().is_class(p); }) && !cr.variadic;
    if (simple) {
      invoker = w_->entities.entity({EntityKind::function, ins().names.intern("__invoke"), it, LangLinkage::cxx,
                                     w_->entities.rec(fr.closure).linkage, {}, fr.closure});
      rec.members.push_back({ins().names.intern("__invoke"), invoker, it, true, true, false, false, false, false, {}, {}});
      invoker_body = invoker_tree(fr.closure, fr.call, call_type, body_begin);
    }
  }
  q_->publish_class(fr.closure, std::move(rec), *this);
  u16 flags = u16((fr.mutable_ ? closure_flag::mutable_ : 0) | (captureless ? closure_flag::captureless : 0) |
                  (fr.static_ ? closure_flag::static_call : 0) |
                  (generic || in_pattern() ? closure_flag::pattern : 0) |
                  (fr.polymorphic_result ? closure_flag::polymorphic_result : 0));
  // The enclosing function's constants a generic body names stand as their values when it is
  // instantiated, where that function's locals are gone.
  // The enclosing context's locals visible here, innermost first and one per name: constants
  // with their values, the others for unevaluated operands; and its type aliases.
  std::vector<ast::LocalRec> consts;
  std::vector<ConstId> values;
  std::vector<std::size_t> const_depths;
  std::vector<std::pair<NameId, QualType>> aliases;
  std::vector<std::size_t> alias_depths;
  std::vector<GenericClosure::CapturedUsing> usings;
  if (generic) {
    std::set<u32> seen;
    for (std::size_t d = scopes_.size(); d-- > 0 && context_of(d) == lambdas_.size();)
      for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p) {
        if ((p->second & capture_bit) || p->first == no_name() || !seen.insert(std::to_underlying(p->first)).second) continue;
        consts.push_back(locals_[p->second]);
        consts.back().decl = NodeId::none;
        values.push_back(p->second < local_values_.size() && local_values_[p->second] ? *local_values_[p->second] : ConstId::none);
        const_depths.push_back(d);
      }
    std::set<u32> seen_types;
    for (std::size_t d = scopes_.size(); d-- > 0;) {
      for (auto t = block_types_.rbegin(); t != block_types_.rend(); ++t)
        if (t->depth == d && seen_types.insert(std::to_underlying(t->name)).second) {
          aliases.push_back({t->name, t->type});
          alias_depths.push_back(d);
        }
      for (auto u = block_usings_.rbegin(); u != block_usings_.rend(); ++u)
        if (u->depth == d && u->type_index && seen_types.insert(std::to_underlying(u->name)).second)
          usings.push_back({d, u->name, u->decls, u->type_index});
    }
  }
  if (generic) {
    std::vector<NameId> names;
    for (std::size_t i = 0; i < nparams && i < locals.size(); ++i) names.push_back(locals[i].name);
    std::vector<NameId> captures;
    for (const FieldRec& fd : fr.fields) captures.push_back(fd.name);
    std::vector<Expansion> exps;
    // In an instantiation, they are those its definition recorded.
    const std::vector<Expansion>* recorded = declaration_part ? pattern_expansions() : nullptr;
    for (const Expansion& x : recorded ? *recorded : expansions)
      if (x.begin >= body_begin && x.begin < end) exps.push_back(x);
    std::vector<std::vector<ast::TemplateParamSema>> outer(template_scopes.begin(), template_scopes.end() - (template_scopes.empty() ? 0 : 1));
    generic_closures_[std::to_underlying(fr.call)] = {fr.closure, fr.call, call_type, fr.head, names, captures, body_begin, flags, consts,
                                                       values, const_depths, exps, outer, template_args, aliases, alias_depths, usings,
                                                       diag_count() > fr.diags_at};
    if (declaration_part)
      generic_closures_[std::to_underlying(fr.call)].part = GenericClosure::Part{item_, loc_base_, current_class};
    q_->generic_defined(generic_closures_[std::to_underlying(fr.call)]);
  }
  std::vector<NameId> capture_names;
  for (const FieldRec& fd : fr.fields) capture_names.push_back(fd.name);
  closure_locals.push_back(locals);
  closures.push_back({fr.closure, fr.call, call_type, body.id, b_->seq(std::span<const ast::LocalRec>(locals)), invoker,
                      EntityId::none, invoker_body, NodeId::none, fr.begin, body_begin, end, flags,
                      b_->seq(std::span<const ast::TemplateParamSema>(fr.head)), b_->seq(std::span<const ast::LocalRec>(consts)),
                      b_->seq(std::span<const ConstId>(values)), b_->seq(std::span<const NameId>(capture_names))});
  std::vector<Child> inits;
  for (std::size_t i = 0; i < fr.inits.size(); ++i) {
    const Ex& x = fr.inits[i];
    if (!x.ok()) return {NodeId::none, fr.begin, end};
    Ex mi = node(ast::MemberInit{u32(i), fr.fields[i].type, at(x, x.begin)}, x.begin, x.end);
    inits.push_back(at(mi, fr.begin));
  }
  ExprInfo info{types().class_type(fr.closure), ValueCategory::prvalue, 0};
  return node(ast::Lambda{info, fr.closure, b_->list(inits)}, fr.begin, end, info);
}

auto Sema::using_enum_type(NameId name) -> std::optional<QualType> {
  if (compatibility() != ast::Compatibility::gcc) return class_name(name, /*elaborated=*/true);
  // GCC rejects `using enum E;` when a non-type declaration hides E (CWG 2877),
  // while still rejecting an unshadowed template parameter `using enum T;`.
  bool shadowed =
      std::ranges::any_of(block_types_, [&](const BlockType& t) { return t.depth < scopes_.size() && t.name == name; }) ||
      std::ranges::any_of(block_usings_, [&](const BlockUsing& u) { return u.depth < scopes_.size() && u.name == name; });
  if (!shadowed && outer()) {
    EntityId last = outermost_class(current_class);
    for (const OuterBlock* b = outer(); b && is_local_class(last) && !shadowed; b = b->outer.get()) {
      bool hidden = false;
      shadowed = outer_block_type(*b, name, /*elaborated=*/false, /*aliases=*/true, hidden).has_value() || hidden;
      last = b->cls;
    }
  }
  if (!shadowed && template_param(name)) return std::nullopt;
  return class_name(name, /*elaborated=*/false);
}

}  // namespace ycxx::sema
