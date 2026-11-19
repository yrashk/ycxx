// The ELF gABI layout and the System V AMD64 relocation definitions. The code bytes in the
// fixture are hand-encoded from the Intel SDM.
import std;
import ycxx.obj.elf;
import ycxx.unit;

using ycxx::unit::check;
namespace obj = ycxx::obj;

namespace {

obj::object fixture() {
  obj::object o;
  o.name = "elf-fixture.o";
  o.arch = obj::target::x86_64_linux;
  auto text = o.add_section(obj::standard_section::text);
  auto rodata = o.add_section(obj::standard_section::rodata);
  auto data = o.add_section(obj::standard_section::data);
  auto bss = o.add_section(obj::standard_section::bss);
  o.define("elf_call", obj::binding::global, text);
  auto external = o.reference("elf_external");
  auto local = o.define("elf_local", obj::binding::local, rodata);
  auto optional = o.reference("elf_optional");
  o.symbols[optional].weak = true;
  o.define("elf_pointer", obj::binding::hidden, data);
  o.define("elf_zero", obj::binding::global, bss);

  auto& code = o.sections[text];
  code.emit(std::array<std::uint8_t, 4>{0x48, 0x83, 0xec, 8});  // sub rsp, 8
  code.emit(std::array<std::uint8_t, 5>{0xe8, 0, 0, 0, 0});    // call elf_external
  code.relocate(obj::reloc_kind::x86_64_plt32, external, -4);
  code.emit(std::array<std::uint8_t, 4>{0x48, 0x83, 0xc4, 8});  // add rsp, 8
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});  // add eax, [rip + elf_local]
  code.relocate(obj::reloc_kind::x86_64_pc32, local, -4);
  code.emit(std::array<std::uint8_t, 1>{0xc3});                // ret

  o.sections[rodata].align(2);
  o.sections[rodata].emit32(17);
  o.sections[rodata].emit32(29);
  o.sections[data].align(3);
  o.sections[data].emit64(0);
  o.sections[data].relocate(obj::reloc_kind::abs64, local, 4);
  o.define("elf_weak_pointer", obj::binding::global, data);
  o.sections[data].emit64(0);
  o.sections[data].relocate(obj::reloc_kind::abs64, optional);
  o.sections[bss].align(4);
  o.sections[bss].reserve_zerofill(8192);
  return o;
}

std::uint64_t field(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{bytes[offset + i]} << (8 * i);
  return value;
}

