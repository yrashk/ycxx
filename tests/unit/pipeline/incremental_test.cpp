// The pipeline's cells (#63): a second build of an unchanged unit executes nothing; an edit to one
// function's body re-executes that function's cells, `tu` and the link, and no other function's;
// and every incremental result equals a cold build of the same text (DECISIONS §4.2), even where
// the warm session's interners number entities differently from a cold one.
import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.obj;
import ycxx.obj.archive;
import ycxx.obj.macho;
import ycxx.pipeline;
import ycxx.pp;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace engine = ycxx::engine;
namespace obj = ycxx::obj;
namespace pl = ycxx::pipeline;
namespace pp = ycxx::pp;
using ycxx::unit::check;

namespace {

constexpr pl::TuId tu{0};
constexpr pl::ProgramId program{0};

const std::string v0 = R"(int twice(int x) { return x + x; }
int square(int x) { return x * x; }
const char* greeting = "hi";
int main() { return twice(3) + square(2) - 10; }
)";
// Whitespace only, inside `square`'s body.
const std::string v1 = R"(int twice(int x) { return x + x; }
int square(int x) {
  return x  *  x;
}
const char* greeting = "hi";
int main() { return twice(3) + square(2) - 10; }
)";
// `square`'s body.
const std::string v2 = R"(int twice(int x) { return x + x; }
int square(int x) { return x * x * 1; }
const char* greeting = "hi";
int main() { return twice(3) + square(2) - 10; }
)";
// A new function before the others: a cold session interns it first, the warm one last.
const std::string v3 = R"(int one() { return 1; }
int twice(int x) { return x + x; }
int square(int x) { return x * x * 1; }
const char* greeting = "hi";
int main() { return twice(3) + square(2) - 10 + one() - 1; }
)";

void build(pl::Session& s, const std::string& text) {
  s.set_source(tu, "incremental.cpp", text, "incremental.o");
  s.set_program(program, {{tu}, {}, "incremental"});
}

// A cold build of `text` in a session of its own.
std::vector<std::uint8_t> cold(const std::string& text) {
  pl::Session s;
  build(s, text);
  const pl::Linked& l = s.get<pl::Link>(program);
  check(l.errors.empty(), "the cold build links");
  return l.image;
}

ast::EntityId function(pl::Session& s, std::string_view name) {
  for (const pl::Entity& e : s.get<pl::TuUnit>(tu).entities)
    if (e.name == name) return e.id;
  check(false, std::format("'{}' is declared", name));
  return ast::EntityId::none;
}

// How many times each of a function's cells has run.
struct Runs {
  std::uint32_t gen, ir, lower, unit, codegen, optimized;
  bool operator==(const Runs&) const = default;
};
Runs runs(pl::Session& s, ast::EntityId f) {
  const pl::FnKey k{tu, f};
  const engine::Db& db = s.db();
  const auto& unit = s.get<pl::UnitQ>(k);
  std::uint32_t optimized = unit.unit ? db.runs<pl::Optimized>({*unit.unit, ycxx::obj::target::aarch64_macos, 0}) : 0;
  return {db.runs<pl::FunctionGen>(k), db.runs<pl::FunctionIr>(k), db.runs<pl::Lower>(k), db.runs<pl::UnitQ>(k),
          db.runs<pl::Codegen>({k}), optimized};
}

void incremental() {
  pl::Session s;
  build(s, v0);
  std::vector<std::uint8_t> image = s.get<pl::Link>(program).image;
  check(!image.empty() && s.get<pl::Link>(program).errors.empty(), "the first build links");
  check(image == cold(v0), "a cold build is reproducible");

  // The same inputs again: nothing runs.
  s.db().reset_stats();
  build(s, v0);
  check(s.get<pl::Link>(program).image == image, "the second build's image is the first's");
  check(s.db().stats().executed == 0, "a second build of unchanged inputs executes nothing");

  // A new revision through an input no cell of the program reads: every cell is validated and
  // green, and nothing executes.
  s.db().reset_stats();
  s.set_source(pl::TuId{1}, "elsewhere.cpp", "int x;", "elsewhere.o");
  check(s.get<pl::Link>(program).image == image, "an unrelated input changes nothing");
  check(s.db().stats().executed == 0 && s.db().stats().validated > 0,
        std::format("an unrelated input executes nothing ({} executed)", s.db().stats().executed));

  const ast::EntityId twice = function(s, "twice"), square = function(s, "square"), main_ = function(s, "main");
  const Runs twice0 = runs(s, twice), square0 = runs(s, square), main0 = runs(s, main_);
  const std::uint32_t source0 = s.db().runs<pl::FunctionSourceQ>({tu, twice});

  // Whitespace inside a body: the front end runs again and gives the same definitions, so
  // nothing behind it runs.
  build(s, v1);
  check(s.get<pl::Link>(program).image == image, "a whitespace edit gives the same image");
  check(s.db().runs<pl::FunctionSourceQ>({tu, twice}) == source0 && runs(s, square) == square0,
        "a whitespace edit executes nothing past the front end");

  // One body: only its function's cells, `tu` and the link run.
  const std::uint32_t tu0 = s.db().runs<pl::Tu>(pl::Tu::Key{tu}), link0 = s.db().runs<pl::Link>(program);
  build(s, v2);
  const std::vector<std::uint8_t> edited = s.get<pl::Link>(program).image;
  check(!edited.empty() && edited != image, "the edit changes the image");
  check(edited == cold(v2), "after a body edit, incremental equals cold");
  check(runs(s, twice) == twice0 && runs(s, main_) == main0, "the other functions' cells stay green");
  const Runs square1 = runs(s, square);
  check(square1.gen == square0.gen + 1 && square1.ir == square0.ir + 1 && square1.lower == square0.lower + 1 &&
            square1.unit == square0.unit + 1 && square1.codegen == square0.codegen + 1 && square1.optimized == 1,
        "the edited function's cells run once each, and its new unit is optimized once");
  check(s.db().runs<pl::Tu>(pl::Tu::Key{tu}) == tu0 + 1 && s.db().runs<pl::Link>(program) == link0 + 1, "tu and the link run again");

  // A function inserted first, which this session numbers after every other entity.
  build(s, v3);
  check(s.get<pl::Link>(program).image == cold(v3), "after an insertion, incremental equals cold");
  // The unit's symbol list changed, but only the per-entity answers read it (#938).
  check(runs(s, twice) == twice0, "an insertion leaves the other functions' cells green");
}

