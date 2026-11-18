// Unit tests of the pipeline (#43): the line table, linkage names, and the back half from
// analysed bodies built with the AST builder (as sema builds them) through function_ir, lower,
// codegen and ycxx's linker. On arm64 macOS the executables are run (`--run DIR`).
import std;
import ycxx.ast;
import ycxx.ir;
import ycxx.irgen;
import ycxx.pipeline;
import ycxx.unit;

namespace ast = ycxx::ast;
namespace pl = ycxx::pipeline;
using ast::Child;
using ast::ConstId;
using ast::EntityId;
using ast::NodeId;
using ast::QualType;
using ycxx::unit::check;

namespace {

// ------------------------------------------------------------------ the line table

void lines() {
  pl::LineTable t("ab\ncd\r\nef\rg");
  check(t.line_column(0) == pl::LineColumn{1, 1}, "the first byte");
  check(t.line_column(2) == pl::LineColumn{1, 3}, "the new-line ends its line");
  check(t.line_column(3) == pl::LineColumn{2, 1}, "after LF");
  check(t.line_column(7) == pl::LineColumn{3, 1}, "after CR LF");
  check(t.line_column(10) == pl::LineColumn{4, 1}, "after a lone CR");
  check(t.line_column(99) == pl::LineColumn{4, 2}, "past the end is the end");
  check(pl::LineTable("").line_column(0) == pl::LineColumn{1, 1}, "an empty file");
}

// ------------------------------------------------------------------ the universe

// String literals are stand-ins until the values interner holds them (#41): a string literal
// object is a numbered integer constant, and so is its address.
struct TestConstants final : pl::Constants {
  explicit TestConstants(ast::Interners& in) : in(&in) {}
  ast::Interners* in;
  std::map<ConstId, std::string> objects;
  std::map<ConstId, Address> addresses;
  std::int64_t next = 1'000'000;

  ConstId number() { return ast::int_value(*in, ast::aarch64_darwin, ast::Builtin::unsigned_long_long, next++); }
  ConstId string(std::string s) {
    ConstId c = number();
    objects[c] = s;
    return c;
  }
  std::optional<std::vector<std::uint8_t>> string_object(ConstId c) const override {
    auto it = objects.find(c);
    if (it == objects.end()) return std::nullopt;
    std::vector<std::uint8_t> units(it->second.begin(), it->second.end());
    units.push_back(0);
    return units;
  }
  std::optional<Address> address(ConstId c) const override {
    auto it = addresses.find(c);
    if (it == addresses.end()) return std::nullopt;
    return it->second;
  }
  ConstId address_of(ConstId object) override {
    for (auto& [c, a] : addresses)
      if (a.object == object && a.offset == 0) return c;
    ConstId c = number();
    addresses[c] = {object, 0};
    return c;
  }
};

struct World {
  ast::Interners in;
  TestConstants consts{in};
  pl::TranslationUnit tu;
  QualType void_t, bool_t, char_t, int_t, long_t, cchar_t, cchar_p;

  World() {
    using enum ast::Builtin;
    void_t = ast::builtin(in, void_);
    bool_t = ast::builtin(in, bool_);
    char_t = ast::builtin(in, char_);
    int_t = ast::builtin(in, int_);
    long_t = ast::builtin(in, long_);
    cchar_t = ast::builtin(in, char_, ast::Cv::const_);
    cchar_p = ast::pointer_to(in, cchar_t);
    tu.name = "test.o";
  }
  QualType fn(QualType ret, std::initializer_list<QualType> params, ast::LangLinkage lang = ast::LangLinkage::cxx) {
    return ast::function_type(in, ret, std::span(params.begin(), params.size()), {.linkage = lang});
  }
  QualType array(QualType elem, std::int64_t n) {
    return ast::array_of(in, ast::aarch64_darwin, elem, ast::int_value(in, ast::aarch64_darwin, ast::Builtin::unsigned_long, n));
  }
  EntityId entity(std::string name, QualType type, bool is_function, ast::LangLinkage lang = ast::LangLinkage::cxx) {
    EntityId e = in.entities.add<EntityId>(ast::seed("test entity " + name));
    tu.entities.push_back({e, name, type, is_function, lang, ast::Linkage::external, false, std::nullopt});
    if (name == "main") tu.main = e;
    return e;
  }
};

// A body under construction, as sema builds it: drafts of one frame builder.
struct Body {
  explicit Body(World& world) : w(world) {}
  World& w;
  ast::Builder b{w.in};
  std::vector<ast::LocalRec> locals;

