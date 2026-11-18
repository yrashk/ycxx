// ycxx.obj, ycxx.obj.macho and ycxx.link (#42): SHA-256 vectors, the MH_OBJECT writer and reader,
// .tbd export lists, linker diagnostics and determinism everywhere; and, given `--run <dir>` on
// arm64 macOS, executables that are written, signed, checked with `codesign -v` and run.
//
// Programs are hand-encoded A64 instructions (Arm ARM encodings), so nothing here depends on a
// code generator. Given `--cc <compiler>`, a C file compiled by that compiler is also linked and
// run: its object is a black-box input (DECISIONS §2.3).
import std;
import ycxx.link;
import ycxx.link.sha256;
import ycxx.obj.macho;
import ycxx.unit;

using ycxx::unit::check;
namespace obj = ycxx::obj;
namespace link = ycxx::link;
using obj::reloc_kind;

namespace {

// --- SHA-256 (FIPS 180-4 examples) -------------------------------------------------------------

constexpr std::array<std::uint8_t, 3> abc = {'a', 'b', 'c'};
static_assert(link::sha256_of(abc) == link::sha256_digest{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                                          0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                                          0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad});
static_assert(link::sha256_of({}) == link::sha256_digest{0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                                         0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                                         0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55});

void test_sha256_long() {
  // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" spans two blocks.
  constexpr std::string_view m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const auto d = link::sha256_of(std::span(reinterpret_cast<const std::uint8_t*>(m.data()), m.size()));
  check(d == link::sha256_digest{0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
                                 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1},
        "SHA-256 of the two-block FIPS example");
  // A million 'a's, fed in uneven pieces.
  link::sha256 h;
  const std::vector<std::uint8_t> chunk(997, 'a');
  std::size_t left = 1'000'000;
  while (left > 0) {
    const std::size_t n = std::min(left, chunk.size());
    h.update(std::span(chunk).first(n));
    left -= n;
  }
  check(h.finish() == link::sha256_digest{0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
                                          0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0},
        "SHA-256 of a million 'a's, streamed");
}

// --- A64 encodings (Arm ARM) -------------------------------------------------------------------

constexpr std::uint32_t movz_w(unsigned rd, std::uint16_t imm) { return 0x52800000 | std::uint32_t{imm} << 5 | rd; }
constexpr std::uint32_t ret = 0xd65f03c0;
constexpr std::uint32_t bl = 0x94000000;
constexpr std::uint32_t adrp(unsigned rd) { return 0x90000000 | rd; }
constexpr std::uint32_t add_x_imm(unsigned rd, unsigned rn) { return 0x91000000 | rn << 5 | rd; }
constexpr std::uint32_t ldr_w(unsigned rt, unsigned rn) { return 0xb9400000 | rn << 5 | rt; }  // ldr wt, [xn, #off]
constexpr std::uint32_t ldr_x(unsigned rt, unsigned rn) { return 0xf9400000 | rn << 5 | rt; }  // ldr xt, [xn, #off]
constexpr std::uint32_t add_w(unsigned rd, unsigned rn, unsigned rm) { return 0x0b000000 | rm << 16 | rn << 5 | rd; }
constexpr std::uint32_t stp_fp_lr_pre = 0xa9bf7bfd;   // stp x29, x30, [sp, #-16]!
constexpr std::uint32_t mov_fp_sp = 0x910003fd;       // mov x29, sp
constexpr std::uint32_t ldp_fp_lr_post = 0xa8c17bfd;  // ldp x29, x30, [sp], #16

// int main() { return 42; }
obj::object return42() {
  obj::object o;
  o.name = "return42.o";
  auto text = o.add_section(obj::standard_section::text);
  o.define("_main", obj::binding::global, text);
  o.sections[text].emit32(movz_w(0, 42));
  o.sections[text].emit32(ret);
  return o;
}

// a.o: int main() { return f(40) ; }    b.o: int f(int x) { return x + 2; }  (f adds 2 via a data word)
std::vector<obj::object> two_objects() {
  obj::object a;
  a.name = "a.o";
  auto at = a.add_section(obj::standard_section::text);
  auto f = a.reference("_f");
  a.define("_main", obj::binding::global, at);
  auto& t = a.sections[at];
  t.emit32(stp_fp_lr_pre);
  t.emit32(mov_fp_sp);
  t.emit32(movz_w(0, 40));
  t.emit32(bl);
  t.relocate(reloc_kind::branch26, f);
  t.emit32(ldp_fp_lr_post);
  t.emit32(ret);

  obj::object b;
  b.name = "b.o";
  auto bt = b.add_section(obj::standard_section::text);
  auto bd = b.add_section(obj::standard_section::data);
  b.sections[bd].align(2);
  auto two = b.define("_two", obj::binding::hidden, bd);
  b.sections[bd].emit32(2);
  b.define("_f", obj::binding::global, bt);
  auto& u = b.sections[bt];
  u.emit32(adrp(8));
  u.relocate(reloc_kind::page21, two);
  u.emit32(ldr_w(9, 8));
  u.relocate(reloc_kind::pageoff12, two);
  u.emit32(add_w(0, 0, 9));
  u.emit32(ret);
  return {std::move(a), std::move(b)};
}

// Data and address relocations: main returns *table[1] + *(GOT load of _ten) + 2 = 30 + 10 + 2.
//   __DATA,__data:  _values = { 20, 30 }, _ten = 10
//   __DATA,__const: _table = { &_values[0], &_values[1] }   (abs64 with an addend)
//   __TEXT,__const: _delta = _ten - _values (abs32 difference), used to check subtraction
obj::object data_program() {
  obj::object o;
  o.name = "data.o";
  auto text = o.add_section(obj::standard_section::text);
  auto data = o.add_section(obj::standard_section::data);
  auto cdata = o.add_section(obj::standard_section::data_const);
  auto rodata = o.add_section(obj::standard_section::rodata);
  auto& d = o.sections[data];
  d.align(2);
  auto values = o.define("_values", obj::binding::global, data);
  d.emit32(20);
  d.emit32(30);
  auto ten = o.define("_ten", obj::binding::global, data);
  d.emit32(10);
  auto& c = o.sections[cdata];
  c.align(3);
  auto table = o.define("_table", obj::binding::local, cdata);
  c.emit64(0);
  c.relocate(reloc_kind::abs64, values);
  c.emit64(0);
  c.relocate(reloc_kind::abs64, values, 4);
  auto& r = o.sections[rodata];
  r.align(2);
  auto delta = o.define("_delta", obj::binding::local, rodata);
  r.emit32(0);
  r.relocate(reloc_kind::abs32, ten, 0, std::nullopt, values);

  o.define("_main", obj::binding::global, text);
  auto& t = o.sections[text];
  t.emit32(adrp(8));
  t.relocate(reloc_kind::page21, table);
  t.emit32(add_x_imm(8, 8));
  t.relocate(reloc_kind::pageoff12, table);
  t.emit32(ldr_x(8, 8) | 1 << 10);  // ldr x8, [x8, #8]
  t.emit32(ldr_w(0, 8));            // w0 = 30
  t.emit32(adrp(9));
  t.relocate(reloc_kind::got_page21, ten);
  t.emit32(ldr_x(9, 9));
  t.relocate(reloc_kind::got_pageoff12, ten);
  t.emit32(ldr_w(9, 9));  // w9 = 10
  t.emit32(add_w(0, 0, 9));
  t.emit32(adrp(10));
  t.relocate(reloc_kind::page21, delta);
  t.emit32(ldr_w(10, 10));
  t.relocate(reloc_kind::pageoff12, delta);
  t.emit32(movz_w(11, 4));
  t.emit32(0x4b0b014a);  // sub w10, w10, w11: _ten - _values - 4 == 4
  t.emit32(0x531f794a);  // lsl w10, w10, #1 (ubfm w10, w10, #31, #30): 8
  t.emit32(add_w(0, 0, 10));
  t.emit32(0x51001800);  // sub w0, w0, #6: 30 + 10 + 8 - 6 = 42
  t.emit32(ret);
  return o;
}

// puts("hello, world") from libSystem, the string in __TEXT,__cstring.
obj::object hello() {
  obj::object o;
  o.name = "hello.o";
  auto text = o.add_section(obj::standard_section::text);
  auto cstr = o.add_section(obj::standard_section::cstring);
  auto str = o.define("l_.str", obj::binding::local, cstr);
  o.sections[cstr].emit(std::string_view("hello, world\0", 13));
  auto puts = o.reference("_puts");
  o.define("_main", obj::binding::global, text);
  auto& t = o.sections[text];
  t.emit32(stp_fp_lr_pre);
  t.emit32(mov_fp_sp);
  t.emit32(adrp(0));
  t.relocate(reloc_kind::page21, str);
  t.emit32(add_x_imm(0, 0));
  t.relocate(reloc_kind::pageoff12, str);
  t.emit32(bl);
  t.relocate(reloc_kind::branch26, puts);
  t.emit32(movz_w(0, 0));
  t.emit32(ldp_fp_lr_post);
  t.emit32(ret);
  return o;
}

link::options with_exports(std::initializer_list<std::string_view> names) {
  link::options opts;
  std::set<std::string, std::less<>> ex;
  for (auto n : names) ex.emplace(n);
  opts.dylibs = {link::libsystem(std::move(ex))};
  return opts;
}

// --- The object writer and reader --------------------------------------------------------------

// An object's meaning with symbol indexes replaced by names: Mach-O groups the symbol table
// (locals, external definitions, undefined), so a round trip may permute the symbols.
std::string describe(const obj::object& o) {
  std::string out = o.name + "\n";
  for (const auto& s : o.sections) {
    out += std::format("section {},{} flags={:#x} align={} size={} bytes={}\n", s.segment, s.name, s.flags, s.align_log2, s.size(),
                       s.bytes);
    for (const auto& r : s.relocs) {
      out += std::format("  reloc {} {} {} {} {}\n", r.offset, std::to_underlying(r.kind), o.symbols[r.symbol].name, r.addend,
                         r.minus ? o.symbols[*r.minus].name : "-");
    }
  }
  std::vector<std::string> syms;
  for (const auto& s : o.symbols) {
    syms.push_back(std::format("symbol {} {} {} {} {}", s.name, std::to_underlying(s.bind), s.section.value_or(~0u), s.offset, s.weak));
  }
  std::ranges::sort(syms);
  for (const auto& s : syms) out += s + "\n";
  return out;
}

void test_roundtrip() {
  for (const auto& o : {return42(), two_objects()[0], two_objects()[1], data_program(), hello()}) {
    const auto bytes = obj::macho::write_object(o);
    auto back = obj::macho::read_object(bytes, o.name);
    check(back.has_value(), std::format("{} reads back", o.name));
    if (!back) {
      std::println(std::cerr, "  {}", back.error());
      continue;
    }
    check(describe(*back) == describe(o), std::format("{} reads back equal", o.name));
    check(obj::macho::write_object(*back) == bytes, std::format("{} rewrites byte-identically", o.name));
  }
  // A zero-fill section and a weak definition survive too.
  obj::object o;
  o.name = "bss.o";
  auto bss = o.add_section(obj::standard_section::bss);
  o.sections[bss].align(3);
  auto& sym = o.symbols[o.define("_buf", obj::binding::global, bss)];
  sym.weak = true;
  o.sections[bss].reserve_zerofill(64);
  o.reference("_ext");
  auto back = obj::macho::read_object(obj::macho::write_object(o), o.name);
  check(back && describe(*back) == describe(o), "zero-fill section and weak definition read back");
  check(!obj::macho::read_object(std::vector<std::uint8_t>(8, 0), "junk").has_value(), "junk is rejected");
}

// --- .tbd export lists -------------------------------------------------------------------------

void test_tbd() {
  constexpr std::string_view tbd = R"(--- !tapi-tbd
tbd-version:     4
targets:         [ x86_64-macos, arm64e-macos ]
install-name:    '/usr/lib/libSystem.B.dylib'
exports:
  - targets:         [ x86_64-macos ]
    symbols:         [ _only_x86 ]
  - targets:         [ x86_64-macos, arm64-macos,
                       arm64e-macos ]
    symbols:         [ _puts, 'R8289209$_close',
                       _putchar ]
    weak-symbols:    [ _weak_one ]
    objc-classes:    [ NSObject ]
undefineds:
  - targets:         [ arm64e-macos ]
    symbols:         [ _not_exported ]
--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e-macos ]
install-name:    '/usr/lib/system/libsystem_c.dylib'
reexports:
  - targets:         [ arm64e-macos ]
    symbols:         [ _printf ]
...
)";
  const auto ex = link::tbd_exports(tbd);
  const std::set<std::string, std::less<>> want = {"R8289209$_close", "_printf", "_putchar", "_puts", "_weak_one"};
  check(ex == want, "tbd exports for arm64 macOS");
}