// A file of many independent modules, as `playground/genbig.py` writes them (#938): an edit to
// one module's constant re-executes the cells of the functions that use it and of no other
// function, and a comment re-executes none. Only the unit-wide cells and the cheap per-function
// and per-entity projections of the unit (`FunctionSourceQ`, `SymbolAccessQ`) run again.
std::string modules(int edited_k, bool comment, std::string_view run_type = "int") {
  constexpr int count = 12;
  std::string s;
  for (int i = 0; i < count; ++i) {
    s += std::format(R"(namespace m{0} {{
constexpr int K = {1};
struct Box {{
  int v;
  Box(int x) : v(x) {{}}
  int get() const {{ return v * K; }}
}};
template <class T> T twice(T t) {{ return t + t; }}
int plain(int x) {{ return x + 1; }}
const char* name() {{ return "m{0}"; }}
{2} run() {{ Box b(plain(2)); return b.get() + twice(K) + name()[0]; }}
}}
)", i, i == 3 ? edited_k : i + 2, i == 3 ? run_type : "int");
    if (comment && i == 3) s += "// a comment\n\n\n";
  }
  s += "int main() {\n  int sum = 0;\n";
  for (int i = 0; i < count; ++i) s += std::format("  sum += m{}::run();\n", i);
  s += "  return sum == 0;\n}\n";
  return s;
}

void one_module_edit() {
  pl::Session s;
  build(s, modules(5, false));
  check(s.get<pl::Link>(program).errors.empty(), "the modules link");
  auto executed = [&](const std::string& text) {
    s.db().reset_stats();
    s.db().count_per_kind(true);
    build(s, text);
    check(s.get<pl::Link>(program).image == cold(text), "incremental equals cold");
    std::map<std::string, std::uint64_t, std::less<>> out;
    for (const auto& [kind, stats] : s.db().stats_per_kind()) {
      // `ycxx::pipeline::Lower@ycxx.pipeline` → `Lower`
      std::string_view name = kind.substr(0, kind.find('@'));
      name.remove_prefix(name.rfind("::") == std::string_view::npos ? 0 : name.rfind("::") + 2);
      if (stats.executed) out[std::string(name)] += stats.executed;
    }
    s.db().count_per_kind(false);
    return out;
  };
  auto described = [](const auto& m) {
    std::string out;
    for (const auto& [kind, n] : m) out += std::format(" {}={}", kind, n);
    return out;
  };
  // `m3::Box::get` and `m3::run` read `m3::K`; no other function does.
  auto per_function = [&](const auto& m, std::uint64_t want, std::string_view what) {
    for (std::string_view kind : {"FunctionGen", "FunctionIr", "Lower", "UnitQ", "Codegen", "Optimized"}) {
      auto found = m.find(kind);
      check((found == m.end() ? 0 : found->second) == want,
            std::format("{}: {} runs for {} function(s):{}", what, kind, want, described(m)));
    }
  };
  // Each kind that is neither per function nor a projection runs at most once per unit.
  auto per_unit = [&](const auto& m, std::string_view what) {
    for (const auto& [kind, n] : m) {
      if (kind == "FunctionSourceQ" || kind == "SymbolAccessQ" || kind == "LayoutConstant" || kind == "FunctionGen" ||
          kind == "FunctionIr" || kind == "Lower" || kind == "UnitQ" || kind == "Codegen" || kind == "Optimized")
        continue;
      check(n <= 1, std::format("{}: {} runs once at most:{}", what, kind, described(m)));
    }
  };
  auto groups = [&] { return s.group_counts(); };
  const auto before = groups();
  const auto edit = executed(modules(6, false));
  // A group is a top-level item (#947): the twelve modules and `main`. Only the edited module's is
  // analysed again; `main`'s read `m3::run`'s item, which did not change.
  const auto after = groups();
  check(after.first - before.first == 1 && after.second - before.second == 12,
        std::format("one module's constant analyses its group alone ({} analysed, {} reused)", after.first - before.first,
                    after.second - before.second));
  // Every kind named above ran, so a renamed kind cannot pass the comment's zero counts unseen.
  for (std::string_view kind : {"FunctionGen", "FunctionIr", "Lower", "UnitQ", "Codegen", "Optimized"})
    check(edit.contains(kind), std::format("{} is counted:{}", kind, described(edit)));
  per_function(edit, 2, "one module's constant");
  per_unit(edit, "one module's constant");
  check(edit.contains("TuSema"), "a changed token re-runs the analysis");
  // A comment and blank lines move every later token but change none (#947): the analysis does
  // not run again (the link reads no placed message).
  const auto commented = executed(modules(6, true));
  per_function(commented, 0, "a comment");
  per_unit(commented, "a comment");
  check(!commented.contains("TuSema") && commented.contains("PpTokens"),
        std::format("a comment re-runs no analysis:{}", described(commented)));
  // A signature `main` reads: its group is analysed again too.
  const auto before_signature = groups();
  executed(modules(6, true, "long"));
  const auto after_signature = groups();
  check(after_signature.first - before_signature.first == 2,
        std::format("a changed signature analyses its callers' groups ({} analysed)", after_signature.first - before_signature.first));
}

// A warning the preprocessor reports holds its byte offset: a comment before it moves it, and
// only whether there is an error reaches the analysis (#947).
void moved_warning() {
  pl::Session s;
  const std::string text = "#warning \"kept\"\nint f() { return 1; }\nint main() { return f() - 1; }\n";
  build(s, text);
  check(s.get<pl::Link>(program).errors.empty(), "a unit with a warning links");
  const std::uint32_t sema0 = s.db().runs<pl::TuSema>(tu);
  build(s, "// moved\n" + text);
  check(s.get<pl::Link>(program).image == cold("// moved\n" + text), "incremental equals cold");
  check(s.db().runs<pl::TuSema>(tu) == sema0, "a moved warning leaves the analysis green");
  check(pl::TuFront::errors(s.get<pl::TuFront>(tu)).empty() && !s.get<pl::TuFront>(tu).messages.empty() &&
            s.get<pl::TuFront>(tu).messages.front().where.find(":2:") != std::string::npos,
        "the warning is placed on its new line");
}

// Two units in one session; then one with an error, which keeps the program from linking, and
// fixed again.
void two_units() {
  pl::Session s;
  const pl::TuId a{0}, b{1};
  s.set_source(a, "a.cpp", "int f() { return 1; }\n", "a.o");
  s.set_source(b, "b.cpp", "int f();\nint main() { return f() - 1; }\n", "b.o");
  s.set_program(program, {{a, b}, {}, "two"});
  const std::vector<std::uint8_t> image = s.get<pl::Link>(program).image;
  check(!image.empty() && s.get<pl::Link>(program).errors.empty(), "two units link");

  s.set_source(a, "a.cpp", "int f() { return 1 }\n", "a.o");
  check(s.get<pl::TuFront>(a).has_errors(), "the front end reports the error");
  check(!s.get<pl::Tu>(pl::Tu::Key{a}).object, "a unit with front-end errors has no object");
  const pl::Linked& broken = s.get<pl::Link>(program);
  check(broken.image.empty() && !broken.errors.empty(), "a unit with front-end errors keeps the program from linking");

  s.set_source(a, "a.cpp", "int f() { return 1; }\n", "a.o");
  check(s.get<pl::Link>(program).image == image, "fixed again, the program links as before");
}

