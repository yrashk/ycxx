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

auto Sema::invoker_tree(EntityId cls, EntityId member, QualType call_type, u32 at_) -> NodeId {
  TypeRec cr = types().rec(call_type);
  QualType ct = types().class_type(cls);
  ExprInfo li{ct, ValueCategory::prvalue, 0};
  Ex closure = node(ast::Lambda{li, cls, b_->list(std::vector<Child>{})}, at_, at_, li);
  Ex object = materialize(closure, ct, false);
  ExprInfo pi{types().pointer(ct), ValueCategory::prvalue, 0};
  Ex self = node(ast::Unary{pi, ast::UnaryOp::address_of, at(object, at_)}, at_, at_, pi);
  std::vector<Child> args;
  for (std::size_t i = 0; i < cr.params.size(); ++i) {
    QualType p = cr.params[i];
    ExprInfo ai{types().referee(p), ValueCategory::lvalue, 0};
    Ex a = node(ast::LocalRef{ai, u32(i)}, at_, at_, ai);
    if (!types().is_reference(p)) a = rvalue(a);
    args.push_back(at(a, at_));
  }
  ExprInfo ri = result_info(cr.inner);
  Ex call = node(ast::MemberCall{ri, member, at(self, at_), b_->list(args)}, at_, at_, ri);
  if (types().is_void(cr.inner)) {
    Ex st = node(ast::ExprStmt{at(call, at_)}, at_, at_);
    Ex ret = node(ast::Return{at(Ex{}, at_)}, at_, at_);
    return node(ast::Compound{b_->list(std::vector<Child>{at(st, at_), at(ret, at_)})}, at_, at_).id;
  }
  Ex ret = node(ast::Return{at(call, at_)}, at_, at_);
  return node(ast::Compound{b_->list(std::vector<Child>{at(ret, at_)})}, at_, at_).id;
}

auto Sema::generic_call(const MemberDeclRec& m, std::span<const TemplateArg> args, u32 at_, bool demand_definition) -> std::optional<GenericCall> {
  ast::DeclSema tmpl{};
  tmpl.entity = m.entity;
  tmpl.type = m.type;
  tmpl.kind = ast::DeclKind::function;
  tmpl.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(m.head));
  auto st = templates().specialization_type(tmpl, args);
  if (!st) return std::nullopt;
  EntityId spec = specialization(tmpl, args, *st);
  // In a template's definition, its instantiation decides ([temp.dep.expr]/3).
  if (in_pattern()) return GenericCall{spec, with_return(*st, builtin(Builtin::dependent))};
  // In its own body, once a return statement deduced its type ([dcl.spec.auto.general]/11).
  if (!lambdas_.empty() && lambdas_.back().call == spec && return_deduced_) return GenericCall{spec, with_return(*st, return_type_)};
  const GenericClosure* known = nullptr;
  if (auto g = generic_closures_.find(std::to_underlying(m.entity)); g != generic_closures_.end()) known = &g->second;
  if (known && known->erroneous) return std::nullopt;  // its definition's errors say it
  // Its definition is required ([temp.inst]/8): emitted, and what it requires with it.
  if (demand_definition) demands.push_back({spec, ast::Loc{item_, loc_base_ + at_}});
  GenericBinding binding{template_args, current_function};
  GenericResult t = q_->generic_instance(spec, known, binding);
  if (!t.type) {
    if (!t.diagnosed) diag(at_, Diag::auto_undeduced, w_->entities.rec(m.entity).name);
    return std::nullopt;
  }
  return GenericCall{spec, *t.type};
}

auto Sema::open_generic(const GenericClosure& g, EntityId spec, QualType spec_type, std::span<const TemplateArg> args) -> void {
  LambdaFrame f;
  // The template-heads around its definition, with their arguments, then its own.
  f.outer_scopes = std::exchange(template_scopes, g.scopes);
  f.outer_args = std::exchange(template_args, g.args);
  template_scopes.push_back(g.head);
  u16 depth = g.head.empty() ? 0 : g.head[0].depth;
  if (template_args.levels.size() <= depth) template_args.levels.resize(depth + 1);
  template_args.levels[depth].assign(args.begin(), args.end());
  f.closure = g.closure;
  f.call = spec;
  f.call_type = spec_type;
  f.scope = spec;
  f.mutable_ = (g.flags & closure_flag::mutable_) != 0;
  f.static_ = (g.flags & closure_flag::static_call) != 0;
  QualType ct = types().class_type(g.closure);
  f.this_type = types().pointer(f.mutable_ ? ct : types().add_cv(ct, Cv::const_));
  if (const ClassRec* r = class_rec(g.closure)) f.fields = r->fields;
  for (u32 i = 0; i < f.fields.size() && i < g.captures.size(); ++i) f.fields[i].name = g.captures[i];
  for (u32 i = 0; i < f.fields.size(); ++i)
    if (f.fields[i].name == ins().names.intern("this")) {
      f.this_field = i;
      f.star_this = !types().is_pointer(f.fields[i].type);
    }
  f.outer_locals = std::exchange(locals_, {});
  f.outer_values = std::exchange(local_values_, {});
  f.return_type = return_type_;
  f.declared_return = declared_return_;
  f.return_deduced = return_deduced_;
  f.return_seen = return_seen_;
  f.no_return_deduction = std::exchange(no_return_deduction, 0);
  f.loop_depth = std::exchange(loop_depth, 0);
  set_return_type(types().rec(spec_type).inner);
  f.scope_base = scopes_.size();
  lambdas_.push_back(std::move(f));
  push_scope();
  guard_scope();
  // The parameters: a pack's elements are its specialization's parameters at its place
  // ([temp.variadic]/8).
  TypeRec r = types().rec(spec_type);
  TypeRec pat = types().rec(g.call_type);
  std::size_t elems = r.params.size() + 1 >= pat.params.size() ? r.params.size() + 1 - pat.params.size() : 0;
  for (std::size_t i = 0, k = 0; i < pat.params.size(); ++i) {
    NameId n = i < g.params.size() ? g.params[i] : no_name();
    if (types().kind(pat.params[i]) == TypeKind::pack_expansion) {
      declare_param_pack(n);
      for (std::size_t e = 0; e < elems && k < r.params.size(); ++e) declare_pack_element(n, r.params[k++]);
      continue;
    }
    if (k < r.params.size()) declare_local(n, r.params[k++], local_flag::parameter, 0, false);
  }
  const LambdaFrame& fr = lambdas_.back();
  for (u32 i = 0; i < fr.fields.size(); ++i)
    if (fr.fields[i].name != no_name() && (!fr.this_field || *fr.this_field != i)) scopes_.back().push_back({fr.fields[i].name, capture_bit | i});
}

auto Sema::generic_invoker(QualType from, QualType to, u32 at_, bool demand_definition) -> std::optional<GenericInvoker> {
  if (!types().is_class(from) || !types().is_pointer(to) || !types().is_function(types().pointee(to))) return std::nullopt;
  const EntityId closure = types().class_of(from);
  const ClassRec* record = class_rec(closure);
  if (!record || !record->closure || record->default_deleted) return std::nullopt;
  auto member = std::ranges::find_if(record->members, [&](const MemberDeclRec& m) {
    return m.is_template && m.name == ins().names.intern("operator()");
  });
  if (member == record->members.end()) return std::nullopt;
  const MemberDeclRec call_member = *member;
  ast::DeclSema parameter_pattern{};
  parameter_pattern.entity = call_member.entity;
  parameter_pattern.type = invoker_type(call_member.type);
  parameter_pattern.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(call_member.head));
  parameter_pattern.constraints = w_->entities.rec(call_member.entity).lambda_constraints;
  const QualType target = types().pointee(to);
  // A nonthrowing invoker can also convert to a potentially throwing pointer.
  QualType deduction_target = types().rec(parameter_pattern.type).fn_noexcept ? with_noexcept(target) : target;
  auto args = templates().deduce_declaration(parameter_pattern, deduction_target, {}, /*address=*/true);
  if (!args || !constraints_satisfied(parameter_pattern, *args)) return std::nullopt;
  auto call = generic_call(call_member, *args, at_, demand_definition);
  if (!call) return std::nullopt;
  const QualType type = invoker_type(call->type);
  if (types().canonical(type) != types().canonical(target) && !types().function_pointer_conversion(type, target))
    return std::nullopt;
  return GenericInvoker{closure, *call, type, call_member.head, std::move(*args), call_member.is_static};
}

auto Sema::define_generic_invoker(const GenericInvoker& selected, u32 at_) -> EntityId {
  if (selected.static_call) return selected.call.spec;
  const auto closure = w_->entities.rec(selected.closure);
  const NameId name = ins().names.intern("__invoke");
  EntityRec declaration{EntityKind::function_template, name, invoker_type(closure.type), LangLinkage::cxx,
                        closure.linkage, {}, selected.closure};
  declaration.generic_invoker_head = invoker_head(selected.head);
  ast::DeclSema pattern{};
  pattern.entity = w_->entities.entity(std::move(declaration));
  const EntityId invoker = specialization(pattern, selected.args, selected.type);
  if (unevaluated || std::ranges::contains(closures, invoker, &ast::ClosureDef::invoker)) return invoker;

  std::vector<ast::LocalRec> locals;
  for (QualType parameter : types().rec(selected.type).params)
    locals.push_back({no_name(), parameter, EntityId::none, NodeId::none, local_flag::parameter});
  closure_locals.push_back(locals);
  closures.push_back({.closure = selected.closure, .call = EntityId::none, .call_type = selected.call.type,
                      .body = NodeId::none, .locals = b_->seq(std::span<const ast::LocalRec>(locals)),
                      .invoker = invoker, .conversion = EntityId::none,
                      .invoker_body = invoker_tree(selected.closure, selected.call.spec, selected.call.type, at_),
                      .conversion_body = NodeId::none, .begin = at_, .body_begin = at_, .end = at_,
                      .flags = closure_flag::generic_invoker, .template_params = {},
                      .consts = b_->seq(std::span<const ast::LocalRec>{}),
                      .const_values = {}, .captures = {}});
  return invoker;
}

auto Sema::define_closure_conversion(ClosureTarget invoker, u32 at_, std::span<const ast::TemplateParamSema> head) -> EntityId {
  const auto target = w_->entities.rec(invoker.entity);
  const NameId name = ins().names.intern(lex::conversion_function_name);
  const QualType pointer = types().pointer(invoker.type);
  const QualType type = types().function(pointer, {}, false, LangLinkage::cxx, Cv::const_, ast::RefQual::none, true);
  EntityId conversion;
  if (target.templ == EntityId::none) {
    conversion = w_->entities.entity({EntityKind::function, name, type, LangLinkage::cxx, target.linkage, {}, target.scope});
  } else {
    const auto source = w_->entities.rec(target.templ);
    // The ABI conversion type encodes the original invoker's dependent result and parameters.
    const auto closure = w_->entities.rec(target.scope);
    const auto source_type = closure.static_closure ? invoker_type(closure.type) : source.type;
    EntityRec pattern{EntityKind::function_template, name, source_type, LangLinkage::cxx, target.linkage, {}, target.scope};
    pattern.generic_invoker_head = closure.static_closure ? invoker_head(head) : source.generic_invoker_head;
    pattern.lambda_trailing_constraint = closure.lambda_trailing_constraint;
    ast::DeclSema declaration{};
    declaration.entity = w_->entities.entity(std::move(pattern));
    conversion = specialization(declaration, target.args, type);
  }
  if (std::ranges::contains(closures, conversion, &ast::ClosureDef::conversion)) return conversion;
  const Ex result = invoker_address(invoker, at_, at_);
  const Ex statement = node(ast::Return{at(result, at_)}, at_, at_);
  const Ex body = node(ast::Compound{b_->list(std::array{at(statement, at_)})}, at_, at_);
  const auto empty_locals = b_->seq(std::span<const ast::LocalRec>{});
  closure_locals.emplace_back();
  closures.push_back({.closure = target.scope, .call = EntityId::none, .call_type = type,
                      .body = NodeId::none, .locals = empty_locals, .invoker = EntityId::none, .conversion = conversion,
                      .invoker_body = NodeId::none, .conversion_body = body.id, .begin = at_, .body_begin = at_, .end = at_,
                      .flags = closure_flag::generated_conversion, .template_params = {}, .consts = empty_locals,
                      .const_values = {}, .captures = {}});
  return conversion;
}

auto Sema::deduce_return(const Ex* e, u32 at_) -> std::optional<QualType> {
  std::optional<QualType> t;
  QualType replaced{};
  if (!e) {
    // [dcl.type.auto.deduct]/2.2: as from a void operand, for `auto` or `decltype(auto)` alone.
    if (types().kind(declared_return_) != TypeKind::placeholder) {
      diag(at_, Diag::auto_deduction_failed, declared_return_, builtin(Builtin::void_));
      return std::nullopt;
    }
    t = types().add_cv(builtin(Builtin::void_), Types::cv(declared_return_));
  } else {
    t = deduce_placeholder(declared_return_, *e, at_, &replaced);
  }
  // In a discarded statement, or a branch of a dependent `if constexpr`, the operand is checked
  // against what it alone deduces.
  if (no_return_deduction > 0) return !t || types().is_dependent(*t) || !return_deduced_ ? t : std::optional(return_type_);
  return_seen_ = true;
  if (!t || types().is_dependent(*t)) return t;
  if (return_deduced_ && types().canonical(*t) != types().canonical(return_type_)) {
    diag(at_, Diag::auto_inconsistent, declared_return_, *t, return_type_);
    return std::nullopt;
  }
  return_deduced_ = true;
  return_type_ = *t;
  return t;
}

auto Sema::immediate_invocation(Ex call) -> Ex {
  if (!call.ok() || immediate_context || unevaluated || !evaluate_immediate ||
      (escalated_immediate && lambdas_.empty())) return call;
  if (const auto unresolved = unresolved_default_source(call)) {
    diag(call.begin, *unresolved);
    return {NodeId::none, call.begin, call.end};
  }
  const Evaluation v = evaluate_immediate(call);
  if (!v.constant) {
    if (!v.function_instantiation_failure && escalate_immediate()) return call;
    if (v.function_instantiation_failure) ++diagnosed_constant_calls;
    else diag(call.begin, Diag::not_constant, v.reason);
    return {NodeId::none, call.begin, call.end};
  }
  if (!v.value) {
    // A void function: the invocation is replaced by its (empty) result, `(void)0`, so the
    // consteval function's body never runs at run time.
    QualType i = builtin(Builtin::int_);
    Ex zero = literal(i, w_->values.integer(i, 0), ValueCategory::prvalue, call.begin, call.end);
    return convert_node(zero, ast::ConversionKind::to_void, builtin(Builtin::void_));
  }
  // A manifestly constant-evaluated expression may consume an immediate pointer.
  // Its initializer checks the final value for an escape.
  if (auto entity = immediate_result_entity(*v.value)) {
    if (!constant_reference_context && !trial_initializer) {
      diag(call.begin, Diag::immediate_address, w_->entities.rec(*entity).name);
      return {NodeId::none, call.begin, call.end};
    }
    folded_immediate_result = *entity;
  }
  return literal(call.type(), *v.value, ValueCategory::prvalue, call.begin, call.end);
}

auto Sema::outer_block_here() -> OuterBlock {
  OuterBlock b;
  b.function = local_scope();
  b.cls = current_class;
  std::set<u32> seen_value;
  std::set<u32> seen_type;
  for (std::size_t d = scopes_.size(); d-- > 0;) {
    const u32 ctx = context_of(d);
    for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p) {
      if (p->first == no_name() || (p->second & capture_bit) || !seen_value.insert(std::to_underlying(p->first)).second) continue;
      OuterBlock::Name n;
      n.name = p->first;
      n.local = ctx == lambdas_.size() ? locals_[p->second] : lambdas_[ctx].outer_locals[p->second];
      n.local.decl = NodeId::none;
      const auto& values = ctx == lambdas_.size() ? local_values_ : lambdas_[ctx].outer_values;
      if (p->second < values.size()) n.value = values[p->second];
      b.names.push_back(std::move(n));
    }
    for (auto t = block_types_.rbegin(); t != block_types_.rend(); ++t)
      if (t->depth == d && seen_type.insert(std::to_underlying(t->name)).second) {
        seen_value.insert(std::to_underlying(t->name));
        b.names.push_back({OuterBlock::Name::Kind::type, t->name, {}, std::nullopt, t->type, {}, EntityId::none, true, false});
      }
    std::map<u32, std::size_t> depth_usings;
    for (const BlockUsing& u : block_usings_) {
      if (u.depth != d) continue;
      const u32 key = std::to_underlying(u.name);
      const bool has_value = std::ranges::any_of(u.decls, [](const ast::DeclSema& decl) { return !is_type(decl.kind); });
      if (auto it = depth_usings.find(key); it != depth_usings.end()) {
        OuterBlock::Name& existing = b.names[it->second];
        existing.decls.insert(existing.decls.end(), u.decls.begin(), u.decls.end());
        if (u.type_index && !existing.type) {
          const ast::DeclSema& decl = u.decls[*u.type_index];
          existing.type = decl.type;
          existing.type_alias = decl.kind == ast::DeclKind::type_alias;
          existing.type_template = (decl.flags & decl_flag::template_) != 0;
        }
        continue;
      }
      const bool keep_type = u.type_index && !seen_type.contains(key);
      const bool keep_value = (has_value || keep_type) && !seen_value.contains(key);
      if (!keep_type && !keep_value) continue;
      OuterBlock::Name n;
      n.kind = OuterBlock::Name::Kind::using_;
      n.name = u.name;
      n.decls = u.decls;
      if (u.type_index) {
        const ast::DeclSema& decl = u.decls[*u.type_index];
        n.type = decl.type;
        n.type_alias = decl.kind == ast::DeclKind::type_alias;
        n.type_template = (decl.flags & decl_flag::template_) != 0;
      }
      depth_usings.emplace(key, b.names.size());
      b.names.push_back(std::move(n));
    }
    for (const auto& [key, idx] : depth_usings) {
      if (b.names[idx].type) seen_type.insert(key);
      seen_value.insert(key);
    }
    for (const BlockAlias& a : block_aliases_)
      if (a.depth == d && seen_value.insert(std::to_underlying(a.name)).second)
        b.names.push_back({OuterBlock::Name::Kind::namespace_alias, a.name, {}, std::nullopt, std::nullopt, {}, a.ns});
    for (auto u = using_enums_.rbegin(); u != using_enums_.rend(); ++u)
      if (u->first == d) b.using_enums.push_back(u->second);
    for (const auto& [depth, ns] : block_directives_)
      if (depth == d) b.directives.push_back(ns);
  }
  b.template_scopes = template_scopes;
  b.template_args = template_args;
  b.templated = in_pattern();
  b.outer = outer() ? outer_block : nullptr;
  return b;
}

auto Sema::outer_block_expr(const OuterBlock& b, NameId name, u32 begin, u32 end, bool callee) -> std::optional<Ex> {
  for (const OuterBlock::Name& n : b.names) {
    if (n.name != name) continue;
    switch (n.kind) {
    case OuterBlock::Name::Kind::type:
      diag(begin, Diag::type_as_expression, name);
      return Ex{NodeId::none, begin, end};
    case OuterBlock::Name::Kind::namespace_alias:
      diag(begin, Diag::expected_expression);
      return Ex{NodeId::none, begin, end};
    case OuterBlock::Name::Kind::using_:
      return decls_expr(std::vector<ast::DeclSema>(n.decls), true, name, begin, end, callee, true);
    case OuterBlock::Name::Kind::local: break;
    }
    const ast::LocalRec& l = n.local;
    if (l.entity != EntityId::none) {  // a static or thread_local local ([basic.stc])
      ExprInfo info{types().referee(l.type), ValueCategory::lvalue, 0};
      return node(ast::DeclRef{info, l.entity}, begin, end, info);
    }
    if (unevaluated > 0) {
      QualType declared = local_declared_type(l);
      ExprInfo info{types().referee(declared), ValueCategory::lvalue, 0};
      Ex e = node(ast::Dependent{info, b_->list(std::vector<Child>{})}, begin, end, info);
      declared_types_[std::to_underlying(e.id)] = declared;
      return e;
    }
    if (n.value && !types().is_reference(l.type) && types().is_scalar(l.type))
      return literal(Types::unqualified(l.type), *n.value, ValueCategory::prvalue, begin, end);
    diag(begin, Diag::local_class_odr_use, name);
    return Ex{NodeId::none, begin, end};
  }
  for (EntityId e : b.using_enums)
    if (auto r = find_enumerator(e, name, begin, end)) return *r;
  return std::nullopt;
}

