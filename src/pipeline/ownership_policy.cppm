// Normalize typed C++ declarations into the ownership policy consumed by the body checker.
module ycxx.pipeline:ownership_policy;

import std;
import ycxx.ast;
import ycxx.sema;

namespace ycxx::pipeline {

using ast::EntityId;
using ast::QualType;

enum class Ownership { ordinary, affine, linear };
// Values match the public C++26 `ycxx::ownership::operation` enumerators.
enum class OwnershipEffect { unknown = 0, issue = 1, observe = 2, consume = 3, transfer = 4 };

struct OwnershipPolicyAlias {
  EntityId entity;
  bool active;
};

struct OwnershipPolicySite {
  OwnershipPolicyAlias declaration;
  ast::Loc at;
};

struct OwnershipAnnotationSite {
  EntityId declaration;
  QualType declaration_type;
  QualType annotation_type;
  ast::ConstId value;
  ast::Loc at;
};

struct OwnershipOrigin {
  ast::u32 unit;
  ast::Loc at;
};

struct OwnershipPolicyProblem {
  OwnershipOrigin origin;
  std::string message;
  std::optional<OwnershipOrigin> previous{};
};

// An internal-linkage declaration in a shared header can have the same entity handle in
// several inputs. Its policy applies to the function in the input that activated it.
using OwnershipOperation = std::pair<EntityId, std::optional<ast::u32>>;

OwnershipOperation ownership_operation(const sema::World& world, EntityId entity, ast::u32 unit) {
  const auto* declaration = world.entities.find(entity);
  return {entity, declaration && declaration->linkage == ast::Linkage::internal ? std::optional(unit) : std::nullopt};
}

struct OwnershipDeclarations {
  std::vector<std::tuple<QualType, Ownership, OwnershipOrigin>> resources;
  std::vector<std::tuple<OwnershipOperation, OwnershipEffect, OwnershipOrigin>> operations;
  std::vector<OwnershipPolicyProblem> problems;
};

struct OwnershipPolicy {
  std::vector<std::pair<QualType, Ownership>> resources;
  std::vector<std::pair<OwnershipOperation, OwnershipEffect>> operations;

  Ownership ownership(QualType canonical) const {
    const auto type = ast::without_cv(canonical);
    const auto it = std::ranges::lower_bound(resources, type, std::ranges::less{}, &decltype(resources)::value_type::first);
    return it != resources.end() && it->first == type ? it->second : Ownership::ordinary;
  }

