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

auto ItemAnalysis::same_friend_parameter_type(QualType a, QualType b, const FriendHeadDepth& head) -> std::optional<bool> {
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

auto ItemAnalysis::templated_friend(const DeclSpec& spec, const parse::Declarator& d, bool own_head) -> void {
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

auto ItemAnalysis::member_alias(const ast::DeclSema& c, NameId name, u32 at, QualType type, bool alias_template) -> bool {
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

auto ItemAnalysis::defaulted_comparison_ok(const parse::Declarator& d, QualType cls, bool member) -> bool {
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

auto ItemAnalysis::define_namespace_member(const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition, u32 ordinal, bool explicit_lang) -> bool {
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
      target = owner;
    }
    if (fn) {
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
      if (explicit_lang && ((ds->flags ^ sem.flags) & sema::decl_flag::lang_c)) {
        linkage_conflict = true;
        continue;
      }
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
  if (fn && type != s_.types().canonical(mem.type)) sem.type = mem.type;
  if (c_namespace && (mem.flags & sema::decl_flag::lang_c)) {
    sem.type = mem.type;
    sem.flags |= sema::decl_flag::lang_c;
    p_->qualified_c_functions_.emplace(std::pair{k_, ordinal}, std::move(*c_namespace));
  }
  return true;
}

auto ItemAnalysis::out_of_class_special(QualType cls, u32 next, u16 flags) -> void {
  EntityId c = s_.types().class_of(cls);
  for (u32 j = 0; j < next; ++j) ps_.bump();
  bool ctor = !ps_.is(Tk::tilde);
  const bool specialized_destructor = explicit_ && !ctor;
  const bool definition = it_->kind == ItemKind::function;
  const bool late_defaulted = p_->defaulted_body(k_);
  u32 name_at = ps_.pos();
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
  if ((flags & (sema::decl_flag::constexpr_ | sema::decl_flag::consteval_)) && !finish_member_definition_declarator()) return bad();
  if (specialized_destructor && !m->is_virtual) {
    if (!check_destructor_specialization(*cr, *m, fs, name_at, definition)) return bad();
  } else if (mem->flags & sema::decl_flag::definition) {
    s_.diag(name_at, Diag::redefinition, m->name);
    return bad();
  }
  if (late_defaulted) {
    if (auto previous = p_->declaration(m->entity, k_); previous && (previous->flags & sema::decl_flag::definition)) {
      s_.diag(name_at, Diag::redefinition, m->name);
      return bad();
    }
    const auto ordinary = fs.params.empty();
    const auto copy_or_move = [&] {
      if (!ctor || fs.params.size() != 1) return false;
      const QualType parameter = fs.params.front().type;
      if (!s_.types().is_reference(parameter)) return false;
      const auto referee = s_.types().canonical(s_.types().referee(parameter));
      return ast::without_cv(referee) == s_.types().canonical(cls) &&
             !sema::Types::is_volatile(referee) &&
             (!s_.types().rec(parameter).rvalue_ref || !sema::Types::is_const(referee));
    }();
    if (!definition || template_ || explicit_ || inst_ || m->is_template || cr->union_ || cr->polymorphic ||
        !cr->bases.empty() || (!ordinary && !copy_or_move) || fs.variadic || fs.cv != sema::Cv::none ||
        fs.ref != ast::RefQual::none ||
        !Program::encloses(p_->ns_of(k_), p_->enclosing_namespace_path(owner)) ||
        std::ranges::any_of(cr->fields, [&](const sema::FieldRec& field) {
          const QualType element = s_.types().innermost_array_element(field.type);
          return !s_.types().is_scalar(element) ||
                 (ctor && ordinary && !field.dmi && sema::Types::is_const(element));
        })) {
      const auto error = !ctor ? Diag::defaulted_destructor_out_of_class
                       : ordinary ? Diag::defaulted_default_constructor_definition : Diag::defaulted_copy_declaration;
      s_.diag(name_at, error);
      return bad();
    }
    // [dcl.fct.def.default]: a later defaulted definition is user-provided and uses
    // the earlier declaration's exception specification.
    if (ctor) {
      if (nonthrowing != s_.types().rec(m->type).fn_noexcept) {
        s_.diag(name_at, Diag::conflicting_declaration, m->name);
        return bad();
      }
    } else {
      ps_.exception_specifier(fs, ok);
      if (!ok) return bad();
      const bool definition_noexcept = !fs.has_noexcept || ps_.noexcept_value(fs.noexcept_condition, ok);
      if (!ok) return bad();
      if (cr->destructor_noexcept != definition_noexcept) {
        s_.diag(name_at, Diag::conflicting_declaration, m->name);
        return bad();
      }
    }
    if (!finish_member_definition_declarator()) return bad();
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

auto ItemAnalysis::check_destructor_specialization(const sema::ClassRec& cls, const sema::MemberDeclRec& member, Parser::Suffix& suffix, u32 name_at, bool definition) -> bool {
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
  if (definition) return finish_member_definition_declarator();
  ps_.skip_attributes();
  return ps_.expect(Tk::semi);
}

auto ItemAnalysis::namespace_definition() -> void {
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
    if (p_->reopened_as_inline(path, k_)) {
      s_.diag(n.offset, Diag::namespace_reopened_inline, p_->world().ins.names.intern(n.name));
      return bad();
    }
    ast::DeclSema sem = class_decl(p_->namespace_entity(path), true);
    sem.kind = ast::DeclKind::namespace_;
    sem.type = s_.builtin(sema::Builtin::void_);
    decls_.push_back(sem);
  }
}

auto ItemAnalysis::namespace_head_ok() -> bool {
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

auto ItemAnalysis::constructor_flags(u32 count, bool destructor, std::optional<u32>* virtual_at, std::optional<u32>* explicit_at, parse::Parser::ExplicitSpec* explicit_spec) -> std::optional<u16> {
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
        s_.diag(ps_.pos(), Diag::explicit_placement);
        return std::nullopt;
      }
      if (*explicit_at) {
        s_.diag(ps_.pos(), Diag::duplicate_specifier, kind);
        return std::nullopt;
      }
      *explicit_at = ps_.pos();
      ps_.bump();
      // A value-dependent condition of a constructor template waits for deduction
      // ([over.match.funcs.general]); one that depends only on an enclosing class template's
      // parameters is evaluated when the member is instantiated.
      auto spec = ps_.explicit_specifier(/*want_atom=*/member_template_);
      if (!spec) return std::nullopt;
      if (explicit_spec) *explicit_spec = *spec;
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

auto ItemAnalysis::special_member(const ast::DeclSema& c, bool ctor, u16 flags, std::optional<u32> virtual_at, parse::Parser::ExplicitSpec explicit_spec) -> void {
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
    if (!finish_member_definition_declarator()) return bad();
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
             !finish_member_definition_declarator()) {
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
  // A value-dependent condition is not explicit in a pattern: its specializations say
  // ([dcl.fct.spec]). A constructor template's is read again after deduction.
  sem.is_explicit = explicit_spec.value.value_or(false);
  sem.explicit_condition = explicit_spec.atom;
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

auto ItemAnalysis::plain_virtual_signature(const items::Item& source, u32 type_end, u32 name_at, bool check_exception) -> bool {
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

auto ItemAnalysis::native_virtual_definition(EntityId cls, const sema::ClassRec& record, EntityId function, const parse::Declarator& d, const ast::DeclSema& sem) -> bool {
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

auto ItemAnalysis::define_member(const DeclSpec& spec, const parse::Declarator& d, ast::DeclSema& sem, bool fn, bool definition, bool has_init) -> bool {
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
  if (p_->defaulted_body(k_) && s_.ins().names.spelling(d.name) == "operator=") {
    auto& types = s_.types();
    const auto signature = types.rec(matched_type);
    const auto source = p_->assignment_parameter(*m, *t);
    const EntityId owner = p_->world().entities.rec(cls).scope;
    if (template_ || explicit_ || inst_ || cr->union_ || cr->polymorphic || !cr->bases.empty() || !source ||
        !types.is_reference(*source) || sema::Types::is_volatile(types.canonical(types.referee(*source))) ||
        signature.fn_cv != sema::Cv::none || signature.fn_ref == ast::RefQual::rvalue || signature.variadic ||
        types.canonical(signature.inner) != types.lvalue_ref(*t) ||
        !Program::encloses(p_->ns_of(k_), p_->enclosing_namespace_path(owner)) ||
        std::ranges::any_of(cr->fields, [&](const sema::FieldRec& field) {
          const QualType type = types.canonical(field.type);
          return !types.is_scalar(type) || sema::Types::is_const(type) || sema::Types::is_volatile(type) ||
                 field.no_unique_address;
        })) {
      s_.diag(d.name_at, Diag::defaulted_copy_assignment_declaration);
      return false;
    }
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

auto ItemAnalysis::member_function_or_static(const DeclSpec& spec, parse::Declarator& d, ast::DeclSema c, bool is_virtual, bool is_final, const sema::MemberDeclRec* overridden) -> bool {
  sema::Types& types = s_.types();
  bool fn = types.is_function(d.type);
  if (fn && !member_operator_ok(d, spec.is_static)) return false;
  if (!defaults_allowed(d.params, fn, d.name)) return false;
  if (!fn && member_template_ && (d.template_args || ps_.is(Tk::less))) {
    s_.diag(d.name_at, Diag::not_supported, Tk::less);  // a partial specialization (#1594)
    return false;
  }
  if (fn) {
    const auto cv = types.rec(d.type).fn_cv;
    const auto object = spec.is_static ? std::nullopt : std::optional(types.pointer(types.add_cv(c.type, cv)));
    if (!member_requires_clause(d.params, object, p_->items().items[it_->parent].template_depth != 0)) return false;
  }
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
    // [class.copy.assign], [dcl.fct.def.delete]: an explicitly deleted move assignment is still a
    // declared overload. A virtual deleted move needs separate support.
    if (!supported_class || (!defaulted && (!parameter || (is_virtual && types.rec(*parameter).rvalue_ref))) ||
        (defaulted && (is_virtual || spec.is_consteval))) {
      s_.diag(d.name_at, Diag::not_supported, defaulted ? Tk::kw_default : Tk::kw_delete);
      return false;
    }
    // A trailing requires-clause must affect overload viability ([over.match.viable], #1417).
    if (deleted && types.rec(*parameter).rvalue_ref && !finish_member_definition_declarator()) return false;
    if (defaulted) {
      if (!finish_member_definition_declarator()) return false;  // a requires-clause
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
  sem.is_explicit = spec.explicit_at.has_value();
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
  } else if (member_template_ && !var_) {
    return static_member_template(spec, d, c, sem, flags);
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
    // A static data member template's specialization owns its own ([temp.inst]/7).
    const sema::Sema::ClosureOwnerScope closures(
        s_, var_ ? variable_closures(var_->spec, true)
                 : sema::Sema::ClosureOwner{s_.world().entities.reserve(member),
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
    if (s_.compatibility() == ast::Compatibility::clang && !s_.require_concrete_object(d.type, d.name_at))
      return false;
    if (spec.is_inline || spec.is_constexpr) {
      // An inline static member defines an object ([class.static.data]).
      if (!s_.require_complete(d.type, d.name_at) || !s_.require_concrete_object(d.type, d.name_at) ||
          !s_.require_supported_virtual_layout(d.type, d.name_at) ||
          !s_.check_destructor_access(d.type, d.name_at)) return false;
      flags |= sema::decl_flag::definition;
    }
    // A static data member template's specialization: the entity its use named, whose template
    // the class declared ([temp.inst]/7, `Program::variable_instance`).
    sem.entity = var_ ? var_->spec
                      : s_.world().entities.entity(
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
    if (inst_ && !var_ && !early && (spec.is_inline || spec.is_constexpr) && ps_.is(Tk::equal) && member_value_ != ordinal) {
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
      if (spec.is_constexpr && !evaluator.check_constant_destruction(d.type, *v.value, at)) {
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

auto ItemAnalysis::static_member_template(const DeclSpec& spec, parse::Declarator& d, ast::DeclSema c, ast::DeclSema& sem, u16 flags) -> bool {
  sema::Types& types = s_.types();
  if (spec.is_constexpr) d.type = types.add_cv(d.type, sema::Cv::const_);  // [dcl.constexpr]
  const bool has_init = ps_.is(Tk::equal) || ps_.is(Tk::l_brace);
  if (spec.is_constexpr && !has_init) {
    s_.diag(d.name_at, Diag::constexpr_uninitialized, d.name);
    return false;
  }
  sema::EntityRec e{sema::EntityKind::variable_template, d.name, d.type, LangLinkage::cxx, ast::Linkage::external, {}, c.entity};
  e.head = head_fp(s_.template_scopes.back());
  // A class template specialization declares the member template; its initializer is
  // instantiated only for the specializations that are used ([temp.inst]/3, /7).
  const bool skip_initializer = inst_.has_value();
  // A placeholder type stays while the initializer is dependent; each specialization deduces
  // its own ([dcl.type.auto.deduct], `Program::specialization_type`). Its lambdas' closure
  // types are the member template's (docs/design/closures.md).
  std::optional<Ex> early;
  if (types.has_placeholder(d.type) && !skip_initializer) {
    std::unique_ptr<sema::Braced> list;
    using Replay = Parser::TemplateArgumentReplay;
    const Parser::TemplateArgumentReplayScope replay(ps_, Replay::variable_initializer);
    const auto& owner = s_.world().entities.rec(c.entity);
    const sema::Sema::ClosureOwnerScope closures(
        s_, sema::Sema::ClosureOwner{s_.world().entities.reserve(e),
                                     owner.linkage == ast::Linkage::internal || Program::unnamed(p_->ns_of(k_))
                                         ? ast::Linkage::internal : ast::Linkage::external});
    if (ps_.is(Tk::l_brace) || !deduce_variable(d, list, early, /*pattern=*/true) || list) {
      if (ps_.is(Tk::l_brace)) s_.diag(ps_.pos(), Diag::not_supported, Tk::l_brace);
      return false;
    }
  }
  sem.kind = ast::DeclKind::variable;
  sem.storage = ast::Storage::static_;
  sem.type = d.type;
  flags |= sema::decl_flag::template_;
  // An initializer in the class is what each specialization is instantiated from.
  if (has_init) flags |= sema::decl_flag::definition;
  sem.flags = flags;
  sem.template_params = b_.seq(std::span<const ast::TemplateParamSema>(s_.template_scopes.back()));
  sem.constraints = associated_constraints();
  sem.head_constraint = head_constraint_;
  e.type = d.type;
  e.constraints = constraints_fp_;
  sem.entity = s_.world().entities.entity(e);
  s_.own.push_back({d.name, sem});
  p_->declared(sem, k_);
  Ex init;
  const u32 ordinal = u32(decls_.size());
  if (skip_initializer) {
    // Where the template's analysis found the initializer's end; without one, it was diagnosed.
    if (auto extent = p_->static_initializer(k_, ordinal)) ps_.seek(extent->end);
    else if (has_init) ps_.skip_to_semi();
    declarators_.push_back(s_.node(ast::Declarator{u32(decls_.size()), sema::Sema::at(init, d.begin), ConstId::none},
                                   d.begin, ps_.pos()));
    decls_.push_back(sem);
    return true;
  }
  if (early) {
    init = types.has_placeholder(d.type) ? *early : s_.convert(*early, d.type, early->begin);
  } else if (has_init) {
    using Replay = Parser::TemplateArgumentReplay;
    const Parser::TemplateArgumentReplayScope replay(ps_, Replay::variable_initializer);
    const sema::Sema::ClosureOwnerScope closures(s_, variable_closures(sem.entity, true));
    if (ps_.is(Tk::l_brace) || ps_.kind(1) == Tk::l_brace) {
      const auto form = ps_.accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
      auto list = ps_.braced_init_list();
      init = s_.list_init(d.type, *list, form, /*extend_reference=*/true);
    } else {
      ps_.bump();
      Ex x = ps_.assignment_expression();
      init = s_.convert(x, d.type, x.begin);
    }
  }
  if (has_init) p_->set_static_initializer(k_, ordinal, {ps_.index(), false});
  declarators_.push_back(s_.node(ast::Declarator{ordinal, sema::Sema::at(init, d.begin), ConstId::none},
                                 d.begin, ps_.pos()));
  decls_.push_back(sem);
  return !has_init || init.ok();
}

auto ItemAnalysis::variant_member(const parse::Declarator& d) -> bool {
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
  if (q_.classes().copy_of(et).first ||
      (!(supported_scope && named) && (s_.destructor_of(et) != EntityId::none || s_.has_member_inits(et)))) {
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

auto ItemAnalysis::member_declared(NameId n, std::optional<QualType> function, bool compare_parameters, bool is_static) -> bool {
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

auto ItemAnalysis::allocation_declaration_ok(const DeclSpec& spec, const parse::Declarator& d, LangLinkage lang, std::string_view name) -> bool {
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

auto ItemAnalysis::declare(const DeclSpec& spec, parse::Declarator d, LangLinkage lang, bool explicit_lang, bool is_function_def, bool ordinary_friend) -> bool {
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
  // [dcl.type.class.deduct]/3: a deduced class type is no function's return type or parameter.
  if (fn && types.deduced_class_in(types.rec(d.type).inner) != EntityId::none) {
    s_.diag(d.name_at, Diag::class_deduction_context,
            s_.world().entities.rec(types.deduced_class_in(types.rec(d.type).inner)).name);
    return false;
  }
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
  bool has_init = !deleted && (early || early_expr || early_args_ || ps_.is(Tk::equal) || ps_.is(Tk::l_brace) ||
                               (!fn && ps_.is(Tk::l_paren)));
  bool definition = fn ? is_function_def || deleted : !(effective_extern && !has_init);
  if (!fn) {
    if ((definition || s_.compatibility() == ast::Compatibility::clang) && !s_.require_concrete_object(d.type, d.name_at))
      return false;
    if (definition && !s_.require_supported_virtual_layout(d.type, d.name_at)) return false;
  }
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
      const sema::TypeRec& r = types.rec(d.type);
      LangLinkage type_linkage = r.linkage;
      if (!explicit_lang && !template_) {
        // [dcl.link]/6: the name and the function type inherit independently.
        for (const auto& earlier : prior | std::views::reverse) {
          if (earlier.kind != ast::DeclKind::function || (earlier.flags & sema::decl_flag::template_) ||
              !same_function_parameters(earlier.type, d.type))
            continue;
          if (earlier.flags & sema::decl_flag::lang_c) lang = LangLinkage::c;
          type_linkage = types.rec(earlier.type).linkage;
          break;
        }
      }
      if (ordinary_friend && is_function_def && lang == LangLinkage::c) {
        s_.diag(d.name_at, Diag::friend_c_linkage);
        return false;
      }
      if (r.fn_cv != sema::Cv::none && !d.qualified) {
        s_.diag(d.name_at, Diag::cv_on_non_member);  // [dcl.fct]/6
        return false;
      }
      std::vector<QualType> ps = r.params;
      sem.type = types.function(r.inner, ps, r.variadic, type_linkage, r.fn_cv, r.fn_ref, r.fn_noexcept);
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
    // A static data member template's definition (#256) is one such member template too.
    if (two_heads_ && !(d.qualified && d.scope.type && s_.types().is_class(*d.scope.type) &&
                        s_.world().entities.rec(s_.types().class_of(*d.scope.type)).templ != EntityId::none &&
                        (fn || s_.member_variable_template(s_.types().class_of(*d.scope.type), d.name)))) {
      s_.diag(d.name_at, Diag::extraneous_template_head);
      return false;
    }
    if (template_ && d.qualified && d.scope.type && s_.types().is_class(*d.scope.type)) {
      const sema::EntityRec& c = s_.world().entities.rec(s_.types().class_of(*d.scope.type));
      if (c.templ == EntityId::none) {
        // `template<class T> R S::f(T)` or `template<class T> T S::v = …`: a member template of
        // a class defined outside it ([temp.mem]).
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
      if (fn) trailing_requires_clause(d.params);
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
      if (fn && explicit_lang && ((p.flags ^ sem.flags) & sema::decl_flag::lang_c)) {
        s_.diag(d.name_at, Diag::linkage_conflict, d.name);
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
          sema::decl_flag::lang_c && s_.compatibility() != ast::Compatibility::gcc &&
      p_->function_defined_before(sem.entity, k_)) {
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
  const auto constant_destruction = [&](ConstId initialized, u32 at) {
    return !spec.is_constexpr || sema::Evaluator(s_, q_).check_constant_destruction(d.type, initialized, at);
  };
  if (fn && has_init && !is_function_def) {
    s_.diag(ps_.pos(), Diag::function_initialized, d.name);
    ps_.skip_to_semi();
  } else if (!fn && (early || ps_.is(Tk::l_brace) || (ps_.is(Tk::equal) && ps_.kind(1) == Tk::l_brace))) {
    // List-initialization; constant initialization when every element is a constant
    // ([basic.start.static]).
    const auto form = early_form_ ? *std::exchange(early_form_, std::nullopt)
                      : ps_.accept(Tk::equal) ? sema::Construction::copy_list : sema::Construction::direct_list;
    auto b = early ? std::move(early) : ps_.braced_init_list();
    init = s_.list_init(d.type, *b, form, /*extend_reference=*/true);
    if (init.ok() && !pattern) {
      if (ordinary_reference) {
        evaluate_reference(b->begin);
      } else {
        const auto v = sema::Evaluator(s_, q_).evaluate_initializer(init);
        if (v.value) {
          if (constant_destruction(*v.value, b->begin)) {
            value = *v.value;
            if (spec.is_constexpr) p_->set_constant(sem.entity, value, k_);
          }
        } else if (!v.diagnosed_failure()) {
          dynamic_ok(b->begin, v.reason);
        }
      }
    }
  } else if (!fn && (early_expr || ps_.accept(Tk::equal))) {
    early_form_.reset();
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
          if (constant_destruction(*v.value, e.begin)) {
            value = *v.value;
            if (spec.is_constexpr || (sema::Types::is_const(d.type) && !sema::Types::is_volatile(d.type) &&
                                      types.is_integral(d.type)))
              p_->set_constant(sem.entity, value, k_);
          }
        } else if (!v.diagnosed_failure()) {
          dynamic_ok(e.begin, v.reason);
        }
      }
    }
  } else if (!fn && (early_args_ || ps_.is(Tk::l_paren))) {
    // Direct-initialization ([dcl.init.general]): `T x(args)`, constant or dynamic; a pattern's
    // is evaluated in its specializations.
    u32 open = ps_.pos();
    std::vector<Ex> args;
    bool closed = true;
    if (early_args_) {
      open = early_args_->first;
      args = std::move(early_args_->second);
      early_args_.reset();
    } else {
      ps_.bump();
      if (!ps_.is(Tk::r_paren))
        do args.push_back(ps_.assignment_expression());
        while (ps_.accept(Tk::comma));
      closed = ps_.expect(Tk::r_paren);
    }
    if (!closed) ps_.skip_to_semi();
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
        if (constant_destruction(*v.value, open)) {
          value = *v.value;
          if (spec.is_constexpr) p_->set_constant(sem.entity, value, k_);
        }
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
  if (value != ConstId::none &&
      (requires_constinit || s_.folded_immediate_result != EntityId::none ||
       (spec.is_constexpr && s_.compatibility() != ast::Compatibility::spec)))
    s_.check_immediate_initializer(value, init.begin);
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

auto ItemAnalysis::deduce_variable(parse::Declarator& d, std::unique_ptr<sema::Braced>& list, std::optional<Ex>& expr, bool pattern) -> bool {
  sema::Types& types = s_.types();
  early_form_.reset();
  early_args_.reset();
  // A placeholder for a deduced class type ([dcl.type.class.deduct]): class template argument
  // deduction from the initializer, which is kept for the initialization below.
  if (const EntityId templ = types.deduced_class_in(d.type); templ != EntityId::none) {
    if (types.kind(d.type) != sema::TypeKind::placeholder) {
      s_.diag(d.name_at, Diag::class_deduction_declarator, d.type, d.name);
      return false;
    }
    if (!ps_.is(Tk::equal) && !ps_.is(Tk::l_brace) && !ps_.is(Tk::l_paren)) {
      s_.diag(d.name_at, Diag::class_deduction_uninitialized, d.name);
      return false;
    }
    auto in = ps_.deduced_initializer();
    if (!in.ok) return false;
    auto t = s_.deduce_class_type(templ, in.args, in.list.get(), in.form, in.open);
    if (!t) return false;
    if (pattern && types.is_dependent(*t)) return true;
    // Every declarator deduces the same type ([dcl.type.class.deduct]/1, [dcl.type.auto.deduct]/2).
    if (auto_replaced_ && !types.is_dependent(*t) && !types.is_dependent(*auto_replaced_) &&
        types.canonical(*auto_replaced_) != types.canonical(*t)) {
      s_.diag(d.name_at, Diag::class_deduction_inconsistent, d.name, *t, *auto_replaced_);
      return false;
    }
    auto_replaced_ = *t;
    d.type = types.add_cv(*t, sema::Types::cv(d.type));
    early_form_ = in.form;
    if (in.list) list = std::move(in.list);
    else if (in.form == sema::Construction::copy) expr = in.args[0];
    else early_args_ = std::pair{in.open, std::move(in.args)};
    return true;
  }
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

auto ItemAnalysis::explicit_declaration(const DeclSpec& spec, const parse::Declarator& d, ast::DeclSema& sem, u16 flags, bool fn, bool definition, bool has_init) -> bool {
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

auto AtomAnalysis::run(std::vector<engine::Diagnostic>& hard, bool* substitution_failed) -> std::optional<bool> {
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
    if (atom_.lookup_class != EntityId::none && p_->this_type(d)) {
      const auto cv = s_.types().rec(d.type).fn_cv;
      s_.this_type = s_.types().pointer(s_.types().add_cv(s_.types().class_type(atom_.lookup_class), cv));
    }
  }
  const bool in_lambda = ins.seqs.size(atom_.lambda_scope) != 0;
  const auto lambda = in_lambda ? ins.seqs.at(atom_.lambda_scope, 0) : ast::AtomLambdaScope{};
  if (ins.seqs.size(lambda.head) != 0)
    s_.template_scopes.push_back(ins.seqs.elements(lambda.head));
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
  // A normalized fold operand binds the element itself. Its spelling denotes
  // that single parameter throughout the atom's replay.
  for (auto& head : s_.template_scopes)
    for (auto& parameter : head)
      if (parameter.depth == atom_.depth && parameter.pack)
        if (auto arg = s_.template_args.at(parameter.depth, parameter.index); arg && arg->kind != ast::TemplateArgKind::pack)
          parameter.pack = false;
  // The function's parameters, of their substituted types, for an atom that names them.
  if (atom_.fn_params && (in_lambda || ins.seqs.size(is.decls) != 0)) {
    s_.push_scope();
    const auto params = in_lambda ? lambda.parameters : ins.seqs.at(is.decls, 0).params;
    for (const ast::ParamSema& prm : ins.seqs.elements(params)) {
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
  Ex e = fold_ ? ps_.constraint_fold_satisfaction(*fold_) : ps_.expression();
  bool failed = !e.ok() || !ps_.at_end();
  if (!failed && ps_.replayed_expression_fold && s_.compatibility() == ast::Compatibility::gcc) {
    // GCC 16.2 checks the functions used by every substituted operand of a
    // written fold, even when evaluation later short-circuits that operand.
    const auto first_function = p_->instance_order_.size();
    const auto first_variable = p_->var_instances_.size();
    const auto first_member = p_->member_initializers_.size();
    for (const auto& demand : s_.demands) {
      if (demand.variable_type) continue;
      if (const auto* instance = p_->instance(demand.spec, q_.at_point.value_or(k_)); instance && instance->failed) {
        hard.append_range(instance->body.diags);
        failed = true;
      }
    }
    p_->mark_type_only_instances(first_function, first_variable, first_member);
  }
  for (const engine::Diagnostic& d : sink_) {
    if (sema::survives_substitution(d.id)) hard.push_back(d);
    failed = true;
  }
  if (q_.observed_type_trait_failure) return std::nullopt;
  if (failed && substitution_failed) *substitution_failed = true;
  if (failed) return false;
  if (explicit_specifier_) return explicit_value(e, hard, substitution_failed);
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

auto BodyAnalysis::analyse() -> Result<ast::BodySema> {
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
    s_.can_escalate_immediate = inst_ && function_template && !generic_ &&
                                (f->flags & sema::decl_flag::constexpr_) &&
                                !(f->flags & sema::decl_flag::destructor);
    // A member of a local class sees the block around the class ([class.local]), and the
    // arguments of a templated function's specialization.
    if (auto block = p_->local_block(k_, inst_ ? p_->world().entities.rec(f->entity).scope : EntityId::none)) {
      s_.template_args = block->template_args;
      s_.outer_block = std::move(block);
    }
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
    } else if (defaulted && (ctor || (f->flags & sema::decl_flag::destructor))) {
      root = s_.node(ast::Compound{b_.list(std::vector<ast::Child>{})}, 0, 0);
    } else if (defaulted && s_.ins().names.spelling(p_->world().entities.rec(f->entity).name) == "operator=") {
      root = p_->copy_assignment_body(s_, s_.current_class, s_.local_ref(param_locals.front(), 0, 0), false);
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
    if (root.ok() && ctor) {
      if (defaulted && !param_locals.empty()) {
        const QualType parameter = p_->world().ins.seqs.at(f->params, 0).type;
        const bool move = s_.types().rec(s_.types().canonical(parameter)).rvalue_ref;
        Ex source = s_.local_ref(param_locals.front(), 0, 0);
        if (move) source = s_.static_cast_to(source, parameter, 0);
        root = p_->copy_constructor_body(s_, s_.current_class, source, root, move);
      } else {
        root = constructor_body(std::move(inits), root, signature.fn_noexcept);
      }
    }
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
                  b_.seq(std::span<const ast::ClosureDef>(s_.closures)),
                  b_.seq(std::span<const ast::TemplateTypeSpelling>(s_.template_type_spellings))};
  out_.value = b_.commit(v);
  // A dependent invocation in a constexpr template can supply its immediate context
  // when the function specializes. Its instantiated body checks the addresses.
  if (!keeps || !f || !(f->flags & sema::decl_flag::constexpr_))
    s_.check_immediate_addresses(out_.value.tree, root.begin);
  p_->generated_closure_members(out_.value.closures, k_, begin());
  if (!keeps) out_.demands = std::move(s_.demands);
  return std::move(out_);
}

auto BodyAnalysis::implicit_default_constructor(const ast::DeclSema& ctor) -> Result<ast::BodySema> {
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

auto BodyAnalysis::generic_body() -> Result<ast::BodySema> {
  const Program::GenericInst& gi = *generic_;
  p_->running_bodies_.push_back({gi.spec, k_, inst_});
  struct Pop {
    Program* p;
    ~Pop() { p->running_bodies_.pop_back(); }
  } pop{p_};
  s_.push_scope();
  for (const auto& [n, t] : gi.g.aliases) s_.local_type_alias(n, t, 0);
  for (const auto& u : gi.g.usings) s_.replay_block_using(u.name, u.decls, u.type_index);
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
  p_->generated_closure_members(out_.value.closures, k_, begin());
  out_.demands = std::move(s_.demands);
  return std::move(out_);
}

auto BodyAnalysis::mem_initializers() -> ConstructorInitializers {
  ConstructorInitializers r;
  const sema::ClassRec& rec = p_->classes_.rec(s_.current_class);
  QualType ct = s_.types().class_type(s_.current_class);
  bool seen_initializer = false;
  bool reported_mixed = false;
  bool names_base_pack = false;  // whether the last mem-initializer-id is a pack of base classes
  bool skipped = false;          // whether the last mem-initializer was skipped after an error
  // One mem-initializer: false when the list cannot be read further.
  const auto one = [&]() -> bool {
    if (!ps_.is(Tk::identifier)) {
      s_.diag(ps_.pos(), Diag::expected_unqualified_id);
      if (r.delegation) r.delegation.emplace();
      return false;
    }
    const bool had_initializer = std::exchange(seen_initializer, true);
    u32 at = ps_.pos();
    NameId n = ps_.name_of(ps_.tok());
    names_base_pack = false;
    skipped = false;
    ps_.bump();
    u32 field = 0;
    while (field < rec.fields.size() && rec.fields[field].name != n) ++field;
    std::optional<QualType> type;
    std::optional<QualType> named;
    auto destination = sema::ConstructorDestination::object;
    // A dependent mem-initializer-id, such as a base that is a template parameter or a pack's
    // element ([temp.dep.type]), names its base only in an instantiation, which checks it.
    bool dependent_target = false;
    if (field < rec.fields.size()) {
      type = rec.fields[field].type;
    } else {
      // [class.base.init]/2: a direct base or the constructor's class, ignoring parameters.
      named = s_.class_name(n, false, true, std::nullopt, /*search_locals=*/false);
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
      } else if (named && s_.types().is_dependent(*named)) {
        type = *named;
        dependent_target = true;
      }
      if (const ast::TemplateParamSema* p = s_.template_param(n); named && p && p->pack) names_base_pack = true;
    }
    const bool delegates = destination == sema::ConstructorDestination::delegation;
    if (r.delegation || (delegates && had_initializer)) {
      if (!std::exchange(reported_mixed, true)) s_.diag(at, Diag::delegating_initializer_not_alone);
      r.delegation.emplace();
      skip_mem_initializer();
      skipped = true;
      return true;
    }
    if (!type) {
      // A class that is no direct base, such as a pack's element, is named by its type.
      if (named && s_.types().is_class(*named)) s_.diag(at, Diag::not_direct_base, *named, ct);
      else s_.diag(at, Diag::no_member, n, ct);
      skip_mem_initializer();
      skipped = true;
      return true;
    }
    if (delegates) {
      r.delegation.emplace();
      if (!s_.template_scopes.empty() || inst_) {
        s_.diag(at, Diag::not_supported, Tk::colon);
        skip_mem_initializer();
        skipped = true;
        return true;
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
      if (!ps_.expect(Tk::l_paren)) return false;
      std::vector<Ex> args;
      // Each initializer-clause of the expression-list may be a pack expansion
      // ([class.base.init]/1, [temp.variadic]/5), as a call's arguments are. An expansion whose
      // packs are not bound yet leaves the number of arguments unknown, and a dependent type
      // leaves the kind of initialization unknown: the initializer stays dependent, and the
      // instantiation checks the expanded arguments.
      bool unexpanded = false;
      if (!ps_.is(Tk::r_paren))
        do
          for (const Ex& x : ps_.expandable([&] { return ps_.assignment_expression(); }, nullptr, &unexpanded))
            args.push_back(x);
        while (ps_.accept(Tk::comma));
      if (!ps_.expect(Tk::r_paren)) return false;
      if (unexpanded || s_.types().is_dependent(ft)) {
        if (std::ranges::all_of(args, [](const Ex& a) { return a.ok(); })) init = s_.dependent_node(args, open, ps_.pos());
      } else if (destination != sema::ConstructorDestination::object) {
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
    if (dependent_target) {
      r.dependent_bases = true;
      return true;
    }
    if (delegates) r.delegation = init;
    else {
      if (r.subobjects.contains(field)) s_.diag(at, Diag::redeclared_member, n);  // [class.base.init]/3
      r.subobjects[field] = init;
    }
    return true;
  };
  ps_.bump();
  do {
    // A mem-initializer followed by `...` is a pack expansion ([class.base.init]/15,
    // [temp.variadic]/5): `Bs(args)...` initializes each base of the pack. The definition
    // records its packs; an instantiation reads it once for each element, with the packs bound
    // to that element, as an expandable operand is read.
    const u32 begin = ps_.pos();
    const u32 start = ps_.index();
    if (const sema::Expansion* x = s_.expansion_at(begin)) {
      const sema::Expansion e = *x;
      const std::size_t before = s_.diag_count();
      auto n = s_.expansion_length(e, begin);
      if (!n && s_.diag_count() != before) {  // packs of different lengths, diagnosed
        ps_.seek(start + (e.end - begin));
        if (!ps_.expect(Tk::ellipsis)) return r;
        continue;
      }
      if (n) {
        for (std::size_t i = 0; i < *n; ++i) {
          ps_.seek(start);
          s_.begin_pattern();
          s_.push_frame(e.packs, u32(i));
          const bool ok = one();
          s_.pop_frame();
          s_.end_element(e.packs);
          if (!ok) return r;
        }
        ps_.seek(start + (e.end - begin));
        if (!ps_.expect(Tk::ellipsis)) return r;
        continue;
      }
    }
    s_.begin_pattern();
    const bool ok = one();
    const u32 ellipsis = ps_.pos();
    const bool expanded = ok && !skipped && ps_.is(Tk::ellipsis);
    const auto packs = s_.end_pattern(expanded, begin, ellipsis);
    if (!ok) return r;
    if (skipped) ps_.accept(Tk::ellipsis);  // its error is reported
    if (expanded) {
      ps_.bump();
      // [class.base.init]: the expansion initializes the bases a pack expansion in the
      // base-specifier-list specifies, so its mem-initializer-id names that pack of bases.
      if (packs.empty()) s_.diag(ellipsis, Diag::expansion_without_pack);
      else if (!names_base_pack) s_.diag(begin, Diag::mem_initializer_expansion_not_base_pack);
    }
  } while (ps_.accept(Tk::comma));
  return r;
}

auto BodyAnalysis::constructor_body(ConstructorInitializers inits, Ex body, bool nonthrowing) -> Ex {
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
    } else if (inits.dependent_bases) {
      continue;
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

}  // namespace ycxx::parse