// --- The linker --------------------------------------------------------------------------------

void test_diagnostics() {
  {
    auto o = return42();
    o.sections[0].emit32(bl);
    o.sections[0].relocate(reloc_kind::branch26, o.reference("_missing"));
    auto r = link::link(std::span(&o, 1), with_exports({"_puts"}));
    check(!r && r.error().size() == 1 && r.error()[0].message == "undefined symbol _missing, referenced from return42.o",
          "undefined symbol is diagnosed");
  }
  {
    std::vector<obj::object> objs = {return42(), return42()};
    objs[1].name = "again.o";
    auto r = link::link(objs);
    check(!r && r.error().size() == 1 && r.error()[0].message == "duplicate symbol _main in return42.o and again.o",
          "duplicate symbol is diagnosed");
  }
  {
    std::vector<obj::object> objs = {return42(), return42()};
    objs[1].symbols[0].weak = true;
    check(link::link(objs).has_value(), "a weak definition yields to a strong one");
  }
  {
    obj::object o;
    o.name = "nomain.o";
    o.add_section(obj::standard_section::text);
    auto r = link::link(std::span(&o, 1));
    check(!r && r.error()[0].message == "entry point _main is not defined", "missing entry point is diagnosed");
  }
  {
    // An absolute pointer in __TEXT would need a text relocation.
    auto o = return42();
    auto ro = o.add_section(obj::standard_section::rodata);
    o.sections[ro].emit64(0);
    o.sections[ro].relocate(reloc_kind::abs64, 0);
    auto r = link::link(std::span(&o, 1));
    check(!r && r.error()[0].message.contains("text relocation"), "pointer in __TEXT is diagnosed");
  }
}

