// The ELF gABI layout and the System V AMD64 relocation definitions. The code bytes in the
// fixture are hand-encoded from the Intel SDM.
import std;
import ycxx.hash;
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
  check(field(bytes, symbols + 2 * 24 + 4, 1) == 0x10 && field(bytes, symbols + 2 * 24 + 16, 8) == 0,
        "untyped code symbol stays STT_NOTYPE with zero size");
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

void rtti_name_group() {
  constexpr std::string_view signature = "_ZTS9RttiProbe";
  constexpr std::string_view name_bytes("9RttiProbe\0", 11);
  constexpr std::string_view ordinary_bytes("ORD!\0", 5);
  obj::object input;
  input.arch = obj::target::x86_64_linux;
  const auto ordinary = input.add_section(obj::standard_section::rodata);
  const auto member = input.add_section(obj::standard_section::rodata);
  input.sections[ordinary].emit(ordinary_bytes);
  input.sections[member].emit(name_bytes);
  const auto name = input.define(std::string(signature), obj::binding::global, member, 0);
  input.symbols[name].weak = true;
  input.symbols[name].object_size = name_bytes.size();
  // The local symbol sorts ahead of the signature in the emitted table.
  input.define("local", obj::binding::local, ordinary, 0);
  const auto data = input.define("ordinary_data", obj::binding::global, ordinary, 0);
  input.symbols[data].object_size = ordinary_bytes.size();
  input.comdat_groups.push_back({std::string(signature), {member}});

  auto bad = input;
  bad.symbols[name].object_size = 0;
  rejects("invalid ELF COMDAT signature symbol", bad);
  bad = input;
  bad.comdat_groups.front().signature = "missing";
  rejects("missing ELF COMDAT signature symbol", bad);
  bad = input;
  bad.symbols[name].object_size = name_bytes.size() + 1;
  rejects("object symbol '_ZTS9RttiProbe' is outside its section", bad);

  const auto file = obj::elf::write_object(input);
  check(file.has_value(), file ? "write RTTI name COMDAT" : file.error());
  if (!file) return;
  const auto read = obj::elf::read_object(*file, "rtti-name.o");
  check(read && read->comdat_groups == input.comdat_groups && read->sections == input.sections,
        "read the RTTI name signature, member section and separate ordinary data");

  if (file->size() < 64) return check(false, "RTTI name ELF header fits the file");
  const auto shoff = field(*file, 40, 8);
  const auto shnum = field(*file, 60, 2);
  const auto contains = [&](std::uint64_t start, std::uint64_t size) {
    return start <= file->size() && size <= file->size() - start;
  };
  if (field(*file, 58, 2) != 64 || !contains(shoff, shnum * 64))
    return check(false, "RTTI name section header table fits the file");
  const auto shdr = [&](std::uint64_t index, std::uint64_t offset, std::uint64_t width) {
    return field(*file, shoff + index * 64 + offset, width);
  };
  std::uint64_t group = 0, symtab = 0;
  for (std::uint64_t i = 1; i < shnum; ++i) {
    if (shdr(i, 4, 4) == 17) {
      check(group == 0, "one RTTI name SHT_GROUP");
      group = i;
    }
    if (shdr(i, 4, 4) == 2) symtab = i;
  }
  if (group == 0 || symtab == 0) return check(false, "RTTI name group and symbol table present");
  const auto group_offset = shdr(group, 24, 8);
  const auto symbol_offset = shdr(symtab, 24, 8);
  if (!contains(group_offset, 8) || !contains(symbol_offset, 4 * 24))
    return check(false, "RTTI group and symbol records fit the file");
  check(shdr(group, 8, 8) == 0 && shdr(group, 32, 8) == 8 && shdr(group, 56, 8) == 4 &&
        shdr(group, 40, 4) == symtab && shdr(group, 44, 4) == 2 && shdr(symtab, 44, 4) == 2,
        "group links to the symbol table and uses the signature index after the local symbol");
  const auto member_index = field(*file, group_offset + 4, 4);
  if (member_index <= group || member_index >= shnum)
    return check(false, "RTTI name group header precedes its member");
  check(field(*file, group_offset, 4) == 1, "RTTI name group starts with GRP_COMDAT");
  check(shdr(member_index, 4, 4) == 1 && shdr(member_index, 8, 8) == 0x202,
        "RTTI member is read-only SHT_PROGBITS with SHF_ALLOC and SHF_GROUP");
  const auto name_record = symbol_offset + 2 * 24;
  check(field(*file, name_record + 4, 1) == 0x21 && field(*file, name_record + 5, 1) == 0 &&
        field(*file, name_record + 6, 2) == member_index && field(*file, name_record + 8, 8) == 0 &&
        field(*file, name_record + 16, 8) == name_bytes.size(),
        "RTTI name is a weak default-visible STT_OBJECT with its exact NUL-terminated extent");
  const auto payload_is = [&](std::uint64_t index, std::string_view expected) {
    const auto offset = shdr(index, 24, 8);
    return shdr(index, 32, 8) == expected.size() && contains(offset, expected.size()) &&
           std::string_view(reinterpret_cast<const char*>(file->data() + offset), expected.size()) == expected;
  };
  check(payload_is(member_index, name_bytes), "RTTI name member contains exactly the mangled name bytes and NUL");
  const auto ordinary_index = field(*file, symbol_offset + 3 * 24 + 6, 2);
  check(ordinary_index != 0 && ordinary_index < shnum && ordinary_index != member_index &&
        (shdr(ordinary_index, 8, 8) & 0x200) == 0 && payload_is(ordinary_index, ordinary_bytes),
        "unrelated object bytes are outside the RTTI name group");
}

