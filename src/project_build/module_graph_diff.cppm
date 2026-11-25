// Match the semantic contents of two module graph snapshots (#1383).
export module ycxx.build.module_graph_diff;

import std;
import ycxx.build.inspection_model;

export namespace ycxx::build {

template <class Record>
struct ModuleChange {
  std::string change;
  std::optional<Record> before;
  std::optional<Record> after;
};

struct ModuleGraphChanges {
  std::optional<std::string> unavailable;
  std::optional<std::string> matching;
  std::optional<ModuleChange<bool>> completeness;
  std::vector<ModuleChange<ModuleUnitInspection>> units;
  std::vector<ModuleChange<ModuleImportInspection>> imports;
  std::vector<ModuleChange<ModuleConflictInspection>> conflicts;
  std::vector<ModuleChange<std::vector<std::size_t>>> cycles;
};

std::expected<ModuleGraphChanges, std::string> compare_module_graph(
    const ModuleGraphInspection& before, const ModuleGraphInspection& after);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

using UnitIdentities = std::map<ModuleUnitConfiguration, std::size_t>;
using UnitIds = std::map<std::size_t, std::size_t>;
using Site = std::pair<std::size_t, std::size_t>;
using KindName = std::pair<std::string_view, std::string_view>;
using DirectiveKey = std::tuple<std::size_t, std::string_view, std::string_view, std::size_t>;
using Declarations = std::map<Site, DirectiveKey>;
using Candidates = std::vector<DirectiveKey>;
using Members = std::vector<std::size_t>;

template <class Key, class Record, class Meaning>
using Index = std::map<Key, std::pair<const Record*, Meaning>>;

struct GraphIndex {
  Index<std::size_t, ModuleUnitInspection, std::pair<std::string_view, std::vector<KindName>>> units;
  Index<DirectiveKey, ModuleImportInspection, std::pair<std::string_view, Candidates>> imports;
  Index<std::string_view, ModuleConflictInspection, Candidates> conflicts;
  Index<Members, Members, std::monostate> cycles;
};

std::string unit_source(const ModuleUnitInspection& unit, const ModuleGraphInspection& graph) {
  return unit.configuration ? unit.configuration->source : logical_module_path(unit.source, graph.native_target);
}

struct UnitCounts {
  std::array<std::size_t, 2> counts = {};
  bool legacy = false;
};

std::expected<std::set<std::string>, std::string> legacy_sources(
    const ModuleGraphInspection& before, const ModuleGraphInspection& after) {
  std::map<std::string, UnitCounts> sources;
  const std::array graphs{&before, &after};
  for (std::size_t side = 0; side < graphs.size(); ++side) {
    for (const auto& unit : graphs[side]->units) {
      auto& usage = sources[unit_source(unit, *graphs[side])];
      ++usage.counts[side];
      usage.legacy |= !unit.configuration;
    }
  }
  std::set<std::string> result;
  for (const auto& [source, usage] : sources) {
    if (!usage.legacy) continue;
    if (std::ranges::max(usage.counts) > 1) return std::unexpected("ambiguous_legacy_configuration");
    result.insert(source);
  }
  return result;
}

std::expected<Candidates, std::string> candidate_keys(std::string_view name,
    std::span<const ModuleCandidateInspection> candidates, const Declarations& declarations) {
  Candidates result;
  for (const auto& candidate : candidates) {
    const auto found = declarations.find({candidate.unit_id, candidate.directive_index});
    if (found == declarations.end() || std::get<2>(found->second) != name)
      return std::unexpected(std::format("invalid provider for '{}' at unit {} directive {}",
                                         name, candidate.unit_id, candidate.directive_index));
    result.push_back(found->second);
  }
  std::ranges::sort(result);
  if (std::ranges::adjacent_find(result) != result.end())
    return std::unexpected(std::format("duplicate provider for '{}'", name));
  return result;
}

std::expected<GraphIndex, std::string> index_graph(const ModuleGraphInspection& graph,
    const std::set<std::string>& fallbacks, UnitIdentities& identities) {
  GraphIndex index;
  UnitIds ids;
  Declarations declarations;
  for (const auto& unit : graph.units) {
    const auto source = unit_source(unit, graph);
    const auto configuration = fallbacks.contains(source) ? ModuleUnitConfiguration{source, {}} : *unit.configuration;
    const auto identity = identities.try_emplace(configuration, identities.size()).first->second;
    if (!ids.emplace(unit.id, identity).second)
      return std::unexpected(std::format("duplicate module unit id {}", unit.id));

    std::vector<KindName> names;
    std::map<KindName, std::size_t> occurrences;
    for (const auto& declaration : unit.declarations) {
      const KindName name{declaration.kind, declaration.name};
      const DirectiveKey key{identity, declaration.kind, declaration.name, occurrences[name]++};
      if (!declarations.emplace(Site{unit.id, declaration.directive_index}, key).second)
        return std::unexpected(std::format("duplicate directive in module unit {}", unit.id));
      names.push_back(name);
    }
    std::ranges::sort(names);
    if (!index.units.emplace(identity, std::pair{&unit, std::pair{std::string_view(unit.scan), std::move(names)}}).second)
      return std::unexpected(std::format("duplicate module unit configuration for '{}'", source));
  }

  std::set<Site> sites;
  std::map<std::tuple<std::size_t, std::string_view, std::string_view>, std::size_t> occurrences;
  for (const auto& import : graph.imports) {
    const auto unit = ids.find(import.unit_id);
    if (unit == ids.end()) return std::unexpected(std::format("unknown importing unit {}", import.unit_id));
    const Site site{import.unit_id, import.directive_index};
    if (declarations.contains(site) || !sites.insert(site).second)
      return std::unexpected(std::format("duplicate directive in module unit {}", import.unit_id));
    const auto name = std::tuple{unit->second, std::string_view(import.kind), std::string_view(import.name)};
    const DirectiveKey key{unit->second, import.kind, import.name, occurrences[name]++};
    auto providers = candidate_keys(import.name, import.candidates, declarations);
    if (!providers) return std::unexpected(std::move(providers.error()));
    index.imports.emplace(key, std::pair{&import, std::pair{std::string_view(import.status), std::move(*providers)}});
  }
  for (const auto& conflict : graph.conflicts) {
    auto providers = candidate_keys(conflict.name, conflict.candidates, declarations);
    if (!providers) return std::unexpected(std::move(providers.error()));
    if (!index.conflicts.emplace(conflict.name, std::pair{&conflict, std::move(*providers)}).second)
      return std::unexpected(std::format("duplicate module conflict '{}'", conflict.name));
  }
  for (const auto& cycle : graph.cycles) {
    Members members;
    for (const auto unit : cycle) {
      const auto found = ids.find(unit);
      if (found == ids.end()) return std::unexpected(std::format("unknown cyclic unit {}", unit));
      members.push_back(found->second);
    }
    std::ranges::sort(members);
    if (members.empty() || std::ranges::adjacent_find(members) != members.end() ||
        !index.cycles.emplace(std::move(members), std::pair{&cycle, std::monostate{}}).second)
      return std::unexpected("invalid or repeated cyclic component");
  }
  return index;
}

template <class Key, class Record, class Meaning>
std::vector<ModuleChange<Record>> changes(
    const Index<Key, Record, Meaning>& before, const Index<Key, Record, Meaning>& after) {
  std::vector<ModuleChange<Record>> result;
  for (const auto& [key, old] : before) {
    const auto next = after.find(key);
    if (next == after.end()) result.push_back({"removed", *old.first, std::nullopt});
    else if (old.second != next->second.second) result.push_back({"changed", *old.first, *next->second.first});
  }
  for (const auto& [key, next] : after)
    if (!before.contains(key)) result.push_back({"added", std::nullopt, *next.first});
  return result;
}

}  // namespace

std::expected<ModuleGraphChanges, std::string> compare_module_graph(
    const ModuleGraphInspection& before, const ModuleGraphInspection& after) {
  ModuleGraphChanges result{};
  auto fallbacks = legacy_sources(before, after);
  if (!fallbacks) {
    result.unavailable = std::move(fallbacks.error());
    return result;
  }
  if (!fallbacks->empty()) result.matching = "legacy_source_only";

  UnitIdentities identities;
  auto old_graph = index_graph(before, *fallbacks, identities);
  if (!old_graph) return std::unexpected(std::format("saved module graph: {}", old_graph.error()));
  auto new_graph = index_graph(after, *fallbacks, identities);
  if (!new_graph) return std::unexpected(std::format("current module graph: {}", new_graph.error()));

  if (before.complete != after.complete) result.completeness = ModuleChange<bool>{"changed", before.complete, after.complete};
  result.units = changes(old_graph->units, new_graph->units);
  result.imports = changes(old_graph->imports, new_graph->imports);
  result.conflicts = changes(old_graph->conflicts, new_graph->conflicts);
  result.cycles = changes(old_graph->cycles, new_graph->cycles);
  return result;
}

}  // namespace ycxx::build
