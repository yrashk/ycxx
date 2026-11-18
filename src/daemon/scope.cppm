// Which daemon serves a request (daemon design §2, §3.1): the project root found from the first
// source file, and the socket named by that root and by the ycxx binary.
export module ycxx.daemon:scope;

import std;
import ycxx.hash;
import :posix;
import :os;

export namespace ycxx::daemon {

// The scope's root, found from `start` (a file or directory; its real path is walked):
// `YCXX_ROOT` if set; else the first directory upward holding `.ycxx` or a version-control marker
// (`.git`, `.hg`, `.jj`), where a `.git` file of a submodule does not count; else `home`.
// `.ycxx` may say `root = <relative path>` to widen the scope.
std::string find_root(const std::string& start, const std::string& home);

// The root for a request whose first input is `input` (relative to `cwd`), or `cwd` itself.
std::string root_for(const std::optional<std::string>& input, const std::string& cwd);

// `YCXX_RUNTIME_DIR`, else the platform's default (§3.1).
std::string runtime_dir();

// A 64-bit hash written as 16 hex digits.
std::string hex16(std::uint64_t v) { return std::format("{:016x}", v); }

// The build hash: the identity of the running ycxx binary (its real path, device, inode, size and
// modification time), so two builds never share a daemon. A content hash would cost a read of the
// whole binary per compile; the persistent cache (#17) may key by content.
std::uint64_t build_hash();

// The files of one scope's daemon, in the runtime directory: `<scope>-<build>.sock` and the
// `.lock`, `.start` and `.log` next to it.
struct Paths {
  std::string root;
  std::string dir;
  std::string base;  // dir + "/" + scope hash + "-" + build hash
  std::string socket() const { return base + ".sock"; }
  std::string lock() const { return base + ".lock"; }
  std::string start_lock() const { return base + ".start"; }
  std::string log() const { return base + ".log"; }
};
Paths paths_for(const std::string& root, const std::string& dir);

}  // namespace ycxx::daemon

namespace ycxx::daemon {
namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_small(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(in), {}};
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// Whether `dir/.git`, a file, is a submodule's: its `gitdir:` path has a `modules` component
// below a `.git` component (`.git/modules/x`, `.git/worktrees/w/modules/x`).
bool is_submodule_git_file(const fs::path& git) {
  auto text = read_small(git);
  if (!text) return false;
  std::string_view t = trim(*text);
  if (!t.starts_with("gitdir:")) return false;
  fs::path gitdir(std::string(trim(t.substr(7))));
  bool below_git = false;
  for (const fs::path& c : gitdir) {
    if (c == ".git") below_git = true;
    else if (below_git && c == "modules") return true;
  }
  return false;
}

// `root = <relative path>` in a `.ycxx` file, resolved against its directory.
std::optional<fs::path> widened(const fs::path& marker) {
  std::error_code ec;
  if (!fs::is_regular_file(marker, ec)) return std::nullopt;
  auto text = read_small(marker);
  if (!text) return std::nullopt;
  for (auto line : std::views::split(*text, '\n')) {
    std::string_view l = trim(std::string_view(line.begin(), line.end()));
    if (!l.starts_with("root")) continue;
    l = trim(l.substr(4));
    if (!l.starts_with('=')) continue;
    fs::path rel(std::string(trim(l.substr(1))));
    fs::path p = (marker.parent_path() / rel).lexically_normal();
    if (auto r = real_path(p.native_encoded_string())) return fs::path(*r);
    return p;
  }
  return std::nullopt;
}

}  // namespace

std::string find_root(const std::string& start, const std::string& home) {
  if (const char* r = std::getenv("YCXX_ROOT"); r && *r) return real_path(r).value_or(r);
  std::error_code ec;
  fs::path p(real_path(start).value_or(start));
  if (!fs::is_directory(p, ec)) p = p.parent_path();
  for (; !p.empty(); p = p.parent_path()) {
    fs::path marker = p / ".ycxx";
    if (fs::exists(marker, ec)) {
      if (auto w = widened(marker)) return w->native_encoded_string();
      return p.native_encoded_string();
    }
    for (std::string_view vcs : {".hg", ".jj"})
      if (fs::exists(p / vcs, ec)) return p.native_encoded_string();
    fs::path git = p / ".git";
    if (fs::is_directory(git, ec)) return p.native_encoded_string();
    if (fs::is_regular_file(git, ec) && !is_submodule_git_file(git)) return p.native_encoded_string();
    if (p == p.root_path()) break;
  }
  return real_path(home).value_or(home);
}

std::string root_for(const std::optional<std::string>& input, const std::string& cwd) {
  const char* h = std::getenv("HOME");
  std::string home = h && *h ? h : "/";
  if (!input) return find_root(cwd, home);
  fs::path p(*input);
  if (p.is_relative()) p = fs::path(cwd) / p;
  // A source that does not exist yet still has a directory to start from.
  std::error_code ec;
  if (!fs::exists(p, ec)) p = p.parent_path();
  return find_root(p.native_encoded_string(), home);
}

std::string runtime_dir() {
  if (const char* d = std::getenv("YCXX_RUNTIME_DIR"); d && *d) return d;
  return default_runtime_dir();
}

std::uint64_t build_hash() {
  static const std::uint64_t h = [] {
    std::string exe = executable_path();
    Hasher hasher;
    hasher.bytes(exe).uint(std::uint8_t(0));
    if (auto id = stat_path(exe)) hasher.uint(id->dev).uint(id->ino).uint(id->size).uint(std::uint64_t(id->mtime_ns));
    return hasher.finish().lo;
  }();
  return h;
}

Paths paths_for(const std::string& root, const std::string& dir) {
  return Paths{root, dir, std::format("{}/{}-{}", dir, hex16(fingerprint128(root).lo), hex16(build_hash()))};
}

}  // namespace ycxx::daemon