auto Sema::accessible(EntityId c, ast::Access a, EntityId naming) -> bool {
  if (a == ast::Access::public_ || a == ast::Access::none) return true;
  if (a == ast::Access::protected_ && naming != EntityId::none && naming != c && accessible(naming, ast::Access::private_))
    return true;
  const ClassRec* cr = class_rec(c);
  if (!cr) return true;
  auto befriended = [&](EntityId e) { return cr->befriends(e, w_->entities); };
  if (current_function != EntityId::none && befriended(current_function)) return true;
  for (EntityId k = current_class; k != EntityId::none; k = w_->entities.rec(k).scope) {
    const EntityRec& r = w_->entities.rec(k);
    // A local class has the access of the function it is in ([class.local]/1).
    if (r.kind == EntityKind::function || r.kind == EntityKind::function_template || r.kind == EntityKind::constructor ||
        r.kind == EntityKind::destructor) {
      if (befriended(k)) return true;
      continue;
    }
    if (r.kind != EntityKind::class_ && r.kind != EntityKind::class_template) break;
    if (k == c || befriended(k)) return true;
    int paths = 0;
    if (a == ast::Access::protected_) {
      if (base_path(k, c, paths)) return true;
      auto bases = k == current_class && !completing_bases.empty() ? completing_bases : q_->instantiating_base_records(k);
      if (bases.empty()) bases = class_prefix_bases(k);
      for (const BaseRec& base : bases)
        if (base.cls == c || base_path(base.cls, c, paths)) return true;
    }
  }
  return false;
}

auto Sema::direct_member(EntityId c, const ClassRec& cr, NameId name) -> std::optional<Found> {
  for (u32 i = 0; i < cr.fields.size(); ++i)
    if (cr.fields[i].name == name) return Found{&cr.fields[i], i, nullptr, c};
  for (const MemberDeclRec& m : cr.members) {
    if (m.name != name || m.is_ctor || m.is_dtor) continue;
    // The synthetic invoker remains available to the checked lambda conversion.
    if (cr.closure && m.is_static && m.is_function && ins().names.spelling(name) == "__invoke" &&
        classes().layout_requires_virtual_support(types().rec(m.type).inner)) continue;
    return Found{nullptr, 0, &m, c};
  }
  // A base member a using-declaration of `c` introduced is found as a member of `c` ([namespace.udecl]/1).
  for (const MemberDeclRec& m : cr.introduced)
    if (m.name == name) return Found{nullptr, 0, &m, c};
  for (const NestedType& nested : cr.types) {
    // A type alias does not introduce the enumerators of the enumeration it names.
    if (!types().is_enum(nested.type) || types().enum_of(nested.type) != nested.entity) continue;
    const EnumRec* e = enum_rec(nested.entity);
    if (!e || e->scoped) continue;
    for (const EnumeratorRec& value : e->enumerators)
      if (value.name == name) return Found{.owner = c, .nested_type = nested.entity, .enumerator = value.entity};
  }
  for (const NestedType& nested : cr.types)
    if (nested.name == name) return Found{.owner = c, .nested_type = nested.entity};
  for (const NestedType& alias : cr.alias_templates)
    if (alias.name == name) return Found{.owner = c, .nested_type = alias.entity};
  return std::nullopt;
}

auto Sema::qualified_class_prefix_value(EntityId cls, NameId name, u32 begin, u32 end) -> std::optional<Ex> {
  bool in_class = cls == current_class;
  for (const OwnDecl& d : own) {
    if (d.sema.kind != ast::DeclKind::enum_ || d.sema.entity == EntityId::none ||
        w_->entities.rec(d.sema.entity).scope != cls) continue;
    if (const EnumRec* e = q_->defining_enum(d.sema.entity)) {
      in_class = true;
      if (!e->scoped)
        if (auto value = find_enumerator(d.sema.entity, name, begin, end)) return value;
    }
  }
  if (!in_class) return std::nullopt;
  const auto static_data = [&](const ast::DeclSema& d) {
    return d.kind == ast::DeclKind::variable && (d.flags & decl_flag::static_) &&
           d.entity != EntityId::none && w_->entities.rec(d.entity).scope == cls;
  };
  for (const OwnDecl& d : own)
    if (d.name == name && static_data(d.sema)) return static_member(cls, d.sema.entity, d.sema.type, begin, end);
  if (auto declaration = q_->earlier_member(name)) {
    if (static_data(*declaration)) return static_member(cls, declaration->entity, declaration->type, begin, end);
    if (declaration->kind == ast::DeclKind::enumerator && declaration->entity != EntityId::none) {
      const EntityId enumeration = w_->entities.rec(declaration->entity).scope;
      if (w_->entities.rec(enumeration).scope == cls)
        return enumerator(declaration->entity, declaration->type, begin, end);
    }
  }
  return std::nullopt;
}

auto Sema::member_call(Ex callee, std::vector<Ex> args, u32 end, u32 paren_at, bool assignment_notation) -> Ex {
  u32 begin = callee.begin;
  Ex fail{NodeId::none, begin, end};
  auto m = *as<ast::Member>(callee);
  const Bound& bound = bound_.at(std::to_underlying(callee.id));
  // An enum-prefix binding has no receiver. Only its selected static candidate can be called.
  const bool objectless_prefix = !bound.prefix_candidates.empty();
  std::vector<const Braced*> lists;
  if (auto b = std::ranges::find_if(args, [&](const Ex& a) { return is_braced(a); }); b != args.end()) {
    if (!objectless_prefix) {
      diag(b->begin, Diag::not_supported, lex::Tk::l_brace);  // a braced list for a member function's parameter
      return fail;
    }
    for (const Ex& a : args) lists.push_back(braced_list(a));
  }
  if (!objectless_prefix && std::ranges::any_of(bound.cands, [&](const MemberDeclRec* candidate) {
        return candidate->is_virtual && types().rec(candidate->type).fn_ref != ast::RefQual::none;
      })) {
    diag(paren_at, Diag::virtual_ref_call);
    return fail;
  }
  std::optional<Ex> object = bound.object;
  // The object's cv-qualifiers, which a non-static member function must have ([class.this]).
  std::optional<Cv> object_cv;
  if (object) object_cv = Types::cv(m.arrow ? types().pointee(object->type()) : object->type());
  // The candidates: each member function, and of each member function template the
  // specialization deduction gives ([temp.over]/1, [temp.mem]); explicit template arguments name
  // only templates ([temp.arg.explicit]).
  auto ex = explicit_args_.find(std::to_underlying(callee.id));
  std::span<const TemplateArg> explicit_args;
  if (ex != explicit_args_.end()) explicit_args = ex->second;
  std::vector<ArgShape> shapes;
  if (lists.empty())
    for (const Ex& a : args) shapes.push_back(argument_shape(a));
  std::vector<Candidate> cands;
  std::vector<const MemberDeclRec*> recs;
  std::deque<ast::DeclSema> patterns;
  std::vector<std::vector<TemplateArg>> deduced;  // a generic lambda operator's arguments, by candidate
  const bool lvalue_assignment = object && (m.arrow || object->cat() == ValueCategory::lvalue) &&
                                 ins().names.spelling(bound.cands.front()->name) == "operator=";
  for (const MemberDeclRec* c : bound.cands) {
    if (lvalue_assignment && types().rec(c->type).fn_ref == ast::RefQual::rvalue) continue;
    if (!c->head.empty()) {
      // A generic lambda's function call operator, a template whose definition is its lambda's:
      // the specialization deduction gives ([temp.over]/1), instantiated by `generic_call`.
      ast::DeclSema& td = patterns.emplace_back();
      td.entity = c->entity;
      td.type = c->type;
      td.kind = ast::DeclKind::function;
      td.constraints = w_->entities.rec(c->entity).lambda_constraints;
      td.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(c->head));
      Templates t = templates();
      auto targs = t.deduce_call(td, shapes, explicit_args);
      if (!targs || !constraints_satisfied(td, *targs)) continue;
      auto st = t.specialization_type(td, *targs);
      if (!st) continue;
      cands.push_back({c->entity, *st, c->is_static, c->is_deleted});
      recs.push_back(c);
      deduced.push_back(std::move(*targs));
      continue;
    }
    if (!c->is_template) {
      if (ex != explicit_args_.end() || c->ineligible || !trailing_constraints_satisfied(c->constraints)) continue;
      // [over.match.funcs.general]/8: a defaulted move assignment operator defined as deleted is
      // no candidate.
      if (c->explicit_defaulted_assignment && types().rec(types().rec(c->type).params.front()).rvalue_ref &&
          (c->copy_assignment == CopyAssignment::deleted || c->copy_assignment == CopyAssignment::const_member)) continue;
      cands.push_back({c->entity, c->type, c->is_static, c->is_deleted, nullptr, c->defaults});
      cands.back().constraints = c->constraints;
      recs.push_back(c);
      deduced.emplace_back();
      continue;
    }
    auto tmpl = q_->template_decl(c->entity);
    if (!tmpl) continue;
    const ast::DeclSema& td = patterns.emplace_back(*tmpl);
    if (!lists.empty()) {
      // Keep the potential candidate until the other arguments are checked (#231).
      cands.push_back({c->entity, c->type, c->is_static, c->is_deleted, &td, c->defaults});
      recs.push_back(c);
      deduced.emplace_back();
      continue;
    }
    Templates t = templates_for_use(callee.begin);
    auto targs = t.deduce_call(td, shapes, explicit_args, c->defaults);
    if (!targs) continue;
    if (td.constraints != ConstId::none) {
      std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
      auto nf = t.substitute_constraint(td.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
      auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
      if (sat && !*sat) continue;
    }
    auto st = t.specialization_type(td, *targs);
    if (!st) continue;
    cands.push_back({specialization(td, *targs, *st), *st, c->is_static, c->is_deleted, &td, c->defaults});
    recs.push_back(c);
    deduced.emplace_back();
  }
  NameId name = bound.cands.front()->name;
  if (cands.empty()) {
    diag(begin, Diag::no_viable_function, name);
    return fail;
  }
  // [class.this]: a non-static member function needs the object's cv-qualifiers.
  if (object_cv && std::ranges::none_of(cands, [&](const Candidate& c) {
        return c.no_object || cv_within(*object_cv, types().rec(c.type).fn_cv);
      })) {
    diag(begin, Diag::const_member_call, name);
    return fail;
  }
  // A sole trivial implicit copy can bind to a temporary made by a converting constructor.
  // Prepare that conversion before ranking the reference binding. With competing assignment
  // candidates, their user-defined conversions need to be ranked together.
  if (assignment_notation && lvalue_assignment && args.size() == 1 && recs.size() == 1 &&
      recs.front()->copy_assignment == CopyAssignment::trivial) {
    const QualType parameter = types().rec(cands.front().type).params.front();
    const QualType target = types().referee(parameter);
    Ex& source = args.front();
    if (object_cv == Cv::none && !same_unqualified(source.type(), target) && !ics(source, parameter).viable) {
      const ClassRec* cls = class_rec(bound.cls);
      if (cls && cls->user_ctors && this->object_cv(target) == Cv::const_) {
        source = convert(source, Types::unqualified(target), source.begin);
        if (!source.ok()) return fail;
      }
    }
  }
  const bool object_rvalue = object && !m.arrow && object->cat() != ValueCategory::lvalue;
  auto chosen = resolve(cands, args, begin, name, object_cv, std::nullopt, lists, object_rvalue,
                        /*defer_no_viable=*/false, /*scalar_list_arguments=*/objectless_prefix);
  if (!chosen) return fail;
  const MemberDeclRec* d = recs[*chosen];
  // A base member a using-declaration introduced: overload resolution took the implicit object
  // parameter as the naming class's ([over.match.funcs.general]/4); the call's object is the
  // base subobject that declares it, `object_cls`.
  const bool introduced = introduced_member(bound.cls, d);
  const EntityId object_cls = introduced ? w_->entities.rec(d->entity).scope : bound.cls;
  const Candidate& k = cands[*chosen];
  if (!check_access(bound.cls, d->entity, d->name, bound.name_at, bound.naming)) return fail;
  if (objectless_prefix && !d->is_static) {
    diag(begin, Diag::member_without_object, d->name);
    return fail;
  }
  std::optional<u32> virtual_slot;
  const ClassRec* dynamic_owner = d->is_virtual ? class_rec(w_->entities.rec(d->entity).scope) : nullptr;
  if (dynamic_owner && !dynamic_owner->dynamic) dynamic_owner = nullptr;
  const bool immediate_virtual = d->is_virtual && d->is_consteval;
  // A pattern's own member: its specializations have the tables ([temp.dep.type]).
  const bool pattern_member = d->is_virtual && types().is_dependent(types().class_type(w_->entities.rec(d->entity).scope));
  if (unevaluated == 0 && !pattern_member) {
    if (immediate_virtual) {
      if (!dynamic_owner || !object || assignment_notation) {
        diag(paren_at, Diag::virtual_immediate_call);
        return fail;
      }
    } else if (dynamic_owner) {
      // Itanium C++ ABI §3.2.4: dispatch through the slot the declaration occupies in its own
      // class's table. A qualified name calls the named function directly ([class.virtual]/16).
      if (!object || assignment_notation) {
        diag(paren_at, Diag::virtual_call);
        return fail;
      }
      if (!bound.qualified) {
        virtual_slot = dynamic_owner->dynamic->slot_of(d->entity);
        if (!virtual_slot) {
          diag(paren_at, Diag::virtual_call);
          return fail;
        }
      }
    } else if (d->is_virtual) {
      const auto* record = virtual_consumer(object_cls);
      // The pure interface has a known call shape independently of its object layout.
      if (!record && d->is_pure && bound.explicit_receiver && !bound.qualified && bound.naming == bound.cls &&
          q_->peer_pure_dispatch(object_cls)) record = class_rec(object_cls);
      if (!record || !object || assignment_notation ||
          (!m.arrow && (object->cat() != ValueCategory::lvalue ||
                        (record->peer_virtual_layout == PeerVirtualLayout::primary_base && !peer_object_lvalue(*object))))) {
        diag(paren_at, Diag::virtual_call);
        return fail;
      }
      if (!bound.qualified) {
        const bool derived = record->peer_virtual_layout == PeerVirtualLayout::primary_base;
        const ClassRec* root = derived ? class_rec(record->bases.front().cls) : record;
        if (root) virtual_slot = native_virtual_root_slot(*root, derived ? d->overridden_virtual : d->entity);
        if (!virtual_slot) {
          diag(paren_at, Diag::virtual_call);
          return fail;
        }
      }
    } else if (!d->is_static && !require_supported_virtual_layout(types().class_type(object_cls), paren_at)) {
      return fail;
    }
  }
  const auto copy_assignment = d->copy_assignment;
  const bool representation_copy = copy_assignment == CopyAssignment::trivial && !d->trivial_call;
  if (copy_assignment != CopyAssignment::none) {
    QualType ct = types().class_type(object_cls);
    if (!usable_copy_assignment(copy_assignment, ct, paren_at)) return fail;
    if (copy_assignment == CopyAssignment::memberwise && !d->explicit_defaulted_assignment &&
        (!lvalue_assignment || args[0].cat() != ValueCategory::lvalue || !same_unqualified(args[0].type(), ct))) {
      diag(paren_at, Diag::unsupported_copy_assignment, ct);
      return fail;
    }
    if ((!representation_copy || !assignment_notation) &&
        !usable_copy_assignment(q_->require_copy_assignment(d->entity), ct, paren_at)) return fail;
  }
  EntityId fe = k.entity;
  std::optional<QualType> dt;
  if (!d->head.empty()) {
    auto g = generic_call(*d, deduced[*chosen], begin);
    if (!g) return fail;
    fe = g->spec;
    dt = g->type;
  } else {
    auto type = k.pattern ? q_->replay_member_exception(*this, *k.pattern, k.entity, k.type) : std::optional(k.type);
    if (!type) return fail;
    if (k.pattern) {
      if (*type != k.type) fe = specialization(*k.pattern, w_->entities.rec(k.entity).args, *type);
      // An unevaluated call needs the exception specification and return type ([temp.inst]).
      if (!unevaluated || types().has_placeholder(types().rec(*type).inner))
        demands.push_back({fe, ast::Loc{item_, loc_base_ + begin}});
    }
    dt = deduced_function(fe, *type, begin);
  }
  if (!dt) return fail;
  TypeRec fr = types().rec(*dt);
  if (unevaluated == 0 && d->head.empty() && closure_member(*w_, fe) &&
      !require_supported_virtual_layout(fr.inner, paren_at))
    return fail;
  const u32 invocation_at = bound.explicit_receiver ? bound.name_at : begin;
  auto children = call_args(fr, args, begin, paren_at, invocation_at, cands[*chosen].defaults,
                            DefaultArgumentOrigin::interned, fe);
  if (!children) return fail;
  ExprInfo info = result_info(fr.inner);
  if (d->is_static) {
    // The object expression is evaluated, then discarded ([expr.ref]).
    Ex call = node(ast::Call{info, fe, at(callee, begin), b_->list(*children)}, begin, end, info);
    // Lookup can supply a `this` pointer to rank a mixed unqualified or qualified overload
    // set. Selecting a static member evaluates a receiver only when the call has one.
    if (!object || (m.arrow && !bound.explicit_receiver && ordinary_immediate_member(fe, fr.inner)))
      return call;
    Ex receiver = types().is_class(fr.inner) && ordinary_immediate_member(fe, fr.inner) ? discarded(*object) : *object;
    if (!receiver.ok()) return fail;
    Ex effects = convert_node(receiver, ast::ConversionKind::to_void, builtin(Builtin::void_));
    return node(ast::OrderedBinary{info, ast::OrderedOp::comma, at(effects, begin), at(call, begin)}, begin, end, info);
  }
  if (!object) {
    diag(begin, Diag::member_without_object, d->name);
    return fail;
  }
  Ex o = *object;
  // The using-declaration's access governs ([namespace.udecl]/16), not the base's.
  if (introduced) {
    if (!m.arrow && !o.glvalue()) o = materialize(o, o.type(), false);
    o = to_base(o, object_cls, paren_at, false, /*check_access=*/false);
    if (!o.ok()) return fail;
  }
  if (assignment_notation && representation_copy) {
    // The chosen reference argument already designates the source object. Assignment does not
    // initialize a new class value, even when the copy constructor is deleted.
    return node(ast::Assign{info, ast::AssignOp::trivial_class_copy, at(o, begin), children->front(), info.type},
                begin, end, info);
  }
  if (!m.arrow) {
    // A prvalue object is materialized: the call's object is a temporary ([class.temporary]/2).
    if (!o.glvalue()) o = materialize(o, o.type(), false);
    ExprInfo pi{types().pointer(o.type()), ValueCategory::prvalue, 0};
    o = node(ast::Unary{pi, ast::UnaryOp::address_of, at(o, o.begin)}, o.begin, o.end, pi);
  }
  if (assignment_notation) {
    contract_assert(children->size() == 1);
    return node(ast::AssignmentCall{info, fe, children->front(), at(o, begin)}, begin, end, info);
  }
  if (immediate_virtual) {
    if (!unevaluated &&
        (fr.variadic || info.category != ValueCategory::prvalue || !supported_immediate_member_result(info.type))) {
      diag(paren_at, Diag::virtual_immediate_call);
      return fail;
    }
    return immediate_invocation(node(ast::ImmediateVirtualCall{info, fe, at(o, begin), b_->list(*children), u8(bound.qualified)},
                                     begin, end, info));
  }
  if (virtual_slot) {
    // A virtual function that is not pure is odr-used ([basic.def.odr]): a specialization's is instantiated.
    if (!d->is_pure && w_->entities.rec(w_->entities.rec(d->entity).scope).templ != EntityId::none)
      demands.push_back({d->entity, ast::Loc{item_, loc_base_ + begin}});
    // BOOTSTRAP: -Warray-bounds: reserved and appended. At -O3 GCC 16.2 reports libycxx's
    // in-place insert after a one-element vector as writing past its allocation (#748).
    // Without the bug: `std::vector<QualType> parameters{o.type()};` and
    // `parameters.insert(parameters.end(), fr.params.begin(), fr.params.end());`
    std::vector<QualType> parameters;
    parameters.reserve(1 + fr.params.size());
    parameters.push_back(o.type());
    parameters.append_range(fr.params);
    // The pure dispatch profile certifies a nonthrowing prototype for initializer selection.
    const auto pointer = types().pointer(types().function(fr.inner, parameters, false, LangLinkage::cxx,
                                                          Cv::none, ast::RefQual::none,
                                                          (d->is_pure || dynamic_owner) && fr.fn_noexcept));
    const auto selection = ins().intern_seq<ast::VirtualMemberSelection>({{*virtual_slot, dynamic_owner ? fe : EntityId::none}});
    return node(ast::VirtualMemberCall{info, pointer, selection, at(o, begin), b_->list(*children)}, begin, end, info);
  }
  return node(ast::MemberCall{info, fe, at(o, begin), b_->list(*children)}, begin, end, info);
}

