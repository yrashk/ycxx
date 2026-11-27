// A unit's analysis a group at a time, and the groups' analyses kept between its revisions
// (#947): `TuSema`. A module implementation unit, so that what it keeps stays out of the module's
// interface (GCC 16.2 writes some interfaces it cannot read back: docs/LESSONS.md).
module ycxx.pipeline;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.hash;
import ycxx.items;
import ycxx.lex;
import ycxx.parse;
import ycxx.pp;
import ycxx.sema;
import :back;
import :front;
import :policy_registry;

namespace ycxx::pipeline {

// One group's analysis (#947), in values that hold no Program and no item index: its own
// items' messages, located by item, and its instances'; what an editor reads; the class and
// enumeration shapes its Program made; and, when its messages hold no error, its part of the
// unit's definitions.
struct GroupMessage {
  bool error = true;
  std::string text;
  engine::Loc at;
  bool depth_limit = false;  // [temp.inst]/16's limit, which a unit reports once
  lex::Warning warning = lex::Warning::uncontrolled;
};
struct DynamicLayout {
  EntityId primary_base = EntityId::none;
  std::optional<ast::Loc> virtual_declaration;
};
struct PeerLayout {
  EntityId cls = EntityId::none;
  DynamicLayout layout;
};
struct GroupResult {
  std::vector<GroupMessage> messages;
  std::vector<std::pair<EntityId, ast::Loc>> declared;
  std::vector<BodyAt> bodies;
  std::vector<PolicySite> policy_aliases;
  std::vector<PolicyAnnotationSite> policy_annotations;
  std::vector<ClassShape> classes;
  std::vector<EnumShape> enums;
  std::set<EntityId> uncertain_layouts;  // includes entities with no shape in this group
  std::map<EntityId, DynamicLayout> dynamic_layouts;
  std::vector<PeerLayout> peer_layouts;
  bool depth_at_end = false;  // its depth-limit message, if any, came from its END worklist
  std::optional<UnitPart> part;
};
// What a group's analysis read of its unit, checked before the analysis is reused (#947): its
// own items; each item it read and that item's content; each by-name question and its answer;
// the order of every item it read or was answered; and the facts every group reads (namespaces,
// using-directives, specializations). Items are named by their ids, never their positions.
struct GroupTrace {
  std::vector<Fp128> group;
  std::vector<std::pair<Fp128, Fp128>> items;  // (id, content), in item order
  std::vector<std::pair<parse::Program::NameProbe, std::vector<std::pair<Fp128, std::uint32_t>>>> probes;
  std::vector<Fp128> order;
  Fp128 unit;
};
// A universe's groups, by unit and the group's first item.
struct GroupCache {
  struct Entry {
    GroupTrace trace;
    GroupResult result;
  };
  std::mutex mutex;
  // Shared: a group's entry is verified and copied outside the lock.
  std::map<std::pair<Fp128, Fp128>, std::shared_ptr<const Entry>> entries;
  std::uint64_t analysed = 0, reused = 0;
};
struct GroupCaches {
  std::mutex mutex;
  std::map<const Universe*, std::unique_ptr<GroupCache>> by_universe;
};
GroupCaches& group_caches() {
  static GroupCaches caches;
  return caches;
}
GroupCache& group_cache(const Universe* u) {
  GroupCaches& all = group_caches();
  std::lock_guard lock(all.mutex);
  auto& cache = all.by_universe[u];
  if (!cache) cache = std::make_unique<GroupCache>();
  return *cache;
}
void forget_group_cache(const Universe* u) {
  GroupCaches& all = group_caches();
  std::lock_guard lock(all.mutex);
  all.by_universe.erase(u);
}
std::pair<std::uint64_t, std::uint64_t> group_cache_counts(const Universe* u) {
  GroupCache& cache = group_cache(u);
  std::lock_guard lock(cache.mutex);
  return {cache.analysed, cache.reused};
}

// The facts of one revision a group's trace is made of and checked against (#947).
class GroupFacts {
 public:
  GroupFacts(const parse::Program::Index& x, const items::Items& items, const pp::Region& r) : x_(&x), items_(&items), r_(&r) {
    unit_ = unit_facts();
  }
  GroupTrace trace(std::uint32_t first, std::uint32_t end, const std::vector<std::uint8_t>& reads,
                   const std::set<parse::Program::NameProbe>& probes) {
    GroupTrace t;
    t.unit = unit_;
    for (std::uint32_t k = first; k < end; ++k) t.group.push_back(id(k));
    std::set<std::uint32_t> order;
    for (std::uint32_t k = 0; k < reads.size(); ++k)
      if (reads[k]) {
        order.insert(k);
        t.items.push_back({id(k), content(k)});
      }
    for (const parse::Program::NameProbe& q : probes) {
      std::vector<std::pair<Fp128, std::uint32_t>> answer;
      for (auto [j, n] : parse::Program::answer(*x_, q)) {
        answer.push_back({id(j), n});
        order.insert(j);
      }
      t.probes.push_back({q, std::move(answer)});
    }
    // A lookup sees the using-directives and aliases, the inline namespaces and the explicit
    // specializations before its point, which no question names: where they are relative to what
    // the group read is part of what it read.
    for (const auto& d : x_->directives) order.insert(d.item);
    for (const auto& [path, children] : x_->inline_namespaces)
      for (const auto& child : children) order.insert(child.item);
    for (std::uint32_t k : x_->specializations) order.insert(k);
    for (std::uint32_t k : order) t.order.push_back(id(k));
    return t;
  }
  // Whether every question the trace holds has the same answer in this revision.
  bool verify(const GroupTrace& t, std::uint32_t first, std::uint32_t end) {
    if (t.unit != unit_ || t.group.size() != end - first || !unique()) return false;
    for (std::uint32_t k = first; k < end; ++k)
      if (id(k) != t.group[k - first]) return false;
    for (const auto& [item, fp] : t.items) {
      auto k = at(item);
      if (!k || content(*k) != fp) return false;
    }
    for (const auto& [q, answer] : t.probes) {
      const auto now = parse::Program::answer(*x_, q);
      if (now.size() != answer.size()) return false;
      for (std::size_t i = 0; i < now.size(); ++i)
        if (id(now[i].first) != answer[i].first || now[i].second != answer[i].second) return false;
    }
    std::optional<std::uint32_t> previous;
    for (Fp128 item : t.order) {
      auto k = at(item);
      if (!k || (previous && *k <= *previous)) return false;
      previous = k;
    }
    return true;
  }