// Headers are `FileContent` inputs, provided on first read (#93): an edit to one, set as the
// daemon sets it, re-runs `pp` and what depends on it, an unedited one leaves everything green,
// and the same text in another directory reads that directory's header.
void headers() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / std::format("ycxx-incremental-headers-{}", std::random_device{}());
  fs::create_directories(dir / "a");
  fs::create_directories(dir / "b");
  auto write = [](const fs::path& f, std::string_view text) { std::ofstream(f, std::ios::binary | std::ios::trunc) << text; };
  const std::string main_text = "#include \"v.h\"\nint main() { return V; }\n";
  write(dir / "a" / "v.h", "#define V 1\n");
  write(dir / "b" / "v.h", "#define V 2\n");
  auto build_in = [&](pl::Session& s, const fs::path& cwd) {
    s.set_source(tu, "m.cpp", main_text, "m.o", cwd.native_encoded_string());
    s.set_program(program, {{tu}, {}, "m"});
    return s.get<pl::Link>(program).image;
  };
  auto cold_in = [&](const fs::path& cwd) {
    pl::Session s;
    return build_in(s, cwd);
  };

  pl::Session s;
  std::vector<std::uint8_t> a1 = build_in(s, dir / "a");
  check(a1 == cold_in(dir / "a") && !a1.empty(), "a unit with a header, built");
  s.db().reset_stats();
  build_in(s, dir / "a");
  check(s.db().stats().executed == 0, "rebuilt with no edit: nothing executes");

  const pp::PathId h = pp::path_id((dir / "a" / "v.h").lexically_normal().generic_native_encoded_string());
  write(dir / "a" / "v.h", "#define V 3\n");
  check(s.db().set<pp::FileContent>(h, std::string("#define V 3\n")), "the header's input changes");
  s.db().reset_stats();
  std::vector<std::uint8_t> a3 = build_in(s, dir / "a");
  check(s.db().stats().executed > 0 && a3 != a1 && a3 == cold_in(dir / "a"), "a header edit re-runs pp: equal to cold");

  // The same argument vector and text in another directory: its own header.
  std::vector<std::uint8_t> b = build_in(s, dir / "b");
  check(b == cold_in(dir / "b") && b != a3, "the same text in another directory reads its own header");
  fs::remove_all(dir);
}

// The editor view (daemon design §6): its unit reads open buffers over the disk, a buffer edit or
// close reaches it, and no disk-view cell executes or reads a buffer: a build after a keystroke
// executes nothing, and its bytes are the disk's.
void editor_view() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / std::format("ycxx-incremental-editor-{}", std::random_device{}());
  fs::create_directories(dir);
  auto write = [](const fs::path& f, std::string_view text) { std::ofstream(f, std::ios::binary | std::ios::trunc) << text; };
  const std::string main_text = "#include \"v.h\"\nint main() { return V - 1; }\n";
  write(dir / "m.cpp", main_text);
  write(dir / "v.h", "#define V 1\n");
  auto id = [&](std::string_view name) { return pp::path_id((dir / name).lexically_normal().generic_native_encoded_string()); };
  auto build_in = [&](pl::Session& s) {
    s.set_source(tu, "m.cpp", main_text, "m.o", dir.native_encoded_string());
    s.set_program(program, {{tu}, {}, "m"});
    return s.get<pl::Link>(program).image;
  };
  const pl::TuId editor{1u << 31};
  pp::Config editor_config;
  editor_config.view = pp::View::editor;
  auto check_editor = [&](pl::Session& s) {
    s.set_source(editor, "m.cpp", "", "m.o", dir.native_encoded_string(), editor_config);
    return !s.get<pl::TuFront>(editor).has_errors();
  };

  pl::Session s;
  const std::vector<std::uint8_t> disk = build_in(s);
  check(!disk.empty() && check_editor(s), "the editor view of an unedited unit is clean");
  // Opening the editor's unit set its `low` inputs: the next build verifies, once.
  build_in(s);

  // A keystroke in the header: the editor's unit sees it, the build does not.
  s.db().set<pp::Buffer>(id("v.h"), std::string("#define W 1\n"));
  check(!check_editor(s), "an unsaved header reaches the editor view");
  s.db().reset_stats();
  // Nothing deep-verified either: a disk-view cell that read a buffer would take its durability
  // and be validated after every keystroke without executing (§6.3).
  check(build_in(s) == disk && s.db().stats().executed == 0 && s.db().stats().validated == 0,
        "a build after a keystroke executes and validates nothing");

  // A keystroke in the unit itself, then the header's buffer closed.
  s.db().set<pp::Buffer>(id("m.cpp"), std::string("#include \"v.h\"\nint main() { return W; }\n"));
  check(check_editor(s), "the unit's own buffer is what the editor view reads");
  s.db().set<pp::Buffer>(id("v.h"), std::nullopt);
  check(!check_editor(s), "a closed buffer falls back to the disk");
  s.db().set<pp::Buffer>(id("m.cpp"), std::nullopt);
  check(check_editor(s), "with every buffer closed the editor view is the disk's");
  s.db().reset_stats();
  check(build_in(s) == disk && s.db().stats().executed == 0 && s.db().stats().validated == 0, "the build is still the disk's");
  fs::remove_all(dir);
}

