// Filesystem observations for compiler capability probes. Compiler invocations discover
// search roots separately. Validation can then reobserve them without running any tools.
module;
#include <errno.h>
#include <sys/stat.h>

export module ycxx.build.toolchain_inputs;

import std;
import ycxx.hash;

export namespace ycxx::build {

struct ToolchainProbeInputs {
  std::vector<std::string> trees;
  // A file path or a directory whose immediate entries are relevant, such as -L or -B.
  std::vector<std::string> files;
};

Fp128 fingerprint_toolchain_probe_inputs(const ToolchainProbeInputs& inputs);
ToolchainProbeInputs configured_toolchain_probe_inputs(
    const std::filesystem::path& cwd, std::string_view executable, std::span<const std::string> flags,
    std::span<const std::pair<std::string, std::string>> environment, std::span<const std::string> declared);
ToolchainProbeInputs reported_toolchain_probe_inputs(std::string_view include_search, std::string_view search_dirs);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;

void sort_unique(std::vector<std::string>& paths) {
  std::ranges::sort(paths);
  const auto duplicates = std::ranges::unique(paths);
  paths.erase(duplicates.begin(), duplicates.end());
}

void normalize(ToolchainProbeInputs& inputs) {
  sort_unique(inputs.trees);
  sort_unique(inputs.files);
}

void add(std::vector<std::string>& paths, fs::path path) {
  if (!path.is_absolute() || path.native().contains('\0')) return;
  // Following a symlink before interpreting `..` is significant. A lexical collapse here
  // would fingerprint a different directory from the one the driver searched.
  if (std::ranges::none_of(path, [](const fs::path& component) { return component == ".."; }))
    path = path.lexically_normal();
  while (path != path.root_path() && path.filename().empty()) path = path.parent_path();
  paths.push_back(path.native());
}

std::string_view trim(std::string_view text) {
  constexpr std::string_view spaces = " \t\r";
  const auto first = text.find_first_not_of(spaces);
  if (first == text.npos) return {};
  return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}

void add_search_path(std::vector<std::string>& paths, std::string_view list) {
  for (const auto component : list | std::views::split(':')) add(paths, fs::path(std::string_view(component)));
}

void add_sysroot(ToolchainProbeInputs& inputs, const fs::path& path) {
  if (!path.is_absolute()) return;
  add(inputs.files, path);
  for (const auto* header_path : {"usr/include", "usr/local/include", "include",
                                 "System/Library/Frameworks", "Library/Frameworks"})
    add(inputs.trees, path / header_path);
  for (const auto* link_path : {"usr/lib", "usr/local/lib", "lib"}) add(inputs.files, path / link_path);
}

enum class PathRole { includes, framework_under_sysroot, shallow, sysroot };

struct PathOption {
  std::string_view spelling;
  PathRole role;
  bool joined;
};

// Long spellings precede prefixes such as -I and -L. Include options can carry a path
// beginning with `=` or `$SYSROOT`, so an equals separator is only stripped for driver options
// that use it as part of their command-line spelling.
constexpr PathOption path_options[] = {
    {"-iframeworkwithsysroot", PathRole::framework_under_sysroot, true},
    {"-iframework", PathRole::includes, true},
    {"-idirafter", PathRole::includes, true},
    {"-isysroot", PathRole::sysroot, true},
    {"-isystem", PathRole::includes, true},
    {"-iquote", PathRole::includes, true},
    {"-include", PathRole::shallow, false},
    {"-imacros", PathRole::shallow, false},
    {"--sysroot", PathRole::sysroot, false},
    {"--gcc-toolchain", PathRole::shallow, false},
    {"--gcc-install-dir", PathRole::shallow, false},
    {"-resource-dir", PathRole::shallow, false},
    {"--resource-dir", PathRole::shallow, false},
    {"-specs", PathRole::shallow, false},
    {"--config", PathRole::shallow, false},
    {"-fplugin", PathRole::shallow, false},
    {"-I", PathRole::includes, true},
    {"-F", PathRole::includes, true},
    {"-B", PathRole::shallow, true},
    {"-L", PathRole::shallow, true},
};

void append_metadata(Hasher& h, const auto& st) {
  hash_append(h, std::tuple{st.st_dev, st.st_ino, st.st_mode, st.st_uid, st.st_gid, st.st_size});
  if constexpr (requires { st.st_mtimespec; }) {
    hash_append(h, std::tuple{st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec,
                             st.st_ctimespec.tv_sec, st.st_ctimespec.tv_nsec});
  } else {
    hash_append(h, std::tuple{st.st_mtim.tv_sec, st.st_mtim.tv_nsec, st.st_ctim.tv_sec, st.st_ctim.tv_nsec});
  }
}

enum class Depth { path, directory, tree };

bool wide_directory(const fs::path& path) {
  // Even if reported through a symlink, a search path at the root must never cause a scan
  // of the whole machine. These broad prefixes can still contribute their immediate entries.
  constexpr std::string_view broad[] = {"/usr", "/usr/local", "/usr/lib", "/usr/lib64",
                                       "/usr/local/lib", "/opt", "/Library", "/System"};
  if (path == path.root_path()) return true;
  return std::ranges::any_of(broad, [&](auto prefix) { return path.native() == prefix; });
}

using DirectoryId = std::pair<dev_t, ino_t>;

void sample(Hasher& h, const fs::path& path, Depth depth, bool root, std::set<DirectoryId>& visited) {
  struct stat st{};
  const int lookup_error = ::lstat(path.c_str(), &st) == 0 ? 0 : errno;
  hash_append(h, lookup_error);
  if (lookup_error != 0) return;

  append_metadata(h, st);
  std::error_code ec;
  const bool is_link = S_ISLNK(st.st_mode);
  if (is_link) {
    auto link = fs::read_symlink(path, ec);
    hash_append(h, ec.value());
    hash_append(h, link.native());
    const int target_error = ::stat(path.c_str(), &st) == 0 ? 0 : errno;
    hash_append(h, target_error);
    if (target_error != 0) return;
    append_metadata(h, st);
  }

  fs::path resolved;
  if (root || is_link) {
    resolved = fs::canonical(path, ec);
    hash_append(h, ec.value());
    hash_append(h, resolved.native());
  }
  if (!S_ISDIR(st.st_mode) || depth == Depth::path) return;
  if (wide_directory(resolved.empty() ? path : resolved)) depth = Depth::directory;
  const bool unseen = visited.emplace(st.st_dev, st.st_ino).second;
  hash_append(h, unseen);
  if (!unseen) return;

  std::vector<std::string> children;
  for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
    children.push_back(it->path().filename().native());
  std::ranges::sort(children);
  hash_append(h, ec.value());
  hash_append(h, children.size());
  const Depth child_depth = depth == Depth::tree ? Depth::tree : Depth::path;
  for (const auto& child : children) {
    hash_append(h, child);
    sample(h, path / child, child_depth, false, visited);
  }
}

}  // namespace