void rtti_data_group() {
  constexpr auto signature = "_ZTI1B";
  constexpr auto type_vtable = "_ZTVN10__cxxabiv117__class_type_infoE";
  constexpr auto type_name = "_ZTS1B";
  obj::object input;
  input.name = "rtti-data.o";
  input.arch = obj::target::x86_64_linux;
  const auto member = input.add_section(obj::standard_section::data_const);
  const auto table = input.reference(type_vtable);
  const auto descriptor = input.define(signature, obj::binding::global, member);
  const auto name = input.reference(type_name);
  input.symbols[descriptor].weak = true;
  input.symbols[descriptor].object_size = 16;
  auto& data = input.sections[member];
  data.align(3);
  data.emit64(0);
  data.relocate(obj::reloc_kind::abs64, table, 16);
  data.emit64(0);
  data.relocate(obj::reloc_kind::abs64, name);
  input.comdat_groups.push_back({signature, {member}});

  const auto file = obj::elf::write_object(input);
  check(file.has_value(), file ? "write relocatable RTTI COMDAT" : file.error());
  if (!file) return;
  const auto read = obj::elf::read_object(*file, input.name);
  check(read && *read == input, "round-trip the RTTI COMDAT and its two named relocations");

  const auto contains = [&](std::uint64_t start, std::uint64_t size) {
    return start <= file->size() && size <= file->size() - start;
  };
  if (!contains(0, 64)) return check(false, "relocatable RTTI ELF header fits the file");
  const auto shoff = field(*file, 40, 8);
  const auto shnum = field(*file, 60, 2);
  if (field(*file, 58, 2) != 64 || !contains(shoff, shnum * 64))
    return check(false, "relocatable RTTI section headers fit the file");
  const auto shdr = [&](std::uint64_t index, std::uint64_t offset, std::uint64_t width) {
    return field(*file, shoff + index * 64 + offset, width);
  };
  const auto string_at = [&](std::uint64_t section, std::uint64_t index) -> std::string_view {
    if (section == 0 || section >= shnum || shdr(section, 4, 4) != 3) return {};
    const auto start = shdr(section, 24, 8);
    const auto size = shdr(section, 32, 8);
    if (!contains(start, size) || index >= size) return {};
    const std::string_view tail(reinterpret_cast<const char*>(file->data() + start + index), size - index);
    const auto end = tail.find('\0');
    return end == tail.npos ? std::string_view{} : tail.substr(0, end);
  };

  std::uint64_t group = 0, payload = 0, rela = 0;
  const auto section_names = field(*file, 62, 2);
  for (std::uint64_t i = 1; i < shnum; ++i) {
    if (shdr(i, 4, 4) == 17) {
      check(group == 0, "one relocatable RTTI SHT_GROUP");
      group = i;
    }
    const auto section_name = string_at(section_names, shdr(i, 0, 4));
    if (section_name == ".data.rel.ro") payload = i;
    if (section_name == ".rela.data.rel.ro") rela = i;
  }
  if (group == 0 || payload <= group || rela <= group)
    return check(false, "RTTI group header precedes both named members");
  const auto group_offset = shdr(group, 24, 8);
  const auto symtab = shdr(group, 40, 4);
  if (!contains(group_offset, 12) || symtab == 0 || symtab >= shnum || shdr(symtab, 4, 4) != 2)
    return check(false, "RTTI group data and linked SHT_SYMTAB are present");
  const auto first = field(*file, group_offset + 4, 4);
  const auto second = field(*file, group_offset + 8, 4);
  check(shdr(group, 8, 8) == 0 && shdr(group, 32, 8) == 12 && shdr(group, 56, 8) == 4 &&
        field(*file, group_offset, 4) == 1 &&
        ((first == payload && second == rela) || (first == rela && second == payload)),
        "GRP_COMDAT lists exactly the payload and relocation section indexes");
  check(shdr(payload, 4, 4) == 1 && shdr(payload, 8, 8) == 0x203 && shdr(payload, 32, 8) == 16,
        "16-byte RTTI payload uses SHT_PROGBITS and SHF_WRITE, SHF_ALLOC and SHF_GROUP");
  check(shdr(rela, 4, 4) == 4 && (shdr(rela, 8, 8) & 0x200) != 0 &&
        shdr(rela, 40, 4) == symtab && shdr(rela, 44, 4) == payload && shdr(rela, 56, 8) == 24,
        "grouped SHT_RELA links to the symbol table and identifies the RTTI payload");

  const auto symbols = shdr(symtab, 24, 8);
  const auto symbol_bytes = shdr(symtab, 32, 8);
  const auto symbol_names = shdr(symtab, 40, 4);
  const auto signature_index = shdr(group, 44, 4);
  if (!contains(symbols, symbol_bytes) || shdr(symtab, 56, 8) != 24 || signature_index >= symbol_bytes / 24)
    return check(false, "RTTI signature index is in the symbol table");
  const auto symbol_name = [&](std::uint64_t index) -> std::string_view {
    if (index >= symbol_bytes / 24) return {};
    return string_at(symbol_names, field(*file, symbols + index * 24, 4));
  };
  const auto symbol = symbols + signature_index * 24;
  check(symbol_name(signature_index) == signature && field(*file, symbol + 4, 1) == 0x21 &&
        field(*file, symbol + 5, 1) == 0 && field(*file, symbol + 6, 2) == payload &&
        field(*file, symbol + 8, 8) == 0 && field(*file, symbol + 16, 8) == 16,
        "sh_info identifies the weak default-visible RTTI object defined in the grouped payload");

  const auto entries = shdr(rela, 24, 8);
  if (shdr(rela, 32, 8) != 48 || !contains(entries, 48))
    return check(false, "RTTI payload has exactly two RELA records");
  const auto has_abs64 = [&](std::uint64_t offset, std::string_view target, std::uint64_t addend) {
    for (std::uint64_t entry : {entries, entries + 24}) {
      const auto info = field(*file, entry + 8, 8);
      if (field(*file, entry, 8) == offset && (info & 0xffffffff) == 1 &&
          symbol_name(info >> 32) == target && field(*file, entry + 16, 8) == addend) return true;
    }
    return false;
  };
  check(has_abs64(0, type_vtable, 16) && has_abs64(8, type_name, 0),
        "RTTI RELA records preserve both R_X86_64_64 targets, offsets and addends");
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
  check(ycxx::fingerprint(*read) == ycxx::fingerprint(expected), "owned and ELF-backed names fingerprint by spelling");
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

void read_shared_names() {
  constexpr std::size_t length = 8192;
  const std::string text(length, 'x');
  auto retained = [&]() -> std::vector<obj::symbol> {
    obj::object input;
    input.arch = obj::target::x86_64_linux;
    const auto section = input.add_section(obj::standard_section::text);
    input.define(text, obj::binding::local, section);
    for (std::size_t i = 0; i <= length; ++i) input.define("", obj::binding::local, section);
    auto file = obj::elf::write_object(input);
    check(file.has_value(), file ? "write one long ELF name" : file.error());
    if (!file) return {};

    const auto shoff = field(*file, 40, 8);
    const auto symtab_header = shoff + 64 * (field(*file, 60, 2) - 3);
    const auto symtab = field(*file, symtab_header + 24, 8);
    const auto strtab_header = shoff + 64 * field(*file, symtab_header + 40, 4);
    const auto strtab = field(*file, strtab_header + 24, 8);
    const auto strsize = field(*file, strtab_header + 32, 8);
    check(strsize == length + 2, "one stored string with its boundary NULs");
    for (std::size_t i = 1; i <= length; ++i) set_field(*file, symtab + 24 * i, 4, i);
    set_field(*file, symtab + 24 * (length + 1), 4, 1);  // repeat an earlier offset
    set_field(*file, symtab + 24 * (length + 2), 4, strsize - 1);  // name at a terminal NUL

    auto parsed = obj::elf::read_object(*file, "suffix.o");
    check(parsed.has_value(), parsed ? "read shared ELF suffix names" : parsed.error());
    if (!parsed) return {};
    auto other = obj::elf::read_object(*file, "suffix.o");
    check(other && *other == *parsed && ycxx::fingerprint(other->symbols.front().name) ==
              ycxx::fingerprint(parsed->symbols.front().name),
          "equal names from independently owned tables compare and fingerprint by spelling");

    auto bad = *file;
    set_field(bad, symtab + 24, 4, strsize);
    auto invalid = obj::elf::read_object(bad, "bad-offset.o");
    check(!invalid && invalid.error().contains("invalid name for ELF symbol 1"), "reject an offset one past the table");
    bad = *file;
    bad[strtab + strsize - 1] = 'x';
    invalid = obj::elf::read_object(bad, "bad-termination.o");
    check(!invalid && invalid.error().contains("invalid ELF symbol name string table"), "reject a missing terminal NUL");
    bad = *file;
    set_field(bad, strtab_header + 32, 8, 0);
    for (std::size_t i = 1; i <= length + 2; ++i) set_field(bad, symtab + 24 * i, 4, 0);
    auto empty = obj::elf::read_object(bad, "empty-string-table.o");
    check(empty && std::ranges::all_of(empty->symbols, [](const auto& s) { return s.name_view().empty(); }),
          "zero name indices are valid in an empty symbol string table");
    set_field(bad, symtab + 24, 4, 1);
    invalid = obj::elf::read_object(bad, "bad-empty-offset.o");
    check(!invalid && invalid.error().contains("invalid name for ELF symbol 1"), "reject a nonzero index in an empty table");

    auto copied = *parsed;
    return std::move(copied.symbols);
  }();
  if (retained.empty()) return;
  auto moved = std::move(retained);
  moved.reserve(moved.size() + 1);
  const auto first = moved.front().name_view();
  check(first == text, "ELF names survive input and object destruction and vector moves");
  if (first.size() != length || moved.size() != length + 2) return;
  bool shared = true;
  for (std::size_t i = 0; i < length; ++i) {
    const auto suffix = moved[i].name_view();
    shared &= suffix.data() == first.data() + i && suffix.size() == length - i;
  }
  check(shared && moved[length].name_view().data() == first.data() && moved.back().name_view().empty(),
        "different suffixes and repeated offsets share backing bytes");
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

void read_function_symbols() {
  obj::object input;
  input.name = "functions.o";
  input.arch = obj::target::x86_64_linux;
  const auto text = input.add_section(obj::standard_section::text);
  const auto data = input.add_section(obj::standard_section::data);
  const auto local = input.define("local", obj::binding::local, text, 0);
  const auto weak = input.define("weak_hidden", obj::binding::hidden, text, 1);
  const auto reference = input.reference("reference");
  input.symbols[local].function_size = 0;
  input.symbols[weak].function_size = 6;
  input.symbols[weak].weak = true;
  input.symbols[reference].weak = true;
  // A ret, followed by mov eax, 42 and ret.
  input.sections[text].emit(std::array<std::uint8_t, 7>{0xc3, 0xb8, 42, 0, 0, 0, 0xc3});
  input.sections[data].emit64(0);
  input.sections[data].relocate(obj::reloc_kind::abs64, weak);
  input.sections[data].emit64(0);
  input.sections[data].relocate(obj::reloc_kind::abs64, reference);

  const auto file = obj::elf::write_object(input);
  check(file.has_value(), file ? "write ELF function definitions" : file.error());
  if (!file) return;
  const auto symtab = field(*file, field(*file, 40, 8) + 5 * 64 + 24, 8);
  const auto local_record = symtab + 24;
  const auto weak_record = symtab + 2 * 24;
  const auto reference_record = symtab + 3 * 24;
  check(field(*file, local_record + 4, 1) == 0x02 && field(*file, local_record + 16, 8) == 0,
        "local STT_FUNC with unknown size");
  check(field(*file, weak_record + 4, 1) == 0x22 && field(*file, weak_record + 5, 1) == 2 &&
        field(*file, weak_record + 6, 2) == 1 && field(*file, weak_record + 8, 8) == 1 &&
        field(*file, weak_record + 16, 8) == 6, "weak hidden STT_FUNC with exact section and extent");
  const auto read = obj::elf::read_object(*file, input.name);
  check(read && *read == input, "round-trip function metadata, bindings, and relocation indices");
  check(read && obj::elf::write_object(*read) == file, "rewrite ELF function definitions byte for byte");

  auto typed_reference = *file;
  set_field(typed_reference, reference_record + 4, 1, 0x22);
  const auto read_reference = obj::elf::read_object(typed_reference, input.name);
  check(read_reference && *read_reference == input, "undefined STT_FUNC follows ordinary weak reference semantics");

  const auto reject = [&](std::size_t at, std::size_t width, std::uint64_t value, std::string_view reason) {
    auto bad = *file;
    set_field(bad, at, width, value);
    const auto result = obj::elf::read_object(bad, input.name);
    check(!result && result.error().contains(reason), reason);
  };
  reject(weak_record + 6, 2, 2, "not in executable code");
  reject(weak_record + 16, 8, 7, "out-of-section value or size");
  reject(weak_record + 16, 8, std::numeric_limits<std::uint64_t>::max(), "out-of-section value or size");
  reject(local_record + 8, 8, input.sections[text].size(), "not in executable code");
  set_field(typed_reference, reference_record + 16, 8, 1);
  const auto bad_reference = obj::elf::read_object(typed_reference, input.name);
  check(!bad_reference && bad_reference.error().contains("invalid undefined ELF symbol"),
        "reject an undefined function with a nonzero extent");

  auto bad = input;
  bad.symbols[weak].section = data;
  rejects("not in executable code", bad);
  bad = input;
  bad.symbols[weak].function_size = std::numeric_limits<std::uint64_t>::max();
  rejects("outside its section", bad);
  bad = input;
  bad.symbols[local].offset = input.sections[text].size();
  rejects("not in executable code", bad);
  bad = input;
  bad.symbols[reference].function_size = 0;
  rejects("not in executable code", bad);
}

void read_init_array() {
  obj::object input;
  input.name = "init.o";
  input.arch = obj::target::x86_64_linux;
  const auto text = input.add_section(obj::standard_section::text);
  const auto init = input.add_section(obj::standard_section::init_array);
  const auto fn = input.define("init", obj::binding::local, text);
  input.sections[text].emit(std::array<std::uint8_t, 1>{0xc3});  // ret
  input.sections[init].emit64(0);
  input.sections[init].relocate(obj::reloc_kind::abs64, fn);
  const auto file = obj::elf::write_object(input);
  check(file.has_value(), file ? "write .init_array" : file.error());
  if (!file) return;
  const auto header = field(*file, 40, 8) + 64 * 2;
  check(field(*file, header + 4, 4) == 14 && field(*file, header + 8, 8) == 3 &&
        field(*file, header + 32, 8) == 8 && field(*file, header + 48, 8) == 8 &&
        field(*file, header + 56, 8) == 8, "SHT_INIT_ARRAY header with WA flags and pointer-sized entries");
  const auto read = obj::elf::read_object(*file, input.name);
  check(read && *read == input, "round-trip an initializer pointer and its relocation");

  auto noncanonical = *file;
  set_field(noncanonical, header + 56, 8, 0);
  const auto read_zero = obj::elf::read_object(noncanonical, input.name);
  check(read_zero && *read_zero == input, "accept a zero .init_array sh_entsize");
  const auto reject = [&](std::size_t at, std::size_t width, std::uint64_t value) {
    auto bad = *file;
    set_field(bad, header + at, width, value);
    const auto result = obj::elf::read_object(bad, input.name);
    check(!result && result.error().contains(".init_array"), "reject malformed .init_array header");
  };
  reject(4, 4, 1);   // SHT_PROGBITS
  reject(8, 8, 2);   // SHF_ALLOC alone
  reject(32, 8, 7);  // Partial slot
  reject(48, 8, 4);  // Alignment
  reject(56, 8, 4);  // Entry size
  input.sections[init].align_log2 = 4;
  rejects("invalid ELF .init_array flags, alignment or size", input);
}

void read_gnu_marker() {
  // A small ET_REL with the forms used by the GNU-built libycxx linkage marker.
  enum { rodata = 1, debug_info, debug_str, comment, data, stack, rela_data, rela_debug, symtab, strtab, shstrtab };
  constexpr std::array<std::string_view, shstrtab + 1> names{
      "", ".rodata", ".debug_info", ".debug_str", ".comment", ".data.rel.ro.local", ".note.GNU-stack",
      ".rela.data.rel.ro.local", ".rela.debug_info", ".symtab", ".strtab", ".shstrtab"};
  std::vector<std::uint8_t> file(64);
  std::vector<std::uint8_t> headers(64 * names.size());
  std::vector<std::uint8_t> section_names{0};
  auto add = [&](std::size_t i, std::uint32_t type, std::uint64_t flags, std::vector<std::uint8_t> bytes,
                 std::uint64_t align = 1, std::uint64_t entsize = 0, std::uint32_t link = 0, std::uint32_t info = 0) {
    file.resize((file.size() + align - 1) & ~(align - 1));
    const auto offset = file.size();
    auto header = std::span(headers).subspan(64 * i, 64);
    set_field(header, 0, 4, section_names.size());
    section_names.insert(section_names.end(), names[i].begin(), names[i].end());
    section_names.push_back(0);
    set_field(header, 4, 4, type);
    set_field(header, 8, 8, flags);
    set_field(header, 24, 8, offset);
    set_field(header, 32, 8, bytes.size());
    set_field(header, 40, 4, link);
    set_field(header, 44, 4, info);
    set_field(header, 48, 8, align);
    set_field(header, 56, 8, entsize);
    file.insert(file.end(), bytes.begin(), bytes.end());
    return offset;
  };
  add(rodata, 1, 2, {7, 9});
  add(debug_info, 1, 0, std::vector<std::uint8_t>(12));
  add(debug_str, 1, 0x30, {'d', 0}, 1, 1);
  add(comment, 1, 0x30, {'c', 0}, 1, 1);
  add(data, 1, 3, std::vector<std::uint8_t>(16), 8);
  add(stack, 1, 0, {});

  std::vector<std::uint8_t> relas(48);
  auto set_rela = [&](std::size_t i, std::uint64_t site, std::uint32_t type, std::uint32_t symbol, std::uint64_t addend = 0) {
    auto record = std::span(relas).subspan(24 * i, 24);
    set_field(record, 0, 8, site);
    set_field(record, 8, 4, type);
    set_field(record, 12, 4, symbol);
    set_field(record, 16, 8, addend);
  };
  set_rela(0, 0, 1, 5);
  set_rela(1, 8, 1, 2, 1);
  const auto runtime_relocations = add(rela_data, 4, 0x40, relas, 8, 24, symtab, data);
  set_rela(0, 0, 10, 3);
  set_rela(1, 4, 1, 5);
  const auto debug_relocations = add(rela_debug, 4, 0x40, relas, 8, 24, symtab, debug_info);

  std::vector<std::uint8_t> symbols(6 * 24);
  std::vector<std::uint8_t> symbol_names{0};
  auto set_symbol = [&](std::size_t i, std::string_view name, std::uint8_t info, std::uint8_t visibility,
                        std::uint16_t section, std::uint64_t value, std::uint64_t size) {
    auto record = std::span(symbols).subspan(24 * i, 24);
    if (!name.empty()) {
      set_field(record, 0, 4, symbol_names.size());
      symbol_names.insert(symbol_names.end(), name.begin(), name.end());
      symbol_names.push_back(0);
    }
    record[4] = info;
    record[5] = visibility;
    set_field(record, 6, 2, section);
    set_field(record, 8, 8, value);
    set_field(record, 16, 8, size);
  };
  set_symbol(1, "marker.cpp", 4, 0, 0xfff1, 0, 0);  // local FILE with SHN_ABS
  set_symbol(2, "", 3, 0, rodata, 0, 0);           // retained SECTION
  set_symbol(3, "", 3, 0, debug_str, 0, 0);        // discarded SECTION
  set_symbol(4, "anchor", 1, 0, data, 0, 16);      // local OBJECT
  set_symbol(5, "marker", 0x11, 2, rodata, 1, 1);  // hidden global OBJECT
  const auto symbol_table = add(symtab, 2, 0, std::move(symbols), 8, 24, strtab, 5);
  add(strtab, 3, 0, std::move(symbol_names));
  add(shstrtab, 3, 0, {});
  set_field(headers, shstrtab * 64 + 32, 8, section_names.size());
  file.insert(file.end(), section_names.begin(), section_names.end());

  file.resize((file.size() + 7) & ~std::size_t{7});
  const auto shoff = file.size();
  file.insert(file.end(), headers.begin(), headers.end());
  const std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  std::ranges::copy(ident, file.begin());
  set_field(file, 16, 2, 1);
  set_field(file, 18, 2, 62);
  set_field(file, 20, 4, 1);
  set_field(file, 40, 8, shoff);
  set_field(file, 52, 2, 64);
  set_field(file, 58, 2, 64);
  set_field(file, 60, 2, names.size());
  set_field(file, 62, 2, shstrtab);

  obj::object expected;
  expected.name = "marker.o";
  expected.arch = obj::target::x86_64_linux;
  const auto read_only = expected.add_section(obj::standard_section::rodata);
  const auto pointers = expected.add_section(obj::standard_section::data_const);
  expected.sections[read_only].emit(std::array<std::uint8_t, 2>{7, 9});
  const auto section_base = expected.define("", obj::binding::local, read_only, 0);
  expected.define("anchor", obj::binding::local, pointers);
  const auto marker = expected.define("marker", obj::binding::hidden, read_only, 1);
  auto& pointer_data = expected.sections[pointers];
  pointer_data.emit64(0);
  pointer_data.relocate(obj::reloc_kind::abs64, marker);
  pointer_data.emit64(0);
  pointer_data.relocate(obj::reloc_kind::abs64, section_base, 1);
  auto read = obj::elf::read_object(file, expected.name);
  check(read && *read == expected, read ? "GNU ELF keeps runtime section and symbol indices" : read.error());

  auto reject = [&](std::size_t offset, std::size_t width, std::uint64_t value, std::string_view reason) {
    auto bad = file;
    set_field(bad, offset, width, value);
    const auto result = obj::elf::read_object(bad, expected.name);
    check(!result && result.error().contains(reason), reason);
  };
  reject(shoff + 64 * data + 8, 8, 7, "unsupported ELF section '.data.rel.ro.local'");
  reject(shoff + 64 * debug_info + 8, 8, 2, "unsupported ELF section '.debug_info'");
  reject(shoff + 64 * comment + 8, 8, 0x31, "unsupported ELF section '.comment'");
  reject(shoff + 64 * rela_debug + 40, 4, strtab, "invalid ELF relocation section");
  reject(symbol_table + 24 + 6, 2, rodata, "invalid ELF file symbol");
  reject(symbol_table + 3 * 24 + 6, 2, shstrtab, "unsupported section index");
  reject(symbol_table + 5 * 24 + 16, 8, 2, "out-of-section value or size");
  reject(debug_relocations, 8, 9, "ELF relocation lies outside its section");
  reject(debug_relocations + 12, 4, 6, "invalid ELF relocation symbol");
  reject(runtime_relocations + 8, 4, 10, "unsupported x86-64 relocation type");
  reject(runtime_relocations + 12, 4, 1, "invalid ELF relocation symbol");  // FILE
  reject(runtime_relocations + 12, 4, 3, "invalid ELF relocation symbol");  // DWARF section
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
  change(symtab + 24 + 4, 1, 6, "unsupported type, size, binding or visibility");  // STT_TLS
  change(symtab + 24 + 5, 1, 3, "unsupported type, size, binding or visibility");
  change(symtab + 24 + 6, 2, 0xfff2, "unsupported section index");
  change(symtab + 24 + 8, 8, 9, "out-of-section value");
  change(rela, 8, std::uint64_t{1} << 32, "relocation offset exceeds 32 bits");
  change(rela, 8, 17, "relocation lies outside its section");
  change(rela + 8, 4, 10, "unsupported x86-64 relocation type");
  change(rela + 12, 4, 0, "invalid ELF relocation symbol");
  change(rela + 12, 4, 8, "invalid ELF relocation symbol");
  change(rela + 24, 8, 5, "composed ELF relocations");

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
  read_shared_names();
  read_function_symbols();
  rtti_name_group();
  rtti_data_group();
  read_init_array();
  read_gnu_marker();

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
