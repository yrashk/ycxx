// Discover executables for the rule-package bootstrap from the invoking client's PATH.
module;
#include <unistd.h>
export module ycxx.build.rule_compilers;

import std;

export namespace ycxx::build {

struct RuleCompiler {
  std::string executable;
  bool native = false;
};

struct RuleCompilerCandidates {
  std::vector<RuleCompiler> tools;
  std::string search_path;
  std::vector<std::string> observed_paths;
};

// An unset PATH has no search directories. An empty component is the invoking client's cwd.
// A valid pin with no executable yields no tools and still reports the paths that were probed.
std::expected<RuleCompilerCandidates, std::string> discover_rule_compilers(
    const std::filesystem::path& cwd, const std::optional<std::string>& invoking_path,
    std::string_view ycxx, const std::optional<std::string>& pinned);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;

fs::path anchored(const fs::path& cwd, fs::path path) {
  if (path.is_relative()) path = cwd / path;
  // Preserve parent traversal through a symlink. Removing a dot component is harmless.
  if (std::ranges::any_of(path, [](const fs::path& component) { return component == ".."; })) return path;
  path = path.lexically_normal();
  if (path != path.root_path() && path.filename().empty()) path = path.parent_path();
  return path;
}

bool executable(const fs::path& candidate, std::set<std::string>& observations) {
  observations.insert(candidate.generic_native_encoded_string());

  // Watch the target in addition to the invocation spelling. A previously missing tool still
  // contributes its candidate path, so its appearance can change discovery.
  std::error_code ec;
  const auto resolved = fs::weakly_canonical(candidate, ec);
  if (!ec) observations.insert(resolved.generic_native_encoded_string());
  ec.clear();
  return fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0;
}

}  // namespace

std::expected<RuleCompilerCandidates, std::string> discover_rule_compilers(
    const fs::path& cwd, const std::optional<std::string>& invoking_path,
    std::string_view ycxx, const std::optional<std::string>& pinned) {
  if (pinned && (pinned->empty() || *pinned == "." || *pinned == ".." || pinned->contains('\0') ||
                 (pinned->contains('/') && !fs::path(*pinned).is_absolute())))
    return std::unexpected("rule compiler must be a tool name or an absolute executable path");

  std::error_code ec;
  const auto root = fs::absolute(cwd, ec);
  if (ec) return std::unexpected("cannot locate the invoking directory for rule compiler discovery: " + ec.message());

  RuleCompilerCandidates result;
  std::vector<fs::path> directories;
  if (invoking_path) {
    std::string_view rest = *invoking_path;
    if (rest.contains('\0')) return std::unexpected("rule compiler search PATH contains NUL");
    for (;;) {
      const auto colon = rest.find(':');
      const auto component = rest.substr(0, colon);
      auto path = anchored(root, fs::path(component));
      auto text = path.generic_native_encoded_string();
      if (text.contains(':')) return std::unexpected("cannot represent rule compiler search directory in PATH: " + text);
      if (!directories.empty()) result.search_path += ':';
      result.search_path += text;
      directories.push_back(std::move(path));
      if (colon == rest.npos) break;
      rest.remove_prefix(colon + 1);
    }
  }

  std::set<std::string> observations;
  const auto add = [&](const fs::path& candidate, bool native) {
    if (!executable(candidate, observations)) return false;
    result.tools.push_back({candidate.generic_native_encoded_string(), native});
    return true;
  };
  const auto search = [&](std::string_view tool) {
    for (const auto& directory : directories)
      if (add(directory / fs::path(tool), false)) break;
  };
  const auto current = ycxx.empty() || ycxx.contains('\0') ? fs::path{} : anchored(root, fs::path(ycxx));
  const auto use_current = [&] {
    if (!current.empty()) add(current, true);
  };

  if (!pinned) {
    use_current();
    for (std::string_view tool : {"c++", "clang++", "g++"}) search(tool);
  } else if (*pinned == "ycxx") {
    use_current();
  } else if (const fs::path candidate(*pinned); candidate.is_absolute()) {
    const bool native = !current.empty() && fs::equivalent(candidate, current, ec);
    add(candidate, native);
  } else {
    search(*pinned);
  }
  result.observed_paths.assign(observations.begin(), observations.end());
  return result;
}

}  // namespace ycxx::build
