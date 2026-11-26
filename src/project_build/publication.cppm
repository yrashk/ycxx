// Publish the native image and one complete runtime asset generation (#1329).
module;
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module ycxx.build.publication;

import std;
import :os;
import ycxx.build.action;
import ycxx.lsp;

export namespace ycxx::build {

struct RuntimeAssetOutput {
  std::string_view name;
  const Artifact& artifact;
};

// The project request supplies a canonical triple for an explicit target.
std::filesystem::path project_state_path(const std::filesystem::path& workspace,
                                         std::string_view native_triple = {});

std::expected<std::filesystem::path, std::string> build_directory(
    const std::filesystem::path& workspace, std::string_view name, std::string_view child = {},
    std::string_view native_triple = {});

class PreparedPublication {
 public:
  PreparedPublication(PreparedPublication&&) noexcept;
  ~PreparedPublication();

  static std::expected<PreparedPublication, std::string> stage(
      const std::filesystem::path& workspace, std::string_view target,
      std::span<const RuntimeAssetOutput> assets, std::span<const std::uint8_t> image,
      std::string_view native_triple = {});
  // A successful result indicates whether a new asset generation was installed.
  std::expected<bool, std::string> commit() &&;

 private:
  struct State;
  explicit PreparedPublication(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace ycxx::build

namespace ycxx::build {
namespace fs = std::filesystem;

namespace publication_detail {

std::string display(const fs::path& path) { return path.native(); }
std::error_code posix_error() { return {errno, std::system_category()}; }

std::string failed(std::string_view action, const fs::path& path, std::error_code ec) {
  return std::format("cannot {} '{}': {}", action, display(path), ec.message());
}

// Only used on unpublished temporary files and directories.
void remove_temporary(const fs::path& path) {
  std::error_code ec;
  if (fs::is_directory(fs::symlink_status(path, ec))) {
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
      const auto status = it->symlink_status(ec);
      if (!ec && (!fs::is_directory(status) || same_publication_mount(path, it->path()).value_or(false)))
        remove_temporary(it->path());
    }
  }
  fs::remove(path, ec);
}

struct Temporary {
  fs::path path;
  explicit Temporary(fs::path path) : path(std::move(path)) {}
  Temporary(Temporary&& other) noexcept : path(std::exchange(other.path, {})) {}
  ~Temporary() { if (!path.empty()) remove_temporary(path); }
  void release() { path.clear(); }
};

std::expected<Temporary, std::string> temporary_directory(const fs::path& parent) {
  auto path = (parent / ".publish-XXXXXX").native();
  if (!::mkdtemp(path.data())) return std::unexpected(failed("stage a build output in", parent, posix_error()));
  return Temporary(fs::path(path));
}

using Names = std::set<std::string, std::less<>>;
constexpr std::string_view marker_prefix = ".owners-";
constexpr std::size_t manifest_limit = 8 * 1024 * 1024;

struct OwnershipFile {
  int version;
  std::vector<std::string> names;
};

struct Ownership {
  fs::path marker;
  fs::path manifest;
  Names names;
};

bool basename(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\0');
}

std::expected<Ownership, std::string> load_ownership(const fs::path& marker, const fs::path& manifest) {
  std::error_code ec;
  if (fs::file_size(manifest, ec) > manifest_limit || ec)
    return std::unexpected(std::format("cannot read asset ownership record '{}'", display(manifest)));
  std::ifstream file(manifest, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(file), {}};
  const auto json = lsp::parse(text);
  auto body = json ? lsp::from_json<OwnershipFile>(*json) : std::unexpected(json.error());
  if (!file.bad() && body && body->version == 1) {
    Names names(std::from_range, body->names);
    if (names.size() == body->names.size() && std::ranges::all_of(names, basename))
      return Ownership{marker, manifest, std::move(names)};
  }
  return std::unexpected(std::format("invalid asset ownership record '{}'", display(manifest)));
}

std::expected<std::optional<Ownership>, std::string> find_ownership(
    const fs::path& manifests, const std::vector<fs::path>& entries) {
  std::optional<Ownership> active;
  for (const auto& entry : entries) {
    const auto filename = entry.filename();
    if (!filename.native().starts_with(marker_prefix)) continue;
    const auto manifest = manifests / filename;
    struct stat a{}, b{};
    if (::lstat(manifest.c_str(), &a) != 0) {
      if (errno == ENOENT) continue;
      return std::unexpected(failed("read asset ownership record", manifest, posix_error()));
    }
    if (::lstat(entry.c_str(), &b) != 0)
      return std::unexpected(failed("read asset ownership marker", entry, posix_error()));
    if (!S_ISREG(a.st_mode) || !S_ISREG(b.st_mode) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) continue;
    if (active) return std::unexpected(std::format("multiple asset ownership records in '{}'", display(entry.parent_path())));
    auto loaded = load_ownership(entry, manifest);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    active = std::move(*loaded);
  }
  return active;
}

std::optional<std::error_code> write_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return posix_error();
    if (count == 0) return std::make_error_code(std::errc::io_error);
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return std::nullopt;
}

std::expected<Temporary, std::string> stage_ownership(const fs::path& candidate,
                                                       const fs::path& manifests, const Names& names) {
  const auto text = lsp::dump(lsp::to_json(OwnershipFile{1, std::vector<std::string>(std::from_range, names)}));
  if (text.size() > manifest_limit) return std::unexpected("runtime asset ownership record exceeds 8 MiB");
  for (;;) {
    auto path = (manifests / ".owners-XXXXXX").native();
    const auto fd = ::mkstemp(path.data());
    if (fd < 0) return std::unexpected(failed("stage asset ownership record in", manifests, posix_error()));
    Temporary record{fs::path(path)};
    const auto error = write_all(fd, text);
    const auto closed = ::close(fd);
    if (error || closed != 0)
      return std::unexpected(failed("write asset ownership record", record.path, error.value_or(posix_error())));
    const auto marker = candidate / record.path.filename();
    if (::linkat(AT_FDCWD, record.path.c_str(), AT_FDCWD, marker.c_str(), 0) == 0) return record;
    if (errno != EEXIST) return std::unexpected(failed("stage asset ownership marker", marker, posix_error()));
  }
}

using Bytes = std::span<const std::uint8_t>;

bool matches_file(const fs::path& path, const fs::file_status& status, Bytes bytes, bool executable) {
  using enum fs::perms;
  if (!fs::is_regular_file(status) || ((status.permissions() & owner_exec) != none) != executable) return false;
  std::ifstream file(path, std::ios::binary);
  return file && std::ranges::equal(std::span(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                  std::ranges::subrange(std::istreambuf_iterator<char>(file),
                                                        std::istreambuf_iterator<char>()));
}

std::optional<std::string> write_output(const fs::path& staged, const fs::path& live, Bytes bytes, bool executable) {
  using enum fs::perms;
  std::ofstream file(staged, std::ios::binary | std::ios::noreplace);
  if (!bytes.empty())
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file) return std::format("cannot write build output '{}'", display(live));
  auto permissions = owner_read | owner_write | group_read | others_read;
  if (executable) permissions |= owner_exec | group_exec | others_exec;
  std::error_code ec;
  fs::permissions(staged, permissions, fs::perm_options::replace, ec);
  if (ec) return failed("set output permissions for", live, ec);
  return std::nullopt;
}

struct DirectoryStamp {
  std::uintmax_t device;
  std::uintmax_t inode;
  fs::file_time_type modified;
  fs::perms permissions;
  bool operator==(const DirectoryStamp&) const = default;
};

std::expected<std::optional<DirectoryStamp>, std::string> read_directory(const fs::path& directory) {
  struct stat info{};
  if (::lstat(directory.c_str(), &info) != 0) {
    if (errno == ENOENT) return std::nullopt;
    return std::unexpected(failed("stat build directory", directory, posix_error()));
  }
  if (!S_ISDIR(info.st_mode)) return std::unexpected(std::format("expected a build directory at '{}'", display(directory)));
  std::error_code ec;
  const auto modified = fs::last_write_time(directory, ec);
  if (ec) return std::unexpected(failed("read build directory timestamp for", directory, ec));
  return DirectoryStamp{static_cast<std::uintmax_t>(info.st_dev), static_cast<std::uintmax_t>(info.st_ino),
                        modified, static_cast<fs::perms>(info.st_mode & 07777)};
}

std::expected<std::vector<fs::path>, std::string> directory_entries(const fs::path& dir) {
  std::vector<fs::path> entries;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    entries.push_back(it->path());
  if (ec) return std::unexpected(failed("list build directory", dir, ec));
  return entries;
}

using FileIdentity = std::pair<std::uintmax_t, std::uintmax_t>;

std::optional<FileIdentity> identity(const fs::path& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) return std::nullopt;
  return FileIdentity{info.st_dev, info.st_ino};
}

Names observed_names(const fs::path& live, const std::vector<fs::path>& entries, const Names& claimed) {
  Names actual;
  for (const auto& entry : entries) actual.insert(entry.filename().native());
  Names resolved;
  for (const auto& name : claimed) {
    if (actual.contains(name)) {
      resolved.insert(name);
      continue;
    }
    // A filesystem can expose a pathname under a different casing or spelling.
    // When multiple entries share an inode, preserve the ambiguous entries.
    const auto wanted = identity(live / name);
    if (!wanted) continue;
    fs::path match;
    for (const auto& entry : entries) {
      if (identity(entry) != wanted) continue;
      if (!match.empty()) {
        match.clear();
        break;
      }
      match = entry.filename();
    }
    if (!match.empty()) resolved.insert(match.native());
  }
  return resolved;
}

using DirectoryObservations = std::vector<std::pair<fs::path, DirectoryStamp>>;

std::optional<std::string> stage_unrelated(const fs::path& old, const fs::path& staged,
                                           DirectoryObservations& observed) {
  std::error_code ec;
  const auto status = fs::symlink_status(old, ec);
  if (ec) return failed("inspect unrelated asset entry", old, ec);
  if (!fs::is_directory(status)) {
    if (::linkat(AT_FDCWD, old.c_str(), AT_FDCWD, staged.c_str(), 0) != 0)
      return failed("preserve unrelated asset entry", old, posix_error());
    return std::nullopt;
  }
  const auto same = same_publication_mount(old.parent_path(), old);
  if (!same) return failed("inspect mount for unrelated asset directory", old, same.error());
  if (!*same) return std::format("cannot stage mounted asset directory '{}'", display(old));
  auto stamp = read_directory(old);
  if (!stamp) return std::move(stamp.error());
  if (!*stamp) return std::format("asset directory changed during staging at '{}'", display(old));
  observed.emplace_back(old, **stamp);
  if (!fs::create_directory(staged, ec)) return failed("stage unrelated asset directory", old, ec);
  auto entries = directory_entries(old);
  if (!entries) return std::move(entries.error());
  for (const auto& entry : *entries)
    if (auto error = stage_unrelated(entry, staged / entry.filename(), observed)) return error;
  fs::last_write_time(staged, (**stamp).modified, ec);
  if (!ec) fs::permissions(staged, status.permissions(), fs::perm_options::replace, ec);
  if (ec) return failed("preserve unrelated asset directory permissions for", old, ec);
  return std::nullopt;
}

struct AssetStage {
  Temporary candidate;
  Temporary manifest;
  fs::path live;
  std::optional<DirectoryStamp> previous;
  DirectoryObservations observed;
  Names previous_names;
  std::optional<fs::path> old_manifest;
};

std::expected<std::optional<AssetStage>, std::string> stage_assets(const fs::path& workspace,
                                                                 std::string_view target,
                                                                 std::span<const RuntimeAssetOutput> assets,
                                                                 std::string_view native_triple) {
  const auto state_path = project_state_path(workspace, native_triple) / "asset-manifests" / target;
  if (assets.empty()) {
    // Ordinary builds need no asset state. An empty plan still retires a managed set.
    std::error_code ec;
    if (fs::symlink_status(state_path, ec).type() == fs::file_type::not_found) return std::nullopt;
  }
  auto parent = build_directory(workspace, "assets", {}, native_triple);
  if (!parent) return std::unexpected(std::move(parent.error()));
  const auto live = *parent / target;
  auto previous = read_directory(live);
  if (!previous) return std::unexpected(std::move(previous.error()));
  auto entries = *previous ? directory_entries(live) : std::vector<fs::path>{};
  if (!entries) return std::unexpected(std::move(entries.error()));
  auto manifests = build_directory(workspace, "asset-manifests", target, native_triple);
  if (!manifests) return std::unexpected(std::move(manifests.error()));
  auto ownership = find_ownership(*manifests, *entries);
  if (!ownership) return std::unexpected(std::move(ownership.error()));
  if (assets.empty() && !*ownership) return std::nullopt;

  Names names;
  for (const auto& asset : assets) names.emplace(asset.name);
  if (*ownership && (*ownership)->names == names && std::ranges::all_of(assets, [&](const auto& asset) {
        std::error_code ec;
        const auto path = live / asset.name;
        const auto status = fs::symlink_status(path, ec);
        return !ec && matches_file(path, status, asset.artifact.bytes, asset.artifact.executable);
      })) return std::nullopt;

  auto candidate = temporary_directory(*parent);
  if (!candidate) return std::unexpected(std::move(candidate.error()));
  // A target can have any valid filename, including one resembling a temporary name.
  if (candidate->path == live) {
    auto alternate = temporary_directory(*parent);
    if (!alternate) return std::unexpected(std::move(alternate.error()));
    std::swap(candidate->path, alternate->path);
  }
  DirectoryObservations observed;
  auto claimed = observed_names(live, *entries, *ownership ? (*ownership)->names : names);
  for (const auto& entry : *entries) {
    if (*ownership && entry == (*ownership)->marker) continue;
    const auto name = entry.filename().native();
    std::error_code ec;
    const auto status = fs::symlink_status(entry, ec);
    if (ec) return std::unexpected(failed("inspect asset entry", entry, ec));
    if (claimed.contains(name) && !fs::is_directory(status)) continue;
    if (auto error = stage_unrelated(entry, candidate->path / entry.filename(), observed))
      return std::unexpected(std::move(*error));
  }
  for (const auto& asset : assets) {
    const auto staged = candidate->path / asset.name;
    std::error_code ec;
    if (fs::symlink_status(staged, ec).type() != fs::file_type::not_found)
      return std::unexpected(std::format("runtime asset '{}' collides with an unrelated entry", display(live / asset.name)));
    const auto prior = live / asset.name;
    if (claimed.contains(asset.name) &&
        matches_file(prior, fs::symlink_status(prior, ec), asset.artifact.bytes, asset.artifact.executable) &&
        ::linkat(AT_FDCWD, prior.c_str(), AT_FDCWD, staged.c_str(), 0) == 0) continue;
    if (auto error = write_output(staged, prior, asset.artifact.bytes, asset.artifact.executable))
      return std::unexpected(std::move(*error));
  }
  auto manifest = stage_ownership(candidate->path, *manifests, names);
  if (!manifest) return std::unexpected(std::move(manifest.error()));
  std::error_code ec;
  const auto perms = *previous ? (*previous)->permissions : fs::status(*parent, ec).permissions();
  if (!ec) fs::permissions(candidate->path, perms, fs::perm_options::replace, ec);
  if (ec) return std::unexpected(failed("set asset directory permissions for", live, ec));
  std::optional<fs::path> old_manifest;
  if (*ownership) old_manifest = std::move((*ownership)->manifest);
  return AssetStage{std::move(*candidate), std::move(*manifest), live, *previous, std::move(observed),
                    std::move(claimed), std::move(old_manifest)};
}

// The retired directory may still be open elsewhere. Only unlink unrelated entries
// whose inodes are also accessible in the new generation. Leave any late additions.
void retire_shared_entry(const fs::path& retired, const fs::path& live) {
  struct stat old{}, current{};
  if (::lstat(retired.c_str(), &old) != 0 || ::lstat(live.c_str(), &current) != 0) return;
  std::error_code ec;
  if (S_ISDIR(old.st_mode)) {
    if (!S_ISDIR(current.st_mode) ||
        !same_publication_mount(retired.parent_path(), retired).value_or(false) ||
        !same_publication_mount(live.parent_path(), live).value_or(false)) return;
    for (fs::directory_iterator it(retired, ec), end; !ec && it != end; it.increment(ec))
      retire_shared_entry(it->path(), live / it->path().filename());
    fs::remove(retired, ec);  // A directory with unshared entries is kept.
  } else if (old.st_dev == current.st_dev && old.st_ino == current.st_ino) {
    fs::remove(retired, ec);
  }
}

void retire_assets(AssetStage& assets) {
  const auto retired = std::exchange(assets.candidate.path, {});
  if (!assets.previous) return;
  std::error_code ec;
  for (fs::directory_iterator it(retired, ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename();
    const auto status = it->symlink_status(ec);
    if (ec) break;
    const bool marker = assets.old_manifest && name == assets.old_manifest->filename();
    if (marker || (assets.previous_names.contains(name.native()) && !fs::is_directory(status)))
      fs::remove(it->path(), ec);
    else
      retire_shared_entry(it->path(), assets.live / name);
  }
  fs::remove(retired, ec);
  if (assets.old_manifest) fs::remove(*assets.old_manifest, ec);
}

struct ExecutableStage {
  Temporary directory;
  fs::path output;
  bool existed;
  fs::path temporary() const { return directory.path / "data"; }
};

std::expected<std::optional<ExecutableStage>, std::string> stage_executable(
    const fs::path& directory, std::string_view target, Bytes image) {
  const auto output = directory / target;
  std::error_code ec;
  const auto existing = fs::symlink_status(output, ec);
  if (fs::is_directory(existing)) return std::unexpected(std::format("expected a build output file at '{}'", display(output)));
  if (matches_file(output, existing, image, true)) return std::nullopt;
  auto staged = temporary_directory(directory);
  if (!staged) return std::unexpected(std::move(staged.error()));
  auto result = ExecutableStage{std::move(*staged), output, existing.type() != fs::file_type::not_found};
  if (auto error = write_output(result.temporary(), output, image, true)) return std::unexpected(std::move(*error));
  return result;
}

}  // namespace publication_detail

using namespace publication_detail;

fs::path project_state_path(const fs::path& workspace, std::string_view native_triple) {
  auto state = workspace / ".ycxx";
  if (!native_triple.empty()) state = state / "targets" / native_triple;
  return state;
}

std::expected<fs::path, std::string> build_directory(const fs::path& workspace, std::string_view name,
                                                   std::string_view child, std::string_view native_triple) {
  fs::path path = workspace;
  const auto targets = native_triple.empty() ? std::string_view{} : std::string_view("targets");
  for (const auto part : {std::string_view(".ycxx"), targets, native_triple, name, child}) {
    if (part.empty()) continue;
    path /= part;
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) {
      fs::create_directory(path, ec);
      if (!ec) status = fs::symlink_status(path, ec);
    }
    if (ec || !fs::is_directory(status))
      return std::unexpected(std::format("expected a build directory at '{}'", display(path)));
  }
  return path;
}

struct PreparedPublication::State {
  std::optional<AssetStage> assets;
  std::optional<ExecutableStage> executable;

