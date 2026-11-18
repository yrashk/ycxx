// Linking a program (#43): the objects and libSystem, through ycxx's linker (ycxx.link), into a
// signed arm64 macOS executable. Imports are checked against the export list of the SDK's
// `usr/lib/libSystem.B.tbd`. The SDK is `$SDKROOT`, else what `xcrun --show-sdk-path` prints,
// else the Command Line Tools' SDK; it is looked up and its export list read once per process.
module;
#include <stdio.h>
export module ycxx.pipeline:executable;

import std;
export import ycxx.link;

export namespace ycxx::pipeline {

// The SDK directory, if one is found.
const std::optional<std::filesystem::path>& sdk_path();

// The names libSystem exports, from the SDK; without an SDK, none is known and any name is
// accepted (the loader then reports a missing one).
const link::dylib& libsystem();

// The executable's bytes, or the linker's errors. `identifier` names the code signature.
std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(std::span<const obj::object> objects,
                                                                                   std::string identifier);

// Writes bytes to `path` (replacing it), executable when `executable` is set; an error message on
// failure.
std::optional<std::string> write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                      bool executable);

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

// The first line a command prints, if it succeeds.
std::optional<std::string> first_line_of(const char* command) {
  FILE* p = ::popen(command, "r");
  if (!p) return std::nullopt;
  std::array<char, 4096> buf{};
  std::string line;
  if (::fgets(buf.data(), int(buf.size()), p)) line = buf.data();
  int status = ::pclose(p);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (status != 0 || line.empty()) return std::nullopt;
  return line;
}

const std::optional<std::filesystem::path>& sdk_path() {
  static const std::optional<std::filesystem::path> sdk = []() -> std::optional<std::filesystem::path> {
    if (const char* env = std::getenv("SDKROOT"); env && *env) return std::filesystem::path(env);
    if (auto p = first_line_of("xcrun --show-sdk-path 2>/dev/null")) return std::filesystem::path(*p);
    std::filesystem::path clt = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
    std::error_code ec;
    if (std::filesystem::is_directory(clt, ec)) return clt;
    return std::nullopt;
  }();
  return sdk;
}

const link::dylib& libsystem() {
  static const link::dylib lib = [] {
    if (const auto& sdk = sdk_path()) {
      std::ifstream in(*sdk / "usr/lib/libSystem.B.tbd", std::ios::binary);
      if (in) {
        std::string text{std::istreambuf_iterator<char>(in), {}};
        return link::libsystem(link::tbd_exports(text));
      }
    }
    return link::libsystem();
  }();
  return lib;
}

std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(std::span<const obj::object> objects,
                                                                                   std::string identifier) {
  link::options opts;
  opts.dylibs = {libsystem()};
  opts.identifier = std::move(identifier);
  return link::link(objects, opts);
}

std::optional<std::string> write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                      bool executable) {
  // A new file, not the old one rewritten: the kernel caches a signed executable's pages by
  // vnode, and rewriting one in place makes the next run of it fail its signature check.
  std::error_code ec;
  std::filesystem::remove(path, ec);
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return std::format("cannot open '{}' for writing", path.display_string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) return std::format("cannot write '{}'", path.display_string());
  }
  if (executable) {
    using enum std::filesystem::perms;
    std::filesystem::permissions(path, owner_exec | group_exec | others_exec, std::filesystem::perm_options::add, ec);
    if (ec) return std::format("cannot make '{}' executable: {}", path.display_string(), ec.message());
  }
  return std::nullopt;
}

}  // namespace ycxx::pipeline