 private:
  Fp128 id(std::uint32_t k) const { return items_->items[k].id; }
  // The item with an id; none when no item or several have it.
  std::optional<std::uint32_t> at(Fp128 item) {
    unique();
    auto it = by_id_.find(item);
    return it == by_id_.end() ? std::nullopt : std::optional(it->second);
  }
  bool unique() {
    if (!indexed_) {
      indexed_ = true;
      for (std::uint32_t k = 0; k < items_->items.size(); ++k)
        if (!by_id_.emplace(id(k), k).second) unique_ = false;
    }
    return unique_;
  }
  // An item's content, which holds no position: its record with ranges relative to its start,
  // its parent and a class's members by id, what the index says of it, and its tokens' kinds, flags and
  // spellings.
  Fp128 content(std::uint32_t k) {
    {
      std::lock_guard lock(contents_mutex_);
      if (auto known = contents_.find(k); known != contents_.end()) return known->second;
    }
    const items::Item& it = items_->items[k];
    Hasher h;
    auto str = [&](std::string_view s) { h.uint(s.size()).bytes(s); };
    auto range = [&](std::uint32_t b, std::uint32_t e) {
      if (e > b) h.uint(b - it.begin).uint(e - it.begin);
      else h.uint(~0u);
    };
    // A namespace, linkage or export block is its head, `namespace N {`, and how it ends: its
    // members are items of their own, and one added or removed changes no other's reading of it.
    const bool block = it.kind == items::ItemKind::namespace_def || it.kind == items::ItemKind::linkage_block ||
                       it.kind == items::ItemKind::export_block;
    h.uint(std::to_underlying(it.kind)).uint(block ? it.brace_begin - it.begin : it.end - it.begin);
    if (!block) {
      range(it.body_begin, it.body_end);
      range(it.brace_begin, it.brace_end);
    }
    range(it.qualifier_begin, it.qualifier_end);
    h.uint(it.template_depth).uint(std::uint8_t(it.specialization)).uint(it.unnamed_type_ordinal).uint(it.ordinal);
    h.fp(it.id).fp(it.body);
    str(it.scope);
    h.fp(it.parent == items::none ? Fp128{} : id(it.parent));
    // A class's or enumeration's members are read with it; a namespace's are known by their own
    // namespaces (`ns_of`, below), and a new member there does not change what reads the namespace.
    if (it.kind == items::ItemKind::class_def || it.kind == items::ItemKind::enum_def) {
      h.uint(it.members.size());
      for (std::uint32_t m : it.members) h.fp(id(m));
    }
    h.uint(it.names.size());
    for (const items::DeclName& n : it.names) {
      str(n.name);
      h.uint(n.offset).uint(n.ordinal).uint(n.flags).uint(std::uint8_t(n.member_elaborated_candidate));
    }
    str(x_->ns_of[k]);
    str(x_->eff_ns[k]);
    h.uint(std::uint8_t(x_->in_class[k])).uint(std::uint8_t(x_->qualified[k])).uint(std::to_underlying(x_->namespace_visibility[k]));
    if (auto scopes = x_->name_scopes.find(k); scopes != x_->name_scopes.end())
      for (const auto& [ns, eff] : scopes->second) {
        str(ns);
        str(eff);
      }
    // Its own tokens: a nested item's are its own content, read when that item is (`ItemsView`).
    std::uint32_t i = it.begin;
    if (block) {
      for (; i < it.brace_begin; ++i) token(h, i);
      // Whether it is closed, which its analysis reports.
      if (it.brace_end > it.brace_begin) h.uint(std::to_underlying(r_->tokens[it.brace_end - 1].kind));
      if (it.end > it.begin) h.uint(std::to_underlying(r_->tokens[it.end - 1].kind));
    } else {
      for (std::uint32_t m : it.members) {
        for (; i < items_->items[m].begin; ++i) token(h, i);
        i = std::max(i, items_->items[m].end);
      }
      for (; i < it.end; ++i) token(h, i);
    }
    const Fp128 fingerprint = h.finish();
    std::lock_guard lock(contents_mutex_);
    return contents_.try_emplace(k, fingerprint).first->second;
  }
  void token(Hasher& h, std::uint32_t i) const {
    const lex::Tok tok = r_->tokens[i];
    const std::string_view spelling = r_->spelling(tok);
    h.uint(std::to_underlying(tok.kind)).uint(tok.flags).uint(spelling.size()).bytes(spelling);
  }
  // What every group reads: the namespaces, the using-directives and aliases at namespace scope,
  // the inline namespaces, the specialization items and whether virtual layout checks can run.
  // Defaults can read presumed positions in other groups, so the source tables are also unit
  // facts when present (#327).
  Fp128 unit_facts() {
    Hasher h;
    h.uint(std::uint8_t(x_->says_virtual));
    auto str = [&](std::string_view s) { h.uint(s.size()).bytes(s); };
    for (std::uint32_t k = 0; k < items_->items.size(); ++k)
      if (items_->items[k].kind == items::ItemKind::namespace_def) {
        h.fp(id(k));
        str(x_->inner_of[k]);
      }
    for (const auto& d : x_->directives) {
      h.fp(id(d.item));
      str(d.at);
      h.uint(std::uint8_t(d.alias.has_value()));
    }
    for (const auto& [path, children] : x_->inline_namespaces) {
      str(path);
      for (const auto& child : children) {
        str(child.path);
        h.fp(id(child.item));
      }
    }
    for (std::uint32_t k : x_->specializations) h.fp(id(k));
    if (!r_->presumed_lines.empty()) {
      h.uint(r_->presumed_lines.size());
      for (auto line : r_->presumed_lines) h.uint(line);
    }
    if (!r_->presumed_sites.empty()) {
      h.uint(r_->presumed_files.size());
      for (const auto& name : r_->presumed_files) str(name);
      h.uint(r_->presumed_sites.size());
      for (auto site : r_->presumed_sites) h.uint(site.file).uint(site.column);
    }
    return h.finish();
  }