auto Sema::ics(const Ex& a, QualType p, bool constructor_conversion) -> Ics {
  if (!a.ok()) return {};
  // The target parameter can select a function from an overload set ([over.over]).
  if (auto set = address_set(a)) {
    auto f = address_target(p);
    if (!f) return {};
    auto matches = address_matches(*set, *f);
    if (matches.size() != 1) return {};
    QualType t = matches.front().type;
    ExprInfo info = set->address ? ExprInfo{types().pointer(t), ValueCategory::prvalue, 0}
                                 : ExprInfo{t, ValueCategory::lvalue, 0};
    return ics({a.id, a.begin, a.end, info}, p, constructor_conversion);
  }
  if (types().is_reference(p)) {
    TypeRec r = types().rec(p);
    QualType t = r.inner;
    const bool direct_cat = direct_reference_category(a, r);
    const bool function_conversion = types().function_pointer_conversion(a.type(), t);
    const bool class_prvalue = !a.glvalue() && types().is_class(a.type()) &&
                               (r.rvalue_ref || object_cv(t) == Cv::const_);
    if ((direct_cat || class_prvalue) && (same_unqualified(a.type(), t) || function_conversion)) {
      if (!cv_within(object_cv(a.type()), object_cv(t))) return {};
      Ics v = binding({true, 0, function_conversion ? 1 : 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue,
                      cv_bits(object_cv(t)) - cv_bits(object_cv(a.type())));
      v.function_ref = types().is_function(t);
      return v;
    }
    // [over.ics.ref]/1: binding to a base class subobject is a derived-to-base Conversion.
    if (int d = base_depth(a.type(), t); d && (direct_cat || class_prvalue)) {
      if (!cv_within(object_cv(a.type()), object_cv(t))) return {};
      Ics v = binding({true, 2, 0}, r.rvalue_ref, a.cat() != ValueCategory::lvalue, cv_bits(object_cv(t)) - cv_bits(object_cv(a.type())));
      v.base_depth = d;
      return v;
    }
    bool const_ref = object_cv(t) == Cv::const_;
    if (!r.rvalue_ref && !const_ref) return {};
    if (r.rvalue_ref && a.cat() == ValueCategory::lvalue) return {};
    Ics v = ics(a, Types::unqualified(t), constructor_conversion);  // to a temporary
    if (!v.viable) return v;
    return binding(v, r.rvalue_ref, true, cv_bits(Types::cv(t)));
  }
  QualType to = Types::unqualified(p);
  if (types().is_class(to)) {
    if (initializes(to, a)) return {true, 0, 0};
    // [over.best.ics]/6: a base class object copied from a derived class's, a Conversion.
    if (int d = base_depth(a.type(), to)) {
      Ics v{true, 2, 0};
      v.base_depth = d;
      return v;
    }
    if (constructor_conversion && !types().is_class(a.type()))
      return constructor_ics(std::span(&a, 1), to, Construction::copy).value_or(Ics{});
    // A user-defined conversion through a conversion function ([over.best.ics]/2, /4).
    if (constructor_conversion && select_conversion(a, to, a.begin)) return {true, Ics::user_defined, 0};
    return {};
  }
  // A glvalue of type std::nullptr_t is one after the lvalue-to-rvalue conversion ([conv.ptr]/1).
  const bool null_type = types().is(Types::unqualified(types().canonical(a.type())), Builtin::nullptr_t);
  if (types().is_pointer(to) && (null_type || is_null_pointer_constant(a))) return {true, 2, 0};
  if (is_data_member_pointer(to) && (null_type || is_null_pointer_constant(a)))
    return {true, 2, 0};
  // A user-defined conversion: a closure's to a pointer to function ([over.best.ics]/2). Through
  // the one conversion function, the second standard conversion ranks them: identity before a
  // function pointer conversion ([over.ics.rank]/3.3).
  if (auto inv = closure_invoker(a.type(), to)) {
    const bool exact = types().canonical(types().pointer(inv->type)) == types().canonical(Types::unqualified(to));
    return {true, Ics::user_defined, exact ? 0 : 1};
  }
  if (auto inv = generic_invoker(a.type(), to, a.begin, /*demand_definition=*/false)) {
    const bool exact = types().canonical(types().pointer(inv->type)) == types().canonical(to);
    return {true, Ics::user_defined, exact ? 0 : 1};
  }
  QualType from = value_type(a);
  if (constructor_conversion && types().is_class(from) && select_conversion(a, to, a.begin))
    return {true, Ics::user_defined, 0};  // through a conversion function ([over.best.ics]/2, /4)
  if (types().is_class(from) || types().is_void(from)) return {};
  if (types().canonical(from) == types().canonical(to)) return {true, 0, 0};
  if (types().is_enum(to)) return {};
  if (types().is_enum(from)) {
    if (!is_unscoped_enum(from)) return {};
    if (types().is(to, Builtin::bool_) || types().is_floating(to)) return {true, 2, 0};  // [conv.fpint]
    if (!types().is_integral(to)) return {};
    // [conv.prom]/4, [over.ics.rank]/4.2: with a fixed type, to it is the better promotion.
    if (const EnumRec* er = enum_rec(types().enum_of(from)); er && er->fixed) {
      if (types().canonical(*er->underlying) == types().canonical(to)) return {true, 1, 0};
      if (builtin(sema::promoted(w_->target, *types().builtin_of(*er->underlying))) == to) return {true, 1, 1};
      return {true, 2, 0};
    }
    // [conv.prom]/3: without one, to the type its values need.
    const EnumRec* er = enum_rec(types().enum_of(from));
    QualType p = er && er->promotion ? *er->promotion : builtin(sema::promoted(w_->target, *types().builtin_of(underlying_of(from))));
    bool promotion = types().canonical(p) == types().canonical(to);
    return {true, promotion ? 1 : 2, 0};
  }
  if (types().is_pointer(from) && types().is_pointer(to))
  {
    if (qualification_convertible(from, to) || function_indirection_convertible(from, to)) return {true, 0, 1};
    if (int d = base_depth(types().pointee(from), types().pointee(to));
        d && cv_within(Types::cv(types().pointee(from)), Types::cv(types().pointee(to)))) {
      Ics v{true, 2, 0};
      v.base_depth = d;
      return v;
    }
    return pointer_convertible(from, to) ? Ics{true, 2, 0} : Ics{};
  }
  if (types().is(to, Builtin::bool_) && (types().is_arithmetic(from) || types().is_pointer(from))) return {true, 2, 0};
  // [conv.fpprom]: float to double is a promotion; the other floating-point and
  // floating-integral conversions are conversions ([over.ics.scs]).
  if (types().is_arithmetic(from) && types().is_arithmetic(to) && (types().is_floating(from) || types().is_floating(to)))
    return {true, types().is(from, Builtin::float_) && types().is(to, Builtin::double_) ? 1 : 2, 0};
  if (types().is_integral(from) && types().is_integral(to)) {
    bool promotion = builtin(sema::promoted(w_->target, *types().builtin_of(from))) == to && !types().is(from, *types().builtin_of(to));
    return {true, promotion ? 1 : 2, 0};
  }
  return {};
}

auto Sema::list_ics(const Braced& list, QualType parameter, bool scalar_list_arguments) -> std::optional<Ics> {
  const bool flat = std::ranges::none_of(list.clauses, [](const Braced::Clause& c) { return c.list || c.designated; });
  const QualType object = types().referee(parameter);
  if (flat && types().is_class(object) && has_user_ctors(object) &&
      std::ranges::all_of(list.clauses, [&](const Braced::Clause& c) { return c.expr.ok() && !types().is_class(c.expr.type()); })) {
    if (types().is_reference(parameter) && !types().rec(parameter).rvalue_ref && object_cv(object) != Cv::const_)
      return Ics{};
    std::vector<Ex> args;
    for (const Braced::Clause& c : list.clauses) args.push_back(c.expr);
    return constructor_ics(args, object, Construction::copy_list);
  }
  if (types().is_scalar(parameter)) {
    if (!flat || list.clauses.size() > 1) return Ics{};
    if (!types().is_arithmetic(parameter) && !(scalar_list_arguments && types().is_pointer(parameter)))
      return std::nullopt;
    if (list.clauses.empty()) return Ics{true};
    const Ex& expr = list.clauses.front().expr;
    if (expr.ok() && types().is_class(expr.type())) return std::nullopt;
    return ics(expr, parameter);
  }
  if (!flat || !types().is_class(parameter)) return std::nullopt;
  EntityId entity = types().class_of(parameter);
  if (w_->entities.rec(entity).templ != EntityId::none) return std::nullopt;
  const ClassRec* record = class_rec(entity);
  if (!record || record->union_ || record->closure || record->user_ctors || !record->bases.empty() ||
      !std::ranges::all_of(record->fields, [&](const FieldRec& f) {
        return types().is_arithmetic(f.type) && record->access_of(f.entity) == ast::Access::public_;
      }))
    return std::nullopt;
  // A single class-valued clause can instead use the same-class or base conversion.
  for (const Braced::Clause& c : list.clauses) {
    if (!c.expr.ok()) return Ics{};
    if (!types().is_arithmetic(c.expr.type()) && !(scalar_list_arguments && types().is_scalar(c.expr.type())))
      return std::nullopt;
  }
  if (list.clauses.size() > record->fields.size()) return Ics{};
  for (std::size_t i = 0; i < list.clauses.size(); ++i)
    if (!ics(list.clauses[i].expr, record->fields[i].type).viable) return Ics{};
  // The field conversions establish viability. Their ranks and narrowing do not rank the list.
  return Ics{true, Ics::user_defined};
}

auto Sema::constructor_template_guide(const ast::DeclSema& ctor, std::span<const ast::TemplateParamSema> outer, QualType self) -> std::optional<ast::DeclSema> {
  const std::vector<ast::TemplateParamSema> own = ins().seqs.elements(ctor.template_params);
  if (own.empty() || outer.empty()) return std::nullopt;
  const u16 depth = outer[0].depth, inner = own[0].depth;
  if (inner <= depth) return std::nullopt;
  Templates t = templates();
  Bindings b;
  b.levels.resize(inner + 1);
  for (const TemplateArg& a : t.own_args(outer)) b.levels[depth].push_back(a);
  std::vector<ast::TemplateParamSema> ps(outer.begin(), outer.end());
  for (const ast::TemplateParamSema& p : own) {
    ast::TemplateParamSema q = p;
    q.depth = depth;
    q.index = u16(ps.size());
    if (p.kind == TemplateArgKind::template_) return std::nullopt;
    if (p.kind == TemplateArgKind::type) {
      q.type = types().template_param(depth, q.index, p.pack != 0);
      if (p.has_default) {
        auto d = t.substitute(p.default_.type, b);
        if (d) q.default_.type = *d;
        else q.has_default = 0;
      }
    } else {
      auto pt = t.substitute(p.type, b);
      if (!pt) return std::nullopt;
      q.type = *pt;
      q.has_default = 0;  // BOOTSTRAP: a value default of a constructor template is not substituted yet
    }
    b.levels[inner].push_back(t.own_args(std::span(&q, 1)).front());
    ps.push_back(q);
  }
  auto fn = t.substitute(ctor.type, b);
  if (!fn) return std::nullopt;
  const TypeRec fr = types().rec(*fn);
  ast::DeclSema g = ctor;
  g.entity = EntityId::none;
  g.type = types().function(self, fr.params, fr.variadic, LangLinkage::cxx);
  g.template_params = ins().intern_seq(std::span<const ast::TemplateParamSema>(ps));
  g.has_trailing_return = false;
  g.constraints = ConstId::none;
  if (ctor.constraints != ConstId::none) {
    auto c = t.substitute_constraint(ctor.constraints, b);
    if (!c) return std::nullopt;
    g.constraints = *c;
  }
  g.explicit_condition = ConstId::none;
  if (ctor.explicit_condition != ConstId::none) {
    auto c = t.substitute_constraint(ctor.explicit_condition, b);
    if (!c) return std::nullopt;
    g.explicit_condition = *c;
  }
  return g;
}

auto Sema::deduce_class_type(EntityId templ, std::vector<Ex> args, const Braced* list, Construction form, u32 at) -> std::optional<QualType> {
  const NameId name = w_->entities.rec(templ).name;
  if (list) {
    for (const Braced::Clause& c : list->clauses) {
      if (c.list || c.designated) {
        diag(c.begin, Diag::not_supported, lex::Tk::l_brace);  // a nested list or designator
        return std::nullopt;
      }
      args.push_back(c.expr);
    }
  }
  if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return std::nullopt;
  if (any_dependent(args)) return builtin(Builtin::dependent);
  auto head = q_->template_decl(templ);
  const std::vector<ast::TemplateParamSema> params =
      head ? ins().seqs.elements(head->template_params) : std::vector<ast::TemplateParamSema>{};
  if (params.empty()) {
    diag(at, Diag::class_deduction_failed, name);
    return std::nullopt;
  }
  Templates t = templates();
  const QualType self = types().class_type(class_specialization(templ, t.own_args(params)));
  // The guides, each with its kind, its parameters' default arguments, and the class template's
  // own constraints it must also satisfy ([over.match.class.deduct]/1.1).
  struct Guide {
    ast::DeclSema decl;
    GuideKind kind;
    std::vector<NodeId> defaults;
    ConstId class_constraints = ConstId::none;
  };
  std::deque<Guide> guides;
  auto implicit = [&](QualType fn, std::vector<NodeId> defaults, bool is_explicit, ConstId constraints, GuideKind kind) {
    ast::DeclSema g = *head;
    g.entity = EntityId::none;
    g.kind = ast::DeclKind::function;
    g.type = fn;
    g.params = {};
    g.has_trailing_return = false;
    g.is_explicit = is_explicit;
    g.explicit_condition = ConstId::none;
    g.constraints = constraints;
    guides.push_back({g, kind, std::move(defaults), head->constraints});
  };
  if (const ClassRec* cr = class_rec(types().class_of(self)); cr && cr->defined) {
    for (const MemberDeclRec& m : cr->members) {
      if (!m.is_ctor) continue;
      if (m.is_template) {
        auto td = q_->template_decl(m.entity);
        auto g = td ? constructor_template_guide(*td, params, self) : std::nullopt;
        if (!g) continue;
        g->is_explicit = g->is_explicit || m.is_explicit;
        guides.push_back({*g, GuideKind::constructor_template, m.defaults, head->constraints});
      } else {
        const TypeRec fr = types().rec(m.type);
        implicit(types().function(self, fr.params, fr.variadic, LangLinkage::cxx), m.defaults, m.is_explicit,
                 m.constraints, GuideKind::constructor);
      }
    }
    if (!cr->user_ctors) implicit(types().function(self, {}, false, LangLinkage::cxx), {}, false, ConstId::none,
                                  GuideKind::constructor);
  } else {
    implicit(types().function(self, {}, false, LangLinkage::cxx), {}, false, ConstId::none, GuideKind::constructor);
  }
  // The copy deduction candidate, `C(C<T...>) -> C<T...>` ([over.match.class.deduct]/1.3).
  implicit(types().function(self, std::span(&self, 1), false, LangLinkage::cxx), {}, false, ConstId::none,
           GuideKind::copy);
  for (const DeductionGuide& g : q_->deduction_guides(templ)) {
    std::vector<NodeId> defaults;  // only whether there is one matters here ([over.match.viable]/2.3)
    for (std::uint8_t d : g.defaulted) defaults.push_back(d ? NodeId{} : NodeId::none);
    guides.push_back({g.decl, GuideKind::user, std::move(defaults)});
  }
  std::vector<ArgShape> shapes;
  for (const Ex& a : args) shapes.push_back(argument_shape(a));
  auto satisfied = [&](ConstId c, std::span<const TemplateArg> targs, u16 depth) {
    if (c == ConstId::none) return true;
    auto nf = t.substitute_constraint(c, Bindings::of(depth, targs));
    auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
    return !sat || *sat;
  };
  std::vector<Candidate> cands;
  for (const Guide& g : guides) {
    // Copy-initialization excludes a known explicit guide before deduction and constraints
    // ([over.match.copy]/1.1, [over.match.class.deduct]/1).
    if (form == Construction::copy && g.decl.explicit_condition == ConstId::none && g.decl.is_explicit) continue;
    const std::vector<ast::TemplateParamSema> gps = ins().seqs.elements(g.decl.template_params);
    Candidate k{EntityId::none, g.decl.type, true, false, nullptr, g.defaults, g.decl.is_explicit};
    if (!gps.empty()) {
      auto targs = t.deduce_call(g.decl, shapes, {}, g.defaults);
      if (!targs) continue;
      // Unsatisfied associated constraints fail deduction ([temp.deduct.general]/5).
      if (!satisfied(g.decl.constraints, *targs, gps[0].depth) ||
          !satisfied(g.class_constraints, std::span(*targs).first(std::min(targs->size(), params.size())), gps[0].depth))
        continue;
      auto st = t.specialization_type(g.decl, *targs);
      if (!st) continue;
      k.type = *st;
      k.pattern = &g.decl;
      if (g.decl.explicit_condition != ConstId::none) {
        auto is_explicit = explicit_specialization(g.decl, *targs);
        if (!is_explicit) continue;
        k.is_explicit = *is_explicit;
      }
    } else {
      TypeRec fr = types().rec(g.decl.type);
      if (args.size() < k.required(fr.params.size()) || (args.size() > fr.params.size() && !fr.variadic)) continue;
      if (!trailing_constraints_satisfied(g.decl.constraints)) continue;
    }
    // Copy-initialization considers only converting guides ([over.match.copy]/1.1).
    if (form == Construction::copy && k.is_explicit) continue;
    k.guide = g.kind;
    cands.push_back(std::move(k));
  }
  auto chosen = resolve(cands, args, at, name, std::nullopt, std::nullopt, {}, false, /*defer_no_viable=*/true);
  if (!chosen) {
    if (chosen.error() == OverloadFailure::no_viable) diag(at, Diag::class_deduction_failed, name);
    return std::nullopt;
  }
  const Candidate& k = cands[*chosen];
  // [over.match.list]: copy-list-initialization that selects an explicit guide is ill-formed.
  if (form == Construction::copy_list && k.is_explicit) {
    diag(at, Diag::class_deduction_explicit, name);
    return std::nullopt;
  }
  const QualType result = types().rec(k.type).inner;
  if (!types().is_class(result) || w_->entities.rec(types().class_of(result)).templ != templ) {
    diag(at, Diag::deduction_guide_result, name);
    return std::nullopt;
  }
  return Types::unqualified(result);
}

auto Sema::construct(QualType t, std::vector<Ex> args, u32 begin, u32 end, Construction form, ConstructorDestination destination) -> Ex {
  Ex fail{NodeId::none, begin, end};
  const bool list = form == Construction::direct_list || form == Construction::copy_list;
  const bool copy_init = form == Construction::copy;
  const bool polymorphic_base = destination == ConstructorDestination::polymorphic_base;
  const bool lambda_result = destination == ConstructorDestination::lambda_result;
  if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return fail;
  if (types().is_dependent(t) || any_dependent(args))
    return dependent_node(args, begin, end, types().is_dependent(t) ? std::nullopt : std::optional(Types::unqualified(t)));
  if (!require_complete(t, begin) ||
      ((destination == ConstructorDestination::object || lambda_result) && !require_concrete_object(t, begin)) ||
      (!polymorphic_base && !lambda_result && !require_supported_virtual_layout(t, begin))) return fail;
  QualType ut = Types::unqualified(t);
  EntityId c = types().class_of(ut);
  const ClassRec* cr = class_rec(c);
  if (!cr) return fail;
  // [dcl.init.general]/17.6.1: a prvalue of the class itself initializes the object directly.
  // Delegation requires a constructor, and a base is a potentially-overlapping subobject.
  if (destination == ConstructorDestination::object && !list && args.size() == 1 &&
      !args[0].glvalue() && initializes(ut, args[0])) return args[0];
  std::vector<Candidate> cands;
  std::vector<EntityId> ents;
  bool user_copy = false;
  std::deque<ast::DeclSema> patterns;
  std::vector<ArgShape> shapes;
  for (const Ex& a : args)
    shapes.push_back(argument_shape(a));
  for (const MemberDeclRec& m : cr->members)
    if (m.is_ctor && m.is_template) {
      // Copy-initialization excludes a known explicit constructor template before deduction
      // and associated-constraint satisfaction ([over.match.copy]/1.1, [temp.deduct.general]/5).
      if (copy_init && m.is_explicit) continue;
      // A constructor template: the specialization deduction gives ([temp.over]/1), never a
      // copy constructor ([class.copy.ctor]/5).
      auto tmpl = q_->template_decl(m.entity);
      if (!tmpl) continue;
      if (copy_init && tmpl->explicit_condition == ConstId::none && tmpl->is_explicit) continue;
      const ast::DeclSema& td = patterns.emplace_back(*tmpl);
      Templates t = templates();
      auto targs = t.deduce_call(td, shapes, {}, m.defaults);
      if (!targs) continue;
      // Unsatisfied associated constraints fail deduction ([temp.deduct.general]/5), before the
      // explicit-specifier is substituted.
      if (td.constraints != ConstId::none) {
        std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
        auto nf = t.substitute_constraint(td.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
        auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
        if (sat && !*sat) continue;
      }
      auto st = t.specialization_type(td, *targs);
      if (!st) continue;
      Candidate k{EntityId::none, *st, true, m.is_deleted, &td, m.defaults, m.is_explicit || td.is_explicit};
      k.is_consteval = (td.flags & decl_flag::consteval_) != 0;
      // A value-dependent explicit-specifier is substituted after deduction, and the
      // specialization leaves a context that admits only converting constructors
      // ([over.match.funcs.general]/8).
      if (td.explicit_condition != ConstId::none) {
        auto is_explicit = explicit_specialization(td, *targs);
        if (!is_explicit) continue;
        k.is_explicit = *is_explicit;
      }
      if (copy_init && (k.is_explicit || types().rec(*st).params.empty() || k.required(types().rec(*st).params.size()) > 1)) continue;
      // [class.copy.ctor]/5: never `S(S)`, a constructor taking its own class by value.
      if (TypeRec sr = types().rec(*st); sr.params.size() == 1 && Types::unqualified(types().canonical(sr.params[0])) == ut) continue;
      k.entity = specialization(td, *targs, *st);
      cands.push_back(std::move(k));
    } else if (m.is_ctor) {
      TypeRec fr = types().rec(m.type);
      // [over.match.funcs.general]/8: a defaulted move constructor defined as deleted is no candidate.
      if (m.is_deleted && m.defaulted == CopyState::deleted && !fr.params.empty() &&
          types().rec(fr.params.front()).rvalue_ref) continue;
      Candidate k{m.entity, m.type, true, m.is_deleted, nullptr, m.defaults, m.is_explicit};
      k.is_consteval = m.is_consteval;
      k.constraints = m.constraints;
      const std::size_t required = k.required(fr.params.size());
      // A user-declared copy constructor suppresses the implicit one even when it is not a
      // candidate ([class.copy.ctor]/6, [over.match.viable]/3).
      user_copy = user_copy || is_copy_ctor(fr, ut, required);
      // Copy-initialization converts with a constructor that takes one argument ([class.conv.ctor]).
      // Filter candidate eligibility and arity ([over.match.viable]/2) before checking constraints (/3).
      if (copy_init && (m.is_explicit || fr.params.empty() || required > 1)) continue;
      if (args.size() < required || (args.size() > fr.params.size() && !fr.variadic)) continue;
      if (m.ineligible || !trailing_constraints_satisfied(m.constraints)) continue;
      cands.push_back(std::move(k));
    }
  QualType void_ = builtin(Builtin::void_);
  if (!user_copy) {
    QualType source = cr->defaulted_copy_const ? types().add_cv(ut, Cv::const_) : ut;
    QualType p = types().lvalue_ref(source);
    cands.push_back({cr->copy_ctor, types().function(void_, std::span(&p, 1), false, LangLinkage::cxx), true, cr->copy_deleted});
    // [class.copy.ctor]: a class whose implicit copy is deleted can still use a trivial
    // implicit move and transfer its representation. General implicit move is tracked in #244.
    if (cr->copy_deleted && cr->move_usable && !cr->move_nontrivial && !classes().declared_move(c, *cr) && args.size() == 1 &&
        args[0].cat() == ValueCategory::xvalue && same_unqualified(args[0].type(), ut) &&
        object_cv(args[0].type()) == Cv::none) {
      QualType move = types().rvalue_ref(ut);
      cands.push_back({EntityId::none, types().function(void_, std::span(&move, 1), false, LangLinkage::cxx), true, false});
    }
  }
  if (!cr->user_ctors && !copy_init) {
    // A dynamic class's implicit default constructor is a function of its own (#309).
    const EntityId implicit = cr->implicit_default_ctor;
    const QualType type = implicit == EntityId::none ? types().function(void_, {}, false, LangLinkage::cxx)
                                                     : w_->entities.rec(implicit).type;
    // A dynamic class's implicit default constructor can be deleted ([class.default.ctor]/2).
    cands.push_back({implicit, type, true, cr->dynamic && cr->default_deleted});
  }
  if (polymorphic_base && list && std::ranges::any_of(cr->members, [&](const MemberDeclRec& member) {
        if (!member.is_ctor) return false;
        const auto params = types().rec(member.type).params;
        return !params.empty() && initializer_list_element(types().referee(params.front())) &&
               (params.size() == 1 || (member.defaults.size() > 1 && member.defaults[1] != NodeId::none));
      })) {
    // [over.match.list]: an empty list with a default constructor skips the first phase.
    const bool use_default = args.empty() && std::ranges::any_of(cands, [&](const Candidate& ctor) {
      return ctor.required(types().rec(ctor.type).params.size()) == 0;
    });
    if (!use_default) {
      diag(begin, Diag::base_initializer_list_constructor);
      return fail;
    }
  }
  const bool paren_aggregate = form == Construction::direct_paren_aggregate;
  const auto unresolved = diag_count();
  auto chosen = resolve(cands, args, begin, no_name(), std::nullopt, ut, {}, false, paren_aggregate);
  if (!chosen) {
    // The deleted implicit copy stands in for an implicit move that a volatile member's own move
    // constructor makes usable (#244).
    if (!user_copy && cr->copy_deleted && cr->move_usable && cr->move_nontrivial && !classes().declared_move(c, *cr) &&
        args.size() == 1 && args[0].cat() == ValueCategory::xvalue && same_unqualified(args[0].type(), ut) &&
        object_cv(args[0].type()) == Cv::none && classes().has_volatile_class_member(*cr) &&
        diag_count() == unresolved + 1 && diags_->back().id == diag_id(Diag::deleted_constructor)) {
      truncate_diags(unresolved);
      diag(begin, Diag::defaulted_copy_unsupported, ut);
      return fail;
    }
    if (chosen.error() == OverloadFailure::no_viable) return parenthesized_aggregate(t, args, begin, end);
    return fail;
  }
  const Candidate& k = cands[*chosen];
  // The state of the selected constructor when it is defaulted on its first declaration.
  const MemberDeclRec* selected_member = nullptr;
  if (k.entity != EntityId::none && !k.pattern)
    if (const auto m = std::ranges::find(cr->members, k.entity, &MemberDeclRec::entity); m != cr->members.end())
      selected_member = &*m;
  const CopyState defaulted = selected_member ? selected_member->defaulted : CopyState::ordinary;
  const bool defaulted_copy = defaulted != CopyState::ordinary && k.entity != cr->defaulted_default_ctor;
  const bool trivial_call = selected_member && selected_member->trivial_call;
  if (destination == ConstructorDestination::delegation) {
    if (k.entity == current_function) {
      diag(begin, Diag::delegating_constructor_cycle);
      return fail;
    }
    if (k.pattern) {
      diag(begin, Diag::not_supported, lex::Tk::colon);
      return fail;
    }
    if (k.entity == EntityId::none || defaulted_copy) {
      diag(begin, Diag::delegating_copy_unsupported);
      return fail;
    }
  }
  if (form == Construction::copy_list && k.is_explicit) {
    diag(begin, Diag::copy_list_explicit_constructor, ut);
    return fail;
  }
  if (k.entity != EntityId::none && !check_access(c, k.pattern ? k.pattern->entity : k.entity, w_->entities.rec(c).name, begin)) return fail;
  if (polymorphic_base || lambda_result) {
    // [class.base.init], [stmt.return]: the destructor is potentially invoked.
    if (!cr->destructor_usable) {
      diag(begin, polymorphic_base ? Diag::base_unusable_destructor : Diag::return_unusable_destructor, ut);
      return fail;
    }
    const EntityId destructor = cr->destructor_declaration();
    if (destructor != EntityId::none && !check_access(c, destructor, w_->entities.rec(destructor).name, begin)) return fail;
  }
  if (k.entity != EntityId::none && k.entity == cr->defaulted_default_ctor) {
    if (defaulted == CopyState::unsupported) {
      diag(begin, Diag::defaulted_default_constructor, ut);
      return fail;
    }
    if (form == Construction::default_init && Types::is_const(types().canonical(t)) && !const_default_constructible(t)) {
      diag(begin, Diag::defaulted_const_uninitialized, t);
      return fail;
    }
    if (destination == ConstructorDestination::delegation) {
      diag(begin, Diag::defaulted_default_delegation);
      return fail;
    }
    if (defaulted == CopyState::trivial && !trivial_call) {
      ExprInfo info{ut, ValueCategory::prvalue, 0};
      // [dcl.init.general]: value-initialization zero-initializes before the trivial constructor.
      if (form == Construction::default_init) return node(ast::DefaultInit{info}, begin, end, info);
      return node(ast::ValueInit{info}, begin, end, info);
    }
  }
  // A specialization called is one whose definition the program requires ([temp.inst]/8).
  if (k.pattern) demands.push_back({k.entity, ast::Loc{item_, loc_base_ + begin}});
  TypeRec fr = types().rec(k.type);
  if (!require_concrete_parameters(fr, begin)) return fail;
  const bool implicit_copy = k.entity == EntityId::none && !fr.params.empty() && !types().rec(fr.params[0]).rvalue_ref;
  const CopyState copy = defaulted_copy ? defaulted : implicit_copy ? cr->defaulted_copy : CopyState::ordinary;
  // The implicit copy of an rvalue stands in for an implicit move that would call a subobject's
  // own move constructor (#244).
  const bool stands_for_move = (implicit_copy || (k.entity == cr->copy_ctor && !selected_member)) &&
      (cr->defaulted_copy != CopyState::ordinary ? classes().implicit_move_differs(c)
                                                 : classes().implicit_move_differs(c, /*defaulted=*/true)) &&
      args[0].cat() != ValueCategory::lvalue && object_cv(args[0].type()) == Cv::none;
  if (copy == CopyState::unsupported || stands_for_move) {
    diag(begin, Diag::defaulted_copy_unsupported, ut);
    return fail;
  }
  if (copy == CopyState::trivial && !trivial_call) {
    Ex src = bind_reference(args[0], fr.params[0], args[0].begin);
    return src.ok() ? convert_node(src, ast::ConversionKind::lvalue_to_rvalue, ut) : fail;
  }
  if (k.entity == EntityId::none) {
    // Trivial: a copy of the argument's object, or nothing to construct.
    if (fr.params.empty()) {
      if (polymorphic_base || (lambda_result && cr->fields.empty() && cr->bases.empty() && !cr->default_deleted)) {
        // Keep the semantic initializer without laying out the class. For lambda results,
        // this implicit form is bounded to roots without subobject initialization.
        ExprInfo info{ut, ValueCategory::prvalue, 0};
        return node(ast::ValueInit{info}, begin, end, info);
      }
      if (lambda_result) {
        diag(begin, Diag::virtual_layout, ut);
        return fail;
      }
      return value_init_aggregate(ut, begin, end);
    }
    if (polymorphic_base || lambda_result) {
      // An implicit polymorphic copy is nontrivial ([class.copy.ctor]).
      diag(begin, Diag::virtual_layout, ut);
      return fail;
    }
    Ex a = args[0];
    return a.glvalue() ? convert_node(a, ast::ConversionKind::lvalue_to_rvalue, ut) : a;
  }
  const auto supplied = args.size();
  for (std::size_t i = supplied; i < fr.params.size() && i < k.defaults.size(); ++i) {
    auto a = default_argument(k.defaults[i], end, DefaultArgumentOrigin::interned, k.entity, fr.params[i], begin);
    if (!a) return fail;
    args.push_back(*a);
  }
  std::vector<Child> cs;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const Ex& arg = args[i];
    Ex a;
    if (i >= fr.params.size()) {
      a = variadic_argument(arg);
    } else {
      const QualType p = fr.params[i];
      // A base list can bind a class glvalue to a class reference without reading its value.
      // The default arguments have already been checked at their declarations.
      const bool class_ref = polymorphic_base && arg.glvalue() && types().is_class(arg.type()) &&
                             types().is_reference(p) && types().is_class(types().referee(p));
      const bool narrow = list && (!polymorphic_base || (i < supplied && !class_ref));
      a = narrow ? list_element(p, arg) : convert(arg, p, arg.begin);
    }
    if (!a.ok()) return fail;
    // A parameter of class type: a temporary the caller initializes and destroys ([expr.call]/7).
    if (i < fr.params.size() && types().is_class(fr.params[i]) && !a.glvalue())
      a = materialize(a, Types::unqualified(fr.params[i]), false, true);
    if (!a.ok()) return fail;
    cs.push_back({a.id, a.begin >= begin ? a.begin - begin : 0});
  }
  ExprInfo info{ut, ValueCategory::prvalue, 0};
  // [dcl.init.general]/9: value-initialization zero-initializes before a constructor that is
  // not user-provided.
  const bool not_user_provided = k.entity == cr->implicit_default_ctor || k.entity == cr->defaulted_default_ctor;
  const u32 zero_first = not_user_provided && form != Construction::default_init ? 1 : 0;
  Ex construction = node(ast::CtorCall{info, k.entity, zero_first, b_->list(cs)}, begin, end, info);
  if (!k.is_consteval) return construction;
  consteval_.insert(std::to_underlying(k.entity));
  return immediate_invocation(construction);
}

auto Sema::check_destructor_access(QualType type, u32 at, Diag unusable, bool base_subobject) -> bool {
  type = types().innermost_array_element(type);
  if (!types().is_class(type) || types().is_dependent(type)) return true;
  const EntityId owner = types().class_of(type);
  const ClassRec* record = class_rec(owner);
  if (!record) return true;
  if (record->destructor_dependent && in_template_definition()) return true;
  if (!record->destructor_usable) {
    diag(at, unusable, type);
    return false;
  }
  const EntityId destructor = record->destructor_declaration();
  if (destructor == EntityId::none) return true;
  const NameId name = w_->entities.rec(destructor).name;
  if (defer_type_access && !accessible(owner, record->access_of(destructor))) {
    deferred_access_.push_back({owner, destructor, name, at, true});
    return true;
  }
  // [class.protected]: access through a derived context needs a derived object.
  if (!base_subobject && record->access_of(destructor) == ast::Access::protected_ &&
      !accessible(owner, ast::Access::private_)) {
    diag(at, Diag::protected_member, name, types().class_type(owner));
    return false;
  }
  return check_access(owner, destructor, name, at);
}

auto Sema::runtime_default_supported(Ex operand) -> Ex {
  const auto ordinary_result = [&](ExprInfo info) {
    return info.category != ValueCategory::prvalue || !types().is_class(info.type);
  };
  const auto supported_operation = [&](const ast::Red& node) {
    if (node.is<ast::Call>()) return ordinary_result(node.get<ast::Call>().info);
    if (node.is<ast::MemberCall>()) return ordinary_result(node.get<ast::MemberCall>().info);
    if (node.is<ast::ImplicitConversion>()) {
      const auto conversion = node.get<ast::ImplicitConversion>().conversion;
      return conversion != ast::ConversionKind::user_defined && conversion != ast::ConversionKind::array_copy;
    }
    return node.is<ast::Literal>() || node.is<ast::SourceLine>() || node.is<ast::SourceLocation>() ||
           node.is<ast::DefaultSourceContext>() ||
           node.is<ast::DeclRef>() || node.is<ast::LocalRef>() || node.is<ast::This>() ||
           node.is<ast::Paren>() || node.is<ast::Member>() || node.is<ast::Subscript>() ||
           node.is<ast::Unary>() || node.is<ast::Binary>() || node.is<ast::OrderedBinary>() ||
           node.is<ast::Assign>() || node.is<ast::Conditional>() || node.is<ast::DefaultArg>();
  };
  const auto root = b_->commit(ast::Child{operand.id, 0});
  bool supported = true;
  auto visit = [&](this const auto& self, const ast::Red& node) -> void {
    if (node.is<ast::ConstantProbe>()) return;
    if (!supported) return;
    supported = supported_operation(node);
    if (supported) node.for_each_child(self);
  };
  visit(ast::Red(ins().ast, root.id, operand.begin));
  if (!supported || !q_->exceptions_enabled()) return bool_literal(supported, operand.begin, operand.end);
  return unary_noexcept(operand, operand.begin, operand.end);
}

auto Sema::unary_noexcept(Ex operand, u32 begin, u32 end, bool result_destroyed) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (!operand.ok()) return fail;
  if (dependent(operand)) return dependent_node({operand}, begin, end, builtin(Builtin::bool_));
  const auto before = diag_count();
  bool missing_destructor_exception = false;
  auto destructor_throwing = [&](QualType type, u32 at) {
    if (!types().is_class(type) || !require_complete(type, at) || !check_destructor_access(type, at)) return false;
    const EntityId owner = types().class_of(type);
    const ClassRec* record = class_rec(owner);
    if (record && !record->destructor_noexcept && record->declared_destructor != EntityId::none &&
        record->destructor == EntityId::none) {
      if (missing_destructor_exception) return false;
      const auto nonthrowing = q_->replay_destructor_exception(*this, owner, at);
      missing_destructor_exception = !nonthrowing;
      return nonthrowing && !*nonthrowing;
    }
    return record && !record->destructor_noexcept.value_or(record->destructor == EntityId::none);
  };
  bool result_throws = result_destroyed && !operand.glvalue() && destructor_throwing(operand.type(), operand.begin);

  auto discarded_temporary_throwing = [&](Child child, u32 offset) {
    const auto info = expression_info(child.id);
    return info.category == ValueCategory::prvalue && destructor_throwing(info.type, offset + child.at);
  };
  // A call of a function whose condition its specializations decide, or of a closure's function
  // call operator whose condition its template's instantiations decide, in a template's
  // definition, makes the result value-dependent ([temp.dep.constexpr]).
  bool value_dependent = false;
  auto function_throwing = [&](EntityId function) {
    if (const auto nonthrowing = q_->defaulted_nonthrowing(function)) return !*nonthrowing;
    if (dependent_noexcept_calls_.contains(std::to_underlying(function))) value_dependent = true;
    auto written = q_->written_member(function);
    if (!written) written = q_->class_friend(function);
    QualType type = written ? written->type : w_->entities.rec(function).type;
    // A specification that depends on the template's parameters, for this specialization.
    std::optional<ast::DeclSema> pattern = written;
    if (!pattern)
      if (const EntityId t = w_->entities.rec(function).templ; t != EntityId::none) pattern = q_->template_decl(t);
    if (pattern && in_pattern() && q_->deferred_exception(pattern->entity)) {
      value_dependent = true;
    } else if (pattern) {
      if (auto resolved = q_->replay_member_exception(*this, *pattern, function, type)) type = *resolved;
    }
    return !types().rec(type).fn_noexcept;
  };
  auto operation_throwing = [&](const ast::Red& node) {
    if (node.is<ast::Throw>()) return true;
    if (node.is<ast::Call>()) {
      const auto call = node.get<ast::Call>();
      if (call.function != EntityId::none) return function_throwing(call.function);
      QualType type = expression_info(call.callee.id).type;
      if (types().is_pointer(type)) type = types().pointee(type);
      return !types().rec(type).fn_noexcept;
    }
    if (node.is<ast::MemberCall>()) return function_throwing(node.get<ast::MemberCall>().function);
    if (node.is<ast::ImmediateVirtualCall>()) return function_throwing(node.get<ast::ImmediateVirtualCall>().function);
    if (node.is<ast::ExplicitDestroy>()) {
      const auto receiver = expression_info(node.get<ast::ExplicitDestroy>().object.id).type;
      return destructor_throwing(types().pointee(receiver), node.offset());
    }
    if (node.is<ast::AssignmentCall>()) return function_throwing(node.get<ast::AssignmentCall>().function);
    if (node.is<ast::CtorCall>()) return function_throwing(node.get<ast::CtorCall>().ctor);
    // [except.spec], [expr.new]: syntax-only array allocation selects the usual
    // global operator new[] without a Call child. The visitor still checks the operands.
    if (node.is<ast::NewArray>() && node.get<ast::NewArray>().allocation.id == NodeId::none)
      return function_throwing(w_->allocation_function(AllocationFunction::allocate, /*array=*/true).entity);
    if (node.is<ast::DeleteObject>()) {
      // [expr.unary.noexcept]: the destructor is potentially invoked, then the deallocation.
      const auto d = node.get<ast::DeleteObject>();
      if (d.destructor != EntityId::none &&
          destructor_throwing(types().pointee(expression_info(d.address.id).type), node.offset()))
        return true;
      return function_throwing(d.function);
    }
    if (node.is<ast::DeleteArray>()) return function_throwing(node.get<ast::DeleteArray>().function);
    if (node.is<ast::ImplicitConversion>()) {
      const auto conversion = node.get<ast::ImplicitConversion>();
      if (conversion.conversion == ast::ConversionKind::user_defined) return function_throwing(conversion.function);
      if (conversion.conversion == ast::ConversionKind::to_void)
        return discarded_temporary_throwing(conversion.operand, node.offset());
    }
    if (node.is<ast::Materialize>()) return destructor_throwing(node.get<ast::Materialize>().info.type, node.offset());
    if (node.is<ast::OrderedBinary>()) {
      const auto binary = node.get<ast::OrderedBinary>();
      if (binary.op == ast::OrderedOp::comma) return discarded_temporary_throwing(binary.lhs, node.offset());
    }
    return false;
  };
  const auto root = b_->commit(ast::Child{operand.id, 0});
  auto visit = [&](this const auto& self, const ast::Red& node) -> void {
    result_throws |= operation_throwing(node);
    node.for_each_child(self);
  };
  visit(ast::Red(ins().ast, root.id, operand.begin));
  if (missing_destructor_exception || has_error_since(before)) return fail;
  if (value_dependent) return dependent_node({operand}, begin, end, builtin(Builtin::bool_));
  return bool_literal(!result_throws, begin, end);
}

