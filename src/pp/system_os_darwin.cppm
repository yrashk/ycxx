// macOS image and executable path lookup for relocated preprocessor resource resolution (#1510).
// CMake selects this file or system_os_linux.cppm.
module;
#include <dlfcn.h>
#include <mach-o/dyld.h>
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
  std::uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);
  if (size == 0) return std::nullopt;
  std::string buf(size, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
  buf.resize(std::strlen(buf.c_str()));
  if (buf.empty()) return std::nullopt;
  if (char* real = ::realpath(buf.c_str(), nullptr)) {
    buf = real;
    ::free(real);
  }
  return buf;
}

}  // namespace ycxx::pp::detail