struct program {
  std::string name;
  std::vector<obj::object> objects;
  link::options opts;
  int exit_code;
  std::string output;
};

std::vector<program> programs() {
  return {
      {"return42", {return42()}, {}, 42, ""},
      {"two_objects", two_objects(), {}, 42, ""},
      {"data", {data_program()}, {}, 42, ""},
      {"hello", {hello()}, with_exports({"_puts"}), 0, "hello, world\n"},
  };
}

void test_deterministic() {
  for (auto& p : programs()) {
    auto a = link::link(p.objects, p.opts);
    auto b = link::link(p.objects, p.opts);
    check(a.has_value(), std::format("{} links", p.name));
    if (!a) {
      for (const auto& e : a.error()) std::println(std::cerr, "  {}", e.message);
      continue;
    }
    check(b && *a == *b, std::format("{} links byte-identically twice", p.name));
    // Through the object writer and reader, the image is the same.
    std::vector<obj::object> reread;
    for (const auto& o : p.objects) reread.push_back(*obj::macho::read_object(obj::macho::write_object(o), o.name));
    auto c = link::link(reread, p.opts);
    check(c && *c == *a, std::format("{} links identically from written objects", p.name));
  }
}

// Looks `name` up in an executable's exports trie the way a loader walks it: at each node, follow
// the child whose edge is a prefix of the rest of the name.
std::optional<std::uint64_t> trie_lookup(std::span<const std::uint8_t> image, std::string_view name) {
  namespace mo = obj::macho;
  std::uint32_t off = 0, size = 0;
  for (std::size_t at = 32, c = 0; c < mo::load32(image, 16); ++c, at += mo::load32(image, at + 4)) {
    if (mo::load32(image, at) == mo::lc_dyld_exports_trie) {
      off = mo::load32(image, at + 8);
      size = mo::load32(image, at + 12);
    }
  }
  const auto trie = image.subspan(off, size);
  auto uleb = [&](std::size_t& p) {
    std::uint64_t v = 0;
    for (int shift = 0;; shift += 7) {
      const std::uint8_t b = trie[p++];
      v |= std::uint64_t{b & 0x7fu} << shift;
      if (!(b & 0x80)) return v;
    }
  };
  std::size_t node = 0;
  for (;;) {
    std::size_t p = node;
    const std::uint64_t terminal = uleb(p);
    if (name.empty()) {
      if (terminal == 0) return std::nullopt;
      uleb(p);  // flags
      return uleb(p);
    }
    p += terminal;
    const std::uint8_t children = trie[p++];
    std::optional<std::size_t> next;
    for (std::uint8_t c = 0; c < children; ++c) {
      std::string edge;
      while (trie[p] != 0) edge.push_back(static_cast<char>(trie[p++]));
      ++p;
      const std::uint64_t child = uleb(p);
      if (!next && name.starts_with(edge)) {
        name.remove_prefix(edge.size());
        next = child;
      }
    }
    if (!next) return std::nullopt;
    node = *next;
  }
}

