// The pipeline's cells (#63): a second build of an unchanged unit executes nothing; an edit to one
// function's body re-executes that function's cells, `tu` and the link, and no other function's;
// and every incremental result equals a cold build of the same text (DECISIONS §4.2), even where
// the warm session's interners number entities differently from a cold one.
import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.pipeline;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace engine = ycxx::engine;
namespace pl = ycxx::pipeline;
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

}  // namespace

int main() {
  incremental();
  return ycxx::unit::result("unit.pipeline_incremental");
}