Fp128 fingerprint_toolchain_probe_inputs(const ToolchainProbeInputs& inputs) {
  ToolchainProbeInputs normalized = inputs;
  normalize(normalized);
  Hasher h;
  std::set<DirectoryId> visited;
  const auto append = [&](const std::vector<std::string>& paths, Depth depth) {
    hash_append(h, paths.size());
    for (const auto& path : paths) {
      hash_append(h, path);
      if (fs::path(path).is_absolute() && !path.contains('\0')) sample(h, path, depth, true, visited);
    }
  };
  append(normalized.trees, Depth::tree);
  append(normalized.files, Depth::directory);
  return h.finish();
}

ToolchainProbeInputs configured_toolchain_probe_inputs(
    const fs::path& cwd, std::string_view executable, std::span<const std::string> flags,
    std::span<const std::pair<std::string, std::string>> environment, std::span<const std::string> declared) {
  ToolchainProbeInputs inputs;
  add(inputs.files, fs::path(executable));
  for (const auto& path : declared) {
    if (!path.empty()) add(inputs.trees, cwd / path);
  }

  fs::path sysroot;
  for (const auto& [name, value] : environment) {
    if (name == "SDKROOT") {
      sysroot = value;
      add_sysroot(inputs, sysroot);
    } else if (name == "CPATH" || name == "CPLUS_INCLUDE_PATH" || name == "C_INCLUDE_PATH") {
      add_search_path(inputs.trees, value);
    } else if (name == "PATH" || name == "LIBRARY_PATH" || name == "COMPILER_PATH" ||
               name == "LD_LIBRARY_PATH" || name == "DYLD_LIBRARY_PATH" || name == "DYLD_FALLBACK_LIBRARY_PATH") {
      add_search_path(inputs.files, value);
    } else if (name == "GCC_EXEC_PREFIX") {
      add(inputs.files, fs::path(value));
    }
  }

  std::vector<std::pair<PathRole, std::string_view>> paths;
  for (std::size_t i = 0; i < flags.size(); ++i) {
    const std::string_view flag = flags[i];
    if (flag.starts_with('@')) {
      add(inputs.files, fs::path(flag.substr(1)));
      continue;
    }
    for (const auto& option : path_options) {
      const auto prefix = option.spelling;
      if (!flag.starts_with(prefix)) continue;
      auto path = flag.substr(prefix.size());
      if (path.empty()) {
        if (i + 1 < flags.size()) path = flags[++i];
      } else if (!option.joined) {
        if (!path.starts_with('=')) continue;
        path.remove_prefix(1);
      }
      if (option.role == PathRole::sysroot) {
        sysroot = path;
        add_sysroot(inputs, sysroot);
      } else if (!path.empty()) {
        paths.emplace_back(option.role, path);
      }
      break;
    }
  }

  for (auto [role, path] : paths) {
    fs::path location(path);
    if (path.starts_with('=') || path.starts_with("$SYSROOT")) {
      path.remove_prefix(path.starts_with('=') ? 1 : 8);
      if (!sysroot.is_absolute()) continue;
      location = sysroot / fs::path(path).relative_path();
    } else if (role == PathRole::framework_under_sysroot && sysroot.is_absolute()) {
      location = sysroot / location.relative_path();
    }
    // Relative compiler arguments name files in the staged action. Declared probe_inputs
    // are the only paths here interpreted relative to the workspace instead.
    add(role == PathRole::shallow ? inputs.files : inputs.trees, std::move(location));
  }
  normalize(inputs);
  return inputs;
}

