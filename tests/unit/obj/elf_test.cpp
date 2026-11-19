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

}  // namespace

int main(int argc, char** argv) {
  auto o = fixture();
  auto file = obj::elf::write_object(o);
  check(file.has_value(), file ? "valid ELF fixture" : file.error());
  if (file) {
    inspect(*file);
    check(obj::elf::write_object(o) == file, "ELF emission is byte identical");
    if (argc == 2) {
      std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(file->data()), file->size());
      out.close();
      check(bool(out), "write ELF fixture");
    }
  }

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