// Virtual sources and headers are distinct path inputs. Earlier disk candidates still matter,
// and a file under the virtual directory has no fallback to a stale file on disk.
void generated_view() {
  const std::string root = "/ycxx-generated-view-test";
  const std::string generated = root + "/.ycxx/virtual/wire";
  const std::string source_path = generated + "/wire.cpp";
  const std::string header_path = generated + "/wire.h";
  const std::string disk_header = root + "/include/wire.h";
  const std::string disk_source = "int wire() { return 99; }\n";
  const std::string main_text = "#include <wire.h>\nint main() { return VALUE; }\n";
  const auto put_disk = [](engine::Db& db, const std::string& path, std::optional<std::string> text) {
    db.set<pp::FileContent>(pp::path_id(path), std::move(text));
  };

  pl::Session s;
  pp::Config cfg;
  cfg.view = pp::View::build;
  cfg.angle_dirs = {root + "/include", generated};
  cfg.system_from = cfg.angle_dirs.size();
  const auto main = s.unit({"main.cpp", root, cfg, "main.o"});
  const auto wire = s.unit({source_path, root, cfg, "wire.o"});
  const auto request = s.request({.units = {main, wire}, .until = pl::Until::object});
  const pp::FileId main_file{main.n};
  s.apply([&](engine::Db& db) {
    put_disk(db, root + "/main.cpp", main_text);
    put_disk(db, source_path, disk_source);
    s.set_generated(source_path, "#include \"wire.h\"\nint wire() { return VALUE; }\n");
    s.set_generated(header_path, "#define VALUE 3\n");
  });
  const pl::Built first = s.get<pl::Build>(request);
  check(first.units[0].object && first.units[1].object,
        "native build can read a generated source and header: " + pl::Build::errors(first));
  if (!first.units[0].object || !first.units[1].object) return;
  const auto object_bytes = [](const pl::UnitBuilt& unit) {
    return unit.object ? obj::macho::write_object(*unit.object) : std::vector<std::uint8_t>{};
  };
  const auto main_first = object_bytes(first.units[0]);
  const auto wire_first = object_bytes(first.units[1]);
  const auto main_pp = s.db().runs<pp::Pp>(main_file);

  s.apply([&](engine::Db&) {
    s.set_generated(source_path, "#include \"wire.h\"\nint wire() { return VALUE + 2; }\n");
    check(!s.set_generated(header_path, "#define VALUE 3\n"), "an equal generated header input stays equal");
  });
  const pl::Built second = s.get<pl::Build>(request);
  check(object_bytes(second.units[0]) == main_first && s.db().runs<pp::Pp>(main_file) == main_pp,
        "editing a generated sibling leaves a consumer of the header green");
  check(second.units[1].object && object_bytes(second.units[1]) != wire_first,
        "the generated source itself recompiles");

  s.apply([&](engine::Db& db) { put_disk(db, disk_header, "#define VALUE 4\n"); });
  const pl::Built shadowed = s.get<pl::Build>(request);
  check(shadowed.units[0].object && object_bytes(shadowed.units[0]) != main_first &&
        s.db().runs<pp::Pp>(main_file) > main_pp, "a newly appearing earlier include candidate shadows the generated one");
  check(object_bytes(shadowed.units[1]) == object_bytes(second.units[1]),
        "the generated source finds its sibling through quoted include lookup");

  s.apply([&](engine::Db&) { s.set_generated(header_path, "\n#error changed generated header\n"); });
  const auto diagnostic = s.get<pl::Build>(request);
  check(std::ranges::any_of(diagnostic.units[1].messages, [&](const pl::Message& message) {
          return message.where.starts_with(header_path + ":2:") && message.text.contains("changed generated header");
        }), "generated diagnostics report the location in the virtual file");

  s.apply([&](engine::Db&) { s.set_generated(source_path, std::nullopt); });
  check(!s.get<pl::Build>(request).units[1].readable, "the build view cannot consume a removed virtual source from disk");
  s.set_source(tu, source_path, disk_source, "ordinary.o", root);
  const auto ordinary = s.request({.units = {tu}, .until = pl::Until::object});
  check(s.get<pl::Build>(ordinary).units[0].object.has_value(), "the normal compiler view still reads disk at the same path");
}

// A session on a pool (#137): every worker is bound to the session's universe, and the program's
// bytes equal an inline session's, at 2 and 8 threads and under chaos seeds.
void pooled() {
  const std::vector<std::uint8_t> want = cold(v3);
  for (auto [threads, seed] : std::array<std::pair<unsigned, std::optional<std::uint64_t>>, 4>{
           {{2, std::nullopt}, {8, std::nullopt}, {2, 7}, {4, 1234}}}) {
    engine::Pool pool(threads, seed);
    pl::Session s(pool);
    build(s, v3);
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && l.image == want, std::format("pooled at {} threads, seed {}: equal to inline", threads, seed.value_or(0)));
    // Incremental on the pool too: an edit, then back.
    build(s, v2);
    check(s.get<pl::Link>(program).image == cold(v2), "pooled, after an edit");
  }
  // One pool, one session after another: the workers leave the first session's universe and
  // enter the second's.
  engine::Pool pool(4);
  for (const std::string* text : {&v3, &v2, &v3}) {
    pl::Session s(pool);
    build(s, *text);
    check(s.get<pl::Link>(program).image == cold(*text), "a pool serving one session after another");
  }
}

// Several units on a pool (#156): eight units include one header that defines classes and
// enumerations, so their front ends define the same records at once on different workers. The
// program's bytes equal an inline session's at any thread count. The members are defined out of
// line in one unit: inline functions in several objects collide until #124.
void pooled_units() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / std::format("ycxx-incremental-pooled-{}", std::random_device{}());
  fs::create_directories(dir);
  std::ofstream(dir / "s.h", std::ios::binary | std::ios::trunc) << R"(enum class Kind : int { a = 1, b = a + 1, c };
