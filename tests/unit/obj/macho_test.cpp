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

}  // namespace

int main() {
  test_subtractor_pairs();
  return ycxx::unit::result("unit.macho_obj");
}
