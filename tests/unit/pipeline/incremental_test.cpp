// The pipeline's cells (#63): a second build of an unchanged unit executes nothing; an edit to one
// function's body re-executes that function's cells, `tu` and the link, and no other function's;
// and every incremental result equals a cold build of the same text (DECISIONS §4.2), even where
// the warm session's interners number entities differently from a cold one.
import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pipeline;
import ycxx.pp;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace engine = ycxx::engine;
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
          db.runs<pl::Codegen>(k), optimized};
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
  const std::uint32_t tu0 = s.db().runs<pl::Tu>(tu), link0 = s.db().runs<pl::Link>(program);
  build(s, v2);
  const std::vector<std::uint8_t> edited = s.get<pl::Link>(program).image;
  check(!edited.empty() && edited != image, "the edit changes the image");
  check(edited == cold(v2), "after a body edit, incremental equals cold");
  check(runs(s, twice) == twice0 && runs(s, main_) == main0, "the other functions' cells stay green");
  const Runs square1 = runs(s, square);
  check(square1.gen == square0.gen + 1 && square1.ir == square0.ir + 1 && square1.lower == square0.lower + 1 &&
            square1.unit == square0.unit + 1 && square1.codegen == square0.codegen + 1 && square1.optimized == 1,
        "the edited function's cells run once each, and its new unit is optimized once");
  check(s.db().runs<pl::Tu>(tu) == tu0 + 1 && s.db().runs<pl::Link>(program) == link0 + 1, "tu and the link run again");

  // A function inserted first, which this session numbers after every other entity.
  build(s, v3);
  check(s.get<pl::Link>(program).image == cold(v3), "after an insertion, incremental equals cold");
  // Its unit re-reads which entities the object defines, which changed, and comes out the same.
  Runs twice1 = runs(s, twice);
  check(twice1.unit == twice0.unit + 1, "a new definition re-checks the other functions' units");
  twice1.unit = twice0.unit;
  check(twice1 == twice0, "an insertion leaves the other functions' code green");
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
  check(!s.get<pl::Tu>(a).object, "a unit with front-end errors has no object");
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

// A session on a pool (#137): every worker is bound to the session's universe, and the program's
// bytes equal an inline session's, at 2 and 8 threads and under chaos seeds. One unit for now:
// two units' front ends would race in sema's records until #156.
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

}  // namespace

int main() {
  incremental();
  two_units();
  headers();
  editor_view();
  pooled();
  return ycxx::unit::result("unit.pipeline_incremental");
}