enum Flag { off, on = 4 };
struct Part {
  int id;
  Part();
  Part(int i);
  ~Part();
};
struct S {
  int x;
  Kind k = Kind::b;
  Part p;
  Part q;
  S(int v);
  ~S();
  int get() const;
};
)";
  constexpr std::uint32_t units = 8;
  auto build_units = [&](pl::Session& s, int edit) {
    std::vector<pl::TuId> tus;
    for (std::uint32_t i = 0; i < units; ++i) {
      std::string text = "#include \"s.h\"\n";
      if (i == 0) {
        text += R"(Part::Part() : id(9) {}
Part::Part(int i) : id(i) {}
Part::~Part() { id = 0; }
S::S(int v) : x(v), p(v + 1) {}
S::~S() { x = 0; }
int S::get() const { return x + p.id + q.id + (k == Kind::b ? 2 : 0) + on; }
)";
        for (std::uint32_t j = 1; j < units; ++j) text += std::format("int f{}();\n", j);
        text += "int main() { int r = 0;";
        for (std::uint32_t j = 1; j < units; ++j) text += std::format(" r += f{}();", j);
        text += std::format(" return r % 256 - {}; }}\n", edit);
      } else {
        text += std::format("int f{0}() {{ S s({0}); Part t; return s.get() + t.id + {1}; }}\n", i, i == 3 ? edit : 0);
      }
      s.set_source({i}, std::format("u{}.cpp", i), text, std::format("u{}.o", i), dir.native_encoded_string());
      tus.push_back({i});
    }
    s.set_program(program, {tus, {}, "pooled"});
  };
  auto inline_image = [&](int edit) {
    pl::Session s;
    build_units(s, edit);
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && !l.image.empty(), "several units with one header link inline");
    return l.image;
  };
  const std::vector<std::uint8_t> want = inline_image(0), want_edit = inline_image(5);
  for (auto [threads, seed] : std::array<std::pair<unsigned, std::optional<std::uint64_t>>, 5>{
           {{2, std::nullopt}, {8, std::nullopt}, {8, 7}, {4, 1234}, {8, 99}}}) {
    engine::Pool pool(threads, seed);
    pl::Session s(pool);
    build_units(s, 0);
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && l.image == want,
          std::format("{} units on {} threads, seed {}: equal to inline", units, threads, seed.value_or(0)));
    build_units(s, 5);
    check(s.get<pl::Link>(program).image == want_edit, std::format("{} units on {} threads, after an edit", units, threads));
  }
  // A unit does not see a record another unit published before its own definition is complete:
  // both units diagnose the nested type used before its declaration, at any schedule.
  std::ofstream(dir / "late.h", std::ios::binary | std::ios::trunc) << "struct T { E f(); enum E { x }; };\n";
  for (unsigned threads : {1u, 8u}) {
    engine::Pool pool(threads);
    pl::Session s(pool);
    for (std::uint32_t i = 0; i < 2; ++i)
      s.set_source({i}, std::format("l{}.cpp", i), "#include \"late.h\"\n", std::format("l{}.o", i), dir.native_encoded_string());
    s.set_program(program, {{{0}, {1}}, {}, "late"});
    s.get<pl::Link>(program);
    check(s.get<pl::TuFront>({0}).has_errors() && s.get<pl::TuFront>({1}).has_errors(),
          std::format("each unit diagnoses its own class, on {} threads", threads));
  }
  // An enumerator named by qualified name in its own enumerator list ([dcl.enum]/6), and each
  // unit's own class in its unnamed namespace ([namespace.unnamed]), whatever unit comes first.
  std::ofstream(dir / "e.h", std::ios::binary | std::ios::trunc) << "enum E { a, b = E::a + 1 };\nenum class F : int { p, q };\nF g();\n";
  const std::array<std::string, 2> texts{
      "#include \"e.h\"\nnamespace { struct S { int x; int y; }; }\nint fa() { S s{1, 2}; return s.x + b; }\n",
      "#include \"e.h\"\nnamespace { struct S { int z; }; }\nint fa();\nint main() { S s{30}; return s.z + fa(); }\n"};
  for (bool swapped : {false, true})
    for (unsigned threads : {1u, 8u}) {
      engine::Pool pool(threads, 7);
      pl::Session s(pool);
      for (std::uint32_t i = 0; i < 2; ++i)
        s.set_source({i}, std::format("n{}.cpp", i), texts[i ^ swapped], std::format("n{}.o", i), dir.native_encoded_string());
      s.set_program(program, {{{0}, {1}}, {}, "named"});
      const pl::Linked& l = s.get<pl::Link>(program);
      check(!s.get<pl::TuFront>({0}).has_errors() && !s.get<pl::TuFront>({1}).has_errors() && l.errors.empty(),
            std::format("enumerators in their own list and unnamed namespaces per unit, on {} threads{}", threads,
                        swapped ? ", swapped" : ""));
    }
  fs::remove_all(dir);
}

// Class and enumeration records follow an edit (#115): the members' order and types, and an
// enumeration's underlying type, each state equal to a cold build of it.
void records() {
  const std::array<std::string, 4> states{
      "struct S { int x; int y; };\nenum E : int { a, b };\nint main() { S s{1, 2}; return s.x; }\n",
      "struct S { int y; int x; };\nenum E : int { a, b };\nint main() { S s{1, 2}; return s.x; }\n",
      "struct S { char c; long long y; int x; };\nenum E : long long { a = 1099511627776, b };\n"
      "int main() { S s{1, 1099511627776, 3}; long long v = a; return s.y / 1024 == v / 1024 ? s.x : 0; }\n",
      "struct S { int x; int y; };\nenum E : int { a, b };\nint main() { S s{1, 2}; return s.x; }\n"};
  pl::Session s;
  for (const std::string& text : states) {
    build(s, text);
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && l.image == cold(text), "an edited class or enumeration: equal to a cold build");
  }
  // Only the class and the enumeration change, not one function body: the functions' layouts
  // and enumerations come from the unit's shapes, a dependency of their cells.
  const std::string body = "S g{};\nE ge = a;\nint f(S* p) { return p->y; }\nint main() { S s{}; return f(&s) + g.y + ge; }\n";
  for (const std::string head : {"struct S { int x; int y; };\nenum E { a, b };\n",
                                 "struct S { long long x; int y; };\nenum E { a, b = 1099511627776 };\n",
                                 "struct S { char c; int y; };\nenum E : unsigned char { a, b };\n"}) {
    build(s, head + body);
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && l.image == cold(head + body), "only a class and an enumeration edited: equal to a cold build");
  }
}