auto Sema::bind_reference(Ex e, QualType r, u32 at_, bool extend) -> Ex {
  if (!e.ok()) return e;
  if (dependent(e) || types().is_dependent(r)) {
    QualType t = types().referee(r);
    return dependent_node({e}, e.begin, e.end, types().is_dependent(t) ? std::nullopt : std::optional(t),
                          types().rec(r).rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue);
  }
  e = overload_address(e, r, at_);
  if (!e.ok()) return e;
  if (is_bound_member(e) || types().is(e.type(), Builtin::overload_set)) return rvalue(e);
  TypeRec rr = types().rec(r);
  QualType t = rr.inner;
  Ex fail{NodeId::none, e.begin, e.end};
  if (e.glvalue() && e.info.bit_width) {
    diag(at_, Diag::bit_field_reference);
    return fail;
  }
  const bool direct_cat = direct_reference_category(e, rr);
  if (direct_cat && types().function_pointer_conversion(e.type(), t))
    return convert_node(e, ast::ConversionKind::qualification, t, e.cat());
  if (direct_cat && same_unqualified(e.type(), t)) {
    if (cv_within(object_cv(e.type()), object_cv(t))) return extend ? extended(e) : e;
    diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
    return fail;
  }
  // [dcl.init.ref]/5.1, 5.3: to the base class subobject of a derived class's object, a prvalue
  // materialized first (and its lifetime extended with the reference's, [class.temporary]/6).
  if (types().is_class(e.type()) && types().is_class(t) && !same_unqualified(e.type(), t) &&
      (direct_cat || rr.rvalue_ref || object_cv(t) == Cv::const_) &&
      !(rr.rvalue_ref && e.cat() == ValueCategory::lvalue)) {
    int paths = 0;
    if (is_complete(e.type()) && base_path(types().class_of(e.type()), types().class_of(t), paths)) {
      if (!cv_within(object_cv(e.type()), object_cv(t))) {
        diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
        return fail;
      }
      if (!e.glvalue())
        return to_base(materialize(e, types().add_cv(e.type(), object_cv(t)), extend), types().class_of(t), at_);
      const bool binding_lvalue_reference = !rr.rvalue_ref && e.cat() == ValueCategory::lvalue;
      Ex b = to_base(e, types().class_of(t), at_, binding_lvalue_reference);
      return b.ok() && extend ? extended(b) : b;
    }
  }
  bool const_ref = object_cv(t) == Cv::const_;
  if (!rr.rvalue_ref && !const_ref) {
    if (e.cat() == ValueCategory::lvalue && same_unqualified(e.type(), t)) diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
    else diag(at_, Diag::bind_to_rvalue, t);
    return fail;
  }
  // [dcl.init.ref]/5.4.4: an rvalue reference does not bind to an lvalue.
  if (rr.rvalue_ref && e.cat() == ValueCategory::lvalue) {
    diag(at_, Diag::bind_to_lvalue, t);
    return fail;
  }
  if (!e.glvalue() && types().is_class(t) && same_unqualified(e.type(), t) &&
      !cv_within(object_cv(e.type()), object_cv(t))) {
    diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
    return fail;
  }
  if (!e.glvalue() && types().is_array(t) && same_unqualified(e.type(), t) &&
      cv_within(object_cv(e.type()), object_cv(t)))
    return materialize(e, t, extend);  // [dcl.init.ref]/5.3: a reference-compatible array prvalue
  if (rr.rvalue_ref && e.cat() == ValueCategory::xvalue && compatibility() != ast::Compatibility::clang &&
      types().is_pointer(e.type()) && types().is_pointer(t) && similar_types(e.type(), t) == true &&
      !qualification_convertible(types().pointer(e.type()), types().pointer(t))) {
    diag(at_, Diag::bind_drops_qualifiers, e.type(), r);
    return fail;
  }
  Ex v = convert(e, Types::unqualified(t), at_);
  if (!v.ok() || v.glvalue()) return v;
  return materialize(v, t, extend);
}

