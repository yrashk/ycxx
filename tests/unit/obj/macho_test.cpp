// ARM64 Mach-O relocation pairs (Apple's published relocation_info and ARM64 relocation definitions).
import std;
import ycxx.obj;
import ycxx.obj.macho;
import ycxx.unit;

namespace {

using ycxx::unit::check;
namespace obj = ycxx::obj;
namespace mo = obj::macho;

void test_subtractor_pairs() {
  obj::object before;
  before.name = "difference.o";
  const auto data = before.add_section(obj::standard_section::data);
  const auto left = before.define("_left", obj::binding::global, data, 0);
  const auto right = before.define("_right", obj::binding::local, data, 8);
  auto& section = before.sections[data];
  section.emit64(0);
  section.relocate(obj::reloc_kind::abs64, left, -13, std::nullopt, right);
  section.emit32(0);
  section.relocate(obj::reloc_kind::abs32, left, -7, std::nullopt, right);
  section.emit64(0);
  section.relocate(obj::reloc_kind::abs64, left);

  const auto bytes = mo::write_object(before);
  const auto read = mo::read_object(bytes, before.name);
  check(read.has_value(), "valid 32-bit and 64-bit SUBTRACTOR pairs read");
  if (read) {
    const auto& relocs = read->sections.front().relocs;
    check(relocs.size() == 3, "each pair normalizes to one relocation without consuming the following relocation");
    if (relocs.size() == 3) {
      auto difference = [&](const obj::reloc& r, obj::reloc_kind kind, std::uint32_t offset, std::int64_t addend) {
        return r.kind == kind && r.offset == offset && r.addend == addend && r.minus &&
               read->symbols[r.symbol].name_view() == "_left" && read->symbols[*r.minus].name_view() == "_right";
      };
      check(difference(relocs[0], obj::reloc_kind::abs64, 0, -13), "64-bit difference preserves both symbols and its addend");
      check(difference(relocs[1], obj::reloc_kind::abs32, 8, -7), "32-bit difference preserves both symbols and its addend");
      check(!relocs[2].minus && relocs[2].offset == 12 && relocs[2].kind == obj::reloc_kind::abs64,
            "the independent UNSIGNED relocation stays independent");
    }
  }

  // The writer produces one LC_SEGMENT_64 with one section. In that section header,
  // reloff and nreloc are at byte offsets 56 and 60. Each relocation_info occupies 8 bytes.
  constexpr auto header = mo::mach_header_64_size + mo::segment_command_64_size;
  const auto relocations = mo::load32(bytes, header + 56);
  check(mo::load32(bytes, header + 60) == 5, "two raw pairs and one UNSIGNED relocation are available for mutation");
  const auto subtractor_address = relocations;
  const auto subtractor_word = relocations + 4;
  const auto unsigned_word = relocations + 12;
  constexpr std::uint32_t pcrel = 1u << 24;
  constexpr std::uint32_t length_mask = 3u << 25;
  constexpr std::uint32_t type_mask = 15u << 28;
  auto reject = [&](std::uint32_t field, std::uint32_t value, std::string_view label) {
    auto changed = bytes;
    mo::store32(changed, field, value);
    const auto result = mo::read_object(changed, before.name);
    check(!result && result.error().contains("SUBTRACTOR"), label);
  };
  reject(subtractor_address, 4, "the records must have the same address");
  reject(subtractor_word, (mo::load32(bytes, subtractor_word) & ~length_mask) | (2u << 25),
         "the records must have the same width");
  reject(subtractor_word, mo::load32(bytes, subtractor_word) | pcrel, "the SUBTRACTOR record cannot be PC-relative");
  reject(unsigned_word, mo::load32(bytes, unsigned_word) | pcrel, "the paired UNSIGNED record cannot be PC-relative");
  reject(unsigned_word, (mo::load32(bytes, unsigned_word) & ~type_mask) |
                            (std::to_underlying(mo::reloc_type::subtractor) << 28),
         "another SUBTRACTOR cannot interrupt the pair");
  reject(header + 60, 1, "a trailing SUBTRACTOR is rejected");
}

void test_got_relative_addends() {
  obj::object before;
  before.name = "got-relative.o";
  const auto data = before.add_section(obj::standard_section::data);
  before.sections[data].emit64(0);
  const auto constants = before.add_section(obj::standard_section::rodata);
  const auto symbol = before.reference("_target");
  auto& section = before.sections[constants];
  section.emit32(0x11223344);

  // The wire word includes minus the field offset, even when the section's base is nonzero.
  const struct {
    std::uint32_t offset;
    std::int64_t addend;
    std::int32_t wire;
  } cases[] = {{4, 0, -4}, {8, 11, 3}, {19, -9, -28}};
  for (const auto& c : cases) {
    section.bytes.resize(c.offset);
    section.emit32(0);
    section.relocate(obj::reloc_kind::got_rel32, symbol, c.addend);
  }

  auto bytes = mo::write_object(before);
  const auto header = mo::mach_header_64_size + mo::segment_command_64_size + constants * mo::section_64_size;
  check(mo::load64(bytes, header + 32) != 0, "the constants have a nonzero object address");
  const auto payload = mo::load32(bytes, header + 48);
  for (const auto& c : cases) {
    const auto wire = static_cast<std::uint32_t>(c.wire);
    check(mo::load32(bytes, payload + c.offset) == wire, "POINTER_TO_GOT writes the signed addend minus the field offset");
    mo::store32(bytes, payload + c.offset, wire);  // supply the expected wire independently of the writer
  }

  const auto read = mo::read_object(bytes, before.name);
  check(read.has_value(), "valid POINTER_TO_GOT records read");
  if (!read) return;
  const auto& decoded = read->sections[constants];
  check(decoded.bytes == section.bytes, "the reader clears the implicit addend words");
  check(std::ranges::equal(cases, decoded.relocs, [&](const auto& c, const obj::reloc& r) {
    return r.offset == c.offset && r.kind == obj::reloc_kind::got_rel32 && r.addend == c.addend &&
           read->symbols[r.symbol].name_view() == "_target";
  }), "POINTER_TO_GOT restores the canonical addends");
}

}  // namespace

int main() {
  test_subtractor_pairs();
  test_got_relative_addends();
  return ycxx::unit::result("unit.macho_obj");
}