void virtual_declarations() {
  const std::string plain = "struct Root { int field; int value() const { return field; } };\n";
  // A polymorphic class in an unnamed namespace is not a dynamic class yet (#1155): it keeps the
  // virtual layout guard. A dynamic root lays out and emits code (#309).
  const std::string polymorphic =
      "namespace { struct Root { int field; virtual int value() const { return field; } }; }\n";
  const std::string dynamic = "struct Root { int field; virtual int value() const { return field; } };\n";
  const std::string interfaces = R"cpp(
struct Outer { Root elements[2]; };
const Root& pass(const Root* pointer) { return *pointer; }
Root by_value(Root);
static_assert(sizeof(Root*) == sizeof(void*));
int main() { return 0; }
)cpp";
  const std::string layout = "constexpr auto alignment = alignof(Outer);\n";
  const auto blocked_shapes = [](const pl::FrontResult& front) {
    return std::ranges::count(front.shapes.classes, true, &pl::ClassShape::layout_requires_virtual_support);
  };

  pl::Session warm;
  build(warm, plain + interfaces + layout);
  check(warm.get<pl::Tu>(pl::Tu::Key{tu}).object.has_value(), "ordinary member and array layouts produce an object");

  build(warm, dynamic + interfaces + layout);
  const auto& dynamic_front = warm.get<pl::TuFront>(tu);
  check(!dynamic_front.has_errors() && blocked_shapes(dynamic_front) == 0, pl::TuFront::errors(dynamic_front));
  check(warm.get<pl::Link>(program).image == cold(dynamic + interfaces + layout),
        "a dynamic root and its containing class: equal to a cold build");

  build(warm, polymorphic + interfaces + layout);
  const auto& laid_out = warm.get<pl::TuFront>(tu);
  const auto layout_error = pl::TuFront::errors(laid_out);
  check(layout_error.find("object layout requiring virtual function support") != std::string::npos,
        "alignof of a containing class reaches the virtual array element");
  check(blocked_shapes(laid_out) == 2, "root and containing class publish the virtual layout requirement");
  pl::Session scratch;
  build(scratch, polymorphic + interfaces + layout);
  check(layout_error == pl::TuFront::errors(scratch.get<pl::TuFront>(tu)), "the warm layout diagnostic matches a cold build");

  build(warm, polymorphic + interfaces);
  const auto& declarations = warm.get<pl::TuFront>(tu);
  check(!declarations.has_errors(), pl::TuFront::errors(declarations));
  check(blocked_shapes(declarations) == 2, "safe interfaces retain the cached layout requirement");
  const auto& native = warm.get<pl::Tu>(pl::Tu::Key{tu});
  check(!native.object && pl::Tu::errors(native).find("native output for a translation unit with virtual member functions") !=
                              std::string::npos,
        "a semantically checked unused virtual body cannot produce native output");

  build(warm, plain + interfaces + layout);
  const auto& restored = warm.get<pl::TuFront>(tu);
  check(!restored.has_errors() && blocked_shapes(restored) == 0, "removing virtual restores class layout");
  check(warm.get<pl::Link>(program).image == cold(plain + interfaces + layout), "restored native output matches a cold build");

  build(warm, "namespace { struct Root { virtual int value() const; }; }\nint Root::value() const { return 1; }\n");
  const auto key_error = pl::TuFront::errors(warm.get<pl::TuFront>(tu));
  check(key_error.find("an out-of-class definition of a virtual member function") != std::string::npos,
        "an external key definition receives its own semantic diagnostic");

  const std::string lazy = polymorphic + "template<class T> struct Box { T elements[2]; };\nusing Nested = Box<Root>;\n";
  build(warm, lazy + "const Nested* pass(const Nested* p) { return p; }\n");
  check(!warm.get<pl::TuFront>(tu).has_errors(), "forming a pointer to a class specialization needs no layout");
  for (const std::string use : {"Nested stored;\n", "constexpr auto alignment = alignof(Nested);\n",
                               "constexpr auto size = sizeof(Nested);\n"}) {
    build(warm, lazy + use);
    check(pl::TuFront::errors(warm.get<pl::TuFront>(tu)).find("object layout requiring virtual function support") !=
              std::string::npos,
          "a layout demand completes a class specialization with a virtual array element");
  }
}

void bit_fields() {
  std::string text = R"cpp(
enum Width { second = 5 };
struct F { unsigned a : 1 + 2, b : second; };
struct FileSlice { int descriptor; signed int flags : 24; char next; long offset; };
static_assert(sizeof(F) == 4 && alignof(F) == 4);
static_assert(sizeof(FileSlice) == 16 && alignof(FileSlice) == 8);
F packed{3, 17};
FileSlice file{1, -177, 'Q', 2L};
static_assert(__is_same(decltype(packed.a), unsigned int));
static_assert(__is_same(decltype(+packed.a), int));
static_assert(__is_same(decltype(packed.a + packed.b), int));
static_assert(__is_same(decltype(+static_cast<unsigned>(packed.a)), unsigned));
static_assert(__is_same(decltype(+(unsigned)packed.a), unsigned));
static_assert(__is_same(decltype(+unsigned(packed.a)), unsigned));
int main() {
  F f{};
  f.a = 9;
  f.b = 31;
  if (f.a != 1 || f.b != 31) return 1;
  FileSlice g{1, -177, 'Q', 2L};
  if (g.flags++ != -177 || g.flags != -176 || g.next != 'Q') return 2;
  file.flags += 3;
  if (file.flags != -174 || file.next != 'Q' || file.offset != 2) return 3;
  return 0;
}
)cpp";
  pl::Session s;
  build(s, text);
  const auto& front = s.get<pl::TuFront>(tu);
  check(!front.has_errors(), pl::TuFront::errors(front));
  const auto& result = s.get<pl::Tu>(pl::Tu::Key{tu});
  check(result.object.has_value(), pl::Tu::errors(result));
  if (!result.object) return;
  auto bytes_of = [](const auto& obj, std::string_view name) -> std::span<const std::uint8_t> {
    auto sym = std::ranges::find(obj.symbols, name, &ycxx::obj::symbol::name_view);
    if (sym == obj.symbols.end() || !sym->section) return {};
    return std::span(obj.sections[*sym->section].bytes).subspan(sym->offset);
  };
  auto packed = bytes_of(*result.object, "_packed");
  auto file = bytes_of(*result.object, "_file");
  check(packed.size() >= 4 && packed[0] == 0x8b, "static adjacent bit-fields share the same byte");
  check(file.size() >= 16 && file[4] == 0x4f && file[5] == 0xff && file[6] == 0xff && file[7] == 'Q',
        "static signed 24-bit storage preserves the adjacent ordinary byte");

  text.replace(text.find("1 + 2"), 5, "1 + 3");
  build(s, text);
  const auto& edited = s.get<pl::Tu>(pl::Tu::Key{tu});
  check(edited.object.has_value(), pl::Tu::errors(edited));
  if (!edited.object) return;
  auto repacked = bytes_of(*edited.object, "_packed");
  check(repacked.size() >= 4 && repacked[0] == 0x13 && repacked[1] == 0x01,
        "editing a bit-field width republishes the layout and static object bytes");
}

