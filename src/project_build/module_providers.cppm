// Resolve named imports against the module declarations found in a project's reachable units (#1353).
export module ycxx.build.module_providers;

import std;
import ycxx.pipeline;

export namespace ycxx::build {

struct ModuleUnitDependencies {
  std::string source;
  bool complete;
  std::vector<std::pair<pipeline::ModuleDirectiveKind, std::string>> directives;
  std::optional<std::size_t> same_unit_as = std::nullopt;  // earlier occurrence of the same registered TU
};

struct ModuleProviderSite {
  std::size_t unit;
  std::size_t directive;
  bool operator==(const ModuleProviderSite&) const = default;
};

struct ModuleImportResolution {
  ModuleProviderSite importer;
  std::vector<ModuleProviderSite> providers;
};

struct ModuleProviderConflict {
  std::string name;
  std::vector<ModuleProviderSite> providers;
};

struct ModuleResolution {
  bool complete;  // all reachable units were scanned completely
  std::vector<ModuleImportResolution> imports;
  std::vector<std::vector<std::size_t>> cycles;  // cyclic components, in graph unit order
  std::vector<ModuleProviderConflict> conflicts;  // names provided by multiple distinct units
};

ModuleResolution resolve_module_providers(std::span<const ModuleUnitDependencies> graph);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

using UnitEdges = std::vector<std::vector<std::size_t>>;

// Kosaraju's first pass visits providers before it finishes the unit importing them,
// except when they are already on the search path. The stack keeps long import chains off
// the call stack.
std::vector<std::size_t> finish_order(const UnitEdges& providers) {
  struct Frame {
    std::size_t unit;
    std::size_t next;
  };
  std::vector<Frame> stack;
  std::vector<bool> seen(providers.size());
  std::vector<std::size_t> order;
  for (std::size_t unit = 0; unit < providers.size(); ++unit) {
    if (seen[unit]) continue;
    seen[unit] = true;
    stack.push_back({unit, 0});
    while (!stack.empty()) {
      auto& frame = stack.back();
      const auto& edges = providers[frame.unit];
      if (frame.next == edges.size()) {
        order.push_back(frame.unit);
        stack.pop_back();
      } else {
        const auto next = edges[frame.next++];
        if (seen[next]) continue;
        seen[next] = true;
        stack.push_back({next, 0});
      }
    }
  }
  return order;
}

std::vector<std::vector<std::size_t>> cyclic_components(const UnitEdges& providers, const UnitEdges& importers) {
  const auto order = finish_order(providers);
  std::vector<bool> seen(providers.size());
  std::vector<std::vector<std::size_t>> cycles;
  // In reverse finish order, the unvisited importers reachable from a root are its component.
  for (auto root : std::views::reverse(order)) {
    if (seen[root]) continue;
    seen[root] = true;
    std::vector<std::size_t> members{root};
    for (std::size_t i = 0; i < members.size(); ++i) {
      for (const auto unit : importers[members[i]]) {
        if (seen[unit]) continue;
        seen[unit] = true;
        members.push_back(unit);
      }
    }
    if (members.size() == 1 && !std::ranges::contains(providers[root], root)) continue;
    std::ranges::sort(members);
    cycles.push_back(std::move(members));
  }
  std::ranges::sort(cycles, {}, [](const auto& members) { return members.front(); });
  return cycles;
}

}  // namespace

ModuleResolution resolve_module_providers(std::span<const ModuleUnitDependencies> graph) {
  using Kind = pipeline::ModuleDirectiveKind;
  ModuleResolution result{true, {}, {}, {}};
  std::map<std::string_view, std::vector<ModuleProviderSite>> declarations;
  for (std::size_t unit = 0; unit < graph.size(); ++unit) {
    const auto& scan = graph[unit];
    result.complete = result.complete && scan.complete;
    for (std::size_t i = 0; i < scan.directives.size(); ++i) {
      const auto& [kind, name] = scan.directives[i];
      const ModuleProviderSite site{unit, i};
      if (kind == Kind::import || kind == Kind::exported_import)
        result.imports.push_back({site, {}});
      else if (!scan.same_unit_as && (kind == Kind::interface_unit ||
                                     (kind == Kind::implementation_unit && name.contains(':')))) {
        auto& sites = declarations[name];
        if (sites.empty() || sites.back().unit != unit) sites.push_back(site);
      }
    }
  }
  for (const auto& [name, sites] : declarations)
    if (sites.size() > 1) result.conflicts.push_back({std::string(name), sites});
  if (result.imports.empty()) return result;

  UnitEdges providers(graph.size()), importers(graph.size());
  for (auto& entry : result.imports) {
    const auto [unit, directive] = entry.importer;
    const auto found = declarations.find(graph[unit].directives[directive].second);
    if (found == declarations.end()) continue;
    entry.providers = found->second;
    if (entry.providers.size() == 1) {
      const auto provider = entry.providers.front().unit;
      const auto importer = graph[unit].same_unit_as.value_or(unit);
      providers[importer].push_back(provider);
      importers[provider].push_back(importer);
    }
  }
  result.cycles = cyclic_components(providers, importers);
  return result;
}

}  // namespace ycxx::build
