// Read-only descriptions and semantic comparisons of build plans (#1256).
module;
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module ycxx.build.inspection;

import std;
import ycxx.ast;
import ycxx.build.inspection_html;
import ycxx.build.inspection_model;
import ycxx.build.manifest;
import ycxx.build.module_graph_diff;
import ycxx.build.plan;
import ycxx.build.project;
import ycxx.lex;
import ycxx.lsp;

export namespace ycxx::build {

enum class InspectionFormat { text, json, source, html };

std::expected<std::string, std::string> show_project(
    const std::filesystem::path& cwd, std::string_view target, InspectionFormat format,
    const RuleToolchain& toolchain, const std::optional<std::filesystem::path>& against = std::nullopt,
    const ast::Target* selected_target = nullptr);

std::expected<std::string, std::string> show_targets(
    const std::filesystem::path& cwd, InspectionFormat format);

}  // namespace ycxx::build

namespace ycxx::build {

struct PlanComparison {
  int schema = 1;
  std::string target;
  std::vector<PlanChange> changes;
  std::optional<ModuleGraphChanges> module_changes = std::nullopt;
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
  const auto unresolved = [](const PlanFact& fact) { return fact.kind == unresolved_fact_kind; };
  if (std::ranges::any_of(before.facts, unresolved) || std::ranges::any_of(after.facts, unresolved))
    return std::unexpected("cannot compare plans with unresolved rules. Build the targets before taking and comparing snapshots.");
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

std::expected<std::string, std::string> read_text(const std::filesystem::path& path, std::string_view kind) {
  const auto name = path.generic_string<char>();
  std::error_code error;
  std::ifstream file;
  if (!std::filesystem::is_directory(path, error)) file.open(path, std::ios::binary);
  if (!file.is_open()) return std::unexpected(std::format("cannot open {} '{}'", kind, name));
  std::string text;
  std::array<char, 8192> buffer;
  while (file.read(buffer.data(), buffer.size()) || file.gcount() != 0)
    text.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
  if (!file.eof() || file.bad()) return std::unexpected(std::format("cannot read {} '{}'", kind, name));
  return text;
}

std::expected<std::string, std::string> read_catalog_manifest(const std::filesystem::path& path) {
  const auto name = path.generic_string<char>();
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::exists(status))
    return std::unexpected(std::format("cannot open build definition '{}'", name));
  const auto not_regular = [&] {
    return std::unexpected(std::format("build definition '{}' is not a regular file", name));
  };
  if (!std::filesystem::is_regular_file(status)) return not_regular();