void unnamed_union_members() {
  const std::string value = "union { unsigned code; char bytes[12]; } value, mirror;\n";
  const std::string spare = "union { unsigned code; char bytes[12]; } spare;\n";
  const std::string rest = R"cpp(
  short suffix;
} State;
typedef struct { union { unsigned code; char bytes[12]; } value; } Other;
extern State state;
extern Other other;
static_assert(sizeof(State) == 44 && alignof(State) == 4);
static_assert(sizeof(decltype(state.value)) == 12 && alignof(decltype(state.value)) == 4);
static_assert(__is_union(decltype(state.value)));
static_assert(__is_same(decltype(state.value), decltype(state.mirror)));
static_assert(!__is_same(decltype(state.value), decltype(state.spare)));
static_assert(!__is_same(decltype(state.value), decltype(other.value)));
void value_type(decltype(state.value)*) {}
void spare_type(decltype(state.spare)*) {}
int main() {
  State s{};
  s.value.code = 7;
  s.mirror.code = 3;
  s.spare.code = 9;
  return s.value.code + s.mirror.code + s.spare.code - 19;
}
)cpp";
  pl::Session s;
  for (bool swapped : {false, true}) {
    std::string text = "typedef struct { char prefix;\n" + (swapped ? spare + value : value + spare) + rest;
    build(s, text);
    constexpr pl::TuId peer{1};
    s.set_source(peer, "union-peer.cpp", text, "union-peer.o");
    const auto& front = s.get<pl::TuFront>(tu);
    check(!front.has_errors(), pl::TuFront::errors(front));
    const auto& peer_front = s.get<pl::TuFront>(peer);
    check(!peer_front.has_errors() && front.shapes.classes.size() == 5 &&
              std::ranges::equal(front.shapes.classes, peer_front.shapes.classes, {}, &pl::ClassShape::entity, &pl::ClassShape::entity),
          "unnamed nested union identities correspond across translation units");
    const auto& result = s.get<pl::Tu>(pl::Tu::Key{tu});
    check(result.object.has_value(), pl::Tu::errors(result));
    if (!result.object) continue;
    const char* first = swapped ? "__Z10spare_typePN5StateUt_E" : "__Z10value_typePN5StateUt_E";
    const char* second = swapped ? "__Z10value_typePN5StateUt0_E" : "__Z10spare_typePN5StateUt0_E";
    check(std::ranges::contains(result.object->symbols, std::string_view(first), &ycxx::obj::symbol::name_view) &&
              std::ranges::contains(result.object->symbols, std::string_view(second), &ycxx::obj::symbol::name_view),
          "unnamed sibling union types mangle in lexical order");
    const pl::Linked& l = s.get<pl::Link>(program);
    check(l.errors.empty() && l.image == cold(text), "after changing union order, incremental equals cold");
  }

  for (const std::string source : {
           "struct S { union { int variant; } member; }; int get(S* s) { return s->variant; }",
           "struct S { union { int variant; }; };",
           "class S { union { int variant; } member; }; int get(S* s) { return s->member.variant; }",
           "struct S { union { int& variant; } member; };",
           "struct S { union { int f() { return 0; } int variant; } member; };"}) {
    build(s, source);
    check(s.get<pl::TuFront>(tu).has_errors(), source);
  }
}

// Nesting too deep for the stack (#187): on workers with 1 MiB stacks, 20,000 nested parentheses,
// blocks, increments, `sizeof`s and braced lists are diagnosed, not a crash, and the session goes
// on. The rest of the pipeline is #260.
void too_deep() {
  engine::Pool pool(2, std::nullopt, std::size_t{1} << 20);
  pl::Session s(pool);
  constexpr int n = 20000;
  const std::string parens = "int main() { return " + std::string(n, '(') + "0" + std::string(n, ')') + "; }\n";
  const std::string blocks = "int main() { " + std::string(n, '{') + std::string(n, '}') + " return 0; }\n";
  std::string increments = "int main() { int x = 0; return ", sizes = "int main() { return ";
  for (int i = 0; i < n; ++i) increments += "++";
  for (int i = 0; i < n; ++i) sizes += "sizeof ";
  increments += "x; }\n";
  sizes += "0; }\n";
  const std::string braces = "int main() { int a = " + std::string(n, '{') + "0" + std::string(n, '}') + "; return a; }\n";
  for (const std::string* text : std::initializer_list<const std::string*>{&parens, &blocks, &increments, &sizes, &braces}) {
    build(s, *text);
    const pl::FrontResult& f = s.get<pl::TuFront>(tu);
    check(f.has_errors() && std::ranges::any_of(f.messages, [](const pl::Message& m) { return m.text.contains("nested too deeply"); }),
          "nesting too deep for the stack: diagnosed");
  }
  build(s, v0);
  check(s.get<pl::Link>(program).image == cold(v0), "the session goes on after a too-deep unit");
}

// [temp.inst]/16's limit is reported once per unit, where the whole unit's analysis first reaches
// it, whether the unit is analysed a group at a time or whole (a `virtual` makes it whole, #947),
// cold or after edits; here the other messages are the same both ways (#993).
void depth_limit_once() {
  pp::Config config;
  config.template_depth = 7;
  const std::string first = "template<int N> struct First { static constexpr int value = First<N+1>::value; };\n"
                            "int first = First<1>::value;\n";
  const std::string fixed = "template<int N> struct First { static constexpr int value = N; };\n"
                            "int first = First<1>::value;\n";
  const std::string second = "template<int N> struct Second { static constexpr int value = Second<N+1>::value; };\n"
                             "int second = Second<1>::value;\n";
  const std::string whole = "struct Whole { virtual void f() {} };\n";
  auto messages = [&](pl::Session& s, const std::string& text) {
    s.set_source(tu, "depth.cpp", text, "depth.o", {}, config);
    std::vector<std::string> texts;
    for (const pl::Message& m : s.get<pl::TuFront>(tu).messages) texts.push_back(m.text);
    return texts;
  };
  auto cold_messages = [&](const std::string& text) {
    pl::Session s;
    return messages(s, text);
  };
  auto depth = [](const std::vector<std::string>& texts) {
    return std::ranges::count_if(texts, [](const std::string& t) { return t.contains("nest more than 7 deep"); });
  };
  const auto grouped = cold_messages(first + second), whole_unit = cold_messages(first + second + whole);
  check(depth(grouped) == 1, "grouped: the depth limit is reported once");
  check(depth(whole_unit) == 1, "whole: the depth limit is reported once");
  check(grouped == whole_unit, "grouped and whole analysis say the same messages");
  pl::Session s;
  check(messages(s, first + second) == grouped, "a session's first build equals cold");
  // Both families' groups reused: the first reports it, as cold.
  auto before = s.group_counts();
  const auto third = messages(s, first + second + "int third = 3;\n");
  auto after = s.group_counts();
  check(after.first - before.first == 1 && after.second - before.second == 4,
        std::format("an unrelated edit analyses its group alone ({} analysed, {} reused)", after.first - before.first,
                    after.second - before.second));
  check(depth(third) == 1 && third == cold_messages(first + second + "int third = 3;\n"),
        "an unrelated edit: still once, as cold");
  const auto only_second = messages(s, fixed + second);
  check(depth(only_second) == 1 && only_second == cold_messages(fixed + second), "the first family fixed: the second's, as cold");
  check(depth(messages(s, first + second)) == 1, "the first family back: once again");
  // A function template's body reaches the limit only in the END worklist, which the whole unit
  // runs after every item: a later class template's, reached while its item is analysed, is the
  // one reported.
  const std::string late = "template<int N> int f() { return f<N+1>(); }\nint g() { return f<1>(); }\n";
  auto at = [](pl::Session& s) {
    for (const pl::Message& m : s.get<pl::TuFront>(tu).messages)
      if (m.text.contains("nest more than")) return m.where.substr(m.where.find(':'));
    return std::string{};
  };
  pl::Session grouped_late, whole_late;
  const auto g = messages(grouped_late, late + second), w = messages(whole_late, whole + late + second);
  check(depth(g) == 1 && depth(w) == 1, "a limit reached at the end and one in an item: once");
  check(at(grouped_late) == ":3:33" && at(whole_late) == ":4:33", "the item's is reported, both ways");
}

