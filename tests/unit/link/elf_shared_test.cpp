// Program headers and dynamic table records, independent of ELF section headers.
import std;
import ycxx.link.elf.shared;
import ycxx.unit;

using ycxx::unit::check;
namespace elf = ycxx::link::elf;

namespace {

using image = std::vector<std::uint8_t>;
constexpr std::size_t ph_load = 64, ph_dynamic = 120, dynamic = 176, hash = 288, symbols = 312, strings = 408;
constexpr std::uint64_t base = 0x2000;

void put(image& out, std::size_t offset, std::size_t width, std::uint64_t value) {
  for (std::size_t i = 0; i < width; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

image fixture() {
  image out{0x7f, 'E', 'L', 'F', 2, 1, 1};
  out.resize(512);
  put(out, 16, 2, 3);   // ET_DYN
  put(out, 18, 2, 62);  // EM_X86_64
  put(out, 20, 4, 1);   // EV_CURRENT
  put(out, 32, 8, ph_load);
  put(out, 52, 2, 64);
  put(out, 54, 2, 56);
  put(out, 56, 2, 2);
  put(out, ph_load, 4, 1);         // PT_LOAD
  put(out, ph_load + 4, 4, 5);     // PF_R | PF_X
  put(out, ph_load + 16, 8, base);
  put(out, ph_load + 32, 8, out.size());
  put(out, ph_load + 40, 8, out.size());
  put(out, ph_load + 48, 8, 4096);
  put(out, ph_dynamic, 4, 2);  // PT_DYNAMIC
  put(out, ph_dynamic + 8, 8, dynamic);
  put(out, ph_dynamic + 16, 8, base + dynamic);
  put(out, ph_dynamic + 32, 8, hash - dynamic);
  put(out, ph_dynamic + 40, 8, hash - dynamic);

  constexpr char names[] = "\0libleaf.so.1\0function\0data\0unused";
  constexpr std::array tags{std::pair{4, base + hash}, std::pair{5, base + strings},
                            std::pair{6, base + symbols}, std::pair{10, std::uint64_t{sizeof(names)}},
                            std::pair{11, std::uint64_t{24}}, std::pair{14, std::uint64_t{1}}};
  for (std::size_t i = 0; i < tags.size(); ++i) {
    put(out, dynamic + 16 * i, 8, tags[i].first);
    put(out, dynamic + 16 * i + 8, 8, tags[i].second);
  }
  std::ranges::copy(names, out.data() + strings);
  put(out, hash, 4, 1);       // one bucket, starting at symbol 1
  put(out, hash + 4, 4, 4);   // four symbols, including STN_UNDEF
  put(out, hash + 8, 4, 1);
  put(out, hash + 16, 4, 2);  // symbol 1 points to symbol 2
  put(out, hash + 20, 4, 3);  // symbol 2 points to symbol 3
  constexpr auto function = symbols + 24, data = symbols + 48, unused = symbols + 72;
  put(out, function, 4, 14);
  put(out, function + 4, 1, 0x12);  // STB_GLOBAL/STT_FUNC
  put(out, function + 6, 2, 1);
  put(out, function + 8, 8, base + 504);
  put(out, function + 16, 8, 1);
  put(out, data, 4, 23);
  put(out, data + 4, 1, 0x11);  // STB_GLOBAL/STT_OBJECT
  put(out, data + 6, 2, 2);
  put(out, data + 8, 8, base + 505);
  put(out, unused, 4, 28);
  put(out, unused + 4, 1, 0x22);  // an undefined weak function
  return out;
}

void rejects(image out, std::size_t offset, std::size_t width, std::uint64_t value, std::string_view reason) {
  put(out, offset, width, value);
  const auto result = elf::read_shared_object(out, "leaf.so");
  check(!result && result.error().contains(reason), reason);
}

}  // namespace

int main() {
  auto input = fixture();
  const auto result = elf::read_shared_object(input, "leaf.so");
  check(result.has_value(), result ? "read a sectionless shared object" : result.error());
  if (result) {
    check(result->name == "leaf.so" && result->soname == "libleaf.so.1", "record input name and runtime SONAME");
    check(result->exports.size() == 2 && result->exports[0].name == "data" && !result->exports[0].supported_function &&
          result->exports[1].name == "function" && result->exports[1].supported_function,
          "record visible definitions and distinguish supported functions");
    input[504] = 0xc3;  // ret, in the function body outside the metadata
    const auto changed = elf::read_shared_object(input, "leaf.so");
    check(changed && changed->contents != result->contents, "fingerprint includes bytes outside the metadata");
  }

  rejects(input, 16, 2, 2, "expected a little-endian x86-64 ELF shared object");
  rejects(input, 32, 8, std::numeric_limits<std::uint64_t>::max(), "program header table");
  rejects(input, ph_load + 40, 8, std::numeric_limits<std::uint64_t>::max(), "segment bounds");
  rejects(input, ph_dynamic, 4, 3, "position-independent executable");
  rejects(input, ph_dynamic + 16, 8, base + dynamic + 1, "mapped PT_DYNAMIC");
  rejects(input, dynamic + 96, 8, 99, "no DT_NULL terminator");
  // A SysV fixture relabeled as GNU has no room for the GNU header, Bloom word and bucket.
  rejects(input, dynamic, 8, 0x6ffffef5, "DT_GNU_HASH dimensions");
  rejects(input, dynamic, 8, 1, "dependencies are unsupported");
  rejects(input, dynamic, 8, 0x6ffffff0, "versioned ELF shared object symbols");
  rejects(input, dynamic + 72, 8, 16, "symbol table metadata");
  rejects(input, dynamic + 88, 8, 99, "DT_SONAME is outside the string table");
  rejects(input, strings + 1, 1, '/', "nonempty and slash-free");
  rejects(input, dynamic + 24, 8, base + input.size(), "mapped ELF shared object string table");
  rejects(input, dynamic + 56, 8, 13, "mapped ELF shared object string table");
  rejects(input, dynamic + 40, 8, base + input.size() - 1, "mapped ELF shared object symbol table");
  rejects(input, hash + 4, 4, 0xffffffff, "DT_HASH dimensions");
  rejects(input, hash + 8, 4, 4, "DT_HASH symbol index");
  rejects(input, hash + 16, 4, 1, "DT_HASH chain");
  rejects(input, symbols + 24, 4, 99, "symbol name is outside the string table");
  rejects(input, symbols + 32, 8, base + input.size(), "outside file-backed executable memory");
  return ycxx::unit::result("unit.elf_shared");
}