  ast::ExprInfo pr(QualType t) { return {t, ast::ValueCategory::prvalue, 0}; }
  ast::ExprInfo lv(QualType t) { return {t, ast::ValueCategory::lvalue, 0}; }
  ast::SeqId<ast::Cleanup> no_cleanups() { return w.in.intern_seq(std::span<const ast::Cleanup>()); }
  template <class T>
  NodeId add(const T& v) {
    return b.add(v, 1);
  }
  NodeId lit(QualType t, std::int64_t v) { return add(ast::Literal{pr(t), ast::int_value(w.in, ast::aarch64_darwin, t, v)}); }
  NodeId str(std::string s) {
    return add(ast::Literal{lv(w.array(w.cchar_t, std::int64_t(s.size()) + 1)), w.consts.string(s)});
  }
  NodeId conv(ast::ConversionKind k, QualType to, NodeId from) {
    return add(ast::ImplicitConversion{pr(to), k, EntityId::none, {from, 0}});
  }
  NodeId read(NodeId lvalue, QualType t) { return conv(ast::ConversionKind::lvalue_to_rvalue, t, lvalue); }
  NodeId local(std::uint32_t i) { return add(ast::LocalRef{lv(locals[i].type), i}); }
  NodeId global(EntityId e, QualType t) { return add(ast::DeclRef{lv(t), e}); }
  NodeId bin(ast::BinaryOp op, QualType t, NodeId l, NodeId r) { return add(ast::Binary{pr(t), op, {l, 0}, {r, 0}}); }
  NodeId call(EntityId f, QualType fty, QualType ret, std::initializer_list<NodeId> args) {
    std::vector<Child> cs;
    for (NodeId a : args) cs.push_back({a, 0});
    NodeId callee = add(ast::DeclRef{lv(fty), f});
    return add(ast::Call{pr(ret), f, {callee, 0}, b.list(cs)});
  }
  std::uint32_t declare(std::string name, QualType t) {
    locals.push_back({w.in.names.intern(name), t, EntityId::none, NodeId::none, 0});
    return std::uint32_t(locals.size() - 1);
  }
  NodeId ret(NodeId v = NodeId::none) { return add(ast::Return{{v, 0}, no_cleanups()}); }
  NodeId expr(NodeId e) { return add(ast::ExprStmt{{e, 0}}); }
  NodeId if_(NodeId c, NodeId t) { return add(ast::If{{}, {c, 0}, {t, 0}, {NodeId::none, 0}}); }
  NodeId block(std::initializer_list<NodeId> stmts) {
    std::vector<Child> cs;
    for (NodeId s : stmts) cs.push_back({s, 0});
    return add(ast::Compound{b.list(cs), no_cleanups()});
  }
  // Adds the definition of `f` with this body; the parameters are the first locals.
  void define(EntityId f, QualType type, std::vector<std::pair<std::string, QualType>> params, NodeId tree) {
    std::vector<ast::ParamSema> ps;
    for (auto& [name, t] : params) ps.push_back({EntityId::none, t, w.in.names.intern(name), NodeId::none, 0});
    ast::DeclSema decl{f, type, ast::DeclKind::function, ast::Linkage::external, ast::Storage::none, ast::Access::none,
                       0, {}, NodeId::none, NodeId::none, w.in.intern_seq(std::span<const ast::ParamSema>(ps))};
    auto locs = b.seq(std::span<const ast::LocalRec>(locals));
    ast::BodySema body = b.commit(ast::BodySema{tree, locs});
    w.tu.functions.push_back({f, decl, body, ast::ItemId(1), 0});
  }
};

// ------------------------------------------------------------------ linkage names

void mangling() {
  World w;
  auto name = [&](std::string_view n, QualType t, ast::LangLinkage lang = ast::LangLinkage::cxx) {
    return pl::symbol_name(w.in, n, t, true, lang).value_or("<error>");
  };
  QualType cchar_pp = ast::pointer_to(w.in, w.cchar_p);
  QualType char_p = ast::pointer_to(w.in, w.char_t);
  check(name("main", w.fn(w.int_t, {})) == "_main", "main is not mangled");
  check(name("puts", w.fn(w.int_t, {w.cchar_p}, ast::LangLinkage::c), ast::LangLinkage::c) == "_puts",
        "C language linkage is not mangled");
  check(name("fib", w.fn(w.int_t, {w.int_t})) == "__Z3fibi", "one int parameter");
  check(name("gcd", w.fn(w.long_t, {w.long_t, w.long_t})) == "__Z3gcdll", "builtins are not substituted");
  check(name("f", w.fn(w.void_t, {})) == "__Z1fv", "no parameters is v");
  check(name("f", w.fn(w.void_t, {w.cchar_p, w.cchar_p})) == "__Z1fPKcS0_", "PKc is the second candidate");
  check(name("f", w.fn(w.void_t, {cchar_pp, w.cchar_p})) == "__Z1fPPKcS0_",
        "an inner pointer type is a candidate");
  check(name("f", w.fn(w.void_t, {char_p, w.cchar_p, char_p})) == "__Z1fPcPKcS_", "S_ is the first candidate");
  check(name("g", w.fn(w.bool_t, {ast::builtin(w.in, ast::Builtin::unsigned_int), ast::builtin(w.in, ast::Builtin::unsigned_long)})) ==
            "__Z1gjm",
        "unsigned int and unsigned long");
  check(pl::symbol_name(w.in, "count", w.int_t, false, ast::LangLinkage::cxx).value_or("") == "_count",
        "a global-namespace variable is its identifier");
  check(pl::substitution(36) == "SZ_" && pl::substitution(37) == "S10_", "seq-ids are base 36");
}

// ------------------------------------------------------------------ programs

std::optional<std::filesystem::path> run_dir;

// Compiles and links the unit; on arm64 macOS runs it and checks its exit status and output.
void program(World& w, std::string_view name, int status, std::string_view out = "",
             std::source_location at = std::source_location::current()) {
  auto o = pl::compile(w.tu, w.in, w.consts);
  if (!o) {
    for (auto& e : o.error()) check(false, std::format("{}: {}", name, e.message), at);
    return;
  }
  std::vector<ycxx::obj::object> objects{std::move(*o)};
  auto exe = pl::link_executable(objects, std::string(name));
  if (!exe) {
    for (auto& e : exe.error()) check(false, std::format("{}: link: {}", name, e.message), at);
    return;
  }
  if (!run_dir) return;
  std::filesystem::path path = *run_dir / name;
  if (auto err = pl::write_file(path, *exe, true)) {
    check(false, *err, at);
    return;
  }
  std::filesystem::path captured = *run_dir / (std::string(name) + ".out");
  int raw = std::system(std::format("'{}' > '{}'", path.native_encoded_string(), captured.native_encoded_string()).c_str());
  int got = (raw >> 8) & 0xff;
  check(got == status, std::format("{}: exit status {} (raw {}), expected {}", name, got, raw, status), at);
  std::ifstream in(captured);
  std::string text{std::istreambuf_iterator<char>(in), {}};
  check(text == out, std::format("{}: output {:?}, expected {:?}", name, text, out), at);
}

void alpha0() {
  // int main() { return 42; }
  World w;
  Body b{w};
  EntityId m = w.entity("main", w.fn(w.int_t, {}), true);
  b.define(m, w.fn(w.int_t, {}), {}, b.block({b.ret(b.lit(w.int_t, 42))}));
  program(w, "alpha0", 42);
}

void calls_and_globals() {
  // int count = 10; int twice(int n) { return n + n; } int main() { return twice(count) + 1; }
  World w;
  QualType fn_ii = w.fn(w.int_t, {w.int_t});
  EntityId count = w.entity("count", w.int_t, false);
  EntityId twice = w.entity("twice", fn_ii, true);
  EntityId m = w.entity("main", w.fn(w.int_t, {}), true);
  w.tu.variables.push_back({count, w.int_t, ast::int_value(w.in, ast::aarch64_darwin, w.int_t, 10), std::nullopt});
  {
    Body b{w};
    std::uint32_t n = b.declare("n", w.int_t);
    auto rd = [&] { return b.read(b.local(n), w.int_t); };
    b.define(twice, fn_ii, {{"n", w.int_t}}, b.block({b.ret(b.bin(ast::BinaryOp::add, w.int_t, rd(), rd()))}));
  }
  {
    Body b{w};
    NodeId c = b.call(twice, fn_ii, w.int_t, {b.read(b.global(count, w.int_t), w.int_t)});
    b.define(m, w.fn(w.int_t, {}), {}, b.block({b.ret(b.bin(ast::BinaryOp::add, w.int_t, c, b.lit(w.int_t, 1)))}));
  }
  program(w, "calls", 21);
}

void hello() {
  // extern "C" int puts(const char*); const char* msg = "bye";
  // int main() { puts("hello, world"); puts(msg); }
  World w;
  QualType fn_puts = w.fn(w.int_t, {w.cchar_p}, ast::LangLinkage::c);
  EntityId puts = w.entity("puts", fn_puts, true, ast::LangLinkage::c);
  EntityId msg = w.entity("msg", w.cchar_p, false);
  EntityId m = w.entity("main", w.fn(w.int_t, {}), true);
  w.tu.variables.push_back({msg, w.cchar_p, w.consts.address_of(w.consts.string("bye")), std::nullopt});
  Body b{w};
  NodeId lit = b.str("hello, world");
  NodeId decay = b.conv(ast::ConversionKind::array_to_pointer, w.cchar_p, lit);
  NodeId c1 = b.call(puts, fn_puts, w.int_t, {decay});
  NodeId c2 = b.call(puts, fn_puts, w.int_t, {b.read(b.global(msg, w.cchar_p), w.cchar_p)});
  b.define(m, w.fn(w.int_t, {}), {}, b.block({b.expr(c1), b.expr(c2)}));
  program(w, "hello", 0, "hello, world\nbye\n");
}

void unsupported_is_an_error() {
  // A variable of a type the α subset has no data for is an error, not a crash.
  World w;
  QualType arr = w.array(w.int_t, 4);
  EntityId v = w.entity("v", arr, false);
  ast::Loc at{ast::ItemId(3), 2};
  w.tu.variables.push_back({v, arr, ast::int_value(w.in, ast::aarch64_darwin, w.int_t, 1), at});
  auto o = pl::compile(w.tu, w.in, w.consts);
  check(!o && o.error().size() == 1 && o.error()[0].message.contains("not supported yet") && o.error()[0].at == at,
        "an array initialized with an integer is reported at its declarator");
}

void names_only_when_used() {
  // int apply(int (*)(int), int); int main() { return 0; }: a declaration nothing uses needs no
  // linkage name, so its unsupported parameter type is no error. Used, it is one, at its declarator.
  World w;
  QualType fp = ast::pointer_to(w.in, w.fn(w.int_t, {w.int_t}));
  QualType fn_apply = w.fn(w.int_t, {fp, w.int_t});
  EntityId apply = w.entity("apply", fn_apply, true);
  ast::Loc at{ast::ItemId(5), 1};
  w.tu.entities.back().at = at;
  EntityId m = w.entity("main", w.fn(w.int_t, {}), true);
  {
    Body b{w};
    b.define(m, w.fn(w.int_t, {}), {}, b.block({b.ret(b.lit(w.int_t, 0))}));
  }
  check(pl::compile(w.tu, w.in, w.consts).has_value(), "an unused declaration is not mangled");
  w.tu.functions.clear();
  {
    Body b{w};
    NodeId c = b.call(apply, fn_apply, w.int_t, {b.add(ast::Literal{b.pr(fp), w.in.intern_value(ast::VNullptr{})}), b.lit(w.int_t, 1)});
    b.define(m, w.fn(w.int_t, {}), {}, b.block({b.ret(c)}));
  }
  auto o = pl::compile(w.tu, w.in, w.consts);
  check(!o && o.error().size() == 1 && o.error()[0].at == at && o.error()[0].message.contains("the name of 'apply'"),
        "a used declaration's name is mangled, and the error is at its declarator");
}

void write_file_keeps_directories() {
  if (!run_dir) return;
  std::filesystem::path dir = *run_dir / "a-directory";
  std::filesystem::create_directories(dir);
  auto err = pl::write_file(dir, std::vector<std::uint8_t>{1, 2, 3}, true);
  check(err && err->contains("is a directory") && std::filesystem::is_directory(dir), "-o names a directory");
  std::filesystem::path file = *run_dir / "a-file";
  check(!pl::write_file(file, std::vector<std::uint8_t>{1}, false) && !pl::write_file(file, std::vector<std::uint8_t>{1, 2}, true) &&
            std::filesystem::file_size(file) == 2,
        "a regular file is replaced");
  check(!pl::write_file("/dev/null", std::vector<std::uint8_t>{1}, true) && std::filesystem::is_character_file("/dev/null"),
        "a device is written to, not replaced");
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--run") run_dir = std::filesystem::path(args[i + 1]);
  if (run_dir) std::filesystem::create_directories(*run_dir);
  lines();
  mangling();
  alpha0();
  calls_and_globals();
  hello();
  unsupported_is_an_error();
  names_only_when_used();
  write_file_keeps_directories();
  return ycxx::unit::result("unit.pipeline");
}
