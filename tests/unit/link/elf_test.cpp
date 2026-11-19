// A libc-free Linux process, linked from two objects whose instructions are hand-encoded
// from the Intel SDM. ELF fields and relocations follow the ELF gABI and AMD64 psABI.
import std;
import ycxx.link.elf;
import ycxx.unit;

using ycxx::unit::check;
namespace obj = ycxx::obj;
namespace elf = ycxx::link::elf;
using obj::binding;
using obj::reloc_kind;
using obj::standard_section;

namespace {

std::array<obj::object, 2> fixture() {
  obj::object startup;
  startup.name = "startup.o";
  startup.arch = obj::target::x86_64_linux;
  const auto start_text = startup.add_section(standard_section::text);
  const auto constants = startup.add_section(standard_section::data_const);
  const auto start_rodata = startup.add_section(standard_section::rodata);
  auto& start = startup.sections[start_text];
  startup.define("_start", binding::global, start_text);
  const auto fallback = startup.define("compute", binding::global, start_text, 14);
  startup.symbols[fallback].weak = true;
  start.emit(std::array<std::uint8_t, 5>{0xe8, 0, 0, 0, 0});              // call compute
  start.relocate(reloc_kind::x86_64_plt32, fallback, -4);
  start.emit(std::array<std::uint8_t, 9>{0x89, 0xc7,                       // mov edi, eax
                                         0xb8, 60, 0, 0, 0,              // mov eax, 60 (exit)
                                         0x0f, 0x05});                   // syscall
  start.emit(std::array<std::uint8_t, 6>{0xb8, 99, 0, 0, 0, 0xc3});       // fallback returns 99

  startup.define("table", binding::hidden, constants);
  startup.sections[constants].emit64(0);
  startup.sections[constants].relocate(reloc_kind::abs64, startup.reference("values"), 4);
  startup.define("optional_value", binding::hidden, constants);
  const auto missing = startup.reference("optional");
  startup.symbols[missing].weak = true;
  startup.sections[constants].emit64(99);
  startup.sections[constants].relocate(reloc_kind::abs64, missing);
  startup.define("bonus", binding::local, start_rodata);
  startup.sections[start_rodata].emit32(99);

  obj::object compute;
  compute.name = "compute.o";
  compute.arch = obj::target::x86_64_linux;
  const auto text = compute.add_section(standard_section::text);
  const auto rodata = compute.add_section(standard_section::rodata);
  const auto data = compute.add_section(standard_section::data);
  const auto bss = compute.add_section(standard_section::bss);
  const auto table = compute.reference("table");
  const auto optional_value = compute.reference("optional_value");
  const auto bonus = compute.define("bonus", binding::local, rodata);
  const auto scratch = compute.define("scratch", binding::local, bss, 8192);
  compute.define("values", binding::hidden, data);
  compute.define("compute", binding::global, text);
  compute.sections[rodata].align(4);
  compute.sections[rodata].emit32(12);
  compute.sections[data].align(2);
  compute.sections[data].emit32(18);
  compute.sections[data].emit32(30);
  compute.sections[bss].align(4);
  compute.sections[bss].reserve_zerofill(8196);

  auto& code = compute.sections[text];
  code.emit(std::array<std::uint8_t, 7>{0x48, 0x8b, 0x05, 0, 0, 0, 0});  // mov rax, [rip + table]
  code.relocate(reloc_kind::x86_64_pc32, table, -4);
  code.emit(std::array<std::uint8_t, 2>{0x8b, 0x00});                      // mov eax, [rax]
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + bonus]
  code.relocate(reloc_kind::x86_64_pc32, bonus, -4);
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + scratch]
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x89, 0x05, 0, 0, 0, 0});          // mov [rip + scratch], eax
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x8b, 0x05, 0, 0, 0, 0});          // mov eax, [rip + scratch]
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + optional_value]
  code.relocate(reloc_kind::x86_64_pc32, optional_value, -4);
  code.emit(std::array<std::uint8_t, 1>{0xc3});                            // ret
  return {std::move(startup), std::move(compute)};
}

std::uint64_t field(std::span<const std::uint8_t> image, std::size_t offset, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{image[offset + i]} << (8 * i);
  return value;
}

