// Normalize typed C++ declarations and annotations before checking function bodies.
module ycxx.pipeline:policy_registry;

import std;
import ycxx.ast;
import ycxx.sema;

namespace ycxx::pipeline {

using ast::EntityId;
using ast::QualType;

enum class Ownership { ordinary, affine, linear };
// Values match the public C++26 `ycxx::ownership::operation` enumerators.
enum class OwnershipEffect { unknown = 0, issue = 1, observe = 2, consume = 3, transfer = 4, verified_observe = 5 };

bool is_observation(OwnershipEffect effect) {
  return effect == OwnershipEffect::observe || effect == OwnershipEffect::verified_observe;
}

struct PolicyAlias {
  EntityId entity;
  bool active;
};

struct PolicySite {
  PolicyAlias declaration;
  ast::Loc at;
};

struct PolicyAnnotationSite {
  EntityId declaration;
  QualType declaration_type;
  QualType annotation_type;
  ast::ConstId value;
  ast::Loc at;
};

struct PolicyOrigin {
  ast::u32 unit;
  ast::Loc at;
};

struct PolicyProblem {
  PolicyOrigin origin;
  std::string message;
  std::optional<PolicyOrigin> previous{};
};

// An internal-linkage declaration in a shared header can have the same entity handle in
// several inputs. Its policy applies to the function in the input that activated it.
using PolicyOperation = std::pair<EntityId, std::optional<ast::u32>>;

PolicyOperation policy_operation(const sema::World& world, EntityId entity, ast::u32 unit) {
  const auto* declaration = world.entities.find(entity);
  return {entity, declaration && declaration->linkage == ast::Linkage::internal ? std::optional(unit) : std::nullopt};
}

struct EffectBound {
  std::vector<QualType> tags;
  bool trusted;
  std::vector<std::pair<QualType, ast::u64>> grades;
  bool operator==(const EffectBound&) const = default;
};

struct PolicyDeclarations {
  std::vector<std::tuple<QualType, Ownership, PolicyOrigin>> resources;
  std::vector<std::tuple<PolicyOperation, OwnershipEffect, PolicyOrigin>> operations;
  std::vector<std::tuple<PolicyOperation, EffectBound, PolicyOrigin>> effect_bounds;
  std::vector<PolicyProblem> problems;
};

struct OwnershipPolicy {
  std::vector<std::pair<QualType, Ownership>> resources;
  std::vector<std::pair<PolicyOperation, OwnershipEffect>> operations;
  std::vector<PolicyOperation> observer_definitions;
  std::vector<PolicyOperation> labeled_observers;

  bool has_observer_definition(PolicyOperation operation) const {
    return std::ranges::binary_search(observer_definitions, operation);
  }

  Ownership ownership(QualType canonical) const {
    const auto type = ast::without_cv(canonical);
    const auto it = std::ranges::lower_bound(resources, type, std::ranges::less{}, &decltype(resources)::value_type::first);
    return it != resources.end() && it->first == type ? it->second : Ownership::ordinary;
  }

