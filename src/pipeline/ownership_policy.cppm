// Normalize typed C++ declarations into the ownership policy consumed by the body checker.
module ycxx.pipeline:ownership_policy;

import std;
import ycxx.ast;
import ycxx.sema;

namespace ycxx::pipeline {

using ast::EntityId;
using ast::QualType;

enum class Ownership { ordinary, affine, linear };
enum class OwnershipEffect { unknown, issue, observe, consume, transfer };

struct OwnershipPolicyAlias {
  EntityId entity;
  bool active;
};

struct OwnershipPolicySite {
  OwnershipPolicyAlias declaration;
  ast::Loc at;
};

struct OwnershipPolicyProblem {
  ast::Loc at;
  std::string message;
  std::optional<ast::Loc> previous{};
};

struct OwnershipPolicy {
  std::vector<std::pair<QualType, Ownership>> resources;
  std::vector<std::pair<EntityId, OwnershipEffect>> operations;

  Ownership ownership(QualType canonical) const {
    const auto type = ast::without_cv(canonical);
    const auto it = std::ranges::lower_bound(resources, type, std::ranges::less{}, &decltype(resources)::value_type::first);
    return it != resources.end() && it->first == type ? it->second : Ownership::ordinary;
  }

  OwnershipEffect effect(EntityId entity) const {
    const auto it = std::ranges::lower_bound(operations, entity, std::ranges::less{}, &decltype(operations)::value_type::first);
    return it != operations.end() && it->first == entity ? it->second : OwnershipEffect::unknown;
  }
};

struct OwnershipRegistry {
  OwnershipPolicy policy;
  std::vector<OwnershipPolicyProblem> problems;
};

namespace {

enum class Marker { other, policy, use_policy, affine, linear, creates, observes, consumes, transfers };

struct Specialization {
  Marker marker = Marker::other;
  const sema::EntityRec* entity = nullptr;
};

Specialization marker(const sema::World& world, EntityId id) {
  const auto* specialized = world.entities.find(id);
  if (!specialized || specialized->kind != sema::EntityKind::class_) return {};
  const auto* primary = world.entities.find(specialized->templ);
  if (!primary || primary->kind != sema::EntityKind::class_template) return {};
  const auto* inner = world.entities.find(primary->scope);
  if (!inner || inner->kind != sema::EntityKind::namespace_ || world.ins.names.spelling(inner->name) != "ownership") return {};
  const auto* outer = world.entities.find(inner->scope);
  if (!outer || outer->kind != sema::EntityKind::namespace_ || outer->scope != EntityId::none ||
      world.ins.names.spelling(outer->name) != "ycxx") return {};
  const auto name = world.ins.names.spelling(primary->name);
  if (name == "policy") return {Marker::policy, specialized};
  if (name == "use_policy") return {Marker::use_policy, specialized};
  if (name == "affine") return {Marker::affine, specialized};
  if (name == "linear") return {Marker::linear, specialized};
  if (name == "creates") return {Marker::creates, specialized};
  if (name == "observes") return {Marker::observes, specialized};
  if (name == "consumes") return {Marker::consumes, specialized};
  if (name == "transfers") return {Marker::transfers, specialized};
  return {};
}

EntityId class_entity(const sema::World& world, QualType type) {
  return world.types.is_class(type) ? world.types.class_of(type) : EntityId::none;
}

std::optional<EntityId> function_address(const ast::Interners& in, ast::ConstId value) {
  if (in.values.is<ast::VLvalue>(value)) {
    const auto address = in.values.get<ast::VLvalue>(value);
    if (address.offset != 0 || in.seqs.size(address.path) != 0) return std::nullopt;
    value = address.base;
  }
  if (!in.values.is<ast::VEntity>(value)) return std::nullopt;
  return in.values.get<ast::VEntity>(value).entity;
}

OwnershipEffect effect_of(Marker item) {
  switch (item) {
  case Marker::creates: return OwnershipEffect::issue;
  case Marker::observes: return OwnershipEffect::observe;
  case Marker::consumes: return OwnershipEffect::consume;
  case Marker::transfers: return OwnershipEffect::transfer;
  default: return OwnershipEffect::unknown;
  }
}

bool compatible(const sema::World& world, const OwnershipPolicy& policy, EntityId id, OwnershipEffect effect) {
  const auto* entity = world.entities.find(id);
  if (!entity || entity->kind != sema::EntityKind::function) return false;
  if (entity->scope != EntityId::none) {
    const auto* scope = world.entities.find(entity->scope);
    if (!scope || scope->kind != sema::EntityKind::namespace_) return false;
  }
  const auto fn = world.types.rec(entity->type);
  if (fn.kind != sema::TypeKind::function || !fn.fn_noexcept || fn.variadic) return false;
  const auto result = world.types.canonical(fn.inner);
  if (effect == OwnershipEffect::issue)
    return fn.params.empty() && ast::cv_of(result) == ast::Cv::none && policy.ownership(result) != Ownership::ordinary;
  if (fn.params.size() != 1) return false;
  const auto arg = world.types.rec(fn.params.front());
  if (arg.kind != sema::TypeKind::reference) return false;
  const auto resource = world.types.canonical(arg.inner);
  if (policy.ownership(resource) == Ownership::ordinary) return false;
  if (effect == OwnershipEffect::observe)
    return !arg.rvalue_ref && ast::cv_of(resource) == ast::Cv::const_ && world.types.is_void(result);
  if (!arg.rvalue_ref || ast::cv_of(resource) != ast::Cv::none) return false;
  return effect == OwnershipEffect::transfer ? result == resource : world.types.is_void(result);
}

}  // namespace

std::optional<OwnershipPolicyAlias> ownership_policy_alias(const sema::World& world, QualType type) {
  const auto entity = class_entity(world, type);
  const auto kind = marker(world, entity).marker;
  if (kind == Marker::policy || kind == Marker::use_policy) return OwnershipPolicyAlias{entity, kind == Marker::use_policy};
  return std::nullopt;
}

OwnershipRegistry read_ownership_policies(const sema::World& world, std::span<const OwnershipPolicySite> sites) {
  OwnershipRegistry out;
  std::map<QualType, std::pair<Ownership, ast::Loc>> resources;
  std::map<EntityId, std::pair<OwnershipEffect, ast::Loc>> operations;
  std::map<EntityId, ast::Loc> definitions;
  std::set<EntityId> seen;
  std::vector<std::pair<EntityId, ast::Loc>> pending;
  for (const auto& site : sites) definitions.try_emplace(site.declaration.entity, site.at);
  for (const auto& site : std::views::reverse(sites))
    if (site.declaration.active) pending.emplace_back(site.declaration.entity, site.at);
  auto report = [&](ast::Loc at, std::string message, std::optional<ast::Loc> first = {}) {
    out.problems.push_back({at, std::move(message), first});
  };
  while (!pending.empty()) {
    auto [id, at] = pending.back();
    pending.pop_back();
    if (auto origin = definitions.find(id); origin != definitions.end()) at = origin->second;
    const auto [kind, decl] = marker(world, id);
    if (kind == Marker::other) {
      report(at, "unrecognized entry in ownership policy");
      continue;
    }
    if (!seen.insert(id).second) continue;
    const auto& args = decl->args;
    if (kind == Marker::policy || kind == Marker::use_policy) {
      if (args.size() != 1 || args.front().kind != ast::TemplateArgKind::pack) {
        report(at, "an ownership policy requires a pack of entries");
        continue;
      }
      const auto entries = world.ins.seqs.elements(args.front().elems);
      for (const auto& entry : std::views::reverse(entries))
        pending.emplace_back(entry.kind == ast::TemplateArgKind::type ? class_entity(world, entry.type) : EntityId::none, at);
      continue;
    }
    if (kind == Marker::affine || kind == Marker::linear) {
      if (args.size() != 1 || args.front().kind != ast::TemplateArgKind::type || !world.types.is_class(args.front().type) ||
          ast::cv_of(world.types.canonical(args.front().type)) != ast::Cv::none) {
        report(at, "an ownership resource must name an unqualified class type");
        continue;
      }
      const auto type = world.types.canonical(args.front().type);
      const auto owner = kind == Marker::linear ? Ownership::linear : Ownership::affine;
      const auto [it, fresh] = resources.try_emplace(type, owner, at);
      if (!fresh && it->second.first != owner)
        report(at, std::format("conflicting ownership policies for '{}'", world.types.spell(type)), it->second.second);
      continue;
    }
    const auto function = args.size() == 2 && args[1].kind == ast::TemplateArgKind::value
                              ? function_address(world.ins, args[1].value) : std::nullopt;
    if (!function) {
      report(at, "an ownership operation must name a function by address");
      continue;
    }
    const auto effect = effect_of(kind);
    const auto [it, fresh] = operations.try_emplace(*function, effect, at);
    if (!fresh && it->second.first != effect)
      report(at, "conflicting contracts for an ownership operation", it->second.second);
  }
  for (const auto& [type, spec] : resources) out.policy.resources.emplace_back(type, spec.first);
  for (const auto& [entity, spec] : operations) {
    if (!compatible(world, out.policy, entity, spec.first))
      report(spec.second, "ownership operation signature does not match its contract and registered resource");
    else out.policy.operations.emplace_back(entity, spec.first);
  }
  return out;
}

}  // namespace ycxx::pipeline
