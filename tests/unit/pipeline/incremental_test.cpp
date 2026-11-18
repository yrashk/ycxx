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

}  // namespace

int main() {
  incremental();
  two_units();
  headers();
  pooled();
  pooled_units();
  records();
  return ycxx::unit::result("unit.pipeline_incremental");
}