  OwnershipEffect effect(PolicyOperation operation) const {
    const auto it = std::ranges::lower_bound(operations, operation, std::ranges::less{}, &decltype(operations)::value_type::first);
    return it != operations.end() && it->first == operation ? it->second : OwnershipEffect::unknown;
  }
};

struct PolicyRegistry {
  OwnershipPolicy ownership;
  std::map<PolicyOperation, EffectBound> effects;
  std::map<PolicyOperation, PolicyOrigin> effect_origins;
  std::set<PolicyOperation> external_effect_declarations;
  std::map<ast::u32, std::vector<PolicyProblem>> problems;
};

namespace {

enum class Marker {
  other, policy, use_policy, affine, linear, creates, observes, verified_observes, consumes, transfers,
  effect_set, effect_grade, effect_bound, trusted_effect_bound, effect_contract, trusted_effect_contract,
};

struct Specialization {
  Marker marker = Marker::other;
  const sema::EntityRec* entity = nullptr;
};

bool in_policy_namespace(const sema::World& world, EntityId scope, std::string_view name) {
  const auto* inner = world.entities.find(scope);
  if (!inner || inner->kind != sema::EntityKind::namespace_ || world.ins.names.spelling(inner->name) != name) return false;
  const auto* outer = world.entities.find(inner->scope);
  return outer && outer->kind == sema::EntityKind::namespace_ && outer->scope == EntityId::none &&
         world.ins.names.spelling(outer->name) == "ycxx";
}

Specialization marker(const sema::World& world, EntityId id) {
  const auto* specialized = world.entities.find(id);
  if (!specialized || specialized->kind != sema::EntityKind::class_) return {};
  const auto* primary = world.entities.find(specialized->templ);
  if (!primary || primary->kind != sema::EntityKind::class_template) return {};
  const auto name = world.ins.names.spelling(primary->name);
  if (in_policy_namespace(world, primary->scope, "effects")) {
    if (name == "set") return {Marker::effect_set, specialized};
    if (name == "at_most") return {Marker::effect_grade, specialized};
    if (name == "bound") return {Marker::effect_bound, specialized};
    if (name == "trusted") return {Marker::trusted_effect_bound, specialized};
    if (name == "contract") return {Marker::effect_contract, specialized};
    if (name == "trusted_contract") return {Marker::trusted_effect_contract, specialized};
    return {};
  }
  if (!in_policy_namespace(world, primary->scope, "ownership")) return {};
  if (name == "policy") return {Marker::policy, specialized};
  if (name == "use_policy") return {Marker::use_policy, specialized};
  if (name == "affine") return {Marker::affine, specialized};
  if (name == "linear") return {Marker::linear, specialized};
  if (name == "creates") return {Marker::creates, specialized};
  if (name == "observes") return {Marker::observes, specialized};
  if (name == "verified_observes") return {Marker::verified_observes, specialized};
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
  case Marker::verified_observes: return OwnershipEffect::verified_observe;
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
  if (!entity || !in_policy_namespace(world, entity->scope, "ownership")) return AnnotationKind::other;
  const auto name = world.ins.names.spelling(entity->name);
  if (name == "resource_mode") return AnnotationKind::resource;
  if (name == "operation") return AnnotationKind::operation;
  return AnnotationKind::other;
}

std::expected<EffectBound, std::string_view> read_effect_bound(const sema::World& world, const ast::TemplateArg& arg,
                                                               bool trusted) {
  constexpr std::string_view bad_set = "effect bound requires ycxx::effects::set of unqualified class or enum tags or at_most grades";
  if (arg.kind != ast::TemplateArgKind::type) return std::unexpected(bad_set);
  const auto [kind, declaration] = marker(world, class_entity(world, arg.type));
  if (kind != Marker::effect_set || declaration->args.size() != 1 ||
      declaration->args.front().kind != ast::TemplateArgKind::pack)
    return std::unexpected(bad_set);

  std::vector<std::pair<QualType, std::optional<ast::u64>>> entries;
  for (const auto& tag : world.ins.seqs.elements(declaration->args.front().elems)) {
    if (tag.kind != ast::TemplateArgKind::type) return std::unexpected(bad_set);
    auto type = world.types.canonical(tag.type);
    std::optional<ast::u64> maximum;
    const auto [entry, grade] = marker(world, class_entity(world, type));
    if (entry == Marker::effect_grade) {
      if (ast::cv_of(type) != ast::Cv::none || grade->args.size() != 2 ||
          grade->args[0].kind != ast::TemplateArgKind::type || grade->args[1].kind != ast::TemplateArgKind::value ||
          !world.ins.values.is<ast::VInt>(grade->args[1].value))
        return std::unexpected(bad_set);
      const auto bits = ast::int_bits(world.ins, grade->args[1].value);
      if (bits.hi != 0 || bits.lo == 0) return std::unexpected(bad_set);
      maximum = bits.lo;
      type = world.types.canonical(grade->args[0].type);
      if (marker(world, class_entity(world, type)).marker == Marker::effect_grade)
        return std::unexpected(bad_set);
    }
    const auto kind = world.types.rec(type).kind;
    if (ast::cv_of(type) != ast::Cv::none || (kind != sema::TypeKind::class_ && kind != sema::TypeKind::enum_))
      return std::unexpected(bad_set);
    entries.emplace_back(type, maximum);
  }
  std::ranges::sort(entries, {}, &decltype(entries)::value_type::first);
  EffectBound out{{}, trusted, {}};
  out.tags.reserve(entries.size());
  std::optional<ast::u64> previous;
  for (const auto& [tag, maximum] : entries) {
    if (!out.tags.empty() && out.tags.back() == tag) {
      if (maximum != previous) return std::unexpected("conflicting usage grades for the same effect tag in a set");
      continue;
    }
    previous = maximum;
    out.tags.push_back(tag);
    if (maximum) out.grades.emplace_back(tag, *maximum);
  }
  return out;
}

bool namespace_function(const sema::World& world, const sema::EntityRec* entity) {
  if (!entity || entity->kind != sema::EntityKind::function) return false;
  if (entity->scope == EntityId::none) return true;
  const auto* scope = world.entities.find(entity->scope);
  return scope && scope->kind == sema::EntityKind::namespace_;
}

bool effect_signature(const sema::World& world, EntityId id) {
  const auto* entity = world.entities.find(id);
  if (!namespace_function(world, entity)) return false;
  const auto fn = world.types.rec(entity->type);
  const auto scalar_or_reference = [&](QualType type) {
    const auto kind = world.types.rec(type).kind;
    return kind == sema::TypeKind::reference || world.types.is_scalar(type);
  };
  return fn.kind == sema::TypeKind::function && !fn.variadic &&
         (world.types.is_void(fn.inner) || scalar_or_reference(fn.inner)) &&
         std::ranges::all_of(fn.params, scalar_or_reference);
}

bool compatible(const sema::World& world, const OwnershipPolicy& policy, EntityId id, OwnershipEffect effect) {
  const auto* entity = world.entities.find(id);
  if (!namespace_function(world, entity)) return false;
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
  if (is_observation(effect))
    return !arg.rvalue_ref && ast::cv_of(resource) == ast::Cv::const_ && world.types.is_void(result);
  if (!arg.rvalue_ref || ast::cv_of(resource) != ast::Cv::none) return false;
  return effect == OwnershipEffect::transfer ? result == resource : world.types.is_void(result);
}

}  // namespace

std::optional<PolicyAlias> policy_alias(const sema::World& world, QualType type) {
  const auto entity = class_entity(world, type);
  const auto kind = marker(world, entity).marker;
  if (kind == Marker::policy || kind == Marker::use_policy) return PolicyAlias{entity, kind == Marker::use_policy};
  return std::nullopt;
}

bool is_policy_annotation(const sema::World& world, QualType type) {
  const auto kind = marker(world, class_entity(world, type)).marker;
  return kind == Marker::effect_contract || kind == Marker::trusted_effect_contract ||
         annotation_kind(world, type) != AnnotationKind::other;
}

PolicyDeclarations read_policy_declarations(const sema::World& world, ast::u32 unit,
                                            std::span<const PolicySite> sites,
                                            std::span<const PolicyAnnotationSite> annotations) {
  PolicyDeclarations out;
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
    out.resources.emplace_back(type, mode, PolicyOrigin{unit, at});
  };
  auto operation = [&](EntityId entity, OwnershipEffect effect, ast::Loc at) {
    out.operations.emplace_back(policy_operation(world, entity, unit), effect, PolicyOrigin{unit, at});
  };
  auto effect_bound = [&](EntityId entity, const ast::TemplateArg& arg, bool trusted, ast::Loc at) {
    auto bound = read_effect_bound(world, arg, trusted);
    if (bound)
      out.effect_bounds.emplace_back(policy_operation(world, entity, unit), std::move(*bound),
                                     PolicyOrigin{unit, at});
    else report(at, std::string(bound.error()));
  };
  for (const auto& annotation : annotations) {
    const auto [effect_marker, declaration] = marker(world, class_entity(world, annotation.annotation_type));
    if (effect_marker == Marker::effect_contract || effect_marker == Marker::trusted_effect_contract) {
      if (declaration->args.size() == 1)
        effect_bound(annotation.declaration, declaration->args.front(),
                     effect_marker == Marker::trusted_effect_contract, annotation.at);
      else report(annotation.at, "invalid effect contract annotation");
      continue;
    }
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
      if (value.hi != 0 || value.lo < 1 || value.lo > static_cast<ast::u64>(OwnershipEffect::verified_observe)) {
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
    if (kind == Marker::effect_bound || kind == Marker::trusted_effect_bound) {
      const auto function = args.size() == 3 && args[1].kind == ast::TemplateArgKind::value
                                ? function_address(world.ins, args[1].value) : std::nullopt;
      if (function) effect_bound(*function, args[2], kind == Marker::trusted_effect_bound, at);
      else report(at, "an effect bound must name a function by address");
      continue;
    }
    if (effect_of(kind) == OwnershipEffect::unknown) {
      report(at, "unrecognized entry in policy");
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

PolicyRegistry compose_policy_registry(const sema::World& world,
                                       std::span<const std::reference_wrapper<const PolicyDeclarations>> fragments) {
  PolicyRegistry out;
  std::map<QualType, std::pair<Ownership, PolicyOrigin>> resources;
  std::map<PolicyOperation, std::pair<OwnershipEffect, PolicyOrigin>> operations;
  auto report = [&](PolicyOrigin origin, std::string message, std::optional<PolicyOrigin> previous = {}) {
    out.problems[origin.unit].push_back({origin, std::move(message), previous});
  };
  for (const PolicyDeclarations& fragment : fragments) {
    for (const auto& problem : fragment.problems) out.problems[problem.origin.unit].push_back(problem);
    for (const auto& [type, mode, origin] : fragment.resources) {
      const auto [it, fresh] = resources.try_emplace(type, mode, origin);
      if (!fresh && it->second.first != mode)
        report(origin, std::format("conflicting ownership policies for '{}'", world.types.spell(type)), it->second.second);
    }
    for (const auto& [operation, effect, origin] : fragment.operations) {
      const auto [it, fresh] = operations.try_emplace(operation, effect, origin);
      if (fresh || it->second.first == effect) continue;
      if (is_observation(it->second.first) && is_observation(effect)) {
        if (effect == OwnershipEffect::verified_observe) it->second = {effect, origin};
      } else report(origin, "conflicting contracts for an ownership operation", it->second.second);
    }
    for (const auto& [operation, bound, origin] : fragment.effect_bounds) {
      const auto [it, fresh] = out.effects.try_emplace(operation, bound);
      if (fresh) out.effect_origins.emplace(operation, origin);
      else if (it->second != bound)
        report(origin, "conflicting effect bounds for a function", out.effect_origins.at(operation));
    }
  }
  for (const auto& [operation, origin] : out.effect_origins)
    if (!effect_signature(world, operation.first))
      report(origin, "effect bound requires a non-variadic namespace function with scalar or reference parameters and result");
  for (const auto& [type, spec] : resources) out.ownership.resources.emplace_back(type, spec.first);
  for (const auto& [operation, spec] : operations) {
    if (!compatible(world, out.ownership, operation.first, spec.first))
      report(spec.second, "ownership operation signature does not match its contract and registered resource");
    else out.ownership.operations.emplace_back(operation, spec.first);
  }
  return out;
}

}  // namespace ycxx::pipeline