  const parse::Program::Index* x_;
  const items::Items* items_;
  const pp::Region* r_;
  Fp128 unit_;
  bool indexed_ = false, unique_ = true;
  std::map<Fp128, std::uint32_t> by_id_;
  std::mutex contents_mutex_;
  std::map<std::uint32_t, Fp128> contents_;
};

engine::query_task<SemaResult> TuSema::run(engine::Ctx& cx, TuId tu) {
  const TuSource& src = cx.fetch<TuSourceIn>(tu);
  contract_assert(src.file.has_value());  // a unit given as definitions has no front end
  const pp::FileId file{*src.file};
  const pp::Source& unit_source = cx.fetch<pp::SourceOf>(file);
  sema::World& world = *universe().world_for(*unit_source.config.target);
  const pp::Region& region = co_await cx.fetch<PpTokens>(file);
  const items::Items& items = co_await cx.fetch<items::RegionItems>(file);
  SemaResult out;
  // Whether the lexer or preprocessor reported an error, not where: `TuFront` places them.
  out.lex_errors = co_await cx.fetch<LexErrors>(file);
  const bool target_is_linux = unit_source.config.target->triple == ast::x86_64_linux.triple;
  // What one Program says of the items [first, end): where each entity is first declared and the
  // bodies, for an editor's position requests (#170), whether or not the unit has errors; only what
  // the analysis reached, which analysing more would change (its diagnostics). Then its messages,
  // each once, and its classes' and enumerations' shapes, each once.
  std::set<std::uint32_t> declared_seen;
  std::set<EntityId> shaped, enumerated;
  const auto index = parse::Program::make_index(world, region, items);
  // Classes whose layout keeps the unit's virtual tables from being emitted, applied to the unit
  // once it is made.
  auto collect = [&](parse::Program& program, std::uint32_t first, std::uint32_t end, const std::set<EntityId>& uncertain) {
    GroupResult r;
    std::set<EntityId> declared_here;
    for (std::uint32_t k = first; k < end; ++k) {
      const items::Item& it = items.items[k];
      const ast::ItemSema* analysed = program.analysed_item(k);
      if (!analysed) continue;
      const ast::ItemSema& is = *analysed;
      const std::vector<ast::DeclSema> decls = world.ins.seqs.elements(is.decls);
      std::vector<std::uint32_t> at(decls.size(), 0);
      if (is.tree != ast::NodeId::none && world.ins.ast.is<ast::DeclGroup>(is.tree))
        for (ast::Child c : world.ins.ast.children(world.ins.ast.get<ast::DeclGroup>(is.tree).declarators)) {
          auto dr = world.ins.ast.get<ast::Declarator>(c.id);
          if (dr.decl < decls.size()) at[dr.decl] = c.at;
        }
      for (std::size_t i = 0; i < decls.size(); ++i) {
        if (decls[i].entity != EntityId::none && declared_here.insert(decls[i].entity).second)
          r.declared.emplace_back(decls[i].entity, ast::Loc{program.id(k), at[i]});
        if (decls[i].kind == ast::DeclKind::type_alias && program.in_global_scope(k))
          if (const auto policy = policy_alias(world, decls[i].type))
            r.policy_aliases.push_back({*policy, {program.id(k), at[i]}});
        if (decls[i].entity == EntityId::none) continue;
        for (const auto& annotation : world.ins.seqs.elements(decls[i].annotations)) {
          const ast::Loc location{program.id(k), annotation.offset};
          if (const auto policy = policy_alias(world, annotation.type); policy && policy->active)
            r.policy_aliases.push_back({*policy, location});
          else if (is_policy_annotation(world, annotation.type))
            r.policy_annotations.push_back({decls[i].entity, decls[i].type, annotation.type, annotation.value, location});
        }
      }
      if (const ast::BodySema* body = program.analysed_body(k); body && body->tree != ast::NodeId::none && it.has_body())
        r.bodies.push_back({program.id(k), it.decl_length(), body->tree});
    }
    for (const engine::Diagnostic& d : program.diagnostics(first, end)) {
      auto kind = sema::sema_diag_of(d.id);
      if (!kind) continue;
      std::vector<std::uint64_t> args;
      for (const engine::DiagArg& a : d.args) args.push_back(a.raw);
      const auto spec = sema::message_of(*kind);
      r.messages.push_back({spec.severity == lex::Severity::error,
                            sema::render(*kind, args, [&](char how, std::uint64_t raw) { return program.spell(how, raw); }),
                            d.at, *kind == sema::Diag::instantiation_depth, spec.warning});
    }
    // The unit's own records of its classes and enumerations (#115).
    const sema::Classes& classes = program.classes();
    auto nontrivial_destructor = [&](this auto&& recurse, EntityId cls) -> bool {
      const auto* record = classes.find(cls);
      if (!record || record->destructor == EntityId::none || record->virtual_layout_unsupported) return false;
      using enum sema::CopyOperation;
      if (record->trivial_copyability.destructor != unknown)
        return record->trivial_copyability.destructor == eligible_nontrivial;
      if (auto member = std::ranges::find(record->members, record->destructor, &sema::MemberDeclRec::entity);
          member != record->members.end()) {
        // Defaulting the first declaration can still yield a trivial destructor ([dcl.fct.def.default]).
        auto first = program.member_decl(member->entity);
        return !member->is_deleted && first && !program.defaulted_body(program.index(first->item));
      }
      // A variant member does not make the union's own destructor nontrivial ([class.dtor]).
      if (record->union_) return false;
      // An implicit destructor is nontrivial when a direct base or by-value member's is nontrivial.
      return std::ranges::any_of(record->bases, [&](const auto& base) { return recurse(base.cls); }) ||
             std::ranges::any_of(record->fields, [&](const auto& field) {
               auto type = world.types.rec(field.type);
               while (type.kind == sema::TypeKind::array) type = world.types.rec(type.inner);
               return type.kind == sema::TypeKind::class_ && recurse(type.entity);
             });
    };
    std::set<EntityId> implicit_destructors;
    for (const auto& member : program.synthesized())
      if (member.kind == parse::Program::SpecialMember::destructor)
        implicit_destructors.insert(member.decl.entity);
    for (EntityId c : classes.classes()) {
      ClassShape& shape = r.classes.emplace_back(ClassShape{c, {}, {}});
      const auto& record = classes.rec(c);
      const auto& fields = record.fields;
      for (const sema::FieldRec& f : fields) {
        if (f.no_unique_address) shape.overlapping_fields.push_back(std::uint32_t(shape.fields.size()));
        shape.fields.push_back(f.type);
        shape.field_names.push_back(f.name);
        shape.has_mutable_field |= f.mutable_;
      }
      if (std::ranges::any_of(fields, [](const sema::FieldRec& f) { return f.bit_width != 0; }))
        for (const sema::FieldRec& f : fields) shape.bit_widths.push_back(f.bit_width);
      if (std::ranges::any_of(fields, [](const sema::FieldRec& f) { return f.wide_bit_width != 0; }))
        for (const sema::FieldRec& f : fields)
          shape.wide_bit_fields.push_back({f.wide_bit_width,
              ast::over_width_bit_field_alignment(world.target, program.compatibility(), f.wide_bit_width)});
      if (std::ranges::any_of(fields, [](const sema::FieldRec& f) { return f.explicit_alignment != 0; }))
        for (const sema::FieldRec& f : fields) shape.field_alignments.push_back(f.explicit_alignment);
      shape.union_ = record.union_;
      const bool plain_destruction = record.destructor == EntityId::none ||
          (record.destructor_noexcept.value_or(false) && implicit_destructors.contains(record.destructor));
      shape.plain_ownership_record = plain_destruction && record.bases.empty() && !record.union_ &&
          !record.user_ctors && !record.closure && !record.polymorphic &&
          std::ranges::none_of(fields, &sema::FieldRec::no_unique_address);
      if (shape.plain_ownership_record) shape.ownership_destructor = record.destructor;
      if (record.destructor_noexcept.value_or(false)) shape.nonthrowing_destructor = record.destructor;
      shape.pod = record.pod;
      shape.explicit_alignment = record.explicit_alignment;
      shape.clang_weak_alignment = record.clang_weak_alignment;
      for (const sema::BaseRec& b : record.bases) shape.bases.push_back(b.cls);
      const auto call_rule = unit_source.config.calls();
      const bool gcc_differs = classes.trivial_for_calls(c, ast::CallTriviality::gcc) &&
                               !classes.trivial_for_calls(c, ast::CallTriviality::itanium);
      shape.trivial_calls = classes.trivial_for_calls(c, call_rule);
      shape.gcc_stack_calls = target_is_linux && call_rule == ast::CallTriviality::gcc && gcc_differs;
      shape.layout_uncertain = uncertain.contains(c);
      shape.copy_abi_unknown = record.defaulted_copy == sema::CopyState::unsupported;
      {
        // Itanium C++ ABI §3.1.2.3 from the special members one by one ([class.prop]): a
        // non-trivial destructor or copy or move constructor, or neither constructor usable.
        using enum sema::CopyOperation;
        const sema::TrivialCopyability& t = record.trivial_copyability;
        const bool known = t.destructor != unknown && t.copy_constructor != unknown && t.move_constructor != unknown;
        const bool nontrivial = t.destructor == eligible_nontrivial || t.copy_constructor == eligible_nontrivial ||
                                t.move_constructor == eligible_nontrivial ||
                                (t.copy_constructor == ineligible && t.move_constructor == ineligible);
        // A class with virtual functions has no trivial copy or move constructor ([class.copy.ctor]/11).
        // Clang allows callers to reserve result storage for an abstract class with a known layout.
        const bool abstract_result = unit_source.config.compatibility == ast::Compatibility::clang && record.is_abstract();
        // A proven nontrivial destructor settles the ABI even when the copy/move traits are unknown,
        // and so does a nontrivial copy or move constructor defaulted in the class (#157).
        const bool defaulted_nontrivial = std::ranges::any_of(record.members, [&](const sema::MemberDeclRec& m) {
          return m.is_ctor && m.defaulted == sema::CopyState::nontrivial && m.entity != record.defaulted_default_ctor;
        });
        shape.calls_abi_uncertain = !shape.trivial_calls && !gcc_differs && !defaulted_nontrivial && (!known || !nontrivial) &&
                                    !abstract_result && !(target_is_linux && nontrivial_destructor(c));
      }
      // Only a class outside the general dynamic layout lacks a native complete-object type.
      shape.layout_requires_virtual_support = record.virtual_layout_unsupported;
      shape.dynamic_root = record.dynamic && !record.dynamic->primary_base;
      if (record.dynamic) {
        const EntityId base = record.dynamic->primary_base ? record.bases.front().cls : EntityId::none;
        r.dynamic_layouts.emplace(c, DynamicLayout{base, virtual_declaration(program, record)});
      }
      if (record.peer_virtual_layout != sema::PeerVirtualLayout::none ||
          record.peer_automatic_primary_base != EntityId::none) {
        const EntityId primary = record.peer_virtual_layout == sema::PeerVirtualLayout::primary_base
                                     ? record.bases.front().cls : record.peer_automatic_primary_base;
        const bool uncertain_base = primary != EntityId::none && uncertain.contains(primary);
        r.peer_layouts.push_back({c, {primary, virtual_declaration(program, record)}});
        if (!shape.layout_uncertain && !uncertain_base) {
          shape.peer_virtual_layout = record.peer_virtual_layout;
          shape.peer_automatic_primary_base = record.peer_automatic_primary_base;
        }
      }
    }
    for (EntityId e : classes.enums()) r.enums.push_back({e, classes.underlying(e)});
    return r;
  };
  // Puts a group's result into the unit's, in group order: each entity and shape once, and each
  // message an earlier group did not say (an instance two groups made); a group's own repeated
  // messages stay, as the whole unit's Program says them. The instantiation-depth limit is said
  // only by the group `keep_depth` names (#993).
  std::set<std::tuple<std::uint32_t, std::uint32_t, bool, std::string>> said;
  auto merge = [&](const GroupResult& r, bool keep_depth = true) {
    std::set<std::tuple<std::uint32_t, std::uint32_t, bool, std::string>> here;
    for (const GroupMessage& m : r.messages) {
      if (m.depth_limit && !keep_depth) continue;
      std::tuple key{m.at.item, m.at.offset, m.error, m.text};
      if (said.contains(key)) continue;
      here.insert(std::move(key));
      const items::Item& it = items.items[index->index.at(m.at.item)];
      const auto token = std::min<std::size_t>(it.begin + m.at.offset, region.tokens.size() - 1);
      out.messages.push_back({m.error, m.text, static_cast<std::uint32_t>(token), m.warning});
    }
    said.merge(here);
    for (const auto& [e, at] : r.declared)
      if (declared_seen.insert(std::to_underlying(e)).second) out.declared.emplace_back(e, at);
    out.bodies.insert(out.bodies.end(), r.bodies.begin(), r.bodies.end());
    out.policy_aliases.append_range(r.policy_aliases);
    out.policy_annotations.append_range(r.policy_annotations);
    for (const ClassShape& c : r.classes)
      if (shaped.insert(c.entity).second) out.shapes.classes.push_back(c);
    for (const EnumShape& e : r.enums)
      if (enumerated.insert(e.entity).second) out.shapes.enums.push_back(e);
  };
  // Analyse each top-level item of the main file, and each run of the headers' items,
  // with a Program that reads other items as needed.
  auto from_header = [&](std::uint32_t item) { return region.locs[items.items[item].begin].file != 0; };
  std::vector<std::pair<std::uint32_t, std::uint32_t>> groups;
  for (std::uint32_t g : items.top) {
    std::uint32_t last = g;
    while (!items.items[last].members.empty()) last = items.items[last].members.back();
    if (from_header(g) && !groups.empty() && from_header(groups.back().first))
      groups.back().second = last + 1;
    else groups.push_back({g, last + 1});
  }
  // Each Program reads the unit's specializations at END. With template-heavy headers a
  // handful of source declarations can repeat that work many times. Analyse a small source
  // run together while keeping it independent of the surrounding header runs.
  constexpr std::uint32_t small_run_items = 32;
  constexpr std::uint32_t small_run_tokens = 2048;
  if (std::ranges::count_if(index->specializations, from_header) >= small_run_items) {
    decltype(groups) batched;
    batched.reserve(groups.size());
    for (std::size_t i = 0; i < groups.size();) {
      const auto first = groups[i].first;
      if (from_header(first)) {
        batched.push_back(groups[i++]);
        continue;
      }
      std::size_t next = i + 1;
      while (next < groups.size() && !from_header(groups[next].first)) ++next;
      const auto end = groups[next - 1].second;
      const auto tokens = items.items[groups[next - 1].first].end - items.items[first].begin;
      if (end - first <= small_run_items && tokens <= small_run_tokens)
        batched.emplace_back(first, end);
      else
        for (std::size_t j = i; j < next; ++j) batched.push_back(groups[j]);
      i = next;
    }
    groups = std::move(batched);
  }
  // A group's analysis is reused when what it read gives the same answers (`GroupTrace`). Token
  // ranges held for pragmas are positions: a unit with them analyses every group again.
  GroupCache* cache = &group_cache(&universe());
  const bool traceable = cache && region.possibly_packed.empty() && !region.first_visibility_pragma &&
                         !region.has_clang_attribute_pragma;
  const Fp128 unit_key = Hasher().uint(unit_source.path.size()).bytes(unit_source.path).uint(unit_source.cwd.size())
                             .bytes(unit_source.cwd).fp(unit_source.config.fingerprint()).finish();
  GroupFacts facts(*index, items, region);
  std::vector<GroupResult> results(groups.size());
  std::vector<std::shared_ptr<const GroupCache::Entry>> entries(groups.size());
  std::vector<std::size_t> misses;
  std::uint64_t reused = 0;
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const auto [first, end] = groups[i];
    std::shared_ptr<const GroupCache::Entry> kept;
    if (traceable) {
      const std::pair key{unit_key, items.items[first].id};
      std::lock_guard lock(cache->mutex);
      if (auto e = cache->entries.find(key); e != cache->entries.end()) kept = e->second;
    }
    if (kept && facts.verify(kept->trace, first, end)) {
      results[i] = kept->result;
      ++reused;
    } else {
      misses.push_back(i);
    }
  }
  co_await cx.parallel_for(misses.size(), [&](std::size_t miss) {
    const std::size_t i = misses[miss];
    const auto [first, end] = groups[i];
    parse::Program program(world, region, items, unit_source.config, index);
    std::vector<std::uint8_t> reads;
    std::set<parse::Program::NameProbe> probes;
    if (traceable) {
      program.record_reads(&reads);
      program.record_probes(&probes);
    }
    program.analyze_group(first, end);
    auto uncertain = program.uncertain_object_layouts(/*include_lifecycle_signatures=*/true);
    GroupResult r = collect(program, first, end, uncertain);
    r.depth_at_end = program.depth_reported_at_end();
    if (std::ranges::none_of(r.messages, &GroupMessage::error)) {
      r.part.emplace();
      r.part->tu = unit_of(program, world, program.items(), src.name, uncertain, first, end, *r.part);
    }
    r.uncertain_layouts = std::move(uncertain);
    if (traceable) {
      GroupTrace trace = facts.trace(first, end, reads, probes);
      entries[i] = std::make_shared<const GroupCache::Entry>(GroupCache::Entry{std::move(trace), r});
    }
    results[i] = std::move(r);
  });
  co_await cx.stop_point();
  // A group gone from the unit (its first item edited, moved or removed) is forgotten.
  if (traceable) {
    std::set<Fp128> present;
    for (auto [first, end] : groups) present.insert(items.items[first].id);
    std::lock_guard lock(cache->mutex);
    for (std::size_t i : misses)
      cache->entries.insert_or_assign({unit_key, items.items[groups[i].first].id}, std::move(entries[i]));
    cache->analysed += misses.size();
    cache->reused += reused;
    for (auto e = cache->entries.lower_bound({unit_key, Fp128{}}); e != cache->entries.end() && e->first.first == unit_key;)
      e = present.contains(e->first.second) ? std::next(e) : cache->entries.erase(e);
  }
  // The whole unit's Program reports the depth limit once: where it is first reached, analysing
  // the items of every group before any END worklist. So the first group that reached it while
  // analysing its items reports it, else the first that reached it in its END worklist (#993).
  auto reports_depth = [](const GroupResult& r) { return std::ranges::any_of(r.messages, &GroupMessage::depth_limit); };
  auto depth_group = std::ranges::find_if(results, [&](const GroupResult& r) { return reports_depth(r) && !r.depth_at_end; });
  if (depth_group == results.end()) depth_group = std::ranges::find_if(results, reports_depth);
  std::set<EntityId> uncertain_layouts;
  for (auto r = results.begin(); r != results.end(); ++r) {
    merge(*r, r == depth_group);
    uncertain_layouts.merge(r->uncertain_layouts);
  }
  std::optional<PeerLayout> blocked_peer;
  if (!uncertain_layouts.empty()) {
    std::set<EntityId> blocked_classes;
    for (const auto& result : results)
      for (const auto& peer : result.peer_layouts)
        if (uncertain_layouts.contains(peer.cls) || uncertain_layouts.contains(peer.layout.primary_base)) {
          blocked_classes.insert(peer.cls);
          if (!blocked_peer) blocked_peer = peer;
        }
    for (ClassShape& shape : out.shapes.classes) {
      shape.layout_uncertain |= uncertain_layouts.contains(shape.entity);
      if (blocked_classes.contains(shape.entity)) {
        shape.peer_virtual_layout = sema::PeerVirtualLayout::none;
        shape.peer_automatic_primary_base = EntityId::none;
      }
    }
  }
  std::ranges::stable_sort(out.messages, {}, &SemaMessage::token);
  if (!out.has_errors()) {
    std::vector<UnitPart> parts;
    for (GroupResult& r : results) parts.push_back(std::move(*r.part));
    out.unit = finish_unit(parts, world, src.name, region.files.at(0), uncertain_layouts, *index);
    if (!out.unit.virtual_emission_blocked && blocked_peer) {
      out.unit.virtual_emission_blocked = true;
      out.unit.virtual_declaration = blocked_peer->layout.virtual_declaration;
    }
    if (!out.unit.virtual_emission_blocked && !uncertain_layouts.empty()) {
      // A later declaration can make an earlier constructor's owner or primary base's
      // layout uncertain. Validate each emitted lifecycle function against all groups.
      decltype(GroupResult::dynamic_layouts) layouts;
      for (auto& r : results) layouts.merge(r.dynamic_layouts);
      for (const auto& function : out.unit.functions) {
        const EntityId table = function.constructor_table != EntityId::none ? function.constructor_table : function.destructor_table;
        if (table == EntityId::none) continue;
        const EntityId owner = world.entities.rec(table).scope;
        for (EntityId c = owner; c != EntityId::none;) {
          if (uncertain_layouts.contains(c)) {
            out.unit.virtual_emission_blocked = true;
            if (const auto declaration = layouts.find(owner); declaration != layouts.end())
              out.unit.virtual_declaration = declaration->second.virtual_declaration;
            break;
          }
          const auto layout = layouts.find(c);
          c = layout == layouts.end() ? EntityId::none : layout->second.primary_base;
        }
        if (out.unit.virtual_emission_blocked) break;
      }
    }
  }
  co_return out;
}

}  // namespace ycxx::pipeline