ToolchainProbeInputs reported_toolchain_probe_inputs(std::string_view include_search, std::string_view search_dirs) {
  ToolchainProbeInputs inputs;
  bool search_list = false;
  constexpr std::string_view missing = "ignoring nonexistent directory \"";
  constexpr std::string_view framework = " (framework directory)";
  for (const auto line : include_search | std::views::split('\n')) {
    auto text = trim(std::string_view(line));
    if (text == "#include \"...\" search starts here:" || text == "#include <...> search starts here:") {
      search_list = true;
    } else if (text == "End of search list.") {
      search_list = false;
    } else if (search_list) {
      if (text.ends_with(framework)) text.remove_suffix(framework.size());
      add(inputs.trees, fs::path(text));
    } else if (text.starts_with(missing) && text.ends_with('"')) {
      add(inputs.trees, fs::path(text.substr(missing.size(), text.size() - missing.size() - 1)));
    }
  }
  for (const auto line : search_dirs | std::views::split('\n')) {
    const auto text = trim(std::string_view(line));
    const auto colon = text.find(':');
    if (colon == text.npos) continue;
    const auto kind = text.substr(0, colon);
    auto value = trim(text.substr(colon + 1));
    if (value.starts_with('=')) value.remove_prefix(1);
    if (kind == "install") add(inputs.files, fs::path(value));
    else if (kind == "programs" || kind == "libraries") add_search_path(inputs.files, value);
  }
  normalize(inputs);
  return inputs;
}

}  // namespace ycxx::build