  std::expected<bool, std::string> commit() {
    if (assets) {
      auto current = read_directory(assets->live);
      if (!current) return std::unexpected(std::move(current.error()));
      if (*current != assets->previous)
        return std::unexpected(std::format("asset directory changed during staging at '{}'", display(assets->live)));
      for (const auto& [directory, stamp] : assets->observed) {
        auto current = read_directory(directory);
        if (!current) return std::unexpected(std::move(current.error()));
        if (*current != stamp)
          return std::unexpected(std::format("asset directory changed during staging at '{}'", display(directory)));
      }
    }
    if (executable) {
      std::error_code ec;
      if (assets) {
        if (fs::is_directory(fs::symlink_status(executable->output, ec)))
          return std::unexpected(std::format("expected a build output file at '{}'", display(executable->output)));
        ec = move_publication(executable->temporary(), executable->output, executable->existed);
      } else {
        fs::rename(executable->temporary(), executable->output, ec);
      }
      if (ec) return std::unexpected(failed("publish", executable->output, ec));
    }
    if (!assets) return false;
    if (auto ec = move_publication(assets->candidate.path, assets->live, assets->previous.has_value())) {
      auto error = failed("publish runtime assets at", assets->live, ec);
      if (executable) {
        auto restore = move_publication(executable->output, executable->temporary(), executable->existed);
        if (restore == std::errc::no_such_file_or_directory && executable->existed)
          restore = move_publication(executable->temporary(), executable->output, false);
        if (restore) {
          error += "\n" + failed("restore the previous executable at", executable->output, restore);
          if (executable->existed) {
            error += std::format("\nprevious executable kept at '{}'", display(executable->temporary()));
            executable->directory.release();
          }
        }
      }
      return std::unexpected(std::move(error));
    }
    assets->manifest.release();
    retire_assets(*assets);
    return true;
  }
};

PreparedPublication::PreparedPublication(std::unique_ptr<State> state) : state_(std::move(state)) {}
PreparedPublication::PreparedPublication(PreparedPublication&&) noexcept = default;
PreparedPublication::~PreparedPublication() = default;

std::expected<PreparedPublication, std::string> PreparedPublication::stage(
    const fs::path& workspace, std::string_view target, std::span<const RuntimeAssetOutput> assets, Bytes image,
    std::string_view native_triple) {
  auto directory = build_directory(workspace, "out", {}, native_triple);
  if (!directory) return std::unexpected(std::move(directory.error()));
  auto generation = stage_assets(workspace, target, assets, native_triple);
  if (!generation) return std::unexpected(std::move(generation.error()));
  auto executable = stage_executable(*directory, target, image);
  if (!executable) return std::unexpected(std::move(executable.error()));
  return PreparedPublication(std::make_unique<State>(std::move(*generation), std::move(*executable)));
}

std::expected<bool, std::string> PreparedPublication::commit() && {
  auto state = std::move(state_);
  return state->commit();
}

}  // namespace ycxx::build