  struct Fd {
    explicit Fd(int value) : value(value) {}
    Fd(const Fd&) = delete;
    ~Fd() { if (value >= 0) ::close(value); }
    int value;
  };
  // The path can change after status(). Check the opened file before reading it.
  const Fd file(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (file.value < 0) return std::unexpected(std::format("cannot open build definition '{}'", name));
  struct stat opened{};
  if (::fstat(file.value, &opened) != 0)
    return std::unexpected(std::format("cannot stat build definition '{}'", name));
  if (!S_ISREG(opened.st_mode)) return not_regular();

  std::string contents;
  std::array<char, 8192> buffer;
  for (;;) {
    const auto size = ::read(file.value, buffer.data(), buffer.size());
    if (size == 0) return contents;
    if (size < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(std::format("cannot read build definition '{}'", name));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(size));
  }
}

std::optional<SourceLocation> invalid_utf8_location(std::string_view text) {
  auto rest = text;
  while (!rest.empty()) {
    const auto decoded = lex::unicode::decode_utf8(rest);
    if (!decoded) {
      const auto prefix = text.substr(0, text.size() - rest.size());
      const auto line = static_cast<std::size_t>(std::ranges::count(prefix, '\n')) + 1;
      const auto newline = prefix.rfind('\n');
      const auto column = newline == std::string_view::npos ? prefix.size() + 1 : prefix.size() - newline;
      return SourceLocation{line, column};
    }
    rest.remove_prefix(decoded->length);
  }
  return std::nullopt;
}

std::expected<PlanInspection, std::string> read_snapshot(const std::filesystem::path& path) {
  const auto text = read_text(path, "build snapshot");
  if (!text) return std::unexpected(text.error());
  const auto name = path.generic_string<char>();
  auto json = lsp::parse(*text);
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

std::expected<std::string, std::string> show_targets(const std::filesystem::path& cwd, InspectionFormat format) {
  if (format == InspectionFormat::source)
    return std::unexpected("--targets requires text, JSON, or HTML output");
  const auto text = read_catalog_manifest(cwd / "build.ycxx");
  if (!text) return std::unexpected(text.error());
  if (const auto location = invalid_utf8_location(*text))
    return std::unexpected(std::format("build.ycxx:{}:{}: invalid UTF-8 in build definition",
                                       location->line, location->column));
  auto manifest = parse_manifest(*text);
  if (!manifest) {
    const auto& error = manifest.error();
    return std::unexpected(std::format("build.ycxx:{}:{}: {}", error.location.line, error.location.column, error.message));
  }

  TargetCatalog catalog;
  for (auto& target : manifest->targets)
    catalog.targets.push_back({std::move(target.name), std::move(target.kind),
                               {.line = target.location.line, .column = target.location.column}});
  std::ranges::sort(catalog.targets, {}, &TargetCatalogEntry::name);
  if (format == InspectionFormat::json) return lsp::dump(lsp::to_json(catalog)) + "\n";
  if (format == InspectionFormat::html) return render_target_catalog_html(catalog, *text);

  std::string out = "Workspace targets\n";
  if (catalog.targets.empty()) out += "  No target declarations.\n";
  for (const auto& entry : catalog.targets) {
    const auto& [path, line, column] = entry.location;
    std::format_to(std::back_inserter(out), "  {}  {}  {}:{}:{}\n",
                   lsp::dump(lsp::to_json(entry.name)), entry.kind, path, line, column);
  }
  return out;
}

std::expected<std::string, std::string> show_project(
    const std::filesystem::path& cwd, std::string_view target, InspectionFormat format,
    const RuleToolchain& toolchain, const std::optional<std::filesystem::path>& against,
    const ast::Target* selected_target) {
  if (format == InspectionFormat::source) {
    if (against) return std::unexpected("--against requires text, JSON, or HTML output");
    return read_text(cwd / "build.ycxx", "build definition");
  }
  auto inspected = inspect_built_project(cwd, target, toolchain, selected_target);
  if (!inspected) return std::unexpected(inspected.error());

  if (against) {
    auto before = read_snapshot(cwd / *against);
    if (!before) return std::unexpected(before.error());
    auto diff = compare(*before, *inspected);
    if (!diff) return std::unexpected(diff.error());
    if ((format == InspectionFormat::json || format == InspectionFormat::html) &&
        before->module_graph && inspected->module_graph) {
      auto modules = compare_module_graph(*before->module_graph, *inspected->module_graph);
      if (!modules) return std::unexpected(std::move(modules.error()));
      diff->module_changes = std::move(*modules);
    }
    if (format == InspectionFormat::json) return lsp::dump(lsp::to_json(*diff)) + "\n";
    if (format == InspectionFormat::html) {
      std::optional<ModuleGraphComparison> modules;
      if (diff->module_changes) modules.emplace(ModuleGraphComparison{*before->module_graph, *diff->module_changes});
      return render_inspection_html(*inspected, &diff->changes, modules);
    }
    return diff_text(*diff);
  }

  if (format == InspectionFormat::json) return lsp::dump(lsp::to_json(*inspected)) + "\n";
  if (format == InspectionFormat::html) return render_inspection_html(*inspected);
  return as_text(*inspected);
}

}  // namespace ycxx::build
