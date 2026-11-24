// Read-only descriptions and semantic comparisons of build plans (#1256).
export module ycxx.build.inspection;

import std;
import ycxx.build.inspection_html;
import ycxx.build.inspection_model;
import ycxx.build.plan;
import ycxx.lsp;

export namespace ycxx::build {

enum class InspectionFormat { text, json, source, html };

std::expected<std::string, std::string> show_project(
    const std::filesystem::path& cwd, std::string_view target, InspectionFormat format,
    const std::optional<std::filesystem::path>& against = std::nullopt);

}  // namespace ycxx::build

namespace ycxx::build {

struct PlanComparison {
  int schema = 1;
  std::string target;
  std::vector<PlanChange> changes;
};

namespace {

using FactKey = std::tuple<std::string_view, std::string_view, std::string_view>;
using FactIndex = std::map<FactKey, const PlanFact*>;

std::expected<FactIndex, std::string> index_facts(const PlanInspection& plan) {
  FactIndex index;
  for (const PlanFact& fact : plan.facts) {
    if (!index.emplace(FactKey{fact.kind, fact.target, fact.name}, &fact).second)
      return std::unexpected(std::format("duplicate {} '{}' in '{}'", fact.kind, fact.name, fact.target));
  }
  return index;
}

std::expected<PlanComparison, std::string> compare(const PlanInspection& before, const PlanInspection& after) {
  if (before.schema != after.schema)
    return std::unexpected(std::format("unsupported build snapshot schema {}", before.schema));
  if (before.target != after.target)
    return std::unexpected(std::format("snapshot is for '{}', expected '{}'", before.target, after.target));
  auto old_facts = index_facts(before);
  if (!old_facts) return std::unexpected(old_facts.error());
  auto new_facts = index_facts(after);
  if (!new_facts) return std::unexpected(new_facts.error());

  PlanComparison diff{.target = after.target, .changes = {}};
  for (const auto& [key, fact] : *old_facts) {
    const auto found = new_facts->find(key);
    if (found == new_facts->end()) diff.changes.push_back({"removed", *fact, std::nullopt});
    else if (found->second->value != fact->value) diff.changes.push_back({"changed", *fact, *found->second});
  }
  for (const auto& [key, fact] : *new_facts)
    if (!old_facts->contains(key)) diff.changes.push_back({"added", std::nullopt, *fact});
  return diff;
}

std::expected<PlanInspection, std::string> read_snapshot(const std::filesystem::path& path) {
  const auto name = path.generic_native_encoded_string();
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected(std::format("cannot open build snapshot '{}'", name));
  std::string text{std::istreambuf_iterator<char>(file), {}};
  if (file.bad()) return std::unexpected(std::format("cannot read build snapshot '{}'", name));
  auto json = lsp::parse(text);
  if (!json) return std::unexpected(std::format("{}: {}", name, json.error()));
  auto plan = lsp::from_json<PlanInspection>(*json);
  if (!plan) return std::unexpected(std::format("{}: {}", name, plan.error()));
  return std::move(*plan);
}

std::string fact_text(const PlanFact& fact) {
  const std::string value = fact.name == fact.value
      ? fact.value : std::format("{}  {}", fact.name, fact.value);
  std::string text = std::format("{}  {}  {}", fact.target, fact.kind, value);
  if (!fact.origin.empty()) text += std::format("  · {}", fact.origin);
  if (fact.line) text += std::format("  · build.ycxx:{}:{}", fact.line, fact.column);
  return text;
}

std::string as_text(const PlanInspection& plan) {
  std::string text = std::format("{}\n", plan.target);
  for (const PlanFact& fact : plan.facts) text += std::format("  {}\n", fact_text(fact));
  return text;
}

std::string diff_text(const PlanComparison& diff) {
  if (diff.changes.empty()) return std::format("{}  ·  no semantic changes\n", diff.target);
  std::string text = std::format("{}  ·  changes\n", diff.target);
  for (const PlanChange& change : diff.changes) {
    if (change.before) text += std::format("  - {}\n", fact_text(*change.before));
    if (change.after) text += std::format("  + {}\n", fact_text(*change.after));
  }
  return text;
}

}  // namespace

std::expected<std::string, std::string> show_project(
    const std::filesystem::path& cwd, std::string_view target, InspectionFormat format,
    const std::optional<std::filesystem::path>& against) {
  if (against && format == InspectionFormat::source)
    return std::unexpected("--against requires text, JSON, or HTML output");
  auto inspected = inspect_project(cwd, target);
  if (!inspected) return std::unexpected(inspected.error());

  if (against) {
    auto before = read_snapshot(cwd / *against);
    if (!before) return std::unexpected(before.error());
    auto diff = compare(*before, *inspected);
    if (!diff) return std::unexpected(diff.error());
    if (format == InspectionFormat::json) return lsp::dump(lsp::to_json(*diff)) + "\n";
    if (format == InspectionFormat::html) return render_inspection_html(*inspected, std::move(diff->changes));
    return diff_text(*diff);
  }

  if (format == InspectionFormat::source) return std::move(inspected->definition);
  if (format == InspectionFormat::json) return lsp::dump(lsp::to_json(*inspected)) + "\n";
  if (format == InspectionFormat::html) return render_inspection_html(*inspected);
  return as_text(*inspected);
}

}  // namespace ycxx::build
