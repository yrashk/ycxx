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
      out += std::format("  reloc {} {} {} {} {}\n", r.offset, std::to_underlying(r.kind), o.symbols[r.symbol].name_view(), r.addend,
                         r.minus ? o.symbols[*r.minus].name_view() : "-");
    }
  }
  std::vector<std::string> syms;
  for (const auto& s : o.symbols) {
    syms.push_back(std::format("symbol {} {} {} {} {}", s.name_view(), std::to_underlying(s.bind), s.section.value_or(~0u), s.offset, s.weak));
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

// DWARF Call Frame Information and the .eh_frame zR augmentation. The code address is a
// signed, PC-relative difference. A zero-length record optionally terminates the input.
void add_cleanup_frame(obj::object& o, std::uint32_t function, std::uint32_t code_size,
                       std::uint8_t encoding, std::initializer_list<std::uint8_t> instructions, bool terminated = false) {
  namespace mo = obj::macho;
  mo::byte_writer records;
  records.u32(16);
  records.u32(0);  // CIE ID
  records.raw(std::array<std::uint8_t, 12>{1, 'z', 'R', 0, 1, 0x78, 30, 1, encoding, 0x0c, 31, 0});
  const auto fde = records.size();
  records.u32(0);
  records.u32(static_cast<std::uint32_t>(fde + 4));  // CIE offset backwards from this word
  const auto pc = static_cast<std::uint32_t>(records.size());
  if (encoding == 0x1b) {
    records.u32(0);
    records.u32(code_size);
  } else {
    records.u64(0);
    records.u64(code_size);
  }
  records.u8(0);  // FDE augmentation length
  records.raw(instructions);
  records.align(4);
  mo::store32(records.bytes, fde, static_cast<std::uint32_t>(records.size() - fde - 4));
  if (terminated) records.u32(0);
  const auto frame = o.add_section(obj::standard_section::eh_frame);
  const auto anchor = o.define("l_frame", obj::binding::local, frame, 0);
  auto& section = o.sections[frame];
  section.bytes = std::move(records.bytes);
  section.relocate(encoding == 0x1b ? reloc_kind::abs32 : reloc_kind::abs64,
                   function, -static_cast<std::int64_t>(pc), pc, anchor);
}

obj::object cleanup_bridge_object() {
  using obj::binding;
  using obj::reloc_kind;
  using obj::standard_section;
  obj::object object;
  object.name = "cleanup_bridge.o";
  const auto code_section = object.add_section(standard_section::text);
  const auto frame_section = object.add_section(standard_section::eh_frame);
  const auto table_section = object.add_section(obj::section{"__TEXT", "__gcc_except_tab", 0, 2, {}, 0, {}});
  const auto bridge = object.define("_cleanup_bridge", binding::global, code_section);
  const auto frame_base = object.define("ltmp_frame", binding::local, frame_section);
  const auto lsda = object.define("ltmp_cleanup_lsda", binding::local, table_section);
  const auto callback = object.reference("_fixture_callback");
  const auto destroy = object.reference("_fixture_destroy");
  const auto resume = object.reference("__Unwind_Resume");
  const auto personality = object.reference("___gxx_personality_v0");

  auto& code = object.sections[code_section];
  auto call = [&](std::uint32_t callee) {
    code.emit32(0x94000000);  // bl target
    code.relocate(reloc_kind::branch26, callee);
  };
  code.emit32(0xa9be7bfd);  // stp x29, x30, [sp, #-32]!
  code.emit32(0x910003fd);  // add x29, sp, #0
  code.emit32(0xf9000be0);  // str x0, [sp, #16], save the owner pointer
  call(callback);          // offset 12, the protected call
  code.emit32(0xf9400be0);  // ldr x0, [sp, #16]
  call(destroy);
  code.emit32(0xa8c27bfd);  // ldp x29, x30, [sp], #32
  code.emit32(0xd65f03c0);  // ret at offset 28
  code.emit32(0xf9000fe0);  // landing pad at 32, str x0, [sp, #24], save the unwind header
  code.emit32(0xf9400be0);  // ldr x0, [sp, #16]
  call(destroy);
  code.emit32(0xf9400fe0);  // ldr x0, [sp, #24]
  call(resume);
  code.emit32(0xd4200020);  // brk #1, resume does not return

  constexpr std::uint8_t sites[]{0xff, 0xff, 0x01, 12, // omit LPStart and types, ULEB call sites
                                 0, 12, 0, 0,
                                 12, 4, 32, 0,       // one cleanup, with no catch action
                                 16, 40, 0, 0};
  object.sections[table_section].emit(sites);

  auto& frames = object.sections[frame_section];
  constexpr std::uint8_t cie[]{1, 'z', 'P', 'L', 'R', 0, 1, 0x78, 30, 7,
                               0x9b, 0, 0, 0, 0, 0x10, 0x10,  // GOT personality, pcrel LSDA and FDE
                               0x0c, 31, 0, 0x08, 29, 0x08, 30};
  frames.emit32(28);
  frames.emit32(0);
  frames.emit(cie);
  frames.relocate(reloc_kind::got_rel32, personality, 0, 19);
  frames.emit32(60);
  frames.emit32(36);  // the backward CIE reference at offset 36
  frames.emit64(0);
  frames.emit64(56);
  frames.emit(std::array<std::uint8_t, 1>{8});
  frames.emit64(0);
  constexpr std::uint8_t rows[]{0x44, 0x0c, 31, 32, 0x9d, 4, 0x9e, 3, // SP+32 after pair save
                                0x44, 0x0c, 29, 32, 0x9d, 4, 0x9e, 3, // FP+32 after FP set
                                0x54, 0x0c, 31, 0, 0xdd, 0xde,        // entry rules at ret
                                0x44, 0x0c, 29, 32, 0x9d, 4, 0x9e, 3}; // body rules at landing
  frames.emit(rows);
  frames.align(3);
  frames.relocate(reloc_kind::abs64, bridge, -40, 40, frame_base);
  frames.relocate(reloc_kind::abs64, lsda, -57, 57, frame_base);
  return object;
}

struct linked_section_view {
  std::uint64_t address;
  std::span<const std::uint8_t> bytes;
};

std::optional<linked_section_view> linked_section(std::span<const std::uint8_t> image,
                                                  std::string_view segment, std::string_view name) {
  namespace mo = obj::macho;
  for (std::size_t at = mo::mach_header_64_size, c = 0; c < mo::load32(image, 16);
       ++c, at += mo::load32(image, at + 4)) {
    if (mo::load32(image, at) != mo::lc_segment_64) continue;
    for (std::size_t i = 0; i < mo::load32(image, at + 64); ++i) {
      const auto section = at + mo::segment_command_64_size + i * mo::section_64_size;
      const std::string_view names(reinterpret_cast<const char*>(image.data() + section), 32);
      const auto field = [](std::string_view spelling) { return spelling.substr(0, spelling.find('\0')); };
      if (field(names.substr(0, 16)) == name && field(names.substr(16)) == segment)
        return linked_section_view{mo::load64(image, section + 32), image.subspan(mo::load32(image, section + 48),
                                                                           mo::load64(image, section + 40))};
    }
  }
  return std::nullopt;
}

void test_cleanup_frames() {
  namespace mo = obj::macho;
  auto first = return42();
  add_cleanup_frame(first, 0, 8, 0x1b, {0x44, 0x0e, 0, 0}, true);
  auto second = return42();
  second.name = "second.o";
  second.symbols[0].name = "_second";
  add_cleanup_frame(second, 0, 8, 0x10, {});
  second.sections.back().flags = 0x6800000b;  // S_COALESCED, NO_TOC, STRIP_STATIC_SYMS, LIVE_SUPPORT
  std::vector<obj::object> objects;
  for (const auto* input : {&first, &second}) {
    auto object = mo::read_object(mo::write_object(*input), input->name);
    check(object.has_value(), "a cleanup frame round-trips through the Mach-O object reader");
    if (!object) return;
    objects.push_back(std::move(*object));
  }
  auto image = link::link(objects);
  check(image.has_value(), "objects with cleanup frames link");
  if (!image) return;
  const auto frames = linked_section(*image, "__TEXT", "__eh_frame");
  check(frames.has_value(), "the final image retains a cleanup-free __eh_frame section");
  if (!frames) return;
  const auto bytes = frames->bytes;
  const auto second_cie = first.sections.back().bytes.size() - 4;
  check(bytes.size() == second_cie + second.sections.back().bytes.size() + 4,
        "input terminators and alignment gaps do not interrupt the linked frame stream");
  if (bytes.size() != second_cie + second.sections.back().bytes.size() + 4) return;
  constexpr std::uint64_t image_base = 0x100000000;
  constexpr std::size_t pc = 28;
  const auto first_pc = frames->address + pc + static_cast<std::int32_t>(mo::load32(bytes, pc));
  const auto second_pc = frames->address + second_cie + pc + mo::load64(bytes, second_cie + pc);
  check(trie_lookup(*image, "_main") == first_pc - image_base, "the 32-bit FDE points to the placed _main");
  check(trie_lookup(*image, "_second") == second_pc - image_base, "the 64-bit FDE points to the second object's code");
  check(mo::load32(bytes, 24) == 24 && mo::load32(bytes, second_cie + 24) == 24,
        "relative CIE pointers survive the merge");
  check(mo::load32(bytes, bytes.size() - 4) == 0, "one terminator ends the complete stream");

  auto mismatched = first;
  mismatched.sections.back().relocs[0].addend += 4;
  const auto missing = link::link(std::span(&mismatched, 1));
  check(missing && !linked_section(*missing, "__TEXT", "__eh_frame"), "a relocation which does not denote the PC field stays outside the cleanup-frame profile");

  auto debug = second;
  debug.sections[0].flags |= obj::section_flags::debug;
  debug.symbols[0].bind = obj::binding::local;
  const std::array ignored{std::move(debug), return42()};
  const auto debug_link = link::link(ignored);
  check(debug_link && !linked_section(*debug_link, "__TEXT", "__eh_frame"), "a frame referring to discarded debug text is also discarded");
}

// The cleanup profile and the original zR profile share one terminated stream.
void test_hosted_cleanup_metadata() {
  namespace mo = obj::macho;
  auto entry = return42();
  add_cleanup_frame(entry, 0, 8, 0x1b, {}, true);
  entry.define("___gxx_personality_v0", obj::binding::global, 0);
  entry.sections[0].emit32(ret);
  const auto padding = entry.add_section({"__TEXT", "__gcc_except_tab", 0, 2, {}, 0, {}});
  entry.sections[padding].emit32(0);
  auto bridge = cleanup_bridge_object();
  const auto prefix = entry.sections[1].bytes.size() - 4;
  const auto& table = bridge.sections[2].bytes;
  const auto opts = with_exports({"_fixture_callback", "_fixture_destroy", "__Unwind_Resume"});
  const std::array objects{entry, bridge};
  const auto image = link::link(objects, opts);
  check(image.has_value(), "cleanup-only and cleanup-free objects link together");
  if (!image) return;
  const auto frames = linked_section(*image, "__TEXT", "__eh_frame");
  const auto tables = linked_section(*image, "__TEXT", "__gcc_except_tab");
  const auto got = linked_section(*image, "__DATA_CONST", "__got");
  const auto frame_size = prefix + bridge.sections[1].bytes.size() + 4;
  check(frames && frames->bytes.size() == frame_size, "the linked frame stream retains both profiles and one terminator");
  check(tables && tables->bytes.size() == table.size() + 4, "the cleanup table survives after earlier contribution padding");
  if (!frames || frames->bytes.size() != frame_size || !tables || tables->bytes.size() != table.size() + 4 || !got)
    return;
  constexpr std::uint64_t image_base = 0x100000000;
  const auto words = frames->bytes.subspan(prefix);
  const auto address = frames->address + prefix;
  check(mo::load32(words, 36) == 36, "the cleanup FDE keeps its backward CIE pointer");
  const auto start = trie_lookup(*image, "_cleanup_bridge");
  check(start && address + 40 + mo::load64(words, 40) == image_base + *start && mo::load64(words, 48) == 56,
        "the cleanup FDE retains its exact function address and extent");
  check(address + 57 + mo::load64(words, 57) == tables->address + 4, "the FDE's LSDA points to its own contribution");
  check(std::ranges::equal(tables->bytes.subspan(4), table), "the cleanup-only call-site rows survive byte for byte");
  check(mo::load32(frames->bytes, frame_size - 4) == 0, "the terminator follows the cleanup FDE");
  const auto cell = address + 19 + static_cast<std::int32_t>(mo::load32(words, 19));
  const bool in_got = cell >= got->address && cell - got->address < got->bytes.size() &&
                      (cell - got->address) % 8 == 0;
  check(in_got, "the CIE's personality address resolves to a GOT cell");
  if (!in_got) return;
  const auto pointer = mo::load64(got->bytes, cell - got->address);
  const auto personality = trie_lookup(*image, "___gxx_personality_v0");
  check(personality && (pointer >> 63) == 0 && (pointer & ((std::uint64_t{1} << 36) - 1)) == *personality,
        "the personality GOT cell rebases to the resolved personality symbol");
}

// Findings of the review of #42, one check each.
void test_review_cases() {
  {
    // ___dso_handle and __mh_execute_header are defined by the linker, at the header.
    auto o = return42();
    auto d = o.add_section(obj::standard_section::data_const);
    o.sections[d].emit64(0);
    o.sections[d].relocate(reloc_kind::abs64, o.reference("___dso_handle"));
    o.sections[d].emit64(0);
    o.sections[d].relocate(reloc_kind::abs64, o.reference("__mh_execute_header"));
    auto r = link::link(std::span(&o, 1), with_exports({}));
    check(r.has_value(), "___dso_handle and __mh_execute_header resolve to the linker's definitions");
  }
  {
    // Tentative definitions read from an object merge to the largest.
    auto common = [](std::string name, std::uint64_t size) {
      obj::object o;
      o.name = name;
      auto sec = obj::section::standard(obj::standard_section::bss);
      sec.name = "__common";
      sec.zerofill_size = size;
      auto k = o.add_section(std::move(sec));
      o.symbols.push_back({"_buf", obj::binding::global, k, 0, true});
      return o;
    };
    std::vector<obj::object> objs = {return42(), common("small.o", 8), common("big.o", 4096)};
    objs[1].sections[0].align_log2 = 4;
    auto image = link::link(objs);
    check(image.has_value(), "common symbols link");
    check(image && (obj::macho::load32(*image, 24) & obj::macho::mh_weak_defines) == 0, "a merged common is not a weak export");
    // The merged copy (big.o's, alignment 1) takes small.o's alignment of 16.
    if (image) {
      const auto off = trie_lookup(*image, "_buf");
      check(off && *off % 16 == 0, "a merged common has the largest alignment");
    }
    // The reader turns a Mach-O common symbol (N_UNDF, value = size) into that representation.
    auto bytes = obj::macho::write_object(common("c.o", 16));
    auto o = obj::macho::read_object(bytes, "c.o");
    check(o && o->sections.size() == 1 && o->sections[0].name == "__common" && o->symbols[0].weak, "a common round-trips");
  }
  {
    // A weak reference no dylib exports is imported weakly; a strong one is an error in either order.
    auto weak_user = [](bool weak) {
      obj::object o = return42();
      o.name = weak ? "weak.o" : "strong.o";
      if (weak) o.symbols[0].name = "_main";
      else o.symbols[0].name = "_other";
      auto d = o.add_section(obj::standard_section::data_const);
      o.sections[d].emit64(0);
      auto s = o.reference("_absent");
      o.symbols[s].weak = weak;
      o.sections[d].relocate(reloc_kind::abs64, s);
      return o;
    };
    std::vector<obj::object> weak_only = {weak_user(true)};
    check(link::link(weak_only, with_exports({})).has_value(), "an absent weak import links");
    std::vector<obj::object> ab = {weak_user(true), weak_user(false)}, ba = {weak_user(false), weak_user(true)};
    auto r1 = link::link(ab, with_exports({}));
    auto r2 = link::link(ba, with_exports({}));
    check(!r1 && !r2, "an absent symbol with one strong reference is undefined, in either order");
  }
  {
    // Weak definitions are exported as weak, and the header says so.
    auto o = return42();
    o.symbols[0].weak = true;
    auto image = link::link(std::span(&o, 1));
    check(image && (obj::macho::load32(*image, 24) & obj::macho::mh_weak_defines) != 0, "MH_WEAK_DEFINES is set");
  }
  {
    // Globals whose names start with 'l' are exported; only local temporary labels are dropped.
    auto o = return42();
    o.define("lglobal", obj::binding::global, 0, 0);
    auto image = link::link(std::span(&o, 1));
    check(image && trie_lookup(*image, "lglobal").has_value(), "a global named l... is exported");
  }
  {
    // A relocation to a global defined in a discarded section is an error, not an exception.
    auto o = return42();
    auto ld = o.add_section({"__LD", "__compact_unwind", obj::section_flags::debug, 3, std::vector<std::uint8_t>(8, 0), 0, {}});
    auto g = o.define("_in_dropped", obj::binding::global, ld, 0);
    o.sections[0].emit32(bl);
    o.sections[0].relocate(reloc_kind::branch26, g);
    auto r = link::link(std::span(&o, 1));
    check(!r && r.error()[0].message.contains("discarded section"), "reference into a discarded section is diagnosed");
  }
  {
    // An unreferenced local symbol with a bad section index is an error.
    auto o = return42();
    o.symbols.push_back({"bad", obj::binding::local, 7, 0, false});
    auto r = link::link(std::span(&o, 1));
    check(!r && r.error()[0].message.contains("bad section"), "a local symbol with a bad section is diagnosed");
  }
  {
    // GOT-relative words keep their addends through the writer and reader.
    auto o = return42();
    auto d = o.add_section(obj::standard_section::data_const);
    o.sections[d].emit32(0xdeadbeef);
    o.sections[d].relocate(reloc_kind::got_rel32, o.reference("_x"), 4);
    auto back = obj::macho::read_object(obj::macho::write_object(o), o.name);
    check(back && back->sections[d].relocs[0].addend == 4, "got_rel32 addend round-trips");
  }
  {
    // Truncated load commands are rejected, not read past the end of the buffer.
    for (std::uint32_t cmd : {obj::macho::lc_symtab, obj::macho::lc_segment_64}) {
      obj::macho::byte_writer w;
      for (std::uint32_t v : {obj::macho::mh_magic_64, obj::macho::cpu_type_arm64, 0u, obj::macho::mh_object, 1u, 8u, 0u, 0u}) w.u32(v);
      w.u32(cmd);
      w.u32(8);
      check(!obj::macho::read_object(w.bytes, "short.o").has_value(), "a truncated load command is rejected");
    }
  }
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

// The platform linker supplies the peer's typed catch metadata. The executable itself uses
// only the normalized cleanup-only fixture and a tail-call into the peer.
void test_hosted_cleanup(const std::filesystem::path& dir) {
  const auto source = dir / "cleanup_peer.cpp";
  const auto peer = std::filesystem::absolute(dir / "libcleanup_peer.dylib");
  std::ofstream(source) << R"cpp(
namespace {
struct Failure { int payload; };
bool throws = false;
int calls = 0;
}

extern "C" void fixture_callback() {
  ++calls;
  if (throws) throw Failure{783};
}

extern "C" void fixture_destroy(int* count) noexcept { ++*count; }

extern "C" int fixture_run(void (*bridge)(int*)) {
  int count = 0;
  bridge(&count);
  if (calls != 1 || count != 1) return 1;
  throws = true;
  count = 0;
  try {
    bridge(&count);
    return 2;
  } catch (const Failure& failure) {
    if (failure.payload != 783) return 3;
    if (calls != 2 || count != 1) return 4;
  } catch (...) {
    return 5;
  }
  return 0;
}
)cpp";
  const int compiled = shell(std::format("xcrun --sdk macosx clang++ -std=c++20 -stdlib=libc++ -arch arm64 -dynamiclib "
                                         "-install_name '{}' '{}' -o '{}'",
                                         peer.native_encoded_string(), source.native_encoded_string(), peer.native_encoded_string()));
  check(compiled == 0, "the platform compiler builds the typed-handler dylib");
  if (compiled != 0) return;

  obj::object entry;
  entry.name = "cleanup_entry.o";
  const auto text = entry.add_section(obj::standard_section::text);
  entry.define("_main", obj::binding::global, text);
  const auto bridge = entry.reference("_cleanup_bridge");
  const auto run = entry.reference("_fixture_run");
  auto& code = entry.sections[text];
  code.emit32(adrp(0));
  code.relocate(reloc_kind::page21, bridge);
  code.emit32(add_x_imm(0, 0));
  code.relocate(reloc_kind::pageoff12, bridge);
  code.emit32(0x14000000);  // b _fixture_run, which returns directly to the process entry caller
  code.relocate(reloc_kind::branch26, run);

  using exports = std::set<std::string, std::less<>>;
  link::options opts;
  opts.identifier = "hosted_cleanup";
  opts.dylibs = {link::libsystem(exports{"__Unwind_Resume"}),
                 {"/usr/lib/libc++.1.dylib", exports{"___gxx_personality_v0"}},
                 {peer.native_encoded_string(), exports{"_fixture_callback", "_fixture_destroy", "_fixture_run"}}};
  const std::array inputs{std::move(entry), cleanup_bridge_object()};
  const auto image = link::link(inputs, opts);
  check(image.has_value(), "the native cleanup bridge links with the platform peer");
  if (!image) {
    for (const auto& error : image.error()) std::println(std::cerr, "  {}", error.message);
    return;
  }
  run_and_check(dir, "hosted_cleanup", *image, 0, "");
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

void test_hosted_unwind(const std::filesystem::path& dir, std::string_view cc) {
  const auto src = dir / "observe_unwind.c";
  const auto path = dir / "observe_unwind.o";
  std::ofstream(src) << "unsigned long _Unwind_GetIP(void*);\n"
                        "int on_frame(void* context, void* arg) {\n"
                        "  unsigned long* words = arg;\n"
                        "  if (_Unwind_GetIP(context) != words[0]) return 0;\n"
                        "  words[1] = 42;\n"
                        "  return 5;\n"  // _URC_END_OF_STACK
                        "}\n";
  if (shell(std::format("'{}' -x c -O1 -c '{}' -o '{}'", cc, src.native_encoded_string(), path.native_encoded_string())) != 0) {
    check(false, "the host compiler compiles the unwind callback");
    return;
  }
  const auto bytes = slurp(path);
  auto callback = obj::macho::read_object(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()),
                                          "observe_unwind.o");
  check(callback.has_value(), "the unwind callback's object reads");
  if (!callback) return;

  // main keeps {address after the call, result} on its stack and passes it to the callback.
  obj::object main;
  main.name = "unwind_main.o";
  const auto main_text = main.add_section(obj::standard_section::text);
  const auto main_symbol = main.define("_main", obj::binding::global, main_text);
  constexpr std::uint32_t continuation = 40;
  const auto after_call = main.define("l_continuation", obj::binding::local, main_text, continuation);
  const auto observer = main.reference("_on_frame");
  const auto bridge = main.reference("_unwind_bridge");
  auto& text = main.sections[main_text];
  auto address = [&](unsigned rd, std::uint32_t symbol) {
    text.emit32(adrp(rd));
    text.relocate(reloc_kind::page21, symbol);
    text.emit32(add_x_imm(rd, rd));
    text.relocate(reloc_kind::pageoff12, symbol);
  };
  text.emit32(0xa9be7bfd);  // stp x29, x30, [sp, #-32]!
  text.emit32(mov_fp_sp);
  address(2, after_call);
  text.emit32(0xf9000be2);  // str x2, [sp, #16]
  text.emit32(0xf9000fff);  // str xzr, [sp, #24]
  address(0, observer);
  text.emit32(0x910043e1);  // add x1, sp, #16
  text.emit32(bl);
  text.relocate(reloc_kind::branch26, bridge);
  check(text.size() == continuation, "the recorded continuation follows the branch");
  text.emit32(0xb9401be0);  // ldr w0, [sp, #24]
  text.emit32(0xa8c27bfd);  // ldp x29, x30, [sp], #32
  text.emit32(ret);
  add_cleanup_frame(main, main_symbol, static_cast<std::uint32_t>(text.size()), 0x1b,
                    {0x44, 0x0e, 32, 0x9d, 4, 0x9e, 3, 0x6c, 0xdd, 0xde, 0x0e, 0}, true);

  // The bridge saves LR without changing x29. A frame-pointer walk would skip main's
  // continuation, but the FDE describes where to recover it from this frame's stack.
  obj::object leaf;
  leaf.name = "unwind_bridge.o";
  const auto leaf_text = leaf.add_section(obj::standard_section::text);
  const auto leaf_symbol = leaf.define("_unwind_bridge", obj::binding::global, leaf_text);
  const auto backtrace = leaf.reference("__Unwind_Backtrace");
  auto& body = leaf.sections[leaf_text];
  body.emit32(0xd10043ff);  // sub sp, sp, #16
  body.emit32(0xf90007fe);  // str x30, [sp, #8]
  body.emit32(bl);
  body.relocate(reloc_kind::branch26, backtrace);
  body.emit32(0xf94007fe);  // ldr x30, [sp, #8]
  body.emit32(0x910043ff);  // add sp, sp, #16
  body.emit32(ret);
  add_cleanup_frame(leaf, leaf_symbol, static_cast<std::uint32_t>(body.size()), 0x1b,
                    {0x44, 0x0e, 16, 0x44, 0x9e, 1, 0x48, 0xde, 0x44, 0x0e, 0});

  std::vector<obj::object> inputs = {std::move(main), std::move(leaf), std::move(*callback)};
  const auto image = link::link(inputs, with_exports({"__Unwind_Backtrace", "__Unwind_GetIP"}));
  check(image.has_value(), "the hosted unwind program links");
  if (!image) {
    for (const auto& e : image.error()) std::println(std::cerr, "  {}", e.message);
    return;
  }
  run_and_check(dir, "hosted_unwind", *image, 42, "");
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
  test_cleanup_frames();
  test_hosted_cleanup_metadata();
  test_review_cases();
  if (run_dir) {
    test_run(*run_dir);
    if (cc) {
      test_mixed(*run_dir, *cc);
      test_hosted_unwind(*run_dir, *cc);
      test_hosted_cleanup(*run_dir);
    }
  }
  return ycxx::unit::result("unit.link");
}