// A BSD archive with a ranlib index for a single Mach-O object. The test runs on either host.
obj::archive::input runtime_archive(const obj::object& object) {
  namespace mo = obj::macho;
  constexpr std::string_view name = "_runtime_helper";
  mo::byte_writer archive, index;
  archive.raw("!<arch>\n");
  index.u32(8);  // one ranlib entry
  index.u32(0);  // name offset
  index.u32(0);  // member header offset, filled below
  index.u32(static_cast<std::uint32_t>(name.size() + 1));
  index.raw(name);
  index.u8(0);
  constexpr std::size_t header_size = 60;
  const auto member_offset = archive.size() + header_size + index.size() + index.size() % 2;
  mo::store32(index.bytes, 8, static_cast<std::uint32_t>(member_offset));
  const auto append = [&](std::string_view member, std::span<const std::uint8_t> bytes) {
    archive.raw(std::format("{:16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n", member, 0, 0, 0, 644, bytes.size()));
    archive.raw(bytes);
    if (bytes.size() % 2) archive.u8('\n');
  };
  append("__.SYMDEF", index.bytes);
  append("runtime.o", mo::write_object(object));
  return {"runtime.a", std::string(reinterpret_cast<const char*>(archive.bytes.data()), archive.size())};
}

void implicit_runtime_policies() {
  pl::Session s;
  const pl::TuId entry{0}, second{1}, peer{2}, runtime{3};
  const std::string peer_source = "extern \"C\" int peer() { return 2; }";
  s.set_source(entry, "main.cpp", "int second(); int main() { return second() != 42; }", "main.o");
  s.set_source(second, "second.cpp", "extern \"C\" int runtime_helper(); extern \"C\" int peer(); int second() { "
               "return runtime_helper() + peer(); }", "second.o");
  s.set_source(peer, "peer.cpp", peer_source, "peer.o");
  s.set_source(runtime, "runtime.cpp", "extern \"C\" int runtime_helper() { return 40; }", "runtime.o");
  // Build checks path-keyed readability before consuming the translation unit's FileText.
  s.db().set<pp::FileContent>(pp::path_id("peer.cpp"), peer_source);
  const auto& checked = s.get<pl::Build>(s.request({
      .units = {peer}, .until = pl::Until::object, .check_policies = true}));
  const auto& compiled_runtime = s.get<pl::Tu>(pl::Tu::Key{runtime});
  const auto& compiled_peer = s.get<pl::Tu>(pl::Tu::Key{peer});
  const auto& member = compiled_runtime.object;
  const auto& unchecked = compiled_peer.object;
  const bool built = checked.units.size() == 1 && checked.units[0].object && member && unchecked;
  check(built, "ordinary and runtime fixtures build: " + pl::Build::errors(checked) +
               pl::Tu::errors(compiled_runtime) + pl::Tu::errors(compiled_peer));
  if (!built) return;
  const auto darwin = obj::target::aarch64_macos;
  const bool mach = checked.units[0].object->arch == darwin && member->arch == darwin && unchecked->arch == darwin;
  check(mach, "ordinary and runtime fixtures target arm64 Mach-O");
  if (!mach) return;

  using Kind = pl::LinkOperand::Kind;
  pl::ProgramInputs automatic{{entry, second}, {*checked.units[0].object}, "checked-runtime",
      std::nullopt, pl::ArchiveLinkInputs{obj::target::aarch64_macos,
        {{Kind::object, 0}, {Kind::source, 1}, {Kind::darwin_runtime_archive, 0}, {Kind::source, 0}},
        {runtime_archive(*member)}}, true};
  const auto implicit = s.program(automatic);
  const auto linked = s.get<pl::Link>(implicit);
  check(linked.errors.empty() && !linked.image.empty(), "checked sources retain coverage across interleaved inputs");

  auto explicit_archive = automatic;
  explicit_archive.archive_link->operands[2].kind = Kind::archive;
  const auto explicit_id = s.program(std::move(explicit_archive));
  check(implicit != explicit_id, "program identity distinguishes explicit and driver runtime archives");
  check(s.get<pl::Link>(explicit_id).errors ==
        std::vector<std::string>{"checked ownership links require explicit objects and source inputs"},
        "the same archive bytes supplied explicitly are rejected");

  auto missing_coverage = automatic;
  missing_coverage.objects[0] = *unchecked;
  check(s.get<pl::Link>(s.program(std::move(missing_coverage))).errors ==
        std::vector<std::string>{"peer.o: missing checked policy coverage"},
        "ordinary object coverage remains mandatory");

  auto forged_member = *member;
  auto& caller = automatic.objects.front();
  const auto attach = [](obj::object& object, std::string_view function, std::string_view kind) {
    const auto symbol = std::ranges::find(object.symbols, function, &obj::symbol::name_view);
    check(symbol != object.symbols.end() && symbol->defined(), "the test function has a text definition");
    if (symbol == object.symbols.end() || !symbol->defined()) return;
    const auto section = symbol->section;
    const auto offset = symbol->offset;
    std::erase_if(object.symbols, [](const auto& symbol) {
      return symbol.name_view().starts_with("__ycxx_policy.1.checked.");
    });
    object.symbols.push_back({"__ycxx_policy.1.checked.1", obj::binding::local, 0, 0});
    object.symbols.push_back({std::format("__ycxx_policy.3.effect.{}.{}._runtime_helper", kind, std::string(32, '0')),
                              obj::binding::local, section, offset});
  };
  attach(caller, "_peer", "need");
  attach(forged_member, "_runtime_helper", "have");
  automatic.archive_link->archives[0] = runtime_archive(forged_member);
  const auto denied = s.get<pl::Link>(s.program(std::move(automatic)));
  check(denied.image.empty() && denied.errors == std::vector<std::string>{
        "peer.o: verified effect bound '_runtime_helper' has no matching guarantee in runtime.a(runtime.o)"},
        "the selected runtime member resolves the name and supplies no guarantee");
}

}  // namespace

int main() {
  incremental();
  one_module_edit();
  moved_warning();
  two_units();
  headers();
  editor_view();
  generated_view();
  pooled();
  pooled_units();
  records();
  virtual_declarations();
  bit_fields();
  unnamed_union_members();
  too_deep();
  depth_limit_once();
  implicit_runtime_policies();
  return ycxx::unit::result("unit.pipeline_incremental");
}