auto Sema::extended(Ex e) -> Ex {
  if (auto m = as<ast::Materialize>(e))
    return node(ast::Materialize{m->info, m->destructor, 1, m->operand}, e.begin, e.end, m->info);
  if (auto p = as<ast::Paren>(e)) {
    Ex inner = extended({p->operand.id, e.begin + p->operand.at, e.end, {}});
    return node(ast::Paren{p->info, at(inner, e.begin)}, e.begin, e.end, p->info);
  }
  // Through a base subobject or an identity-preserving glvalue cast ([class.temporary]/6).
  if (auto c = as<ast::ImplicitConversion>(e); c &&
      (c->conversion == ast::ConversionKind::derived_to_base || c->conversion == ast::ConversionKind::base_to_derived ||
       c->conversion == ast::ConversionKind::qualification ||
       (c->conversion == ast::ConversionKind::reinterpret && c->info.category != ValueCategory::prvalue))) {
    Ex inner = extended({c->operand.id, e.begin + c->operand.at, e.end, {}});
    return node(ast::ImplicitConversion{c->info, c->conversion, c->function, at(inner, e.begin)}, e.begin, e.end, c->info);
  }
  if (auto m = as<ast::Member>(e); m && !m->arrow && !m->reference) {
    Ex inner = extended({m->object.id, e.begin + m->object.at, e.end, {}});
    return node(ast::Member{m->info, m->member, m->field, 0, 0, at(inner, e.begin)}, e.begin, e.end, m->info);
  }
  if (auto m = as<ast::OrderedBinary>(e)) {
    if (m->op == ast::OrderedOp::dot_star) {
      Ex inner = extended({m->lhs.id, e.begin + m->lhs.at, e.end, {}});
      return node(ast::OrderedBinary{m->info, m->op, at(inner, e.begin), m->rhs}, e.begin, e.end, m->info);
    }
    if (m->op == ast::OrderedOp::comma) {
      Ex inner = extended({m->rhs.id, e.begin + m->rhs.at, e.end, {}});
      return node(ast::OrderedBinary{m->info, m->op, m->lhs, at(inner, e.begin)}, e.begin, e.end, m->info);
    }
  }
  return e;
}

auto Sema::name_expr(NameId name, u32 begin, u32 end, bool callee, bool immediate_default_call) -> Ex {
  if (ins().names.spelling(name) == "__PRETTY_FUNCTION__" && in_function_or_lambda_body())
    return pretty_function(begin, end);
  // Block scopes, innermost first: a scope's locals and the names its using-declarations
  // brought in ([namespace.udecl]) hide those of enclosing scopes ([basic.scope.hiding]);
  // using-declarations of one scope make one set.
  for (std::size_t d = scopes_.size(); d-- > 0;) {
    for (auto p = scopes_[d].rbegin(); p != scopes_[d].rend(); ++p)
      if (p->first == name) {
        u32 ctx = context_of(d);
        if (!(p->second & capture_bit)) {
          const ast::LocalRec& local = ctx == lambdas_.size() ? locals_[p->second] : lambdas_[ctx].outer_locals[p->second];
          if (!unevaluated && (local.flags & local_flag::default_argument_parameter)) {
            diag(begin, Diag::parameter_in_default_argument, name);
            return {NodeId::none, begin, end};
          }
        }
        // A local of an enclosing function, or an init-capture: through the lambdas' captures.
        if (ctx != lambdas_.size() || (p->second & capture_bit)) return captured(ctx, p->second, name, begin, end);
        const ast::LocalRec& l = locals_[p->second];
        // A variable whose type its own initializer deduces ([dcl.spec.auto.general]/11).
        if (types().has_placeholder(l.type)) {
          diag(begin, Diag::auto_used_in_initializer, name);
          return {NodeId::none, begin, end};
        }
        // A function parameter pack of the definition: its uses are expanded ([temp.variadic]).
        if (types().kind(l.type) == TypeKind::pack_expansion) {
          if (pattern_packs_.empty()) {
            diag(begin, Diag::unexpanded_pack, name);  // [temp.variadic]/5: outside every expansion
            return {NodeId::none, begin, end};
          }
          note_pack({0, 0, name});
          return dependent_node({}, begin, end);
        }
        return local_ref(p->second, begin, end);
      }
    if (std::ranges::any_of(block_types_, [&](const BlockType& t) { return t.depth == d && t.name == name; })) {
      diag(begin, Diag::type_as_expression, name);  // [expr.prim.id]: a type is not an expression
      return {NodeId::none, begin, end};
    }
    std::vector<ast::DeclSema> used;
    for (const BlockUsing& u : block_usings_)
      if (u.depth == d && u.name == name) used.insert(used.end(), u.decls.begin(), u.decls.end());
    if (!used.empty()) return decls_expr(std::move(used), true, name, begin, end, callee, true);
  }
  // A function parameter pack of an instantiation, unless a block declares the name: its element
  // ([temp.variadic]/8).
  bool is_pack = false;
  u32 pack_ctx = 0;
  if (auto l = pack_element(name, is_pack, &pack_ctx)) {
    // An element of an enclosing function's pack, in a lambda: captured as any local is.
    if (pack_ctx != lambdas_.size()) return captured(pack_ctx, *l, name, begin, end);
    ExprInfo info{types().referee(locals_[*l].type), ValueCategory::lvalue, 0};
    return node(ast::LocalRef{info, *l}, begin, end, info);
  }
  if (is_pack) {
    diag(begin, Diag::unexpanded_pack, name);
    return {NodeId::none, begin, end};
  }
  // Enumerators a `using enum` brought into a block scope ([enum.udecl]).
  for (auto u = using_enums_.rbegin(); u != using_enums_.rend(); ++u)
    if (auto r = find_enumerator(u->second, name, begin, end)) return *r;
  // A non-type template parameter is a prvalue whose value depends on the argument
  // ([temp.param]/8, [temp.dep.constexpr]).
  if (const ast::TemplateParamSema* p = template_param(name)) {
    if (p->kind == ast::TemplateArgKind::type) {
      diag(begin, Diag::expected_expression);
      return {NodeId::none, begin, end};
    }
    if (p->pack && pattern_packs_.empty() && frames_.empty()) {
      diag(begin, Diag::unexpanded_pack, name);  // [temp.variadic]/5: outside every expansion
      return {NodeId::none, begin, end};
    }
    if (auto a = bound_arg(*p)) {
      if (!ins().values.is<ast::VAggregate>(a->value))
        return literal(Types::unqualified(a->type), a->value, ValueCategory::prvalue, begin, end);
      QualType declared = types().canonical(Types::unqualified(a->type));
      QualType object_type = types().add_cv(declared, Cv::const_);
      EntityId object = w_->entities.entity({.kind = EntityKind::template_parameter_object,
                                             .name = no_name(), .type = object_type, .args = {*a}});
      if (!unevaluated) q_->use_template_parameter_object(object);
      ExprInfo info{object_type, ValueCategory::lvalue, 0};
      Ex result = node(ast::DeclRef{info, object}, begin, end, info);
      declared_types_[std::to_underlying(result.id)] = declared;
      return result;
    }
    ConstId v = ins().intern_value(ast::VDepParam{p->depth, p->index});
    QualType declared = Types::unqualified(p->type);
    const bool class_parameter = types().is_class(declared) && !types().is_dependent(declared);
    ExprInfo info{class_parameter ? types().add_cv(declared, Cv::const_) : declared,
                  class_parameter ? ValueCategory::lvalue : ValueCategory::prvalue, expr_flag::dependent};
    Ex result = node(ast::Literal{info, v}, begin, end, info);
    if (class_parameter) declared_types_[std::to_underlying(result.id)] = declared;
    return result;
  }
  if (const auto* declaration = earlier_nested_enumerator(name))
    return enumerator_in_list(*declaration, begin, end);
  // The enclosing class's members ([basic.scope.class]).
  if (current_class != EntityId::none) {
    // A name a using-declaration takes from a dependent base is type-dependent ([temp.dep.expr]/3).
    if (q_->dependent_member_using(current_class, name)) return dependent_node({}, begin, end);
    if (auto r = class_member(current_class, name, begin, end, begin, std::nullopt, /*unqualified=*/true)) return *r;
    if (!class_rec(current_class))
      if (auto r = incomplete_class_member(name, begin, end, begin, /*unqualified=*/true, callee)) return *r;
    // Then the classes it is nested in ([class.nest], [basic.lookup.unqual]/2).
    for (EntityId k = enclosing_class(current_class); k != EntityId::none; k = enclosing_class(k)) {
      if (q_->dependent_member_using(k, name)) return dependent_node({}, begin, end);
      if (class_rec(k)) {
        if (auto r = class_member(k, name, begin, end, begin, std::nullopt, /*unqualified=*/true)) return *r;
      } else if (auto r = enclosing_prefix_value(k, name, begin, end, begin, /*unqualified=*/true, callee)) {
        return *r;
      }
    }
  }
  if (auto r = outer_block_value(name, begin, end, callee)) return *r;
  if (namespace_lambda && namespace_lambda->name == name)
    return undeduced_namespace_lambda(name, begin, end);
  std::vector<ast::DeclSema> found;
  for (auto p = own.rbegin(); p != own.rend(); ++p)
    if (p->name == name) {
      if (p->enum_initializer != ConstId::none)
        return literal(p->sema.type, p->enum_initializer, ValueCategory::prvalue, begin, end);
      found.push_back(p->sema);
    }
  // An earlier declarator of the member's own declaration (`static constexpr int a = 1, b = a;`).
  if (found.size() == 1 && current_class != EntityId::none && found[0].kind == ast::DeclKind::variable &&
      found[0].storage == ast::Storage::static_)
    return static_member(current_class, found[0].entity, found[0].type, begin, end);
  bool any = !found.empty();
  if (found.empty()) {
    for (DeclId d : q_->visible_decls(name, directives(), immediate_default_call)) {
      any = true;
      ast::DeclSema s = q_->decl_sema(d);
      if (s.entity != EntityId::none && !is_special(s)) found.push_back(s);  // else its item diagnosed it
    }
    if (immediate_default_call && ambiguous_namespace_call(found, name, begin))
      return {NodeId::none, begin, end};
  }
  return decls_expr(std::move(found), any, name, begin, end, callee, true);
}

auto Sema::builtin_overflow_call(ast::BuiltinOp op, std::span<const Ex> args, u32 begin, u32 end) -> Ex {
  const Ex invalid{NodeId::none, begin, end};
  if (!builtin_arity(3, args.size(), begin) ||
      std::ranges::any_of(args, [](const Ex& arg) { return !arg.ok(); }))
    return invalid;
  const QualType result = builtin(Builtin::bool_);
  if (any_dependent(args)) return dependent_node(args, begin, end, result);

  const auto standard_unsigned = [&](QualType type) {
    const auto kind = types().builtin_of(type);
    return kind && ast::is_standard_unsigned(*kind) && ast::width_of(w_->target, *kind) <= 64;
  };
  const QualType integer = builtin(w_->target.intmax_type);
  std::array params{integer, integer, types().pointer(integer)};
  const bool unsigned_call = standard_unsigned(args[0].type());
  if (unsigned_call) {
    if (!standard_unsigned(args[1].type())) {
      diag(args[1].begin, Diag::builtin_overflow_unsigned_operand, args[1].type());
      return invalid;
    }
    const QualType output = types().canonical(args[2].type());
    params[2] = types().is_array(output) ? types().pointer(types().rec(output).inner) : Types::unqualified(output);
    const auto object = types().is_pointer(params[2])
                            ? std::optional(types().canonical(types().pointee(params[2]))) : std::nullopt;
    if (!object || Types::is_const(*object) || !standard_unsigned(*object)) {
      diag(args[2].begin, Diag::builtin_overflow_unsigned_result, args[2].type());
      return invalid;
    }
    // Both the exact sum and the exact product of two unsigned operands up to 64 bits fit here.
    params[0] = params[1] = builtin(Builtin::unsigned_int128);
  }
  std::array<Child, 3> children;
  for (std::size_t i = 0; i < params.size(); ++i) {
    Ex arg = args[i];
    if (!unsigned_call && !same_unqualified(arg.type(), params[i])) {
      diag(arg.begin, Diag::builtin_overflow_operand, params[i], arg.type());
      return invalid;
    }
    arg = convert(arg, params[i], arg.begin);
    if (!arg.ok()) return invalid;
    children[i] = at(arg, begin);
  }
  const ExprInfo info{result, ValueCategory::prvalue, 0};
  return node(ast::BuiltinCall{info, op, b_->list(children)}, begin, end, info);
}