void inspect(std::span<const std::uint8_t> image) {
  constexpr std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  constexpr std::size_t header_size = 64, phdr_size = 56, phdrs = 4;
  constexpr std::size_t headers_size = header_size + phdrs * phdr_size;
  check(image.size() >= headers_size, "ELF and program headers fit the image");
  if (image.size() < headers_size) return;
  check(std::ranges::equal(image.first(ident.size()), ident) && field(image, 16, 2) == 2 && field(image, 18, 2) == 62,
        "little-endian ELF64 ET_EXEC for x86-64");
  check(field(image, 32, 8) == header_size && field(image, 54, 2) == phdr_size && field(image, 56, 2) == phdrs,
        "program header table");
  check(field(image, 40, 8) == 0 && field(image, 60, 2) == 0, "no section header table");
  const auto phdr = [&](std::size_t index, std::size_t offset, std::size_t width) {
    return field(image, header_size + index * phdr_size + offset, width);
  };
  constexpr std::array flags{5u, 4u, 6u};
  for (std::size_t i = 0; i < flags.size(); ++i) {
    const auto offset = phdr(i, 8, 8);
    const auto address = phdr(i, 16, 8);
    const auto size = phdr(i, 32, 8);
    check(phdr(i, 0, 4) == 1 && phdr(i, 4, 4) == flags[i] && phdr(i, 48, 8) == 4096,
          "page-aligned load segment with the expected permissions");
    check(offset % 4096 == 0 && address % 4096 == 0 && offset <= image.size() && size <= image.size() - offset,
          "congruent load segment within the image");
    if (i > 0) check(offset >= phdr(i - 1, 8, 8) + phdr(i - 1, 32, 8) && address > phdr(i - 1, 16, 8),
                     "load segments have independent pages in ascending order");
  }
  check(phdr(3, 0, 4) == 0x6474e551 && phdr(3, 4, 4) == 6, "non-executable GNU stack");
  check(phdr(0, 8, 8) == 0 && field(image, 24, 8) == phdr(0, 16, 8) + headers_size,
        "entry immediately follows the mapped headers");
  check(phdr(2, 32, 8) == 8 && phdr(2, 40, 8) == 8212, "writable segment zero-fills aligned BSS beyond the file");

  const auto read_only = phdr(1, 8, 8);
  if (read_only > image.size() || image.size() - read_only < 36) {
    check(false, "read-only data fits the image");
    return;
  }
  check(field(image, read_only, 8) == phdr(2, 16, 8) + 4, "absolute relocation with a nonzero addend");
  check(field(image, read_only + 8, 8) == 0, "unresolved weak symbol has value zero");
  check(field(image, read_only + 16, 4) == 99 && field(image, read_only + 32, 4) == 12,
        "read-only contributions preserve input order and alignment");
}

void alternate_layouts(const std::array<obj::object, 2>& objects) {
  const std::array reversed{objects[1], objects[0]};
  const auto image = elf::link(reversed);
  check(image && image->size() >= 64 + 4 * 56 + 5, "link the two objects in the opposite order");
  if (image && image->size() >= 64 + 4 * 56 + 5) {
    const auto base = field(*image, 64 + 16, 8);
    const auto entry = field(*image, 24, 8);
    if (entry >= base && entry - base <= image->size() - 5) {
      const auto bits = static_cast<std::uint32_t>(field(*image, entry - base + 1, 4));
      const auto displacement = std::bit_cast<std::int32_t>(bits);
      check(displacement < 0 && static_cast<std::int64_t>(entry + 5) + displacement ==
                                    static_cast<std::int64_t>(base + 64 + 4 * 56),
            "backwards branch resolves to the earlier strong definition");
    } else {
      check(false, "reordered entry is inside the executable segment");
    }
  }

  obj::object small;
  small.arch = obj::target::x86_64_linux;
  const auto text = small.add_section(standard_section::text);
  small.define("_start", binding::global, text);
  small.sections[text].emit(std::array<std::uint8_t, 1>{0xcc});  // int3, only the layout is tested
  const auto text_only = elf::link(std::span(&small, 1));
  check(text_only && text_only->size() == 64 + 2 * 56 + 1 && field(*text_only, 56, 2) == 2,
        "text-only executable has one load segment and a stack header");

  const auto bss = small.add_section(standard_section::bss);
  small.sections[bss].reserve_zerofill(8192);
  const auto bss_only = elf::link(std::span(&small, 1));
  check(bss_only && bss_only->size() == 4096 && field(*bss_only, 56, 2) == 3 &&
        field(*bss_only, 64 + 56 + 32, 8) == 0 && field(*bss_only, 64 + 56 + 40, 8) == 8192,
        "a writable load segment can consist entirely of BSS");
}

