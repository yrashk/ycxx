// Linux image and executable path lookup for relocated preprocessor resource resolution (#1510).
// CMake selects this file or system_os_darwin.cppm.
module;
#include <dlfcn.h>
#include <stdlib.h>
export module ycxx.pp:system_os;

import std;

export namespace ycxx::pp::detail {

std::optional<std::string> image_path_of(const void* address) {
  Dl_info info{};
  if (::dladdr(address, &info) == 0 || !info.dli_fname || !*info.dli_fname) return std::nullopt;
  if (std::string_view(info.dli_fname).find('/') == std::string_view::npos) return std::nullopt;
  if (char* real = ::realpath(info.dli_fname, nullptr)) {
    std::string out = real;
    ::free(real);
    return out;
  }
  return std::string(info.dli_fname);
}

std::optional<std::string> executable_path() {
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || p.empty()) return std::nullopt;
  return p.native();
}

}  // namespace ycxx::pp::detail