auto Sema::constant_probe_safe(const ast::Red& expression, bool runtime_invariant) -> bool {
  if (expression.is<ast::Literal>() || expression.is<ast::ConstantProbe>()) return true;
  if (expression.is<ast::ValueInit>()) return types().is_scalar(expression.get<ast::ValueInit>().info.type);
  const auto known_scalar = [&](QualType t) {
    t = types().canonical(t);
    return Types::is_const(t) && !Types::is_volatile(t) &&
        (types().is_arithmetic(t) || types().is_enum(t) || types().is(t, Builtin::nullptr_t) ||
         (runtime_invariant && types().is_pointer(t)));
  };
  if (expression.is<ast::LocalRef>()) {
    const auto ref = expression.get<ast::LocalRef>();
    // Ordinary const locals can also have a speculative value recorded by the evaluator.
    // Pointer values in this table are from constant initialization, and nullptr_t has
    // only one value. Other local reads need not use their speculative value at runtime.
    if (runtime_invariant && !types().is(ref.info.type, Builtin::nullptr_t) &&
        !(types().is_pointer(ref.info.type) && !types().is_reference(locals_[ref.local].type))) return false;
    return known_scalar(ref.info.type) && local_values_[ref.local].has_value();
  }
  if (expression.is<ast::DeclRef>()) {
    const auto ref = expression.get<ast::DeclRef>();
    if (ref.entity == EntityId::none || ref.reference || !known_scalar(ref.info.type)) return false;
    const auto& entity = w_->entities.rec(ref.entity);
    return entity.templ == EntityId::none &&
        (entity.scope == EntityId::none || w_->entities.rec(entity.scope).kind == EntityKind::namespace_) &&
        (!runtime_invariant || q_->constant_value(ref.entity).has_value());
  }
  if (expression.is<ast::Unary>()) {
    using enum ast::UnaryOp;
    switch (expression.get<ast::Unary>().op) {
    case plus: case minus: case not_: case compl_: break;
    default: return false;
    }
  } else if (expression.is<ast::ImplicitConversion>()) {
    using enum ast::ConversionKind;
    switch (expression.get<ast::ImplicitConversion>().conversion) {
    case lvalue_to_rvalue: case integral: case floating: case floating_integral:
    case boolean: case null_pointer: case qualification: case array_to_pointer: break;
    case pointer:
      if (!runtime_invariant) return false;
      break;
    default: return false;
    }
  } else if (!expression.is<ast::Paren>() && !expression.is<ast::Binary>() &&
             !expression.is<ast::OrderedBinary>() && !expression.is<ast::Conditional>()) {
    return false;
  }
  bool safe = true;
  expression.for_each_child([&](const ast::Red& child) {
    safe = safe && constant_probe_safe(child, runtime_invariant);
  });
  return safe;
}

auto Sema::constant_probe_value(Ex operand, bool runtime_invariant) -> std::optional<ConstId> {
  while (const auto paren = as<ast::Paren>(operand))
    operand = {paren->operand.id, operand.begin + paren->operand.at, operand.end, paren->info};
  if (const auto lit = as<ast::Literal>(operand)) return lit->value;
  if (!types().is_scalar(operand.type()) && !types().is_floating(operand.type())) return std::nullopt;
  const ExprInfo boolean{builtin(Builtin::bool_), ValueCategory::prvalue, 0};
  const auto child = [&](Child expr, ExprInfo info) {
    return Ex{expr.id, operand.begin + expr.at, operand.end, info};
  };
  if (const auto logical = as<ast::OrderedBinary>(operand);
      logical && (logical->op == ast::OrderedOp::logical_and || logical->op == ast::OrderedOp::logical_or)) {
    const auto lhs = constant_probe_value(child(logical->lhs, boolean), runtime_invariant);
    if (lhs && ins().values.is<ast::VInt>(*lhs)) {
      const bool truth = ins().values.get<ast::VInt>(*lhs).bits.lo != 0;
      return truth == (logical->op == ast::OrderedOp::logical_or)
          ? lhs : constant_probe_value(child(logical->rhs, boolean), runtime_invariant);
    }
    return std::nullopt;
  }
  if (const auto cond = as<ast::Conditional>(operand)) {
    const auto choice = constant_probe_value(child(cond->condition, boolean), runtime_invariant);
    if (!choice || !ins().values.is<ast::VInt>(*choice)) return std::nullopt;
    return constant_probe_value(child(ins().values.get<ast::VInt>(*choice).bits.lo ? cond->then_ : cond->else_, cond->info),
                                runtime_invariant);
  }
  const auto root = b_->commit(ast::Child{operand.id, 0});
  if (!constant_of || !constant_probe_safe(ast::Red(ins().ast, root.id, operand.begin), runtime_invariant))
    return std::nullopt;
  const auto diagnostics = diag_count();
  auto value = constant_of(operand);
  if (diag_count() != diagnostics) {
    truncate_diags(diagnostics);
    return std::nullopt;
  }
  return value;
}

auto Sema::builtin_atomic_call(lex::AtomicBuiltin kind, std::vector<Ex>& args, u32 begin, u32 end) -> Ex {
  using enum lex::AtomicBuiltin;
  using ir::MemOrder;
  using ir::Rmw;
  const Ex invalid{NodeId::none, begin, end};
  if (!w_->target.supports(ast::NativeOperation::gnu_atomic32)) {
    diag(begin, Diag::builtin_atomic_target);
    return invalid;
  }
  const bool cas = kind == compare_exchange;
  const std::size_t arity = cas ? 6 : kind == fence ? 1 : kind == load || kind == always_lock_free ? 2 : 3;
  if (!builtin_arity(arity, args.size(), begin) ||
      std::ranges::any_of(args, [](const Ex& arg) { return !arg.ok(); })) return invalid;

  const bool dependent_args = any_dependent(args);
  if (kind == always_lock_free)
    return dependent_args ? dependent_node(args, begin, end, builtin(Builtin::bool_))
                          : builtin_atomic_lock_free(args, begin, end);

  std::optional<QualType> result;
  if (cas) result = builtin(Builtin::bool_);
  else if (kind == store || kind == fence) result = builtin(Builtin::void_);
  if (kind != fence && !dependent(args[0])) {
    args[0] = rvalue(args[0]);  // includes the ordinary array-to-pointer conversion
    if (!args[0].ok()) return invalid;
    if (!types().is_pointer(args[0].type())) {
      diag(args[0].begin, Diag::builtin_atomic_pointer);
      return invalid;
    }
  }
  if (dependent_args) {
    if (!result && types().is_pointer(args[0].type()))
      result = Types::unqualified(types().pointee(args[0].type()));
    return dependent_node(args, begin, end, result);
  }

  ast::AtomicOp operation = ast::AtomicOp::rmw;
  Rmw update = Rmw::xchg;
  bool returns_new = false;
  switch (kind) {
    case load: operation = ast::AtomicOp::load; break;
    case store: operation = ast::AtomicOp::store; break;
    case compare_exchange: operation = ast::AtomicOp::compare_exchange; break;
    case fence: operation = ast::AtomicOp::fence; break;
    case exchange: break;
    case add_fetch: returns_new = true; [[fallthrough]];
    case fetch_add: update = Rmw::add; break;
    case sub_fetch: returns_new = true; [[fallthrough]];
    case fetch_sub: update = Rmw::sub; break;
    case and_fetch: returns_new = true; [[fallthrough]];
    case fetch_and: update = Rmw::and_; break;
    case or_fetch: returns_new = true; [[fallthrough]];
    case fetch_or: update = Rmw::or_; break;
    case xor_fetch: returns_new = true; [[fallthrough]];
    case fetch_xor: update = Rmw::xor_; break;
    case always_lock_free: std::unreachable();
  }

  if (kind != fence) {
    const QualType pointee = types().canonical(types().pointee(args[0].type()));
    const QualType element = Types::unqualified(pointee);
    if (!types().is(element, Builtin::int_) && !types().is(element, Builtin::unsigned_int)) {
      diag(args[0].begin, Diag::builtin_atomic_pointer);
      return invalid;
    }
    if (kind != load && Types::is_const(pointee)) {
      diag(args[0].begin, Diag::builtin_atomic_const);
      return invalid;
    }
    if (!result) result = element;
    if (cas) {
      args[1] = rvalue(args[1]);
      if (!args[1].ok()) return invalid;
      if (!types().is_pointer(args[1].type()) ||
          !same_unqualified(types().pointee(args[1].type()), element) ||
          Types::cv(types().canonical(types().pointee(args[1].type()))) != Cv::none) {
        diag(args[1].begin, Diag::builtin_atomic_expected, element);
        return invalid;
      }
      args[3] = convert(args[3], builtin(Builtin::bool_), args[3].begin);
      if (!args[3].ok()) return invalid;
    }
    if (kind != load) {
      Ex& value = args[cas ? 2 : 1];
      value = convert(value, element, value.begin);
      if (!value.ok()) return invalid;
    }
  }

  Ex& order_arg = args[cas ? 4 : arity - 1];
  std::optional<MemOrder> order;
  if (!builtin_atomic_order(order_arg, order)) return invalid;
  if (order && ((kind == load && !ir::atomic_load_order(*order)) ||
                (kind == store && !ir::atomic_store_order(*order)))) {
    diag(order_arg.begin, Diag::builtin_atomic_order);
    return invalid;
  }
  std::optional<MemOrder> failure;
  if (cas) {
    if (!builtin_atomic_order(args[5], failure)) return invalid;
    if (failure && (!ir::atomic_load_order(*failure) ||
                    (order && !ir::atomic_failure_order(*order, *failure)))) {
      diag(args[5].begin, Diag::builtin_atomic_failure_order);
      return invalid;
    }
    if (!failure || !order) order = failure = MemOrder::seq_cst;
  }
  std::vector<Child> children;
  children.reserve(arity);
  for (const Ex& arg : args) children.push_back(at(arg, begin));
  const ExprInfo info{*result, ValueCategory::prvalue, 0};
  return node(ast::AtomicCall{info, operation, std::to_underlying(order.value_or(MemOrder::seq_cst)),
                             std::to_underlying(failure.value_or(MemOrder::relaxed)), std::to_underlying(update),
                             returns_new, b_->list(children)}, begin, end, info);
}

auto Sema::builtin_call(const lex::BuiltinFunctionInfo& b, std::vector<Ex> args, u32 begin, u32 end, u32 paren_at) -> Ex {
  using enum lex::BuiltinFunction;
  if (b.function == atomic) return builtin_atomic_call(b.atomic_op, args, begin, end);
  if (b.function == source_location) {
    if (!builtin_arity(0, args.size(), begin)) return {NodeId::none, begin, end};
    return builtin_source_location(begin, end, q_->compatibility() == ast::Compatibility::gcc ? paren_at : begin);
  }
  if (b.function == source_line) {
    if (!builtin_arity(0, args.size(), begin)) return {NodeId::none, begin, end};
    const bool gcc = q_->compatibility() == ast::Compatibility::gcc;
    const QualType type = builtin(gcc ? Builtin::int_ : Builtin::unsigned_int);
    const u32 line = source_line_at(gcc ? paren_at : begin);
    if (default_source_observer_) {
      // Constant probes retain unevaluated operands for proof, but the default's invocation
      // line is still unknown. Only an evaluated use binds the enclosing default to a line.
      if (!unevaluated) default_source_observer_->line = true;
      const ExprInfo info{type, ValueCategory::prvalue, 0};
      return node(ast::SourceLine{info, line}, begin, end, info);
    }
    return literal(type, w_->values.integer(type, line), ValueCategory::prvalue, begin, end);
  }
  if (b.function == trap || b.function == unreachable) {
    // [utility.unreachable]: both are `void` and end the path they are on.
    if (!builtin_arity(0, args.size(), begin)) return {NodeId::none, begin, end};
    ExprInfo info{builtin(Builtin::void_), ValueCategory::prvalue, 0};
    const auto op = b.function == trap ? ast::BuiltinOp::trap : ast::BuiltinOp::unreachable;
    return node(ast::BuiltinCall{info, op, b_->list({})}, begin, end, info);
  }
  if (b.function == addressof) {
    if (!builtin_arity(1, args.size(), begin)) return {NodeId::none, begin, end};
    return builtin_addressof(args.front(), begin, end);
  }
  if (b.function == strlen) {
    if (!builtin_arity(1, args.size(), begin)) return {NodeId::none, begin, end};
    QualType param = types().pointer(types().add_cv(builtin(Builtin::char_), Cv::const_));
    return builtin_value_call(ast::BuiltinOp::strlen, builtin(w_->target.size_type), param, args.front(), begin, end);
  }
  if (b.function == ctz || b.function == clzll) {
    if (!builtin_arity(1, args.size(), begin) || !args.front().ok()) return {NodeId::none, begin, end};
    const QualType param = builtin(b.function == ctz ? Builtin::unsigned_int : Builtin::unsigned_long_long);
    const auto op = b.function == ctz ? ast::BuiltinOp::ctz_gcc_zero : ast::BuiltinOp::clzll;
    Ex operand = args.front();
    if (!dependent(operand) && types().is_class(operand.type()))
      operand = exact_conversion_call(operand, param, operand.begin, /*allow_explicit=*/false).value_or(operand);
    return builtin_value_call(op, builtin(Builtin::int_), param, operand, begin, end);
  }
  if (b.function == ctzg) {
    const Ex invalid{NodeId::none, begin, end};
    const QualType result = builtin(Builtin::int_);
    if (args.size() == 2) {
      if (!args[0].ok() || !args[1].ok()) return invalid;
      // Keep dependent fallback calls in libycxx's <bit> well-formed until they are used.
      if (any_dependent(args)) return dependent_node(args, begin, end, result);
      diag(begin, Diag::builtin_ctzg_fallback);
      return invalid;
    }
    if (!builtin_arity(1, args.size(), begin) || !args.front().ok()) return invalid;
    const Ex& arg = args.front();
    if (!types().is_dependent(arg.type()) && !types().is(arg.type(), Builtin::unsigned_int)) {
      diag(arg.begin, Diag::builtin_ctzg_operand, arg.type());
      return invalid;
    }
    return builtin_value_call(ast::BuiltinOp::ctz, result, builtin(Builtin::unsigned_int), arg, begin, end);
  }
  if (b.function == constant_p) {
    if (!builtin_arity(1, args.size(), begin)) return {NodeId::none, begin, end};
    return builtin_constant_probe(args.front(), begin, end);
  }
  if (b.function == operator_new || b.function == operator_delete)
    return builtin_allocation_call(b, std::move(args), begin, end, paren_at);
  if (b.function == library)
    return builtin_library_call(*string_function_named(lex::library_function_name(b)), std::move(args), begin, end);
  if (b.function == add_overflow || b.function == mul_overflow) {
    const auto op = b.function == add_overflow ? ast::BuiltinOp::add_overflow : ast::BuiltinOp::mul_overflow;
    return builtin_overflow_call(op, args, begin, end);
  }
  if (b.function == nansl) {
    if (!builtin_arity(1, args.size(), begin) || !args.front().ok()) return {NodeId::none, begin, end};
    // The parser checked the written literal. The call's type is available independently
    // of the target's signaling NaN value.
    const ExprInfo info{builtin(Builtin::long_double), ValueCategory::prvalue, 0};
    return node(ast::BuiltinCall{info, ast::BuiltinOp::nansl, b_->list({at(args.front(), begin)})}, begin, end, info);
  }
  std::size_t want = b.function == fabs || b.function == nan ? 1 : 0;
  if (!builtin_arity(want, args.size(), begin)) return {NodeId::none, begin, end};
  if (std::ranges::any_of(args, [](const Ex& a) { return !a.ok(); })) return {NodeId::none, begin, end};
  Builtin kind = b.type == 'f' ? Builtin::float_ : b.type == 'l' ? Builtin::long_double : Builtin::double_;
  QualType t = builtin(kind);
  if (!ast::computes(w_->target[kind].format)) {
    if (b.function == fabs) {
      const Ex& arg = args.front();
      if (!types().is_dependent(arg.type()) && !ics(arg, t).viable) {
        diag(arg.begin, Diag::no_conversion, arg.type(), t);
        return {NodeId::none, begin, end};
      }
      // Instantiation reparses the selected statement with bound arguments ([stmt.if]).
      if (dependent(arg)) return dependent_node({arg}, begin, end, t);
    }
    diag(begin, Diag::extended_float_not_supported);
    return {NodeId::none, begin, end};
  }
  switch (b.function) {
    case addressof:
    case atomic:
    case source_line:
    case source_location:
    case trap:
    case unreachable:
    case strlen:
    case ctz:
    case ctzg:
    case clzll:
    case library:
    case operator_new:
    case operator_delete:
    case constant_p:
    case add_overflow:
    case mul_overflow:
    case nansl: std::unreachable();  // Handled before floating type selection.
    case inf:
    case huge_val:
      return literal(t, w_->values.floating(t, std::numeric_limits<double>::infinity()), ValueCategory::prvalue, begin, end);
    case nan: {
      // The payload is a string literal's contents read as `strtoull` reads them: decimal, octal
      // after `0`, hexadecimal after `0x`; empty is 0. It fills the significand below the quiet bit.
      Ex a = args[0];
      while (auto p = as<ast::Paren>(a)) a = {p->operand.id, a.begin + p->operand.at, a.end};
      auto lit = as<ast::Literal>(a);
      std::optional<u64> payload;
      if (auto text = lit ? w_->values.narrow_string(lit->value) : std::nullopt) {
        std::string_view s = *text;
        if (s.ends_with('\0')) s.remove_suffix(1);
        int base = s.starts_with("0x") || s.starts_with("0X") ? 16 : s.starts_with('0') ? 8 : 10;
        if (base == 16) s.remove_prefix(2);
        u64 v = 0;
        auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
        if (s.empty() || (r.ec == std::errc{} && r.ptr == s.data() + s.size())) payload = v;
      }
      if (!payload) {
        diag(args[0].begin, Diag::builtin_nan_string);
        return {NodeId::none, begin, end};
      }
      bool single = ast::float_format(ins(), w_->target, t) == ast::FloatFormat::binary32;
      u64 bits = single ? 0x7fc00000u | (*payload & 0x3fffffu) : 0x7ff8000000000000u | (*payload & 0x7ffffffffffffu);
      return literal(t, ast::float_value(ins(), w_->target, t, ast::Bits128{bits, 0}), ValueCategory::prvalue, begin, end);
    }
    case fabs: return builtin_value_call(ast::BuiltinOp::fabs, t, t, args[0], begin, end);
  }
  std::unreachable();
}

