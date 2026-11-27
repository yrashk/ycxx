// The AArch64 back end (#28, the α subset): encodings against the Arm ARM, the pattern table,
// selection, determinism, and, given `--run <dir>` on arm64 macOS, programs generated from L1,
// linked by ycxx's linker and run. Given `--cc <compiler>`, a C harness compiled by that compiler
// (a black box, DECISIONS §2.3) is linked with generated functions and checks them against C's
// own arithmetic, calling convention included.
import std;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.lower;
import ycxx.obj.macho;
import ycxx.link;
import ycxx.target.aarch64;
import ycxx.unit;
import ycxx.unit.codegen;

using ycxx::unit::check;
namespace ir = ycxx::ir;
namespace l2 = ycxx::l2;
namespace a64 = ycxx::aarch64;
namespace obj = ycxx::obj;
namespace link = ycxx::link;

namespace {

// --- Encodings ---------------------------------------------------------------------------------

// Instruction words from the Arm ARM's encoding diagrams (DDI 0487, C6.2), worked by hand.
void test_encodings() {
  using a64::Form;
  struct Case {
    a64::Encodable in;
    std::uint32_t want;
    std::string_view text;
  };
  constexpr std::int64_t sp = 31, zr = 31, fp = 29, lr = 30;
  const Case cases[] = {
      {{Form::RET, true, 3, {}}, 0xD65F03C0, "ret"},
      {{Form::STPpre, true, 3, {fp, lr, sp, -16}}, 0xA9BF7BFD, "stp x29, x30, [sp, #-16]!"},
      {{Form::LDPpost, true, 3, {fp, lr, sp, 16}}, 0xA8C17BFD, "ldp x29, x30, [sp], #16"},
      {{Form::ADDri, true, 3, {fp, sp, 0}}, 0x910003FD, "mov x29, sp"},
      {{Form::ADDri, true, 3, {0, 0, 1}}, 0x91000400, "add x0, x0, #1"},
      {{Form::SUBri, true, 3, {sp, sp, 16}}, 0xD10043FF, "sub sp, sp, #16"},
      {{Form::SUBri12, true, 3, {sp, sp, 1}}, 0xD14007FF, "sub sp, sp, #1, lsl #12"},
      {{Form::SUBSri, false, 3, {zr, 0, 5}}, 0x7100141F, "cmp w0, #5"},
      {{Form::MOVZ, false, 3, {0, 42, 0}}, 0x52800540, "mov w0, #42"},
      {{Form::MOVN, false, 3, {0, 0, 0}}, 0x12800000, "mov w0, #-1"},
      {{Form::MOVK, true, 3, {0, 0x1234, 1}}, 0xF2A24680, "movk x0, #0x1234, lsl #16"},
      {{Form::ORRrr, true, 3, {0, zr, 1}}, 0xAA0103E0, "mov x0, x1"},
      {{Form::ADDrr, false, 3, {0, 0, 1}}, 0x0B010000, "add w0, w0, w1"},
      {{Form::SUBrr, true, 3, {0, zr, 1}}, 0xCB0103E0, "neg x0, x1"},
      {{Form::EORrr, true, 3, {0, 1, 2}}, 0xCA020020, "eor x0, x1, x2"},
      {{Form::ORNrr, false, 3, {0, zr, 1}}, 0x2A2103E0, "mvn w0, w1"},
      {{Form::MADD, false, 3, {0, 1, 2, zr}}, 0x1B027C20, "mul w0, w1, w2"},
      {{Form::MSUB, false, 3, {0, 1, 2, 3}}, 0x1B028C20, "msub w0, w1, w2, w3"},
      {{Form::SDIV, false, 3, {0, 0, 1}}, 0x1AC10C00, "sdiv w0, w0, w1"},
      {{Form::UDIV, true, 3, {0, 1, 2}}, 0x9AC20820, "udiv x0, x1, x2"},
      {{Form::LSLV, false, 3, {0, 1, 2}}, 0x1AC22020, "lsl w0, w1, w2"},
      {{Form::CSINC, false, 3, {0, zr, zr, 10}}, 0x1A9FA7E0, "cset w0, lt"},
      {{Form::CSEL, false, 3, {0, 1, 2, 1}}, 0x1A821020, "csel w0, w1, w2, ne"},
      {{Form::TST1, false, 3, {0}}, 0x7200001F, "tst w0, #1"},
      {{Form::SBFM, false, 3, {0, 1, 0, 7}}, 0x13001C20, "sxtb w0, w1"},
      {{Form::SBFM, true, 3, {0, 1, 0, 31}}, 0x93407C20, "sxtw x0, w1"},
      {{Form::UBFM, false, 3, {0, 1, 0, 7}}, 0x53001C20, "uxtb w0, w1"},
      {{Form::LDRui, true, 3, {9, sp, 8}}, 0xF94007E9, "ldr x9, [sp, #8]"},
      {{Form::STRui, true, 2, {0, 1, 0}}, 0xB9000020, "str w0, [x1]"},
      {{Form::LDRui, true, 0, {0, 1, 3}}, 0x39400C20, "ldrb w0, [x1, #3]"},
      {{Form::LDRui, true, 1, {0, 1, 6}}, 0x79400C20, "ldrh w0, [x1, #6]"},
      {{Form::ADRP, true, 3, {0}}, 0x90000000, "adrp x0, 0"},
      {{Form::B, true, 3, {8}}, 0x14000002, "b .+8"},
      {{Form::B, true, 3, {-4}}, 0x17FFFFFF, "b .-4"},
      {{Form::BL, true, 3, {}}, 0x94000000, "bl 0"},
      {{Form::Bcc, true, 3, {1, 8}}, 0x54000041, "b.ne .+8"},
      {{Form::TBNZ, true, 3, {0, 0, 8}}, 0x37000040, "tbnz w0, #0, .+8"},
      {{Form::BLR, true, 3, {9}}, 0xD63F0120, "blr x9"},
      {{Form::BRK, true, 3, {1}}, 0xD4200020, "brk #1"},
      // Floating point (C7.2): `wide` is double precision, or a conversion's integer width.
      {{Form::FADD, true, 3, {0, 1, 2}}, 0x1E622820, "fadd d0, d1, d2"},
      {{Form::FSUB, false, 3, {0, 1, 2}}, 0x1E223820, "fsub s0, s1, s2"},
      {{Form::FMUL, true, 3, {3, 4, 5}}, 0x1E650883, "fmul d3, d4, d5"},
      {{Form::FDIV, false, 3, {3, 4, 5}}, 0x1E251883, "fdiv s3, s4, s5"},
      {{Form::FNEG, true, 3, {0, 1}}, 0x1E614020, "fneg d0, d1"},
      {{Form::FABS, true, 3, {0, 1}}, 0x1E60C020, "fabs d0, d1"},
      {{Form::FMOVrr, false, 3, {0, 1}}, 0x1E204020, "fmov s0, s1"},
      {{Form::FCMP, true, 3, {1, 2}}, 0x1E622020, "fcmp d1, d2"},
      {{Form::FCSEL, false, 3, {0, 1, 2, 4}}, 0x1E224C20, "fcsel s0, s1, s2, mi"},
      {{Form::FCVTsd, true, 3, {0, 1}}, 0x1E22C020, "fcvt d0, s1"},
      {{Form::FCVTds, true, 3, {0, 1}}, 0x1E624020, "fcvt s0, d1"},
      {{Form::FCVTZSd, false, 3, {0, 1}}, 0x1E780020, "fcvtzs w0, d1"},
      {{Form::FCVTZSs, true, 3, {0, 1}}, 0x9E380020, "fcvtzs x0, s1"},
      {{Form::FCVTZUs, false, 3, {0, 1}}, 0x1E390020, "fcvtzu w0, s1"},
      {{Form::FCVTZUd, true, 3, {0, 1}}, 0x9E790020, "fcvtzu x0, d1"},
      {{Form::SCVTFd, false, 3, {0, 1}}, 0x1E620020, "scvtf d0, w1"},
      {{Form::SCVTFs, true, 3, {0, 1}}, 0x9E220020, "scvtf s0, x1"},
      {{Form::UCVTFs, false, 3, {0, 1}}, 0x1E230020, "ucvtf s0, w1"},
      {{Form::UCVTFd, true, 3, {0, 1}}, 0x9E630020, "ucvtf d0, x1"},
      {{Form::FMOVtoF, true, 3, {0, 16}}, 0x9E670200, "fmov d0, x16"},
      {{Form::FMOVtoF, false, 3, {0, 16}}, 0x1E270200, "fmov s0, w16"},
      {{Form::FMOVtoI, true, 3, {0, 1}}, 0x9E660020, "fmov x0, d1"},
      {{Form::FMOVtoI, false, 3, {0, 1}}, 0x1E260020, "fmov w0, s1"},
      {{Form::LDRuiF, true, 3, {16, sp, 8}}, 0xFD4007F0, "ldr d16, [sp, #8]"},
      {{Form::STRuiF, true, 2, {16, 1, 4}}, 0xBD000430, "str s16, [x1, #4]"},
  };
  for (const Case& c : cases) {
    auto w = a64::encode(c.in);
    check(w && *w == c.want, std::format("{} encodes as {:08x} (got {})", c.text, c.want,
                                         w ? std::format("{:08x}", *w) : w.error()));
  }
  check(!a64::encode({Form::LDRui, true, 3, {0, 1, 4}}), "an offset that is not a multiple of the access size is refused");
  check(!a64::encode({Form::Bcc, true, 3, {0, 1 << 20}}), "a conditional branch out of range is refused");
  check(!a64::encode({Form::MOVi, true, 3, {0, 1}}), "a pseudo form does not encode");
}

// --- The pattern table -------------------------------------------------------------------------

void test_patterns() {
  const a64::Table& t = a64::patterns();
  check(t.patterns.size() > 50, "the pattern table parses");
  std::set<std::string_view> names;
  for (const auto& p : t.patterns) check(names.insert(p.name).second, std::format("pattern {} is named once", p.name));
  auto err = [](std::string_view text, std::string_view want) {
    auto r = a64::parse_patterns(text);
    check(!r && r.error().message.contains(want),
          std::format("`{}` fails with `{}`, got `{}`", text, want, r ? "success" : r.error().message));
  };
  err("pattern x : (frob ?a) => RET", "unknown L2 op");
  err("pattern x : (add ?a) => RET", "wrong number of operands");
  err("pattern x : (add ?a ?b) => FROB ?d", "unknown form");
  err("pattern x : (add ?a ?b) => ADDrr ?d ?a ?c", "unbound variable");
  err("pattern x : (add ?a ?b) => ADDrr ?d ?a ?b where frob(?a)", "unknown condition");
  err("pattern x : ?a => RET", "must be an op");
}

// --- Building units from L1 text ---------------------------------------------------------------

struct Program {
  std::string l1;
  std::vector<a64::Symbol> symbols;
  std::vector<a64::DataDef> data;
};

a64::DataDef cstring(std::uint32_t entity, std::string_view s) {
  a64::DataDef d{ir::EntityId(entity), a64::DataKind::cstring, {s.begin(), s.end()}, 0, 1, {}};
  d.bytes.push_back(0);
  return d;
}

std::expected<a64::Unit, std::string> unit_of(const Program& p, std::string name) {
  auto fs = ir::parse(p.l1);
  if (!fs) return std::unexpected(std::format("L1 line {}: {}", fs.error().line, fs.error().message));
  a64::Unit u;
  u.name = std::move(name);
  for (const auto& f : *fs) {
    if (auto v = ir::verify(f.body); !v) return std::unexpected(std::format("L1 @{}: {}", ir::raw(f.name), v.error()));
    auto l = ir::lower(f.body, f.name, ycxx::unit::TestLayout{});
    if (!l) return std::unexpected(std::format("lower @{} node {}: {}", ir::raw(f.name), ir::raw(l.error().node), l.error().message));
    if (auto v = l2::verify(*l); !v) return std::unexpected(std::format("L2 @{}: {}", ir::raw(f.name), v.error()));
    u.functions.push_back({f.name, std::move(*l)});
  }
  u.data = p.data;
  u.symbols = p.symbols;
  return u;
}

std::expected<obj::object, std::string> compile(const Program& p, std::string name) {
  auto u = unit_of(p, name);
  if (!u) return std::unexpected(u.error());
  auto o = a64::codegen(*u);
  if (!o) return std::unexpected(std::format("codegen @{}: {}", ir::raw(o.error().entity), o.error().message));
  return std::move(*o);
}

std::expected<a64::MFunction, std::string> selected(std::string_view l1, std::vector<ir::EntityId> defined = {}) {
  auto fs = ir::parse(l1);
  if (!fs) return std::unexpected(fs.error().message);
  auto l = ir::lower((*fs)[0].body, (*fs)[0].name, ycxx::unit::TestLayout{});
  if (!l) return std::unexpected(l.error().message);
  return a64::select(*l, defined);
}

// --- Selection ---------------------------------------------------------------------------------

void test_selection() {
  // Maximal munch: a multiply feeding an add is one MADD; a constant within imm12 folds into
  // ADD; a compare feeding the branch folds into CMP and B.cond; a local's load reads its frame
  // object directly.
  auto m = selected(R"(
func @1 {
^0(%s0: !state, %a: T1, %b: T1, %c: T1):
  %s1: !state, %p: T7 = alloc %s0 [T1, automatic, indeterminate]
  %m: T1 = mul %a, %b
  %x: T1 = add %m, %c
  %k: T1 = const [#12]
  %y: T1 = add %x, %k
  %s2: !state = store %s1, %p, %y [T1, /]
  %v: T1 = load %s2, %p [T1, /]
  %z: T1 = const [#0]
  %t: T2 = icmp %v, %z [sgt]
  cond_br %t ^1, ^2
^1:
  ret %s2, %v
^2:
  ret %s2, %z
}
)");
  check(m.has_value(), "the function selects: " + (m ? std::string() : m.error()));
  if (!m) return;
  std::string text = a64::print(*m);
  for (std::string_view want : {"MADD.w", "ADDri.w", "STRfi.4", "LDRfi.4", "SUBSri.w", "MOVi.w"})
    check(text.contains(want), std::format("selection uses {}:\n{}", want, text));
  check(!text.contains("CSET"), "the compare is fused into the branch:\n" + text);
  check(text.contains("Bcc gt") || text.contains("Bcc le"), "the branch tests the compare's condition:\n" + text);
  check(m->objects.size() == 1 && m->objects[0].size == 4, "the local is one 4-byte frame object");

  // A load is not moved past a store to its user.
  auto n = selected(R"(
func @1 {
^0(%s0: !state, %p: T7, %q: T7):
  %v: T1 = load %s0, %p [T1, /]
  %one: T1 = const [#1]
  %s1: !state = store %s0, %q, %one [T1, /]
  %w: T1 = add %v, %one
  ret %s1, %w
}
)");
  check(n.has_value(), "the second function selects");
  if (n) {
    std::string t = a64::print(*n);
    check(t.find("LDRui") < t.find("STRui"), "the load stays before the store:\n" + t);
  }

  // A memory barrier prevents folding a preceding load into the return. Both barriers
  // survive as separate L2 state changes and emit no instructions.
  auto with_barriers = selected(R"(
func @1 {
^0(%s0: !state, %p: T7):
  %v: T1 = load %s0, %p [T1, /]
  %s1: !state = compiler_memory_barrier %s0
  %s2: !state = compiler_memory_barrier %s1
  ret %s2, %v
}
)");
  auto without_barriers = selected(R"(
func @1 {
^0(%s0: !state, %p: T7):
  %v: T1 = load %s0, %p [T1, /]
  ret %s0, %v
}
)");
  check(with_barriers && without_barriers, "both memory barrier controls select");
  if (with_barriers && without_barriers) {
    const auto& with = with_barriers->blocks.front().insts;
    const auto& without = without_barriers->blocks.front().insts;
    const auto load_with = std::ranges::find(with, a64::Form::LDRui, &a64::MInst::form);
    const auto load_without = std::ranges::find(without, a64::Form::LDRui, &a64::MInst::form);
    check(load_with != with.end() && load_with->origin == 0, "the load stays before the memory barriers");
    check(load_without != without.end() && load_without->origin == 1, "the control load folds into the return");
    check(!with.empty() && with.back().origin == 3, "both barriers survive in L2 before the return");
    check(std::ranges::none_of(with, [](const a64::MInst& i) { return i.origin == 1 || i.origin == 2; }),
          "the memory barriers emit no instruction");
  }
}

// --- Determinism -------------------------------------------------------------------------------

Program hello() {
  return {R"(
func @6 {
^0(%s: !state):
  %g: T9 = const [#2130706688]
  %e: T6 = ptr_member %g [T5, /e0]
  %s1: !state, %r: T1 = call %s, %e [@101, 0, 0, 0]
  %z: T1 = const [#0]
  ret %s1, %z
}
)",
          {{ir::EntityId(6), "_main"}, {ir::EntityId(100), "l_.str.100", obj::binding::local}, {ir::EntityId(101), "_puts"}},
          {cstring(100, "hello, world")}};
}

void test_determinism() {
  auto a = compile(hello(), "hello.o"), b = compile(hello(), "hello.o");
  check(a && b && *a == *b, "the same unit gives the same object");
  if (!a) {
    check(false, a.error());
    return;
  }
  check(a->sections.size() == 2 && a->sections[0].name == "__text" && a->sections[1].name == "__cstring",
        "text, then cstring");
  check(a->symbols.size() == 3 && a->symbols[0].name_view() == "_main" && a->symbols[1].name_view() == "l_.str.100" &&
            a->symbols[2].name_view() == "_puts" && !a->symbols[2].defined(),
        "defined symbols in entity order, then references in order of use");
  auto bytes = obj::macho::write_object(*a);
  auto back = obj::macho::read_object(bytes, "hello.o");
  check(back.has_value(), "the object round-trips through the Mach-O writer and reader");
}

// --- Running (arm64 macOS) ---------------------------------------------------------------------

int shell(const std::string& cmd) {
  const int status = std::system(cmd.c_str());
  return status == -1 ? -1 : (status >> 8) & 0xff;
}

std::string slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

link::options with_exports(std::initializer_list<std::string_view> names, std::string id) {
  link::options opts;
  std::set<std::string, std::less<>> ex;
  for (auto n : names) ex.emplace(n);
  opts.dylibs = {link::libsystem(std::move(ex))};
  opts.identifier = std::move(id);
  return opts;
}

void run(const std::filesystem::path& dir, const std::string& name, std::vector<obj::object> objects, int exit_code,
         std::string_view output) {
  auto image = link::link(objects, with_exports({"_puts", "_putchar", "_printf"}, name));
  if (!image) {
    for (const auto& e : image.error()) check(false, std::format("{}: link: {}", name, e.message));
    return;
  }
  const auto exe = dir / name;
  {
    std::ofstream f(exe, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
  }
  std::filesystem::permissions(exe, std::filesystem::perms::owner_all);
  const auto out = dir / (name + ".out");
  int rc = shell(std::format("'{}' > '{}'", exe.native(), out.native()));
  check(rc == exit_code, std::format("{}: exits with {} (got {})", name, exit_code, rc));
  std::string got = slurp(out);
  check(got == output, std::format("{}: prints {:?} (got {:?})", name, output, got));
}

void run_program(const std::filesystem::path& dir, const std::string& name, const Program& p, int exit_code,
                 std::string_view output = "") {
  auto o = compile(p, name + ".o");
  if (!o) {
    check(false, std::format("{}: {}", name, o.error()));
    return;
  }
  run(dir, name, {std::move(*o)}, exit_code, output);
}

void test_programs(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  // α0.
  run_program(dir, "ret42", {"func @1 {\n^0(%s: !state):\n  %c: T1 = const [#42]\n  ret %s, %c\n}\n", {{ir::EntityId(1), "_main"}}, {}}, 42);

  // Recursion.
  run_program(dir, "fact", {R"(
func @1 {
^0(%s: !state):
  %five: T1 = const [#5]
  %s1: !state, %r: T1 = call %s, %five [@2, 0, 0, 0]
  ret %s1, %r
}
func @2 {
^0(%s: !state, %n: T1):
  %one: T1 = const [#1]
  %c: T2 = icmp %n, %one [sle]
  cond_br %c ^1, ^2
^1:
  ret %s, %one
^2:
  %m: T1 = sub nsw %n, %one
  %s3: !state, %r: T1 = call %s, %m [@2, 0, 0, 0]
  %p: T1 = mul nsw %n, %r
  ret %s3, %p
}
)",
                            {{ir::EntityId(1), "_main"}, {ir::EntityId(2), "_fact"}}, {}},
              120);

  // Locals in memory and a loop: int s = 0; for (int i = 0; i < 10; ++i) s += i; return s;
  run_program(dir, "loop", {R"(
func @1 {
^0(%s0: !state):
  %s1: !state, %i: T7 = alloc %s0 [T1, automatic, indeterminate]
  %s2: !state, %sum: T7 = alloc %s1 [T1, automatic, erroneous]
  %z: T1 = const [#0]
  %s3: !state = store %s2, %i, %z [T1, /]
  br ^1(%s3)
^1(%a: !state):
  %iv: T1 = load %a, %i [T1, /]
  %ten: T1 = const [#10]
  %c: T2 = icmp %iv, %ten [slt]
  cond_br %c ^2, ^3
^2:
  %sv: T1 = load %a, %sum [T1, /]
  %ns: T1 = add nsw %sv, %iv
  %b1: !state = store %a, %sum, %ns [T1, /]
  %one: T1 = const [#1]
  %ni: T1 = add nsw %iv, %one
  %b2: !state = store %b1, %i, %ni [T1, /]
  br ^1(%b2)
^3:
  %r: T1 = load %a, %sum [T1, /]
  ret %a, %r
}
)",
                            {{ir::EntityId(1), "_main"}}, {}},
              45);

  // Block arguments that swap: fib(10) with (a, b) = (b, a + b).
  run_program(dir, "fib", {R"(
func @1 {
^0(%s: !state):
  %z: T1 = const [#0]
  %o: T1 = const [#1]
  %n: T1 = const [#10]
  br ^1(%z, %o, %n)
^1(%a: T1, %b: T1, %k: T1):
  %c: T2 = icmp %k, %z [sgt]
  cond_br %c ^2, ^3
^2:
  %ab: T1 = add %a, %b
  %k2: T1 = sub %k, %o
  br ^1(%b, %ab, %k2)
^3:
  ret %s, %a
}
)",
                           {{ir::EntityId(1), "_main"}}, {}},
              55);

  // α2: hello world through libSystem.
  run_program(dir, "hello", hello(), 0, "hello, world\n");

  // Namespace-scope variables: an int updated in place, and a pointer to a string literal.
  a64::DataDef counter{ir::EntityId(200), a64::DataKind::data, {40, 0, 0, 0}, 0, 4, {}};
  a64::DataDef msg{ir::EntityId(201), a64::DataKind::data_const, std::vector<std::uint8_t>(8), 0, 8, {{0, ir::EntityId(202), 0}}};
  run_program(dir, "globals", {R"(
func @1 {
^0(%s: !state):
  %g: T7 = global [@200]
  %v: T1 = load %s, %g [T1, /]
  %two: T1 = const [#2]
  %n: T1 = add nsw %v, %two
  %s1: !state = store %s, %g, %n [T1, /]
  %m: T15 = global [@201]
  %p: T6 = load %s1, %m [T6, /]
  %s2: !state, %x: T1 = call %s1, %p [@101, 0, 0, 0]
  %r: T1 = load %s2, %g [T1, /]
  ret %s2, %r
}
)",
                               {{ir::EntityId(1), "_main"},
                                {ir::EntityId(101), "_puts"},
                                {ir::EntityId(200), "_counter"},
                                {ir::EntityId(201), "_msg"},
                                {ir::EntityId(202), "l_.str.202", obj::binding::local}},
                               {counter, msg, cstring(202, "from a global")}},
              42, "from a global\n");

  // putchar of each character of a string, through a pointer walk and a char load.
  run_program(dir, "walk", {R"(
func @1 {
^0(%s: !state):
  %g: T9 = global [@100]
  %p0: T6 = ptr_member %g [T6, /e0]
  br ^1(%s, %p0)
^1(%a: !state, %p: T6):
  %ch: T5 = load %a, %p [T5, /]
  %z: T5 = const [#0]
  %more: T2 = icmp %ch, %z [ne]
  cond_br %more ^2, ^3
^2:
  %w: T1 = iconv %ch
  %a1: !state, %r: T1 = call %a, %w [@102, 0, 0, 0]
  %one: T1 = const [#1]
  %q: T6 = ptr_add %p, %one [T5]
  br ^1(%a1, %q)
^3:
  %nl: T1 = const [#10]
  %a2: !state, %r2: T1 = call %a, %nl [@102, 0, 0, 0]
  %zero: T1 = const [#0]
  ret %a2, %zero
}
)",
                            {{ir::EntityId(1), "_main"}, {ir::EntityId(100), "l_.str", obj::binding::local}, {ir::EntityId(102), "_putchar"}},
                            {cstring(100, "walked")}},
              0, "walked\n");
}

// --- The C harness: generated functions checked against C ----------------------------------------

struct Gen {
  std::string l1, decls, checks;
  std::vector<a64::Symbol> symbols;
  std::uint32_t next = 1000;

  std::uint32_t def(std::string name) {
    symbols.push_back({ir::EntityId(next), "_t_" + name});
    return next++;
  }
};

// A binary op `op` over L1 type `t` (C type `c`), returning `rt` (C `rc`), checked against the C
// expression `want` over every pair from `as` and `bs`.
void binary(Gen& g, std::string name, std::string_view t, std::string_view c, std::string_view op,
            std::string_view want, std::string_view as, std::string_view bs, std::string_view rt = {},
            std::string_view rc = {}) {
  if (rt.empty()) rt = t;
  if (rc.empty()) rc = c;
  std::uint32_t id = g.def(name);
  g.l1 += std::format("func @{} {{\n^0(%s: !state, %a: {}, %b: {}):\n  %r: {} = {}\n  ret %s, %r\n}}\n", id, t, t, rt, op);
  g.decls += std::format("{} t_{}({} a, {} b);\n", rc, name, c, c);
  g.checks += std::format(
      "  {{ static const {0} A[] = {{{1}}}; static const {0} B[] = {{{2}}};\n"
      "    for (unsigned i = 0; i < sizeof A / sizeof A[0]; ++i) for (unsigned j = 0; j < sizeof B / sizeof B[0]; ++j) {{\n"
      "      {0} a = A[i], b = B[j]; {3} want = ({3})({4}); {3} got = t_{5}(a, b);\n"
      "      if (got != want) {{ printf(\"{5}(%lld, %lld) = %lld, want %lld\\n\", (long long)a, (long long)b, (long long)got, (long long)want); ++fails; }}\n"
      "    }} }}\n",
      c, as, bs, rc, want, name);
}

// A function written in L1 by hand, checked by a C statement.
void custom(Gen& g, std::string name, std::string_view body, std::string_view decl, std::string_view check_stmt) {
  std::uint32_t id = g.def(name);
  g.l1 += std::format("func @{} {{\n{}}}\n", id, body);
  g.decls += std::format("{};\n", decl);
  g.checks += std::format("  if (!({})) {{ printf(\"{} failed\\n\"); ++fails; }}\n", check_stmt, name);
}

Gen harness() {
  Gen g;
  constexpr std::string_view I = "0, 1, -1, 7, -13, 1000, 123456789, -2147483647 - 1, 2147483647";
  constexpr std::string_view Idiv = "0, 1, -1, 7, -13, 1000, 123456789, 2147483647";
  constexpr std::string_view Ib = "1, -1, 3, -7, 1000, 65536";
  constexpr std::string_view Sh32 = "0, 1, 5, 31";
  constexpr std::string_view U = "0u, 1u, 7u, 1000u, 123456789u, 2147483648u, 4294967295u";
  constexpr std::string_view Ub = "1u, 3u, 1000u, 65536u, 4294967295u";
  constexpr std::string_view L = "0L, 1L, -1L, 7L, -13L, 4294967296L, -123456789012L, 9223372036854775807L";
  constexpr std::string_view Lb = "1L, -1L, 3L, -7L, 4294967296L";
  constexpr std::string_view Sh64 = "0L, 1L, 31L, 32L, 63L";
  constexpr std::string_view UL = "0UL, 1UL, 7UL, 4294967296UL, 18446744073709551615UL";
  constexpr std::string_view ULb = "1UL, 3UL, 4294967296UL, 18446744073709551615UL";
  constexpr std::string_view C = "0, 1, -1, 5, -7, 100, -128, 127";
  constexpr std::string_view Cb = "1, -1, 3, -7, 100";
  constexpr std::string_view Sh8 = "0, 1, 3, 7";
  constexpr std::string_view UC = "0, 1, 5, 100, 128, 255";
  constexpr std::string_view UCb = "1, 3, 100, 255";
  constexpr std::string_view S = "0, 1, -1, 300, -300, 32767, -32768";
  constexpr std::string_view Sb = "1, -1, 7, -300";

  // int
  binary(g, "add_i", "T1", "int", "add %a, %b", "(int)((unsigned)a + (unsigned)b)", I, I);
  binary(g, "sub_i", "T1", "int", "sub %a, %b", "(int)((unsigned)a - (unsigned)b)", I, I);
  binary(g, "mul_i", "T1", "int", "mul %a, %b", "(int)((unsigned)a * (unsigned)b)", I, I);
  binary(g, "sdiv_i", "T1", "int", "sdiv %a, %b", "a / b", Idiv, Ib);
  binary(g, "srem_i", "T1", "int", "srem %a, %b", "a % b", Idiv, Ib);
  binary(g, "shl_i", "T1", "int", "shl %a, %b", "(int)((unsigned)a << b)", I, Sh32);
  binary(g, "ashr_i", "T1", "int", "ashr %a, %b", "a >> b", I, Sh32);
  binary(g, "and_i", "T1", "int", "and %a, %b", "a & b", I, I);
  binary(g, "or_i", "T1", "int", "or %a, %b", "a | b", I, I);
  binary(g, "xor_i", "T1", "int", "xor %a, %b", "a ^ b", I, I);
  for (auto [p, cop] : {std::pair{"eq", "=="}, {"ne", "!="}, {"slt", "<"}, {"sle", "<="}, {"sgt", ">"}, {"sge", ">="}})
    binary(g, std::format("{}_i", p), "T1", "int", std::format("icmp %a, %b [{}]", p), std::format("a {} b", cop), I, I,
           "T2", "_Bool");
  // unsigned
  binary(g, "udiv_u", "T4", "unsigned", "udiv %a, %b", "a / b", U, Ub);
  binary(g, "urem_u", "T4", "unsigned", "urem %a, %b", "a % b", U, Ub);
  binary(g, "lshr_u", "T4", "unsigned", "lshr %a, %b", "a >> b", U, "0u, 1u, 5u, 31u");
  for (auto [p, cop] : {std::pair{"ult", "<"}, {"ule", "<="}, {"ugt", ">"}, {"uge", ">="}})
    binary(g, std::format("{}_u", p), "T4", "unsigned", std::format("icmp %a, %b [{}]", p), std::format("a {} b", cop), U, U,
           "T2", "_Bool");
  // long
  binary(g, "add_l", "T3", "long", "add %a, %b", "(long)((unsigned long)a + (unsigned long)b)", L, L);
  binary(g, "mul_l", "T3", "long", "mul %a, %b", "(long)((unsigned long)a * (unsigned long)b)", L, L);
  binary(g, "sdiv_l", "T3", "long", "sdiv %a, %b", "a / b", L, Lb);
  binary(g, "srem_l", "T3", "long", "srem %a, %b", "a % b", L, Lb);
  binary(g, "shl_l", "T3", "long", "shl %a, %b", "(long)((unsigned long)a << b)", L, Sh64);
  binary(g, "ashr_l", "T3", "long", "ashr %a, %b", "a >> b", L, Sh64);
  binary(g, "slt_l", "T3", "long", "icmp %a, %b [slt]", "a < b", L, L, "T2", "_Bool");
  // unsigned long
  binary(g, "udiv_ul", "T14", "unsigned long", "udiv %a, %b", "a / b", UL, ULb);
  binary(g, "urem_ul", "T14", "unsigned long", "urem %a, %b", "a % b", UL, ULb);
  binary(g, "lshr_ul", "T14", "unsigned long", "lshr %a, %b", "a >> b", UL, "0UL, 1UL, 32UL, 63UL");
  binary(g, "ugt_ul", "T14", "unsigned long", "icmp %a, %b [ugt]", "a > b", UL, UL, "T2", "_Bool");
  // char and unsigned char: narrow operands where the upper bits matter
  binary(g, "add_c", "T5", "signed char", "add %a, %b", "(signed char)(a + b)", C, C);
  binary(g, "sdiv_c", "T5", "signed char", "sdiv %a, %b", "(signed char)(a / b)", C, Cb);
  binary(g, "srem_c", "T5", "signed char", "srem %a, %b", "(signed char)(a % b)", C, Cb);
  binary(g, "ashr_c", "T5", "signed char", "ashr %a, %b", "(signed char)(a >> b)", C, Sh8);
  binary(g, "slt_c", "T5", "signed char", "icmp %a, %b [slt]", "a < b", C, C, "T2", "_Bool");
  binary(g, "eq_c", "T5", "signed char", "icmp %a, %b [eq]", "a == b", C, C, "T2", "_Bool");
  binary(g, "udiv_uc", "T10", "unsigned char", "udiv %a, %b", "(unsigned char)(a / b)", UC, UCb);
  binary(g, "urem_uc", "T10", "unsigned char", "urem %a, %b", "(unsigned char)(a % b)", UC, UCb);
  binary(g, "lshr_uc", "T10", "unsigned char", "lshr %a, %b", "(unsigned char)(a >> b)", UC, Sh8);
  binary(g, "ult_uc", "T10", "unsigned char", "icmp %a, %b [ult]", "a < b", UC, UC, "T2", "_Bool");
  binary(g, "mul_s", "T13", "short", "mul %a, %b", "(short)(a * b)", S, S);
  binary(g, "sdiv_s", "T13", "short", "sdiv %a, %b", "(short)(a / b)", S, Sb);
  binary(g, "sge_s", "T13", "short", "icmp %a, %b [sge]", "a >= b", S, S, "T2", "_Bool");

  // Unary ops, constants and immediates.
  custom(g, "neg", "^0(%s: !state, %a: T1):\n  %r: T1 = neg %a\n  ret %s, %r\n", "int t_neg(int)",
         "t_neg(5) == -5 && t_neg(-2147483647) == 2147483647");
  custom(g, "not", "^0(%s: !state, %a: T1):\n  %m: T1 = const [#4294967295]\n  %r: T1 = xor %a, %m\n  ret %s, %r\n",
         "int t_not(int)", "t_not(0) == -1 && t_not(12345) == ~12345");
  custom(g, "addk", "^0(%s: !state, %a: T1):\n  %k: T1 = const [#4095]\n  %r: T1 = add %a, %k\n  ret %s, %r\n",
         "int t_addk(int)", "t_addk(1) == 4096");
  custom(g, "addn", "^0(%s: !state, %a: T1):\n  %k: T1 = const [#4294967289]\n  %r: T1 = add %a, %k\n  ret %s, %r\n",
         "int t_addn(int)", "t_addn(10) == 3");
  custom(g, "subk", "^0(%s: !state, %a: T1):\n  %k: T1 = const [#100]\n  %r: T1 = sub %a, %k\n  ret %s, %r\n",
         "int t_subk(int)", "t_subk(1) == -99");
  custom(g, "addbig", "^0(%s: !state, %a: T1):\n  %k: T1 = const [#70000]\n  %r: T1 = add %a, %k\n  ret %s, %r\n",
         "int t_addbig(int)", "t_addbig(1) == 70001");
  custom(g, "cmpk", "^0(%s: !state, %a: T1):\n  %k: T1 = const [#10]\n  %r: T2 = icmp %a, %k [slt]\n  ret %s, %r\n",
         "_Bool t_cmpk(int)", "t_cmpk(9) && !t_cmpk(10) && t_cmpk(-5)");
  custom(g, "bigk", "^0(%s: !state):\n  %k: T3 = const [#2130706432]\n  ret %s, %k\n", "long t_bigk(void)",
         "t_bigk() == 0x123456789abcdef0L");
  custom(g, "negk", "^0(%s: !state):\n  %k: T3 = const [#2130706433]\n  ret %s, %k\n", "long t_negk(void)",
         "t_negk() == -4294967297L");
  custom(g, "sel", "^0(%s: !state, %c: T2, %a: T1, %b: T1):\n  %r: T1 = select %c, %a, %b\n  ret %s, %r\n",
         "int t_sel(_Bool, int, int)", "t_sel(1, 3, 4) == 3 && t_sel(0, 3, 4) == 4");
  custom(g, "selc",
         "^0(%s: !state, %x: T1, %y: T1, %a: T1, %b: T1):\n  %c: T2 = icmp %x, %y [ult]\n  %r: T1 = select %c, %a, %b\n"
         "  ret %s, %r\n",
         "int t_selc(int, int, int, int)", "t_selc(1, -1, 3, 4) == 3 && t_selc(-1, 1, 3, 4) == 4");

  // Conversions.
  custom(g, "sext_cl", "^0(%s: !state, %a: T5):\n  %r: T3 = iconv %a\n  ret %s, %r\n", "long t_sext_cl(signed char)",
         "t_sext_cl(-3) == -3L && t_sext_cl(100) == 100L");
  custom(g, "zext_ucl", "^0(%s: !state, %a: T10):\n  %r: T3 = iconv %a\n  ret %s, %r\n", "long t_zext_ucl(unsigned char)",
         "t_zext_ucl(200) == 200L");
  custom(g, "trunc_lc", "^0(%s: !state, %a: T3):\n  %r: T5 = iconv %a\n  ret %s, %r\n", "signed char t_trunc_lc(long)",
         "t_trunc_lc(0x1ff) == -1 && t_trunc_lc(0x17f) == 127");
  custom(g, "zext_ul", "^0(%s: !state, %a: T4):\n  %r: T14 = iconv %a\n  ret %s, %r\n",
         "unsigned long t_zext_ul(unsigned)", "t_zext_ul(4294967295u) == 4294967295UL");
  custom(g, "sext_il", "^0(%s: !state, %a: T1):\n  %r: T14 = iconv %a\n  ret %s, %r\n", "unsigned long t_sext_il(int)",
         "t_sext_il(-1) == 18446744073709551615UL");
  custom(g, "tobool_l", "^0(%s: !state, %a: T3):\n  %r: T2 = tobool %a\n  ret %s, %r\n", "_Bool t_tobool_l(long)",
         "t_tobool_l(4294967296L) && !t_tobool_l(0)");
  custom(g, "bool_i", "^0(%s: !state, %a: T2):\n  %r: T1 = iconv %a\n  ret %s, %r\n", "int t_bool_i(_Bool)",
         "t_bool_i(1) == 1 && t_bool_i(0) == 0");
  custom(g, "not_b",
         "^0(%s: !state, %a: T2):\n  %t: T2 = const [#1]\n  %r: T2 = xor %a, %t\n  ret %s, %r\n",
         "_Bool t_not_b(_Bool)", "t_not_b(0) == 1 && t_not_b(1) == 0");

  // Many arguments: the stack, with Apple's natural packing, both ways.
  auto weighted = [](std::string_view t, int n, std::string_view first) {
    std::string b = std::format("^0(%s: !state{}):\n", [&] {
      std::string ps;
      for (int i = 0; i < n; ++i) ps += std::format(", %a{}: {}", i, i == 0 ? first : t);
      return ps;
    }());
    b += std::format("  %r0: {} = iconv %a0\n", t);
    for (int i = 1; i < n; ++i) {
      b += std::format("  %k{0}: {1} = const [#{2}]\n  %m{0}: {1} = mul %a{0}, %k{0}\n  %r{0}: {1} = add %r{3}, %m{0}\n", i, t,
                       i + 1, i - 1);
    }
    b += std::format("  ret %s, %r{}\n", n - 1);
    return b;
  };
  custom(g, "sum10", weighted("T3", 10, "T3"), "long t_sum10(long, long, long, long, long, long, long, long, long, long)",
         "t_sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 385L");
  custom(g, "isum11", weighted("T1", 11, "T1"), "int t_isum11(int, int, int, int, int, int, int, int, int, int, int)",
         "t_isum11(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -11) == 264");
  custom(g, "mix",
         "^0(%s: !state, %a0: T1, %a1: T1, %a2: T1, %a3: T1, %a4: T1, %a5: T1, %a6: T1, %a7: T1, %c: T5, %h: T13, "
         "%u: T10, %l: T3):\n"
         "  %cw: T3 = iconv %c\n  %hw: T3 = iconv %h\n  %uw: T3 = iconv %u\n  %k: T3 = const [#1000]\n"
         "  %x: T3 = mul %cw, %k\n  %y: T3 = add %x, %hw\n  %z: T3 = mul %y, %k\n  %w: T3 = add %z, %uw\n"
         "  %r: T3 = add %w, %l\n  ret %s, %r\n",
         "long t_mix(int, int, int, int, int, int, int, int, signed char, short, unsigned char, long)",
         "t_mix(0, 0, 0, 0, 0, 0, 0, 0, -3, -200, 250, 7) == -3L * 1000000 - 200L * 1000 + 250 + 7");
  // Calls out with stack arguments, narrow ones among them.
  {
    std::string body = "^0(%s: !state):\n";
    std::string args;
    for (int i = 0; i < 10; ++i) {
      body += std::format("  %v{}: T3 = const [#{}]\n", i, i + 1);
      args += std::format(", %v{}", i);
    }
    body += std::format("  %s1: !state, %r: T3 = call %s{} [@900, 0, 0, 0]\n  ret %s1, %r\n", args);
    custom(g, "call10", body, "long t_call10(void)", "t_call10() == 385L");
  }
  custom(g, "callmix",
         "^0(%s: !state):\n  %z: T1 = const [#0]\n  %c: T5 = const [#4294967293]\n  %h: T13 = const [#4294967096]\n"
         "  %u: T10 = const [#250]\n  %b: T2 = const [#1]\n"
         "  %s1: !state, %r: T3 = call %s, %c, %z, %z, %z, %z, %z, %z, %z, %c, %h, %u, %b [@901, 0, 0, 0]\n  ret %s1, %r\n",
         "long t_callmix(void)", "t_callmix() == 2 * -3L * 1000000 - 200L * 1000 + 250 + 1");
  // A variadic callee (Apple: what the ellipsis matches goes on the stack, 8-byte slots, after the
  // named stack arguments: here the named `signed char` and `int` take bytes 0 and 4).
  custom(g, "callvar",
         "^0(%s: !state):\n  %z: T1 = const [#0]\n  %c: T5 = const [#4294967293]\n  %n: T1 = const [#4]\n"
         "  %m: T1 = const [#4294967295]\n  %l: T3 = const [#2]\n  %i: T1 = const [#3]\n  %p: T3 = const [#4]\n"
         "  %s1: !state, %r: T3 = call %s, %z, %z, %z, %z, %z, %z, %z, %z, %c, %n, %m, %l, %i, %p [@903, 4, 0, 0]\n"
         "  ret %s1, %r\n",
         "long t_callvar(void)", "t_callvar() == (((-3L * 10 - 1) * 10 + 2) * 10 + 3) * 10 + 4");
  custom(g, "callvar_regs",
         "^0(%s: !state):\n  %n: T1 = const [#3]\n  %m: T1 = const [#4294967295]\n  %l: T3 = const [#2]\n"
         "  %i: T1 = const [#7]\n"
         "  %s1: !state, %r: T3 = call %s, %n, %m, %l, %i [@904, 3, 0, 0]\n  ret %s1, %r\n",
         "long t_callvar_regs(void)", "t_callvar_regs() == ((-1L * 10) + 2) * 10 + 7");
  // Through a function pointer.
  custom(g, "apply",
         "^0(%s: !state, %f: T18, %x: T1):\n  %s1: !state, %r: T1 = call_ind %s, %f, %x [0, 0, 0]\n  ret %s1, %r\n",
         "int t_apply(int (*)(int), int)", "t_apply(c_twice, 21) == 42");
  // The address of a function defined elsewhere comes through the GOT.
  custom(g, "fptr", "^0(%s: !state):\n  %f: T18 = func_addr [@902]\n  ret %s, %f\n", "int (*t_fptr(void))(int)",
         "t_fptr() == c_twice && t_fptr()(4) == 8");
  g.symbols.push_back({ir::EntityId(902), "_c_twice"});
  g.symbols.push_back({ir::EntityId(900), "_c_sum10"});
  g.symbols.push_back({ir::EntityId(901), "_c_mix"});
  g.symbols.push_back({ir::EntityId(903), "_c_var"});
  g.symbols.push_back({ir::EntityId(904), "_c_var_regs"});

  // Variables defined here and read by C, and a pointer to one returned.
  g.symbols.push_back({ir::EntityId(950), "_g_counter"});
  custom(g, "bump",
         "^0(%s: !state):\n  %g: T7 = global [@950]\n  %v: T1 = load %s, %g [T1, /]\n  %one: T1 = const [#1]\n"
         "  %n: T1 = add %v, %one\n  %s1: !state = store %s, %g, %n [T1, /]\n  ret %s1, %n\n",
         "int t_bump(void)", "t_bump() == 41 && t_bump() == 42 && g_counter == 42");
  custom(g, "addr", "^0(%s: !state):\n  %g: T7 = global [@950]\n  ret %s, %g\n", "int* t_addr(void)",
         "t_addr() == &g_counter");
  // A frame larger than a page (probed on the way down), and offsets past a scaled imm12.
  custom(g, "far",
         "^0(%s0: !state, %c: T5):\n  %s1: !state, %p: T7 = alloc %s0 [T8, automatic, zero]\n"
         "  %s2: !state, %big: T21 = alloc %s1 [T20, automatic, indeterminate]\n"
         "  %s3: !state = store %s2, %big, %c [T5, /e39999]\n  %v: T5 = load %s3, %big [T5, /e39999]\n"
         "  %w: T5 = load %s3, %p [T5, /e15]\n  %r: T5 = add %v, %w\n  ret %s3, %r\n",
         "signed char t_far(signed char)", "t_far(77) == 77 && t_far(-3) == -3");
  return g;
}

constexpr std::string_view harness_main = R"(
int printf(const char*, ...);
long c_sum10(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9) {
  return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8 + 10 * a9;
}
long c_mix(signed char a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, signed char c, short h,
           unsigned char u, _Bool b) {
  return (long)a0 * 1000000 + c * 1000000L + h * 1000L + u + b + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
int c_twice(int x) { return 2 * x; }
// Reads `n` variadic arguments: ints at even positions, longs at odd ones.
static long c_var_list(long s, int n, __builtin_va_list ap) {
  for (int i = 0; i < n; ++i) s = s * 10 + (i % 2 ? __builtin_va_arg(ap, long) : __builtin_va_arg(ap, int));
  return s;
}
long c_var(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, signed char a8, int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  long s = c_var_list(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8, n, ap);
  __builtin_va_end(ap);
  return s;
}
long c_var_regs(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  long s = c_var_list(0, n, ap);
  __builtin_va_end(ap);
  return s;
}
)";

void test_harness(const std::filesystem::path& dir, std::string_view cc) {
  std::filesystem::create_directories(dir);
  Gen g = harness();
  std::string c = "extern int g_counter;\n";
  c += harness_main;
  c += g.decls;
  c += "int main(void) {\n  int fails = 0;\n";
  c += g.checks;
  c += "  if (fails == 0) printf(\"ok\\n\");\n  return fails ? 1 : 0;\n}\n";
  const auto src = dir / "harness.c", o = dir / "harness.o";
  std::ofstream(src) << c;
  if (shell(std::format("'{}' -x c -O1 -c '{}' -o '{}'", cc, src.native(), o.native())) != 0) {
    check(false, "the host compiler compiles harness.c");
    return;
  }
  const auto bytes = slurp(o);
  auto c_obj = obj::macho::read_object(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), "harness.o");
  if (!c_obj) {
    check(false, "harness.o reads: " + c_obj.error());
    return;
  }
  a64::DataDef counter{ir::EntityId(950), a64::DataKind::data, {40, 0, 0, 0}, 0, 4, {}};
  auto mine = compile({g.l1, g.symbols, {counter}}, "generated.o");
  if (!mine) {
    check(false, "the generated functions compile: " + mine.error());
    return;
  }
  run(dir, "harness", {std::move(*c_obj), std::move(*mine)}, 0, "ok\n");
}

// One object defines an entity once (#157): a second definition would leave references on one of the
// two. Entities that share a spelling are different definitions.
void test_definitions() {
  const std::vector<std::uint8_t> ret{0xC0, 0x03, 0x5F, 0xD6};
  const auto function = [&](std::uint32_t e) { return a64::CompiledFunction{ir::EntityId(e), {ret, {}, {}, {}, {}}}; };
  const auto variable = [](std::uint32_t e) {
    return a64::DataDef{ir::EntityId(e), a64::DataKind::data, {1, 0, 0, 0}, 0, 4, {}};
  };
  const auto assemble = [](std::vector<a64::CompiledFunction> functions, std::vector<a64::DataDef> data,
                           std::vector<a64::Symbol> symbols) {
    return a64::assemble("definitions.o", functions, data, symbols);
  };
  const std::vector<a64::Symbol> names{{ir::EntityId(1), "_a"}, {ir::EntityId(2), "_b"}};
  check(assemble({function(1), function(2)}, {}, names).has_value(), "two functions assemble");
  const auto twice = assemble({function(1), function(2), function(1)}, {}, names);
  check(!twice && twice.error().entity == ir::EntityId(1) && twice.error().message.contains("defined twice"),
        "a function defined twice is an error");
  check(!assemble({}, {variable(2), variable(1), variable(2)}, names), "a variable defined twice is an error");
  check(!assemble({function(1)}, {variable(1)}, names), "a function and a variable of one entity are an error");
  // An asm label gives two entities one spelling (#694); local symbols may share a name.
  const auto labelled = assemble({function(1), function(2)}, {}, {{ir::EntityId(1), "_same"}, {ir::EntityId(2), "_same"}});
  check(labelled.has_value() && std::ranges::count(labelled->symbols, std::string("_same"), &obj::symbol::name) == 2,
        "two entities with one spelling assemble");
  const auto local = assemble({function(1), function(2)}, {},
                              {{ir::EntityId(1), "_l", obj::binding::local}, {ir::EntityId(2), "_l", obj::binding::local}});
  check(local.has_value() && local->symbols.size() == 2 && local->symbols[0].offset != local->symbols[1].offset,
        "two local symbols with one name assemble");
  // The rule both object formats apply.
  check(!a64::repeated_definition({ir::EntityId(3), ir::EntityId(1), ir::EntityId(2)}), "distinct definitions");
  const auto repeated = a64::repeated_definition({ir::EntityId(3), ir::EntityId(1), ir::EntityId(3)});
  check(repeated && repeated->entity == ir::EntityId(3), "a repeated definition is found whatever the order");
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> run_dir;
  std::optional<std::string> cc;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view a = argv[i];
    if (a == "--run") run_dir = argv[i + 1];
    else if (a == "--cc") cc = argv[i + 1];
  }
  test_encodings();
  test_patterns();
  test_selection();
  test_determinism();
  test_definitions();
  if (run_dir) {
    test_programs(*run_dir);
    if (cc) test_harness(*run_dir, *cc);
  }
  return ycxx::unit::result("unit.aarch64");
}