void rejects(std::span<const obj::object> objects, std::string_view reason, std::string_view entry = "_start") {
  const auto result = elf::link(objects, entry);
  check(!result && result.error().contains(reason), reason);
}

void diagnostics(const std::array<obj::object, 2>& good) {
  rejects(good, "entry symbol 'missing' is undefined", "missing");
  rejects(good, "entry symbol 'values' is not inside executable code", "values");
  auto bad = good;
  bad[0].symbols[0].offset = bad[0].sections[0].size();
  rejects(bad, "entry symbol '_start' is not inside executable code");
  bad = good;
  bad[1].arch = obj::target::aarch64_macos;
  rejects(bad, "compute.o: expected an x86-64 Linux object");
  bad = good;
  bad[0].symbols[1].weak = false;
  rejects(bad, "compute.o: duplicate symbol 'compute'");
  bad = good;
  bad[1].reference("undefined");
  rejects(bad, "compute.o: undefined symbol 'undefined'");
  bad = good;
  bad[0].add_symbol({"local_undef", binding::local, std::nullopt, 0, false});
  rejects(bad, "startup.o: invalid local symbol 'local_undef'");
  bad = good;
  bad[0].symbols[0].section = bad[0].sections.size();
  rejects(bad, "startup.o: symbol '_start' is outside its section");
  bad = good;
  bad[1].sections[3].name = "__tls";
  rejects(bad, "compute.o: __DATA,__tls: unsupported ELF section");
  bad = good;
  bad[1].sections[3].flags = obj::section_flags::thread_local_zerofill;
  rejects(bad, "unsupported ELF section flags");
  bad = good;
  bad[1].sections[3].emit32(1);
  rejects(bad, "invalid zero-fill section contents");
  bad = good;
  bad[1].sections[3].align_log2 = 23;
  rejects(bad, "section alignment exceeds 4 MiB");
  bad = good;
  bad[1].sections[3].zerofill_size = std::numeric_limits<std::uint64_t>::max();
  rejects(bad, "section exceeds the ELF image address limit");
  bad = good;
  bad[0].sections[0].relocs[0].kind = reloc_kind::branch26;
  rejects(bad, "unsupported x86-64 ELF relocation kind");
  bad = good;
  bad[0].sections[1].relocs[0].minus = 0;
  rejects(bad, "ELF symbol-difference relocations are unsupported");
  bad = good;
  bad[0].sections[0].relocs[0].offset = 19;
  rejects(bad, "ELF relocation is outside its section");
  bad = good;
  bad[0].sections[0].relocs[0].symbol = bad[0].symbols.size();
  rejects(bad, "invalid ELF relocation symbol index");
  bad = good;
  bad[0].sections[1].relocs[1].offset = 7;
  rejects(bad, "overlapping ELF relocations are unsupported");
  bad = good;
  bad[0].sections[0].relocs[0].addend = std::numeric_limits<std::int64_t>::max();
  rejects(bad, "R_X86_64_PLT32 displacement overflows for 'compute'");
  bad = good;
  bad[1].sections[0].relocs[0].addend = std::numeric_limits<std::int64_t>::min();
  rejects(bad, "R_X86_64_PC32 displacement overflows for 'table'");
}

}  // namespace

int main(int argc, char** argv) {
  const auto objects = fixture();
  const auto image = elf::link(objects);
  check(image.has_value(), image ? "linked a static ELF executable" : image.error());
  if (image) {
    inspect(*image);
    check(elf::link(objects) == image, "ELF link is byte identical");
    check(elf::link(objects, "compute").has_value(), "caller can supply a different executable entry symbol");
    if (argc == 2) {
      std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(image->data()), image->size());
      out.close();
      check(bool(out), "write ELF executable fixture");
    }
  }
  diagnostics(objects);
  alternate_layouts(objects);
  return ycxx::unit::result("unit.elf_link");
}