void set_field(std::span<std::uint8_t> bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
  for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void inspect(std::span<const std::uint8_t> bytes) {
  const std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  check(bytes.size() >= 64, "ELF64 file header");
  if (bytes.size() < 64) return;
  check(std::ranges::equal(bytes.first(ident.size()), ident), "ELF64 little-endian identification");
  check(field(bytes, 16, 2) == 1 && field(bytes, 18, 2) == 62, "ET_REL for EM_X86_64");
  check(field(bytes, 32, 8) == 0 && field(bytes, 56, 2) == 0, "no program headers in a relocatable object");

  const auto shoff = field(bytes, 40, 8);
  const auto shnum = field(bytes, 60, 2);
  const auto shstrndx = field(bytes, 62, 2);
  const auto shentsize = field(bytes, 58, 2);
  const bool table_fits = shoff <= bytes.size() && shnum * shentsize <= bytes.size() - shoff;
  check(table_fits && shoff % 8 == 0 && shnum == 11 && shentsize == 64 && shstrndx == 10, "ELF64 section table");
  if (!table_fits || shnum != 11 || shentsize != 64 || shstrndx != 10) return;

  auto shdr = [&](std::size_t section, std::size_t offset, std::size_t width) {
    return field(bytes, shoff + section * shentsize + offset, width);
  };
  const auto shstrtab = shdr(shstrndx, 24, 8);
  const auto shstrtab_size = shdr(shstrndx, 32, 8);
  if (shstrtab > bytes.size() || shstrtab_size > bytes.size() - shstrtab) {
    check(false, "section names fit the file");
    return;
  }
  const auto table = std::string_view(reinterpret_cast<const char*>(bytes.data() + shstrtab), shstrtab_size);
  const std::array<std::string_view, 11> names{
      "", ".text", ".rodata", ".data", ".bss", ".rela.text", ".rela.data", ".note.GNU-stack", ".symtab", ".strtab", ".shstrtab"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    auto index = shdr(i, 0, 4);
    check(index < table.size() && table.substr(index).starts_with(names[i]) &&
          table.size() > index + names[i].size() && table[index + names[i].size()] == '\0', names[i]);
  }

  check(shdr(4, 4, 4) == 8 && shdr(4, 32, 8) == 8192 && shdr(4, 48, 8) == 16 && bytes.size() < 8192,
        "aligned SHT_NOBITS BSS occupies no file data");
  check(shdr(7, 8, 8) == 0 && shdr(7, 32, 8) == 0, "non-executable GNU stack marker");
  check(shdr(8, 40, 4) == 9 && shdr(8, 44, 4) == 2 && shdr(8, 56, 8) == 24,
        "symbol table links to the string table and places the local symbol first");

  const auto symbols = shdr(8, 24, 8);
  const auto text_relas = shdr(5, 24, 8);
  const auto data_relas = shdr(6, 24, 8);
  const auto contained = [&](std::uint64_t start, std::uint64_t size) {
    return start <= bytes.size() && size <= bytes.size() - start;
  };
  if (!contained(symbols, 8 * 24) || !contained(text_relas, 48) || !contained(data_relas, 48)) {
    check(false, "symbol table and relocations fit the file");
    return;
  }
  check(field(bytes, symbols + 1 * 24 + 4, 1) == 0 && field(bytes, symbols + 1 * 24 + 6, 2) == 2, "local data symbol");
  check(field(bytes, symbols + 4 * 24 + 4, 1) == 0x20 && field(bytes, symbols + 4 * 24 + 6, 2) == 0, "weak undefined symbol");
  check(field(bytes, symbols + 5 * 24 + 5, 1) == 2, "hidden global symbol");

  check(shdr(5, 4, 4) == 4 && shdr(5, 40, 4) == 8 && shdr(5, 44, 4) == 1 && shdr(5, 56, 8) == 24,
        "SHT_RELA references its symbol table and target section");
  const auto check_rela = [&](auto offset, auto site, auto symbol, auto type, std::int64_t addend) {
    check(field(bytes, offset, 8) == site && field(bytes, offset + 8, 8) == (std::uint64_t{symbol} << 32 | type) &&
          field(bytes, offset + 16, 8) == static_cast<std::uint64_t>(addend), "explicit-addend relocation");
  };
  check_rela(text_relas, 5u, 3u, 4u, -4);
  check_rela(text_relas + 24, 15u, 1u, 2u, -4);
  check_rela(data_relas, 0u, 1u, 1u, 4);
  check_rela(data_relas + 24, 8u, 4u, 1u, 0);
}

void rejects(std::string_view reason, const obj::object& o) {
  auto result = obj::elf::write_object(o);
  check(!result && result.error().contains(reason), reason);
}

void read_fixture(const obj::object& input, std::span<const std::uint8_t> file) {
  auto read = obj::elf::read_object(file, "read.o");
  check(read.has_value(), read ? "read ELF fixture" : read.error());
  if (!read) return;
  auto expected = input;
  expected.name = "read.o";
  // The writer moves elf_local ahead of the nonlocal symbols.
  std::rotate(expected.symbols.begin(), expected.symbols.begin() + 2, expected.symbols.begin() + 3);
  expected.sections[0].relocs[0].symbol = 2;
  expected.sections[0].relocs[1].symbol = 0;
  expected.sections[2].relocs[0].symbol = 0;
  check(*read == expected, "reader preserves standard sections, symbols and relocations in ELF order");
  auto rewritten = obj::elf::write_object(*read);
  check(rewritten && std::ranges::equal(*rewritten, file), "reader and writer round trip is byte identical");

  // Put .text after the relocation headers, updating the section indices in its symbol and RELA table.
  std::vector<std::uint8_t> reordered(file.begin(), file.end());
  const auto shoff = field(file, 40, 8);
  const auto symtab = field(file, shoff + 64 * 8 + 24, 8);
  std::swap_ranges(reordered.begin() + shoff + 64, reordered.begin() + shoff + 64 * 2,
                   reordered.begin() + shoff + 64 * 7);
  set_field(reordered, symtab + 24 * 2 + 6, 2, 7);
  set_field(reordered, shoff + 64 * 5 + 44, 4, 7);
  std::rotate(expected.sections.begin(), expected.sections.begin() + 1, expected.sections.end());
  for (auto& symbol : expected.symbols) {
    if (symbol.section) symbol.section = (*symbol.section + 3) % 4;
  }
  read = obj::elf::read_object(reordered, expected.name);
  check(read && *read == expected, "relocations and symbols follow ELF section indices across metadata headers");
}

void read_large_offsets() {
  obj::object expected;
  expected.name = "offsets.o";
  expected.arch = obj::target::x86_64_linux;
  auto strings = expected.add_section(obj::standard_section::cstring);
  auto pointers = expected.add_section(obj::standard_section::data_const);
  auto bss = expected.add_section(obj::standard_section::bss);
  constexpr auto large_offset = (std::uint64_t{1} << 32) + 64;
  expected.sections[strings].emit(std::array<std::uint8_t, 4>{'a', 0, 'b', 0});
  expected.sections[bss].align(16);
  expected.sections[bss].reserve_zerofill(large_offset);
  auto local = expected.define("", obj::binding::local, strings, 2);
  auto weak = expected.define("last", obj::binding::hidden, bss, large_offset);
  expected.symbols[weak].weak = true;
  auto& data = expected.sections[pointers];
  data.emit64(0);
  data.relocate(obj::reloc_kind::abs64, local, std::numeric_limits<std::int64_t>::min());
  data.emit64(0);
  data.relocate(obj::reloc_kind::abs64, weak, std::numeric_limits<std::int64_t>::max());

  auto file = obj::elf::write_object(expected);
  check(file && file->size() < 65536, "aligned BSS does not grow the file");
  if (!file) return;
  const auto shoff = field(*file, 40, 8);
  check(field(*file, shoff + 3 * 64 + 24, 8) > file->size(), "BSS has a conceptual file offset");
  auto read = obj::elf::read_object(*file, expected.name);
  check(read && *read == expected,
        "reader preserves BSS and symbol offsets above 32 bits, unnamed locals and full signed addends");
}

void reject_bad_files(std::span<const std::uint8_t> file) {
  constexpr std::string_view diagnostic_name = "malformed.o";
  auto reject = [&](std::span<const std::uint8_t> bytes, std::string_view reason) {
    auto read = obj::elf::read_object(bytes, std::string(diagnostic_name));
    check(!read && read.error().starts_with(diagnostic_name) && read.error().contains(reason), reason);
  };
  const auto shoff = field(file, 40, 8);
  const auto shdr = [=](std::size_t i) { return shoff + 64 * i; };
  const auto symtab = field(file, shdr(8) + 24, 8);
  const auto strtab = field(file, shdr(9) + 24, 8);
  const auto strsize = field(file, shdr(9) + 32, 8);
  const auto rela = field(file, shdr(5) + 24, 8);
  auto change = [&](std::size_t at, std::size_t width, std::uint64_t value, std::string_view reason) {
    std::vector<std::uint8_t> bad(file.begin(), file.end());
    set_field(bad, at, width, value);
    reject(bad, reason);
  };

  reject(file.first(63), "truncated ELF header");
  reject(file.first(file.size() - 1), "truncated ELF section header table");
  change(5, 1, 2, "little-endian ELF64");
  change(16, 2, 2, "relocatable ELF object");
  change(18, 2, 183, "x86-64 relocatable ELF object");
  change(40, 8, std::numeric_limits<std::uint64_t>::max() - 7, "invalid or truncated ELF section header table");
  change(60, 2, 0, "extended ELF section numbering");
  change(62, 2, 11, "invalid section name table index");
  change(shdr(1), 4, 0xffffffff, "invalid name for ELF section");
  change(shdr(1) + 8, 8, 0x406, "unsupported ELF section '.text'");
  change(shdr(1) + 24, 8, 32, "overlapping ELF file sections");
  change(shdr(1) + 32, 8, std::numeric_limits<std::uint64_t>::max(), "lies outside the file");
  change(shdr(1) + 48, 8, 3, "unsupported alignment");
  change(shdr(5) + 4, 4, 9, "unsupported ELF section '.rela.text'");
  change(shdr(5) + 40, 4, 9, "invalid ELF relocation section");
  change(shdr(5) + 44, 4, 4, "relocations in zero-fill sections");
  change(shdr(5) + 56, 8, 8, "invalid ELF relocation section");
  change(shdr(6) + 44, 4, 1, "multiple ELF relocation sections for one target section");
  change(shdr(7) + 8, 8, 4, "unsupported ELF section '.note.GNU-stack'");
  change(shdr(8) + 44, 4, 1, "local symbols must precede nonlocal symbols");
  change(shdr(8) + 56, 8, 0, "invalid ELF symbol table");
  change(strtab + strsize - 1, 1, 'a', "invalid ELF symbol name string table");
  change(symtab, 1, 1, "invalid null ELF symbol");
  change(symtab + 24, 4, strsize, "invalid name for ELF symbol");
  change(symtab + 24 + 4, 1, 3, "unsupported type, size, binding or visibility");
  change(symtab + 24 + 5, 1, 3, "unsupported type, size, binding or visibility");
  change(symtab + 24 + 6, 2, 0xfff2, "unsupported section index");
  change(symtab + 24 + 8, 8, 9, "out-of-section value");
  change(rela, 8, std::uint64_t{1} << 32, "relocation offset exceeds 32 bits");
  change(rela, 8, 17, "relocation lies outside its section");
  change(rela + 8, 4, 9, "unsupported x86-64 relocation type");
  change(rela + 12, 4, 0, "invalid ELF relocation symbol");
  change(rela + 12, 4, 8, "invalid ELF relocation symbol");
  change(rela + 24, 8, 5, "composed ELF relocations");

  auto repeated_names = std::vector<std::uint8_t>(file.begin(), file.end());
  const auto longest_name = field(file, symtab + 24 * 7, 4);
  for (std::size_t i = 1; i < 7; ++i) set_field(repeated_names, symtab + 24 * i, 4, longest_name);
  reject(repeated_names, "ELF symbol names exceed the string table size");
}

}  // namespace

int main(int argc, char** argv) {
  auto o = fixture();
  auto file = obj::elf::write_object(o);
  check(file.has_value(), file ? "valid ELF fixture" : file.error());
  if (file) {
    inspect(*file);
    read_fixture(o, *file);
    reject_bad_files(*file);
    check(obj::elf::write_object(o) == file, "ELF emission is byte identical");
    if (argc == 2) {
      std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(file->data()), file->size());
      out.close();
      check(bool(out), "write ELF fixture");
    }
  }
  read_large_offsets();

  auto bad = o;
  bad.arch = obj::target::aarch64_macos;
  rejects("expected an x86-64 Linux object", bad);
  bad = o;
  bad.sections[0].relocs[0].kind = obj::reloc_kind::branch26;
  rejects("unsupported x86-64 relocation", bad);
  bad = o;
  bad.sections[0].relocs[0].offset = 17;
  rejects("relocation outside section", bad);
  bad = o;
  bad.sections[0].relocs[0].symbol = bad.symbols.size();
  rejects("invalid relocation symbol", bad);
  bad = o;
  bad.sections[3].emit32(0);
  rejects("invalid zero-fill section", bad);
  return ycxx::unit::result("unit.elf");
}
