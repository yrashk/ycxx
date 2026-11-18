// ycxx-ld: link arm64 Mach-O objects into an executable with ycxx's linker, for tests (#42).
//
//   ycxx-ld [-o out] [-e entry] [--sdk dir] a.o b.o ...
//
// Imports are checked against `<sdk>/usr/lib/libSystem.B.tbd`. The SDK is `--sdk`, else
// $SDKROOT, else the Command Line Tools' SDK; without one, every name the objects leave
// undefined is imported from libSystem.
import std;
import ycxx.link;
import ycxx.obj.macho;

namespace {

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  std::filesystem::path out = "a.out";
  ycxx::link::options opts;
  std::optional<std::filesystem::path> sdk;
  std::vector<std::filesystem::path> inputs;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto a = args[i];
    const bool has_value = i + 1 < args.size();
    if (a == "-o" && has_value) out = args[++i];
    else if (a == "-e" && has_value) opts.entry = args[++i];
    else if (a == "--sdk" && has_value) sdk = args[++i];
    else if (a.starts_with("-")) {
      std::println(std::cerr, "ycxx-ld: unknown option {}", a);
      return 2;
    } else inputs.emplace_back(a);
  }
  if (!sdk) {
    if (const char* env = std::getenv("SDKROOT"); env && *env) sdk = env;
    else sdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
  }
  if (auto tbd = read_file(*sdk / "usr/lib/libSystem.B.tbd")) {
    opts.dylibs = {ycxx::link::libsystem(ycxx::link::tbd_exports(std::string_view(reinterpret_cast<const char*>(tbd->data()), tbd->size())))};
  }
  opts.identifier = out.filename().native_encoded_string();

  std::vector<ycxx::obj::object> objects;
  for (const auto& p : inputs) {
    auto bytes = read_file(p);
    if (!bytes) {
      std::println(std::cerr, "ycxx-ld: cannot read {}", p.native_encoded_string());
      return 1;
    }
    auto o = ycxx::obj::macho::read_object(*bytes, p.native_encoded_string());
    if (!o) {
      std::println(std::cerr, "ycxx-ld: {}", o.error());
      return 1;
    }
    objects.push_back(std::move(*o));
  }
  auto image = ycxx::link::link(objects, opts);
  if (!image) {
    for (const auto& e : image.error()) std::println(std::cerr, "ycxx-ld: error: {}", e.message);
    return 1;
  }
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
  f.close();
  if (!f) {
    std::println(std::cerr, "ycxx-ld: cannot write {}", out.native_encoded_string());
    return 1;
  }
  std::filesystem::permissions(out, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add);
  return 0;
}