  OwnershipEffect effect(OwnershipOperation operation) const {
    const auto it = std::ranges::lower_bound(operations, operation, std::ranges::less{}, &decltype(operations)::value_type::first);
    return it != operations.end() && it->first == operation ? it->second : OwnershipEffect::unknown;
  }
};

struct OwnershipRegistry {
  OwnershipPolicy policy;
  std::map<ast::u32, std::vector<OwnershipPolicyProblem>> problems;
};

namespace {

enum class Marker { other, policy, use_policy, affine, linear, creates, observes, consumes, transfers };

struct Specialization {
  Marker marker = Marker::other;
  const sema::EntityRec* entity = nullptr;
};

bool in_ownership_namespace(const sema::World& world, EntityId scope) {
  const auto* inner = world.entities.find(scope);
  if (!inner || inner->kind != sema::EntityKind::namespace_ || world.ins.names.spelling(inner->name) != "ownership") return false;
  const auto* outer = world.entities.find(inner->scope);
  return outer && outer->kind == sema::EntityKind::namespace_ && outer->scope == EntityId::none &&
         world.ins.names.spelling(outer->name) == "ycxx";
}

Specialization marker(const sema::World& world, EntityId id) {
  const auto* specialized = world.entities.find(id);
  if (!specialized || specialized->kind != sema::EntityKind::class_) return {};
  const auto* primary = world.entities.find(specialized->templ);
  if (!primary || primary->kind != sema::EntityKind::class_template || !in_ownership_namespace(world, primary->scope)) return {};
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

enum class AnnotationKind { other, resource, operation };

AnnotationKind annotation_kind(const sema::World& world, QualType type) {
  const auto t = world.types.rec(type);
  if (t.kind != sema::TypeKind::enum_) return AnnotationKind::other;
  const auto* entity = world.entities.find(t.entity);
  if (!entity || !in_ownership_namespace(world, entity->scope)) return AnnotationKind::other;
  const auto name = world.ins.names.spelling(entity->name);
  if (name == "resource_mode") return AnnotationKind::resource;
  if (name == "operation") return AnnotationKind::operation;
  return AnnotationKind::other;
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

bool is_ownership_annotation(const sema::World& world, QualType type) {
  return annotation_kind(world, type) != AnnotationKind::other;
}

OwnershipDeclarations read_ownership_policies(const sema::World& world, ast::u32 unit,
                                              std::span<const OwnershipPolicySite> sites,
                                              std::span<const OwnershipAnnotationSite> annotations) {
  OwnershipDeclarations out;
  std::map<EntityId, ast::Loc> definitions;
  std::set<EntityId> seen;
  std::vector<std::pair<EntityId, ast::Loc>> pending;
  for (const auto& site : sites) definitions.try_emplace(site.declaration.entity, site.at);
  for (const auto& site : std::views::reverse(sites))
    if (site.declaration.active) pending.emplace_back(site.declaration.entity, site.at);
  auto report = [&](ast::Loc at, std::string message) {
    out.problems.push_back({{unit, at}, std::move(message)});
  };
  auto resource = [&](QualType type, Ownership mode, ast::Loc at) {
    out.resources.emplace_back(type, mode, OwnershipOrigin{unit, at});
  };
  auto operation = [&](EntityId entity, OwnershipEffect effect, ast::Loc at) {
    out.operations.emplace_back(ownership_operation(world, entity, unit), effect, OwnershipOrigin{unit, at});
  };
  for (const auto& annotation : annotations) {
    const auto kind = annotation_kind(world, annotation.annotation_type);
    if (!world.ins.values.is<ast::VInt>(annotation.value)) {
      report(annotation.at, "ownership annotation requires an enum constant");
      continue;
    }
    const auto value = ast::int_bits(world.ins, annotation.value);
    if (kind == AnnotationKind::resource) {
      const auto type = world.types.canonical(annotation.declaration_type);
      if (class_entity(world, type) != annotation.declaration) {
        report(annotation.at, "an ownership resource annotation applies to a class declaration");
      } else if (value.hi != 0 || (value.lo != 1 && value.lo != 2)) {
        report(annotation.at, "unknown ownership resource annotation");
      } else resource(type, value.lo == 1 ? Ownership::affine : Ownership::linear, annotation.at);
    } else if (kind == AnnotationKind::operation) {
      if (value.hi != 0 || value.lo < 1 || value.lo > 4) {
        report(annotation.at, "unknown ownership operation annotation");
      } else operation(annotation.declaration, static_cast<OwnershipEffect>(value.lo), annotation.at);
    }
  }
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
      resource(type, kind == Marker::linear ? Ownership::linear : Ownership::affine, at);
      continue;
    }
    const auto function = args.size() == 2 && args[1].kind == ast::TemplateArgKind::value
                              ? function_address(world.ins, args[1].value) : std::nullopt;
    if (!function) {
      report(at, "an ownership operation must name a function by address");
      continue;
    }
    operation(*function, effect_of(kind), at);
  }
  return out;
}

OwnershipRegistry compose_ownership_policies(const sema::World& world,
                                            std::span<const std::reference_wrapper<const OwnershipDeclarations>> fragments) {
  OwnershipRegistry out;
  std::map<QualType, std::pair<Ownership, OwnershipOrigin>> resources;
  std::map<OwnershipOperation, std::pair<OwnershipEffect, OwnershipOrigin>> operations;
  auto report = [&](OwnershipOrigin origin, std::string message, std::optional<OwnershipOrigin> previous = {}) {
    out.problems[origin.unit].push_back({origin, std::move(message), previous});
  };
  for (const OwnershipDeclarations& fragment : fragments) {
    for (const auto& problem : fragment.problems) out.problems[problem.origin.unit].push_back(problem);
    for (const auto& [type, mode, origin] : fragment.resources) {
      const auto [it, fresh] = resources.try_emplace(type, mode, origin);
      if (!fresh && it->second.first != mode)
        report(origin, std::format("conflicting ownership policies for '{}'", world.types.spell(type)), it->second.second);
    }
    for (const auto& [operation, effect, origin] : fragment.operations) {
      const auto [it, fresh] = operations.try_emplace(operation, effect, origin);
      if (!fresh && it->second.first != effect)
        report(origin, "conflicting contracts for an ownership operation", it->second.second);
    }
  }
  for (const auto& [type, spec] : resources) out.policy.resources.emplace_back(type, spec.first);
  for (const auto& [operation, spec] : operations) {
    if (!compatible(world, out.policy, operation.first, spec.first))
      report(spec.second, "ownership operation signature does not match its contract and registered resource");
    else out.policy.operations.emplace_back(operation, spec.first);
  }
  return out;
}

}  // namespace ycxx::pipeline
