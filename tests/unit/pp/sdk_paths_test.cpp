// Verify static SDK constructor registration (ycxx_pp_register_sdk_paths) resolves default
// preprocessor resource and target include roots (#1510).
import std;
import ycxx.ast.target;
import ycxx.pp;
import ycxx.unit;

using namespace ycxx;
using unit::check;

namespace {

// Read default resource_include_dir during dynamic initialization before main() to verify
// ELF constructor priority prevents static-initialization-order fallback to build paths.
const std::string early_linux_res{pp::resource_include_dir(ast::x86_64_linux)};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: unit_pp_sdk_paths <resource_dir> <target_include_dir>\n";
    return 2;
  }
  const std::filesystem::path want_res_root = std::filesystem::path(argv[1]).lexically_normal();
  const std::filesystem::path want_target = std::filesystem::path(argv[2]).lexically_normal();
  const std::string want_linux = (want_res_root / "linux/include").lexically_normal().native();
  const std::string want_darwin = (want_res_root / "common/include").lexically_normal().native();

  check(pp::resource_include_dir(ast::x86_64_linux) == want_linux,
        "registered static SDK resolves Linux resource include dir");
  check(pp::resource_include_dir(ast::aarch64_darwin) == want_darwin,
        "registered static SDK resolves Darwin resource include dir");
  if (pp::native_target == &ast::x86_64_linux) {
    check(early_linux_res == want_linux,
          "ELF constructor priority runs before consumer global initializers");
  }

  const auto dirs = pp::system_dirs(ast::x86_64_linux);
  check(std::ranges::contains(dirs, std::string_view(want_linux)),
        "system_dirs contains registered Linux resource dir");
  check(std::ranges::contains(dirs, std::string_view(want_target.native())),
        "system_dirs contains registered target include dir");

  pp::Config cfg;
  cfg.target = &ast::x86_64_linux;
  pp::DiskHost host;
  pp::Session s(host, pp::with_system_dirs(std::move(cfg)));
  const auto out = pp::preprocess(
      s, "#include <stddef.h>\n#include <ycxx_sdk_registered.hpp>\nint v = YCXX_SDK_REG_STDDEF + YCXX_SDK_REG_TARGET;\n",
      "probe.cpp");
  check(out.diags.empty(), "registered static SDK preprocesses cleanly");
  std::string rendered;
  for (std::size_t i = 0; i + 1 < out.region.tokens.size(); ++i) {
    if (!rendered.empty()) rendered += ' ';
    rendered += out.region.spelling(out.region.tokens[i]);
  }
  check(rendered.find("int v = 11 + 22 ;") != std::string::npos,
        "registered static SDK headers expand markers");
  return unit::result("unit.pp_sdk_paths");
}
