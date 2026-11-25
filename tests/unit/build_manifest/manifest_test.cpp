import std;
import ycxx.build.manifest;
import ycxx.unit;

namespace {

using namespace ycxx::build;
using ycxx::unit::check;

void declarations() {
  const auto manifest = parse_manifest(R"(# project definition
cpp.library "wire" {
    sources = ["src/one.cc", glob("src/**/*.cc", exclude = ["**/*_test.cc"]),
               artifact("schema", "implementation")]
    options = { "format" = "vulkan", }
}
cpp.executable "app" { deps = ["wire"] }
)");
  check(manifest && manifest->targets.size() == 2, "two declarations in written order");
  if (!manifest || manifest->targets.size() != 2) return;
  const auto& library = manifest->targets[0];
  check(library.kind == "cpp.library" && library.name == "wire" &&
            library.location.line == 2 && library.location.column == 1,
        "declaration kind, name, and source location");
  check(library.fields.size() == 2 && library.fields.front().first == "sources",
        "field order");
  if (library.fields.size() != 2) return;
  const auto* sources = std::get_if<List>(&library.fields[0].second.data);
  check(sources && sources->size() == 3, "list accepts literal, glob and artifact");
  if (!sources || sources->size() != 3) return;
  const auto* path = std::get_if<std::string>(&(*sources)[0].data);
  const auto* glob = std::get_if<Glob>(&(*sources)[1].data);
  const auto* ref = std::get_if<ArtifactRef>(&(*sources)[2].data);
  check(path && *path == "src/one.cc", "literal source");
  check(glob && glob->pattern == "src/**/*.cc" &&
            glob->exclusions == std::vector<std::string>{"**/*_test.cc"},
        "explicit glob pattern and recursive exclusion");
  check(ref && ref->target == "schema" && ref->output == "implementation" &&
            (*sources)[2].location.line == 4 && (*sources)[2].location.column == 16,
        "located artifact reference independent of file existence");
}

void tool_selectors() {
  const auto manifest = parse_manifest(R"(host_action "default" { tool = cxx() }
host_action "named" { tool = cxx("clang++") }
host_action "absolute" { tool = cxx("/opt/host tools/c++") }
)");
  const std::array<std::optional<std::string_view>, 3> expected{
      std::nullopt, "clang++", "/opt/host tools/c++"};
  check(manifest && manifest->targets.size() == expected.size(), "host compiler selectors parse");
  if (!manifest || manifest->targets.size() != expected.size()) return;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& fields = manifest->targets[i].fields;
    const auto* tool = fields.size() == 1 ? std::get_if<CxxTool>(&fields[0].second.data) : nullptr;
    check(tool && tool->compiler == expected[i], "compiler selector preserves the optional spelling");
  }

  const auto empty = parse_manifest(R"(host_action "rules" { tool = cxx("") })");
  check(!empty && empty.error().message.contains("empty compiler selector"), "reject an empty compiler name");
  check(!parse_manifest(R"(host_action "rules" { tool = cxx("a", "b") })"), "reject more than one compiler");
  check(!parse_manifest(R"(host_action "rules" { tool = cxx(clang) })"), "require a quoted compiler string");
}

void diagnostics() {
  const auto other_name = parse_manifest("a \"pkg\" {}\nb \"pkg\" {}");
  check(!other_name && other_name.error().location.line == 2 &&
            other_name.error().location.column == 3 &&
            other_name.error().message.contains("duplicate target"),
        "duplicate target names diagnosed at second declaration");
  const auto duplicate = parse_manifest("a \"pkg\" {\n  sources = []\n  sources = []\n}");
  check(!duplicate && duplicate.error().location.line == 3 &&
            duplicate.error().location.column == 3 &&
            duplicate.error().message.contains("duplicate field"),
        "duplicate fields diagnosed at the later key");
  const auto escape = parse_manifest(R"(a "pkg" { sources = glob("../*.cc") })");
  check(!escape && escape.error().message.contains("relative paths"),
        "glob cannot enumerate above manifest directory");
  const auto truncated = parse_manifest(R"(cpp.library "pkg" { sources = ["a")");
  check(!truncated, "truncated list errors");
}

void glob_observations() {
  using enum EntryKind;
  std::map<std::string, std::vector<DirectoryEntry>, std::less<>> files{
      {".", {{"src", directory}, {"elsewhere", directory}}},
      {"src", {{"z.cc", regular}, {"empty", directory}, {"a_test.cc", regular},
               {"link", other}, {"c.cc", regular}, {"nested", directory}}},
      {"src/empty", {}},
      {"src/nested", {{"x.cc", regular}, {"y_test.cc", regular}}},
  };
  std::vector<std::string> observations;
  const DirectoryReader read = [&](std::string_view dir)
      -> std::expected<std::vector<DirectoryEntry>, std::string> {
    observations.emplace_back(dir);
    const auto found = files.find(dir);
    if (found == files.end()) return std::unexpected("unexpected directory read");
    return found->second;
  };
  const Glob shallow{.pattern = "src/*.cc", .exclusions = {"src/*_test.cc"}};
  const auto top_level = expand_glob(shallow, read);
  check(top_level == std::vector<std::string>{"src/c.cc", "src/z.cc"},
        "nonrecursive matches are sorted and filtered");
  check(observations == std::vector<std::string>{".", "src"},
        "nonrecursive expansion only reads the relevant directory and its parent");

  const Glob deep{.pattern = "src/**/**/*.cc", .exclusions = {"src/**/*_test.cc"}};
  observations.clear();
  const auto before = expand_glob(deep, read);
  check(before == std::vector<std::string>{"src/c.cc", "src/nested/x.cc", "src/z.cc"},
        "recursive matches allow zero or multiple directory levels and deduplicate");
  check(observations == std::vector<std::string>{".", "src", "src/empty", "src/nested"},
        "recursive expansion observes empty subdirectories once and skips symlinks");

  files["src/empty"].push_back({"new.cc", regular});
  files["src/nested"].erase(files["src/nested"].begin());
  const auto after = expand_glob(deep, read);
  check(after == std::vector<std::string>{"src/c.cc", "src/empty/new.cc", "src/z.cc"},
        "directory additions and removals change expansion");
}

}  // namespace

int main() {
  declarations();
  tool_selectors();
  diagnostics();
  glob_observations();
  return ycxx::unit::result("build manifest");
}