void test_exports_trie() {
  // Names that are prefixes of each other, share prefixes, or stand alone.
  obj::object o = return42();
  const std::vector<std::string> names = {"_f", "_fa", "_fab", "_fb", "_g", "_main2", "_x_long_name_without_neighbours"};
  for (std::size_t k = 0; k < names.size(); ++k) {
    o.define(names[k], obj::binding::global, 0, 0);
  }
  o.define("_hidden", obj::binding::hidden, 0, 4);
  auto image = link::link(std::span(&o, 1));
  check(image.has_value(), "trie program links");
  if (!image) return;
  // Every name is at __text's start, as _main is; the header comes before it.
  const auto main_off = trie_lookup(*image, "_main");
  check(main_off.has_value() && *main_off > 0, "_main is exported");
  for (const auto& n : names) check(trie_lookup(*image, n) == main_off, std::format("{} is exported at _main's offset", n));
  check(trie_lookup(*image, "__mh_execute_header") == std::optional<std::uint64_t>{0}, "__mh_execute_header is exported at 0");
  check(!trie_lookup(*image, "_hidden"), "a hidden symbol is not exported");
  check(!trie_lookup(*image, "_fac"), "an absent name is not found");
  check(!trie_lookup(*image, "_ma"), "a prefix of an exported name is not found");
}