auto Sema::va_call(lex::VaBuiltin builtin_op, Ex cursor, std::optional<Ex> source, std::optional<QualType> fetch, u32 begin, u32 end) -> Ex {
  const bool starts = builtin_op == lex::VaBuiltin::start || builtin_op == lex::VaBuiltin::legacy_start;
  const Ex invalid{NodeId::none, begin, end};
  if (starts) {
    const bool variadic = lambdas_.empty() ? variadic_function_body : types().rec(lambdas_.back().call_type).variadic;
    if (!variadic) {
      diag(begin, Diag::va_start_context);
      return invalid;
    }
  }
  if (!cursor.ok() || (source && !source->ok())) return invalid;
  QualType result = builtin(Builtin::void_);
  if (fetch) {
    result = Types::unqualified(types().canonical(*fetch));
    if (!types().is_dependent(result) && !types().is_pointer(result)) {
      auto b = types().builtin_of(result);
      if (b != Builtin::int_ && b != Builtin::unsigned_int && b != Builtin::long_ &&
          b != Builtin::unsigned_long && b != Builtin::long_long && b != Builtin::unsigned_long_long &&
          b != Builtin::double_) {
        diag(begin, Diag::va_arg_type, *fetch);
        return invalid;
      }
    }
  }

  // System V: a local va_list decays to a pointer to its single record; an adjusted parameter
  // is already a pointer. The pointee's identity and cv-qualifiers must match in either case.
  // Apple arm64: the va_list is itself a `char*`, and each operation takes the address of the
  // variable that holds it, so the cursor is a pointer to a `char*` either way.
  const QualType va_list = w_->builtin_va_list();
  const bool pointer_list = w_->target.va_list == ast::VaList::char_pointer;
  const QualType pointer = types().pointer(pointer_list ? va_list : types().rec(va_list).inner);
  auto normalize = [&](Ex e) {
    if (dependent(e)) return e;
    if (pointer_list) {
      if (!e.glvalue() || types().canonical(e.type()) != types().canonical(va_list)) {
        diag(e.begin, Diag::va_list_operand, e.type());
        return Ex{NodeId::none, e.begin, e.end};
      }
      return builtin_address_of(e, e.begin, e.end, e.begin);
    }
    Ex p = rvalue(e);
    if (p.ok() && types().canonical(p.type()) != pointer) {
      diag(e.begin, Diag::va_list_operand, e.type());
      return Ex{NodeId::none, e.begin, e.end};
    }
    return p;
  };
  cursor = normalize(cursor);
  if (source) source = normalize(*source);
  if (!cursor.ok() || (source && !source->ok())) return invalid;

  std::vector<Ex> args{cursor};
  if (source) args.push_back(*source);
  if (types().is_dependent(result) || std::ranges::any_of(args, [&](Ex e) { return dependent(e); }))
    return dependent_node(args, begin, end, result);
  std::vector<ast::Child> children;
  for (Ex e : args) children.push_back(at(e, begin));
  ast::VaOp op = starts ? ast::VaOp::start :
                 fetch ? ast::VaOp::arg : source ? ast::VaOp::copy : ast::VaOp::end;
  ExprInfo info{result, ValueCategory::prvalue, 0};
  return node(ast::VariadicBuiltin{info, op, b_->list(children)}, begin, end, info);
}

auto Sema::decls_expr(std::vector<ast::DeclSema> found, bool any, NameId name, u32 begin, u32 end, bool callee, bool unqualified) -> Ex {
  std::erase_if(found, [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::namespace_; });
  // Class and enumeration names can be hidden by other declarations ([basic.lookup.general]/4).
  auto tag = [](const ast::DeclSema& d) { return d.kind == ast::DeclKind::class_ || d.kind == ast::DeclKind::enum_; };
  if (std::ranges::any_of(found, [&](const ast::DeclSema& d) { return !tag(d); })) std::erase_if(found, tag);
  if (found.empty()) {
    if (!any && callee) {
      ExprInfo info{builtin(Builtin::dependent), ValueCategory::lvalue, 0};
      return node(ast::DepName{info, name}, begin, end, info);
    }
    if (!any) undeclared(begin, name);
    return {NodeId::none, begin, end};
  }
  // Several functions: an overload set, resolved by the call ([over.match]); the latest
  // declaration of each gives its type.
  const ast::DeclSema& d = found.back();
  std::vector<ast::DeclSema> set;
  for (auto o = found.rbegin(); o != found.rend(); ++o)
    if (!std::ranges::any_of(set, [&](const ast::DeclSema& x) { return x.entity == o->entity; })) set.push_back(*o);
  bool adl = unqualified && std::ranges::all_of(set, [&](const ast::DeclSema& x) {
    if (x.kind != ast::DeclKind::function) return false;
    EntityId scope = w_->entities.rec(x.entity).scope;
    return scope == EntityId::none || w_->entities.rec(scope).kind == EntityKind::namespace_;
  });
  bool templ = std::ranges::any_of(set, [](const ast::DeclSema& x) { return (x.flags & decl_flag::template_) != 0; });
  if (set.size() > 1 || templ) {
    if (!std::ranges::all_of(set, [](const ast::DeclSema& x) { return x.kind == ast::DeclKind::function; })) {
      diag(begin, Diag::ambiguous_name, name);  // [basic.lookup.general]/1: two entities
      return {NodeId::none, begin, end};
    }
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex r = node(ast::DeclRef{info, d.entity}, begin, end, info);
    if (adl) unqualified_[std::to_underlying(r.id)] = set;
    overloads_[std::to_underlying(r.id)] = std::move(set);
    return r;
  }
  const bool immediate = d.kind == ast::DeclKind::function && (d.flags & decl_flag::consteval_);
  if (immediate) consteval_.insert(std::to_underlying(d.entity));
  if (d.kind == ast::DeclKind::function && (d.flags & decl_flag::deleted)) deleted_.insert(std::to_underlying(d.entity));
  if (is_type(d.kind)) {  // [expr.prim.id]: a type is not an expression
    diag(begin, Diag::type_as_expression, name);
    return {NodeId::none, begin, end};
  }
  if (d.kind == ast::DeclKind::enumerator) return enumerator(d.entity, d.type, begin, end);
  if (d.kind == ast::DeclKind::function && ins().names.spelling(name) == "main" &&
      w_->entities.rec(d.entity).scope == EntityId::none) {
    diag(begin, Diag::main_named);  // [basic.start.main]/3: main shall not be used
    return {NodeId::none, begin, end};
  }
  QualType dt = d.type;
  if (d.kind == ast::DeclKind::function && !(adl && callee)) {
    auto t = deduced_function(d.entity, d.type, begin);
    if (!t) return {NodeId::none, begin, end};
    dt = *t;
  }
  u8 flags = 0;
  if (immediate) {
    EntityId scope = w_->entities.rec(d.entity).scope;
    flags = immediate_reference_flags(scope == EntityId::none || w_->entities.rec(scope).kind == EntityKind::namespace_);
  }
  ExprInfo info{types().referee(dt), ValueCategory::lvalue, flags};
  bool object_reference = types().is_reference(dt) && !types().is_function(info.type);
  Ex r = node(ast::DeclRef{info, d.entity, object_reference}, begin, end, info);
  if (adl) unqualified_[std::to_underlying(r.id)] = {d};
  if (d.kind == ast::DeclKind::function)
    if (auto ds = found_defaults(d); !ds.empty()) defaults_[std::to_underlying(r.id)] = std::move(ds);
  declared_types_[std::to_underlying(r.id)] = dt;
  return r;
}