// --- Running (arm64 macOS only) ----------------------------------------------------------------

int shell(const std::string& cmd) {
  const int status = std::system(cmd.c_str());
  return status == -1 ? -1 : (status >> 8) & 0xff;  // the shell's exit status (POSIX wait status)
}

std::string slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void write_executable(const std::filesystem::path& p, std::span<const std::uint8_t> bytes) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.close();
  std::filesystem::permissions(p, std::filesystem::perms::owner_all);
}

void run_and_check(const std::filesystem::path& dir, const std::string& name, std::span<const std::uint8_t> image, int exit_code,
                   std::string_view output) {
  const auto exe = dir / name;
  const auto out = dir / (name + ".out");
  write_executable(exe, image);
  check(shell(std::format("codesign -v '{}'", exe.native_encoded_string())) == 0, std::format("{}: codesign -v accepts the signature", name));
  const int rc = shell(std::format("'{}' > '{}'", exe.native_encoded_string(), out.native_encoded_string()));
  check(rc == exit_code, std::format("{}: exits with {} (got {})", name, exit_code, rc));
  check(slurp(out) == output, std::format("{}: prints the expected output", name));
}

void test_run(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
  for (auto& p : programs()) {
    p.opts.identifier = p.name;
    auto image = link::link(p.objects, p.opts);
    if (image) run_and_check(dir, p.name, *image, p.exit_code, p.output);
  }
}

// A C program compiled by a black-box compiler, read back and linked with a hand-encoded object.
void test_mixed(const std::filesystem::path& dir, std::string_view cc) {
  const auto src = dir / "mixed.c";
  const auto o = dir / "mixed.o";
  std::ofstream(src) << "int puts(const char*);\n"
                        "int twice(int);\n"
                        "static int counter = 19;\n"
                        "const char* greeting = \"from C\";\n"
                        "int main(void) { puts(greeting); counter += 2; return twice(counter); }\n";
  if (shell(std::format("'{}' -x c -O1 -c '{}' -o '{}'", cc, src.native_encoded_string(), o.native_encoded_string())) != 0) {
    check(false, "the host compiler compiles mixed.c");
    return;
  }
  const auto bytes = slurp(o);
  auto c_obj = obj::macho::read_object(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), "mixed.o");
  check(c_obj.has_value(), "the host compiler's object reads");
  if (!c_obj) {
    std::println(std::cerr, "  {}", c_obj.error());
    return;
  }
  // twice(x) = x + x, hand-encoded.
  obj::object t;
  t.name = "twice.o";
  auto text = t.add_section(obj::standard_section::text);
  t.define("_twice", obj::binding::global, text);
  t.sections[text].emit32(add_w(0, 0, 0));
  t.sections[text].emit32(ret);
  std::vector<obj::object> objs = {std::move(*c_obj), std::move(t)};
  auto opts = with_exports({"_puts"});
  opts.identifier = "mixed";
  auto image = link::link(objs, opts);
  check(image.has_value(), "the mixed program links");
  if (!image) {
    for (const auto& e : image.error()) std::println(std::cerr, "  {}", e.message);
    return;
  }
  run_and_check(dir, "mixed", *image, 42, "from C\n");
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
  test_sha256_long();
  test_roundtrip();
  test_tbd();
  test_diagnostics();
  test_deterministic();
  test_exports_trie();
  if (run_dir) {
    test_run(*run_dir);
    if (cc) test_mixed(*run_dir, *cc);
  }
  return ycxx::unit::result("unit.link");
}