auto Sema::integer_literal(const lex::Literal& lit, u32 begin, u32 end) -> Ex {
  bool decimal = lit.base == 10;
  std::vector<Builtin> list;
  auto add = [&](std::initializer_list<Builtin> bs) { list.insert(list.end(), bs); };
  switch (lit.int_suffix) {
  case lex::IntSuffix::none:
    if (decimal) add({Builtin::int_, Builtin::long_, Builtin::long_long});
    else add({Builtin::int_, Builtin::unsigned_int, Builtin::long_, Builtin::unsigned_long, Builtin::long_long,
              Builtin::unsigned_long_long});
    break;
  case lex::IntSuffix::u: add({Builtin::unsigned_int, Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
  case lex::IntSuffix::l:
    if (decimal) add({Builtin::long_, Builtin::long_long});
    else add({Builtin::long_, Builtin::unsigned_long, Builtin::long_long, Builtin::unsigned_long_long});
    break;
  case lex::IntSuffix::ul: add({Builtin::unsigned_long, Builtin::unsigned_long_long}); break;
  case lex::IntSuffix::ll:
    if (decimal) add({Builtin::long_long});
    else add({Builtin::long_long, Builtin::unsigned_long_long});
    break;
  case lex::IntSuffix::ull: add({Builtin::unsigned_long_long}); break;
  case lex::IntSuffix::z:
  case lex::IntSuffix::uz: add({lit.int_suffix == lex::IntSuffix::z ? Builtin::long_ : Builtin::unsigned_long}); break;
  }
  Builtin chosen = Builtin::long_long;
  bool fits = false;
  for (Builtin k : list) {
    IntShape s = shape(w_->target, k);
    u64 max = s.is_signed ? (u64{1} << (s.bits - 1)) - 1 : (s.bits == 64 ? ~u64{0} : (u64{1} << s.bits) - 1);
    if (!lit.overflow && lit.value <= max) {
      chosen = k;
      fits = true;
      break;
    }
  }
  // A value no listed type holds was diagnosed by the lexer (integer_too_large), or is
  // ill-formed here; `long long` keeps analysis going.
  if (!fits) {
    if (!lit.overflow) diag(begin, Diag::integer_literal_too_large);  // [lex.icon]/3
    chosen = list.back();
  }
  QualType t = builtin(chosen);
  return literal(t, w_->values.integer(t, lit.value), ValueCategory::prvalue, begin, end);
}

auto Sema::integer_user_literal(NameId name, const lex::Literal& lit, u32 begin, u32 end) -> Ex {
  Ex callee = name_expr(name, begin, end, true);
  if (!callee.ok()) return callee;
  Ex fail{NodeId::none, begin, end};
  if (as<ast::DepName>(callee)) {
    undeclared(begin, name);
    return fail;
  }
  bool cooked = false, raw = false, numeric_template = false;
  auto record = [&](QualType type) {
    auto form = literal_operator_form(type);
    cooked |= form == LiteralOperatorForm::integer;
    raw |= form == LiteralOperatorForm::raw;
  };
  if (auto it = overloads_.find(std::to_underlying(callee.id)); it != overloads_.end()) {
    for (const ast::DeclSema& d : it->second) {
      if (d.flags & decl_flag::template_) numeric_template |= numeric_literal_template(ins().seqs.elements(d.template_params));
      else record(d.type);
    }
  } else {
    record(callee.type());
  }
  if (!cooked) {
    if (raw && numeric_template) diag(begin, Diag::integer_literal_operator_conflict);
    else if (raw || numeric_template) diag(begin, Diag::integer_literal_operator_unsupported);
    else diag(begin, Diag::integer_literal_operator_missing, name);
    return fail;
  }
  if (lit.overflow) {
    diag(begin, Diag::integer_literal_too_large);
    return fail;
  }
  lex::Literal arg;
  arg.base = lit.base;
  arg.int_suffix = lex::IntSuffix::ull;
  arg.value = lit.value;
  return call(callee, {integer_literal(arg, begin, end)}, end, begin);
}

auto Sema::pretty_function(u32 begin, u32 end) -> Ex {
  if (!in_function_body || !lambdas_.empty() || current_function == EntityId::none || !namespace_scoped(current_function)) {
    diag(begin, Diag::pretty_function_context);
    return {NodeId::none, begin, end};
  }
  const EntityRec& function = w_->entities.rec(current_function);
  // BodyAnalysis replays the expression with the selected specialization as current_function.
  // The unbound body knows neither the bytes nor the resulting array bound.
  if (function.kind == EntityKind::function_template)
    return dependent_node({}, begin, end, std::nullopt, ValueCategory::lvalue, expr_flag::type_dependent);

  const TypeRec signature = types().rec(function.type);
  std::string spelling;
  // Reading the declaration avoids asking for the return type the body is still deducing.
  bool have_result = true;
  if (types().has_placeholder(signature.inner)) spelling = types().spell(signature.inner);
  else have_result = append_pretty_type(spelling, signature.inner);
  spelling += ' ';
  if (!have_result || !append_pretty_name(spelling, current_function, /*function_scope=*/true) ||
      !append_pretty_parameters(spelling, signature)) {
    diag(begin, Diag::pretty_function_spelling);
    return {NodeId::none, begin, end};
  }
  if (function.templ != EntityId::none) {
    // Overloads can have the same substituted signature and arguments. Discriminate their
    // patterns using the declaration's semantic key, independent of its instantiation point.
    const EntityRec& pattern = w_->entities.rec(function.templ);
    const auto identity = ast::mix(ast::mix(types().fp(types().canonical(pattern.type)), pattern.head), pattern.constraints);
    spelling += std::format(" [template {:016x}{:016x}]", identity.hi, identity.lo);
  }
  std::vector<u32> units;
  units.reserve(spelling.size() + 1);
  for (unsigned char byte : spelling) units.push_back(byte);
  return string_lvalue(Builtin::char_, std::move(units), begin, end);
}

auto Sema::append_pretty_args(std::string& text, std::span<const TemplateArg> args) -> bool {
  const char* separator = "<";
  for (const TemplateArg& arg : args) {
    text += separator;
    separator = ", ";
    switch (arg.kind) {
      case TemplateArgKind::type:
        if (!append_pretty_type(text, arg.type)) return false;
        break;
      case TemplateArgKind::pack:
        text += "pack";
        if (!append_pretty_args(text, ins().seqs.elements(arg.elems))) return false;
        break;
      case TemplateArgKind::template_:
        text += "template<";
        if (arg.templ == EntityId::none || !append_pretty_name(text, arg.templ)) return false;
        text += '>';
        break;
      case TemplateArgKind::value: {
        text += "value<";
        if (!append_pretty_type(text, arg.type)) return false;
        const auto& values = ins().values;
        std::optional<ast::Bits128> bits;
        if (values.is<ast::VInt>(arg.value)) bits = values.get<ast::VInt>(arg.value).bits;
        if (values.is<ast::VFloat>(arg.value)) bits = values.get<ast::VFloat>(arg.value).bits;
        if (bits) text += std::format(", 0x{:016x}{:016x}>", bits->hi, bits->lo);
        else if (values.is<ast::VNullptr>(arg.value)) text += ", nullptr>";
        else return false;
        break;
      }
    }
  }
  if (args.empty()) text += '<';
  text += '>';
  return true;
}

auto Sema::append_pretty_type(std::string& text, QualType t) -> bool {
  t = types().canonical(t);
  const TypeRec type = types().rec(t);
  if (const Cv cv = Types::cv(t); cv != Cv::none) {
    text += pretty_cv(cv);
    text += ' ';
  }
  switch (type.kind) {
    case TypeKind::builtin:
      if (type.builtin <= Builtin::long_double) text += sema::spelling(type.builtin);
      else if (type.builtin == Builtin::meta_info) text += "std::meta::info";
      else return false;
      return true;
    case TypeKind::class_:
    case TypeKind::enum_:
      return append_pretty_name(text, type.entity);
    case TypeKind::function:
      text += "function<";
      if (!append_pretty_type(text, type.inner) || !append_pretty_parameters(text, type)) return false;
      break;
    case TypeKind::pointer:
      text += "pointer<";
      if (!append_pretty_type(text, type.inner)) return false;
      break;
    case TypeKind::member_pointer:
      text += "member_pointer<";
      if (!append_pretty_type(text, type.member_owner)) return false;
      text += ", ";
      if (!append_pretty_type(text, type.inner)) return false;
      break;
    case TypeKind::reference:
      text += type.rvalue_ref ? "rvalue_reference<" : "lvalue_reference<";
      if (!append_pretty_type(text, type.inner)) return false;
      break;
    case TypeKind::array:
      text += "array<";
      if (type.bound_value != ConstId::none) {
        if (!ins().values.is<ast::VInt>(type.bound_value)) return false;
        text += std::to_string(type.bound);
      }
      text += ", ";
      if (!append_pretty_type(text, type.inner)) return false;
      break;
    default:
      return false;
  }
  text += '>';
  return true;
}

auto Sema::address_matches(const AddressSet& set, std::optional<QualType> fn) -> std::vector<AddressMatch> {
  const auto ex = explicit_args_.find(std::to_underlying(set.name.id));
  const bool explicit_id = ex != explicit_args_.end();
  const std::span<const TemplateArg> args = explicit_id ? std::span<const TemplateArg>(ex->second) : std::span<const TemplateArg>();
  const auto target = fn.transform([&](QualType t) { return types().canonical(t); });
  auto matches_target = [&](QualType type) {
    return !target || types().canonical(type) == *target || types().function_pointer_conversion(type, *target);
  };
  std::vector<AddressMatch> found;
  for (const ast::DeclSema& d : *set.decls) {
    if (!(d.flags & decl_flag::template_)) {
      if (explicit_id || !constraints_satisfied(d)) continue;
      const auto ft = types().has_placeholder(types().rec(d.type).inner) ? q_->deduced_type(d.entity) : std::optional(d.type);
      if (ft && matches_target(*ft)) found.push_back({d.entity, *ft, &d});
      continue;
    }
    Templates t = templates();
    // A pattern whose exception specification is decided per specialization is written
    // potentially throwing: deduce from the target without its `noexcept`, and match the
    // specialization's own type against the target below.
    auto deduced = t.deduce_declaration(d, fn, args, true);
    if (!deduced && fn && types().rec(*fn).fn_noexcept) {
      const TypeRec r = types().rec(*fn);
      deduced = t.deduce_declaration(d, types().function(r.inner, r.params, r.variadic, r.linkage, r.fn_cv, r.fn_ref), args, true);
    }
    if (!deduced || !constraints_satisfied(d, *deduced)) continue;
    auto ft = t.specialization_type(d, *deduced);
    if (!ft) continue;
    EntityId f = specialization(d, *deduced, *ft);
    if (types().has_placeholder(types().rec(*ft).inner)) ft = q_->deduced_type(f);
    // Its exception specification is needed only once it is selected (`overload_address`), so
    // its written, potentially-throwing form may stand for a non-throwing one here.
    if (ft && (matches_target(*ft) || (target && types().rec(*target).fn_noexcept && matches_target(with_noexcept(*ft)))))
      found.push_back({f, *ft, &d});
  }
  auto better = [&](const AddressMatch& a, const AddressMatch& b) {
    const bool at = (a.decl->flags & decl_flag::template_) != 0;
    const bool bt = (b.decl->flags & decl_flag::template_) != 0;
    if (at != bt) return !at;
    Templates t = templates();
    if (at) {
      auto [ab, ba] = t.address_ordering(*a.decl, *b.decl);
      if (ab != ba) return ab;
      if (!ab || !equivalent_for_constraints(*a.decl, *b.decl)) return false;
    }
    return t.subsumes(a.decl->constraints, b.decl->constraints) && !t.subsumes(b.decl->constraints, a.decl->constraints);
  };
  std::vector<AddressMatch> remaining;
  for (const AddressMatch& a : found)
    if (std::ranges::none_of(found, [&](const AddressMatch& b) { return &a != &b && better(b, a); }))
      remaining.push_back(a);
  return remaining;
}

auto Sema::resolve_address(Ex e, const AddressSet& set, std::optional<QualType> fn, u32 at_) -> Ex {
  auto found = address_matches(set, fn);
  NameId name = w_->entities.rec(set.decls->front().entity).name;
  Ex fail{NodeId::none, e.begin, e.end};
  if (found.empty()) {
    if (fn) diag(at_, Diag::overload_address_none, name, *fn);
    else diag(at_, Diag::overload_address, name);
    return fail;
  }
  if (found.size() != 1) {
    diag(at_, Diag::overload_address_ambiguous, name);
    return fail;
  }
  AddressMatch match = found.front();
  // The selected specialization's exception specification ([except.spec]/13): part of its type
  // and identity, as when it is called, and the target must accept it ([conv.fctptr]).
  if (match.decl->flags & decl_flag::template_) {
    auto resolved = q_->replay_member_exception(*this, *match.decl, match.entity, match.type);
    if (!resolved) return fail;
    if (*resolved != match.type) {
      match.type = *resolved;
      match.entity = specialization(*match.decl, w_->entities.rec(match.entity).args, match.type);
    }
    if (fn && types().canonical(match.type) != types().canonical(*fn) && !types().function_pointer_conversion(match.type, *fn)) {
      diag(at_, Diag::overload_address_none, name, *fn);
      return fail;
    }
  }
  if (auto it = bound_.find(std::to_underlying(set.name.id)); it != bound_.end()) {
    const Bound& member = it->second;
    if (!check_access(member.cls, match.decl->entity, name, member.name_at, member.naming)) return fail;
  }
  if (match.decl->flags & decl_flag::deleted) {
    diag(set.name.begin, Diag::deleted_function, name);
    return fail;
  }
  // Taking a specialization's address demands its definition ([temp.inst]).
  if (match.decl->flags & decl_flag::template_)
    demands.push_back({match.entity, ast::Loc{item_, loc_base_ + set.name.begin}});
  const bool immediate = (match.decl->flags & decl_flag::consteval_) != 0 ||
      (!unevaluated && (match.decl->flags & (decl_flag::template_ | decl_flag::constexpr_)) ==
                           (decl_flag::template_ | decl_flag::constexpr_) && q_->is_immediate_function(match.entity));
  if (immediate) consteval_.insert(std::to_underlying(match.entity));
  ExprInfo info{match.type, ValueCategory::lvalue, immediate_reference_flags(immediate)};
  Ex r = node(ast::DeclRef{info, match.entity}, set.name.begin, set.name.end, info);
  if (!set.address) return {r.id, e.begin, e.end, r.info};
  ExprInfo pi{types().pointer(match.type), ValueCategory::prvalue, 0};
  return node(ast::Unary{pi, ast::UnaryOp::address_of, at(r, e.begin)}, e.begin, e.end, pi);
}

auto Sema::rvalue(Ex e) -> Ex {
  if (auto c = e.ok() ? const_captures_.find(std::to_underlying(e.id)) : const_captures_.end(); c != const_captures_.end())
    return literal(Types::unqualified(e.type()), c->second, ValueCategory::prvalue, e.begin, e.end);
  if (auto set = address_set(e)) {
    diag(e.begin, Diag::overload_address, w_->entities.rec(set->decls->front().entity).name);
    return {NodeId::none, e.begin, e.end};
  }
  if (is_bound_member(e)) {
    diag(e.begin, Diag::bound_member_not_called, w_->entities.rec(as<ast::Member>(e)->member).name);
    return {NodeId::none, e.begin, e.end};
  }
  if (!e.ok() || !e.glvalue() || dependent(e)) return e;
  QualType t = e.type();
  // A static data member usable in constant expressions is read as its value, which is no
  // odr-use ([basic.def.odr]), so it needs no definition; through an object (`a.x`, the comma
  // `member` built), the object is still evaluated.
  if (auto v = static_constant(e)) return literal(Types::unqualified(t), *v, ValueCategory::prvalue, e.begin, e.end);
  if (!unevaluated && is_data_member_pointer(t) && object_cv(t) == Cv::const_) {
    NodeId id = e.id;
    while (ast::kind_of(id) == ast::Nodes::index_of<ast::Paren>()) id = b_->get<ast::Paren>(id).operand.id;
    Ex object{id, e.begin, e.end, e.info};
    std::optional<ConstId> v;
    if (auto local = as<ast::LocalRef>(object)) v = local_values_[local->local];
    if (auto decl = as<ast::DeclRef>(object); decl && !decl->reference) v = q_->constant_value(decl->entity);
    if (v && ins().values.is<ast::VMemberPtr>(*v))
      return literal(object_unqualified(t), *v, ValueCategory::prvalue, e.begin, e.end);
  }
  if (auto c = as<ast::OrderedBinary>(e); c && c->op == ast::OrderedOp::comma && types().is_integral(t)) {
    Ex rhs{c->rhs.id, e.begin + c->rhs.at, e.end, c->info};
    if (auto v = static_constant(rhs)) {
      Ex lit = literal(Types::unqualified(t), *v, ValueCategory::prvalue, rhs.begin, rhs.end);
      ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
      return node(ast::OrderedBinary{info, ast::OrderedOp::comma, c->lhs, at(lit, e.begin)}, e.begin, e.end, info);
    }
  }
  if (types().is_array(t)) {
    QualType p = types().pointer(types().rec(t).inner);
    return convert_node(e, ast::ConversionKind::array_to_pointer, p);
  }
  if (types().is_function(t)) return convert_node(e, ast::ConversionKind::function_to_pointer, types().pointer(t));
  if (unevaluated == 0 && !require_supported_virtual_layout(t, e.begin)) return {NodeId::none, e.begin, e.end};
  return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, Types::unqualified(t));
}

auto Sema::decay_copy(Ex operand, u32 begin, u32 end, bool replay_body) -> Ex {
  if (!operand.ok()) return operand;
  const auto supported = [&](QualType type) {
    return (types().is_scalar(type) && !types().is_member_pointer(type)) || types().is_class(type);
  };
  if (dependent(operand)) {
    // [temp.dep.expr]: a value-dependent operand can still determine the prvalue type.
    // [temp.deduct.call]: decay the operand's type without evaluating or converting it.
    std::optional<QualType> type;
    if (!types().is_dependent(operand.type())) type = ast::adjust_parameter(ins(), types().canonical(operand.type()));
    if (!type || supported(*type)) {
      // Instantiation analyses the concrete copy: a function template body or a
      // requires-expression is replayed with its arguments ([temp.inst], [temp.constr.atomic]).
      // This node supplies no canonical template argument value.
      u8 flags = expr_flag::unrepresented_value;
      if (replay_body) flags |= expr_flag::deferred_decay_copy;
      if (!type) flags |= expr_flag::type_dependent;
      return dependent_node({operand}, begin, end, type, ValueCategory::prvalue, flags);
    }
  } else if (QualType decayed = ast::adjust_parameter(ins(), types().canonical(operand.type())); types().is_class(decayed)) {
    // [expr.type.conv]/2: one parenthesized expression is the cast `(T)e`, a direct-initialization
    // ([expr.static.cast]/4); a braced one with an element of the class itself initializes from
    // it ([dcl.init.list]/3.2). A prvalue of the class initializes the result itself
    // ([dcl.init.general]/16.6.1).
    return functional_cast(decayed, {operand}, begin, end);
  } else {
    if (types().is_array(operand.type()) && !operand.glvalue())
      operand = materialize(operand, operand.type(), false);
    Ex value = rvalue(operand);
    if (!value.ok()) return value;
    QualType type = Types::unqualified(value.type());
    if (supported(type)) {
      if (value.info.bit_width)
        return convert_node(value, ast::ConversionKind::integral, type);
      return convert(value, type, begin);
    }
  }
  diag(begin, Diag::not_supported, lex::Tk::kw_auto);
  return {NodeId::none, operand.begin, operand.end};
}

auto Sema::convert(Ex e, QualType to, u32 at_) -> Ex {
  if (!e.ok()) return e;
  if (dependent(e) || types().is_dependent(to)) {
    if (types().is_reference(to)) return bind_reference(e, to, at_);
    return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(Types::unqualified(to)));
  }
  if (types().is_reference(to)) return bind_reference(e, to, at_);
  e = overload_address(e, to, at_);
  if (!e.ok()) return e;
  QualType target = Types::unqualified(to);
  if (auto inv = closure_invoker(e.type(), target)) {
    Ex p = invoker_pointer(e, *inv);
    if (!p.ok() || types().canonical(p.type()) == types().canonical(target)) return p;
    return convert_node(p, ast::ConversionKind::qualification, target);  // [conv.fctptr]
  }
  if (auto selected = generic_invoker(e.type(), target, at_, unevaluated == 0)) {
    const auto function = define_generic_invoker(*selected, at_);
    Ex p = invoker_pointer(e, {function, selected->type}, selected->head);
    if (!p.ok() || types().canonical(p.type()) == types().canonical(target)) return p;
    return convert_node(p, ast::ConversionKind::qualification, target);  // [conv.fctptr]
  }
  if (types().is_array(target)) {
    if (auto r = string_init(target, e)) return *r;
    diag(at_, Diag::array_initializer);  // [dcl.init.general]/16.5: a list or a string literal
    return {NodeId::none, e.begin, e.end};
  }
  // A class object is copy-initialized from an expression of its own class: a glvalue through
  // the (trivial, implicit) copy constructor, a prvalue directly ([dcl.init.general]/17.6).
  if (types().is_class(target)) {
    if (!require_complete(target, at_) || !require_concrete_object(target, at_) ||
        !require_supported_virtual_layout(target, at_)) return {NodeId::none, e.begin, e.end};
    if (initializes(target, e) && !e.glvalue()) return e;  // the prvalue initializes it directly
    const ClassRec* cr = class_rec(types().class_of(target));
    // [over.match.copy]: from another class, a conversion function of the source can initialize
    // it. Converting constructors compete with them only where none is viable here (#1724).
    if (types().is_class(e.type()) && !initializes(target, e) && !base_depth(e.type(), target))
      if (auto choice = select_conversion(e, target, at_)) {
        const auto constructor = cr && cr->user_ctors ? constructor_ics(std::span(&e, 1), target, Construction::copy)
                                                      : std::optional(Ics{});
        if (constructor && !constructor->viable) return call_conversion(e, *choice, to, at_);
      }
    // A copy through a constructor that is not trivial, or one converting another type
    // ([class.conv.ctor]). An implicit copy constructor does not take a volatile object.
    if (cr && (cr->user_ctors || (initializes(target, e) && Types::is_volatile(types().canonical(e.type()))) || cr->copy_ctor != EntityId::none || cr->copy_deleted ||
               cr->defaulted_copy != CopyState::ordinary))
      return construct(target, {e}, e.begin, e.end, Construction::copy);
    if (!initializes(target, e)) {
      // A base class object copied from a derived class's ([dcl.init.general]/17.6.2): from its
      // base class subobject.
      if (base_depth(e.type(), target) && !Types::is_volatile(types().canonical(e.type()))) {
        if (!e.glvalue()) e = materialize(e, Types::unqualified(e.type()), false);
        Ex b = to_base(e, types().class_of(target), at_);
        if (!b.ok()) return b;
        return convert_node(b, ast::ConversionKind::lvalue_to_rvalue, target);
      }
      diag(at_, Diag::no_conversion, e.type(), to);
      return {NodeId::none, e.begin, e.end};
    }
    return convert_node(e, ast::ConversionKind::lvalue_to_rvalue, target);
  }
  // [over.match.conv]: a class object converts to a non-class type through a conversion function.
  if (types().is_class(e.type()))
    if (auto choice = select_conversion(e, target, at_)) return call_conversion(e, *choice, to, at_);
  if (types().is_pointer(target) && is_null_pointer_constant(e))
    return convert_node(e, ast::ConversionKind::null_pointer, target);
  const bool member_target = types().is_member_pointer(target);
  const bool data_member_target = member_target && !types().is_function(types().rec(target).inner);
  if (member_target && is_null_pointer_constant(e)) {
    if (data_member_target) return null_member_pointer_conversion(e, target);
    diag(at_, Diag::member_pointer_values);
    return {NodeId::none, e.begin, e.end};
  }
  e = rvalue(e);
  if (!e.ok()) return e;
  QualType from = e.type();
  if (data_member_target && types().is(from, Builtin::nullptr_t))
    return null_member_pointer_conversion(e, target);
  if ((member_target && types().is(from, Builtin::nullptr_t)) ||
      (types().is(target, Builtin::bool_) && types().is_member_pointer(from))) {
    diag(at_, Diag::member_pointer_values);
    return {NodeId::none, e.begin, e.end};
  }
  // A std::nullptr_t object's value is a null pointer constant too ([conv.ptr]/1).
  if (e.ok() && types().is_pointer(target) && types().is(Types::unqualified(from), Builtin::nullptr_t))
    return convert_node(e, ast::ConversionKind::null_pointer, target);
  // [dcl.enum]: an enumeration is initialized only from its own type; an unscoped one converts
  // to an integer or bool ([conv.integral], [conv.bool]).
  if (types().is_enum(target)) {
    if (e.ok() && types().canonical(Types::unqualified(from)) == types().canonical(target)) return e;
    if (e.ok()) diag(at_, Diag::no_conversion, from, to);
    return {NodeId::none, e.begin, e.end};
  }
  if (e.ok() && is_unscoped_enum(from) && types().is_arithmetic(target)) return arithmetic_conversion(e, target);
  if (types().is_void(from)) {
    diag(at_, Diag::void_value);
    return {NodeId::none, e.begin, e.end};
  }
  if (from == target || (data_member_target && same_unqualified(from, target))) return e;
  if (types().is_arithmetic(from) && types().is_arithmetic(target)) return arithmetic_conversion(e, target);
  if (types().is(target, Builtin::bool_) && (types().is_pointer(from) || types().is(from, Builtin::nullptr_t))) {
    if (types().is_pointer(from)) return convert_node(e, ast::ConversionKind::boolean, target);
  }
  if (types().is_pointer(from) && types().is_pointer(target) &&
      (qualification_convertible(from, target) || function_indirection_convertible(from, target)))
    return convert_node(e, ast::ConversionKind::qualification, target);
  if (types().is_pointer(from) && types().is_pointer(target) && pointer_convertible(from, target))
    return convert_node(e, ast::ConversionKind::pointer, target);
  // [conv.ptr]/3: a pointer to a derived class to one to its base class.
  if (types().is_pointer(from) && types().is_pointer(target) && types().is_class(types().pointee(from)) &&
      types().is_class(types().pointee(target)) && types().class_of(types().pointee(from)) != types().class_of(types().pointee(target)) &&
      cv_within(Types::cv(types().pointee(from)), Types::cv(types().pointee(target))) && is_complete(types().pointee(from))) {
    int paths = 0;
    if (base_path(types().class_of(types().pointee(from)), types().class_of(types().pointee(target)), paths)) {
      Ex b = to_base(e, types().class_of(types().pointee(target)), at_);
      if (!b.ok() || types().canonical(b.type()) == types().canonical(target)) return b;
      return convert_node(b, ast::ConversionKind::qualification, target);
    }
  }
  diag(at_, Diag::no_conversion, from, to);
  return {NodeId::none, e.begin, e.end};
}

auto Sema::cast_base_to_derived(Ex e, QualType target, bool pointer, u32 at_) -> std::optional<Ex> {
  QualType from = pointer ? types().pointee(e.type()) : e.type();
  QualType to = pointer ? types().pointee(target) : target;
  if (!types().is_class(from) || !types().is_class(to) || same_unqualified(from, to) ||
      !cv_within(object_cv(from), object_cv(to)) || !is_complete(to))
    return std::nullopt;
  EntityId d = types().class_of(to), b = types().class_of(from);
  int paths = 0;
  auto path = base_path(d, b, paths);
  if (!path) return std::nullopt;
  if (!check_base_path(to, b, path, paths, at_)) return Ex{NodeId::none, e.begin, e.end};
  if (!require_supported_virtual_layout(to, at_)) return Ex{NodeId::none, e.begin, e.end};
  // Build the conversions in evaluation order, starting with B and undoing one edge at a time.
  for (std::size_t i = path->size(); i-- > 0;) {
    QualType result = target;
    if (i != 0) {
      QualType parent = types().add_cv(types().class_type((*path)[i - 1]), object_cv(to));
      result = pointer ? types().pointer(parent) : parent;
    }
    ExprInfo info{result, e.cat(), 0};
    e = node(ast::ImplicitConversion{info, ast::ConversionKind::base_to_derived, (*path)[i], at(e, e.begin)},
             e.begin, e.end, info);
  }
  return e;
}

auto Sema::select_conversion(const Ex& e, QualType target, u32 at_) -> std::optional<ConversionChoice> {
  if (!e.ok() || dependent(e) || types().is_dependent(target) || types().is_reference(target)) return std::nullopt;
  const QualType source = e.type();
  if (!types().is_class(source) || types().is_dependent(source) || !is_complete(source)) return std::nullopt;
  const EntityId cls = types().class_of(source);
  const ClassRec* record = class_rec(cls);
  if (!record || !record->bases.empty()) return std::nullopt;
  const auto members = member_functions(cls, ins().names.intern(lex::conversion_function_name));
  if (members.empty()) return std::nullopt;
  const bool class_target = types().is_class(target);
  const Cv cv = object_cv(source);
  const bool rvalue = e.cat() != ValueCategory::lvalue;
  struct Viable {
    const MemberDeclRec* member;
    std::vector<TemplateArg> args;
    const ast::DeclSema* pattern;
    QualType type;
    Ics object;
    Ics second;
  };
  std::deque<ast::DeclSema> patterns;
  std::vector<Viable> viable;
  Templates t = templates_for_use(at_);
  for (const MemberDeclRec* m : members) {
    if (m->is_explicit) continue;
    QualType type = m->type;
    std::vector<TemplateArg> args;
    const ast::DeclSema* pattern = nullptr;
    if (m->is_template) {
      auto tmpl = q_->template_decl(m->entity);
      if (!tmpl) continue;
      const ast::DeclSema& td = patterns.emplace_back(*tmpl);
      auto targs = t.deduce_conversion(td, target);
      if (!targs) continue;
      if (td.constraints != ConstId::none) {  // [temp.deduct.general]/5
        std::vector<ast::TemplateParamSema> ps = ins().seqs.elements(td.template_params);
        auto nf = t.substitute_constraint(td.constraints, Bindings::of(ps.empty() ? 0 : ps[0].depth, *targs));
        auto sat = nf ? q_->satisfied(*nf) : std::optional<bool>(false);
        if (sat && !*sat) continue;
      }
      auto st = t.specialization_type(td, *targs);
      if (!st) continue;
      type = *st;
      args = std::move(*targs);
      pattern = &td;
    } else if (m->ineligible || !trailing_constraints_satisfied(m->constraints)) {
      continue;
    }
    const TypeRec fn = types().rec(type);
    const Ics object = implicit_object_rank(fn, cv, rvalue);
    if (!object.viable) continue;
    const ExprInfo result = result_info(fn.inner);
    Ics second;
    if (class_target) {
      if (same_unqualified(result.type, target)) {
        second = {true, 0, 0};
      } else if (int d = base_depth(result.type, target)) {
        second = {true, 2, 0};
        second.base_depth = d;
      }
    } else if (!types().is_class(result.type)) {  // a standard conversion sequence only
      second = ics({e.id, e.begin, e.end, result}, target);
    }
    if (!second.viable) continue;
    viable.push_back({m, std::move(args), pattern, type, object, second});
  }
  if (viable.empty()) return std::nullopt;
  // [over.match.best.general]/2: the implicit object parameter's conversion, then the second
  // standard conversion sequence (/2.2), then a non-template over a template specialization,
  // then the more specialized template (/2.5, [temp.deduct.partial]/3.2), then the more
  // constrained (/2.6, [temp.func.order]/6).
  auto better = [&](const Viable& a, const Viable& b) {
    if (b.object < a.object) return false;
    if (a.object < b.object) return true;
    if (b.second < a.second) return false;
    if (a.second < b.second) return true;
    if (!a.pattern != !b.pattern) return !a.pattern;
    const QualType ra = types().canonical(types().rec(a.type).inner);
    const QualType rb = types().canonical(types().rec(b.type).inner);
    if (!a.pattern) {
      const ConstId ca = a.member->constraints, cb = b.member->constraints;
      if ((ca == ConstId::none && cb == ConstId::none) || !same_parameter_types(a.type, b.type) || ra != rb) return false;
      return t.subsumes(ca, cb) && !t.subsumes(cb, ca);
    }
    auto [a12, a21] = t.conversion_ordering(*a.pattern, *b.pattern);
    if (a12 != a21) return a12;
    if (!a12 || !equivalent_for_constraints(*a.pattern, *b.pattern) ||
        types().canonical(types().rec(a.pattern->type).inner) != types().canonical(types().rec(b.pattern->type).inner))
      return false;
    const ConstId ca = a.pattern->constraints, cb = b.pattern->constraints;
    return t.subsumes(ca, cb) && !t.subsumes(cb, ca);
  };
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < viable.size(); ++i) {
    bool wins = true;
    for (std::size_t j = 0; j < viable.size() && wins; ++j)
      if (i != j) wins = better(viable[i], viable[j]);
    if (wins) best = i;
  }
  if (!best) return ConversionChoice{.member = nullptr, .args = {}, .ambiguous = true};
  return ConversionChoice{.member = viable[*best].member, .args = std::move(viable[*best].args), .ambiguous = false};
}

}  // namespace ycxx::sema
