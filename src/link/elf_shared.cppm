// Dynamic shared-object metadata for the Linux x86-64 linker, from the ELF gABI and AMD64 psABI.
export module ycxx.link.elf.shared;

import std;
import ycxx.hash;

export namespace ycxx::link::elf {

struct shared_symbol {
  std::string name;
  bool supported_function;
};

struct shared_object {
  std::string name;
  std::string soname;
  std::vector<shared_symbol> exports;  // sorted by name, including unsupported visible definitions
  Fp128 contents;
};

std::expected<shared_object, std::string> read_shared_object(std::span<const std::uint8_t> file,
                                                           std::string_view name);

}  // namespace ycxx::link::elf

namespace ycxx::link::elf {
namespace {

using byte_view = std::span<const std::uint8_t>;
constexpr std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
constexpr std::size_t header_size = 64, program_header_size = 56, dynamic_size = 16, symbol_size = 24;
constexpr std::uint16_t et_dyn = 3, em_x86_64 = 62, pn_xnum = 0xffff, shn_loreserve = 0xff00;
constexpr std::uint32_t pt_load = 1, pt_dynamic = 2, pt_interp = 3, pf_x = 1;
constexpr std::uint8_t stb_global = 1, stb_weak = 2, stb_gnu_unique = 10, stt_func = 2;
constexpr std::uint8_t stv_default = 0, stv_protected = 3;
constexpr std::uint64_t dt_null = 0, dt_needed = 1, dt_hash = 4, dt_strtab = 5, dt_symtab = 6;
constexpr std::uint64_t dt_strsz = 10, dt_syment = 11, dt_soname = 14, dt_gnu_hash = 0x6ffffef5;
constexpr std::uint64_t dt_versym = 0x6ffffff0, dt_verdef = 0x6ffffffc, dt_verdefnum = 0x6ffffffd;
constexpr std::uint64_t dt_verneed = 0x6ffffffe, dt_verneednum = 0x6fffffff;
constexpr std::uint64_t dt_flags_1 = 0x6ffffffb, df_1_pie = 0x08000000;
constexpr std::uint64_t dt_auxiliary = 0x7ffffffd, dt_filter = 0x7fffffff;

// The caller first bounds the whole record containing the field.
template <std::unsigned_integral T>
T field(byte_view record, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) value |= std::uint64_t{record[offset + i]} << (8 * i);
  return static_cast<T>(value);
}

std::optional<byte_view> file_range(byte_view file, std::uint64_t offset, std::uint64_t size) {
  if (offset > file.size() || size > file.size() - offset) return std::nullopt;
  return file.subspan(offset, size);
}

struct segment {
  std::uint64_t address, offset, file_size;
  std::uint32_t flags;
};

std::optional<std::uint64_t> mapped_offset(std::span<const segment> loads, std::uint64_t address,
                                           std::uint64_t size, std::uint32_t flags = 0) {
  std::optional<std::uint64_t> result;
  for (const auto& load : loads) {
    if (address < load.address || (load.flags & flags) != flags) continue;
    const auto delta = address - load.address;
    if (delta > load.file_size || size > load.file_size - delta) continue;
    const auto offset = load.offset + delta;
    if (result && *result != offset) return std::nullopt;
    result = offset;
  }
  return result;
}

// ELF gABI, "Hash Table". Bucket membership matters because the loader follows these chains.
std::uint32_t sysv_hash(std::string_view name) {
  std::uint32_t h = 0;
  for (unsigned char c : name) {
    h = (h << 4) + c;
    h ^= (h & 0xf0000000) >> 24;
    h &= 0x0fffffff;
  }
  return h;
}

}  // namespace

std::expected<shared_object, std::string> read_shared_object(byte_view file, std::string_view name) {
  const auto error = [&](std::string_view message) {
    return std::unexpected(std::format("{}: {}", name.empty() ? "ELF shared input" : name, message));
  };
  if (file.size() < header_size || !std::ranges::equal(file.first(ident.size()), ident) ||
      field<std::uint16_t>(file, 16) != et_dyn || field<std::uint16_t>(file, 18) != em_x86_64 ||
      field<std::uint32_t>(file, 20) != 1 || field<std::uint16_t>(file, 52) != header_size)
    return error("expected a little-endian x86-64 ELF shared object (ET_DYN)");

  const auto ph_count = field<std::uint16_t>(file, 56);
  const auto headers = file_range(file, field<std::uint64_t>(file, 32), ph_count * program_header_size);
  if (!headers || ph_count == 0 || ph_count == pn_xnum || field<std::uint16_t>(file, 54) != program_header_size)
    return error("invalid ELF shared object program header table");

  std::vector<segment> loads;
  std::optional<segment> dynamic;
  for (std::size_t i = 0; i < ph_count; ++i) {
    const auto ph = headers->subspan(i * program_header_size, program_header_size);
    const auto type = field<std::uint32_t>(ph, 0);
    if (type == pt_interp) return error("a position-independent executable cannot be an ELF shared input");
    if (type != pt_load && type != pt_dynamic) continue;
    const segment s{field<std::uint64_t>(ph, 16), field<std::uint64_t>(ph, 8), field<std::uint64_t>(ph, 32),
                    field<std::uint32_t>(ph, 4)};
    const auto memory_size = field<std::uint64_t>(ph, 40);
    const auto align = field<std::uint64_t>(ph, 48);
    if (!file_range(file, s.offset, s.file_size) || s.file_size > memory_size ||
        memory_size > std::numeric_limits<std::uint64_t>::max() - s.address ||
        (align > 1 && (!std::has_single_bit(align) || s.offset % align != s.address % align)))
      return error("invalid ELF shared object segment bounds or alignment");
    if (type == pt_load) loads.push_back(s);
    else if (dynamic) return error("multiple PT_DYNAMIC segments in ELF shared object");
    else dynamic = s;
  }
  if (!dynamic || dynamic->file_size == 0 || dynamic->file_size % dynamic_size != 0 ||
      mapped_offset(loads, dynamic->address, dynamic->file_size) != dynamic->offset)
    return error("missing or invalid mapped PT_DYNAMIC in ELF shared object");

  const auto table = file.subspan(dynamic->offset, dynamic->file_size);
  std::map<std::uint64_t, std::uint64_t> tags;
  bool terminated = false;
  for (std::size_t offset = 0; offset < table.size(); offset += dynamic_size) {
    const auto entry = table.subspan(offset, dynamic_size);
    const auto tag = field<std::uint64_t>(entry, 0);
    if (tag == dt_null) {
      terminated = true;
      break;
    }
    const auto value = field<std::uint64_t>(entry, 8);
    switch (tag) {
      case dt_flags_1:
        if ((value & df_1_pie) != 0) return error("a position-independent executable cannot be an ELF shared input");
        break;
      case dt_needed:
      case dt_auxiliary:
      case dt_filter: return error("ELF shared object dependencies are unsupported");
      case dt_versym:
      case dt_verdef:
      case dt_verdefnum:
      case dt_verneed:
      case dt_verneednum: return error("versioned ELF shared object symbols are unsupported");
      case dt_hash:
      case dt_gnu_hash:
      case dt_strtab:
      case dt_strsz:
      case dt_symtab:
      case dt_syment:
      case dt_soname:
        if (!tags.try_emplace(tag, value).second)
          return error("duplicate ELF shared object dynamic table entry");
        break;
      default: break;
    }
  }
  if (!terminated) return error("ELF shared object dynamic table has no DT_NULL terminator");
  if (!tags.contains(dt_hash)) {
    if (tags.contains(dt_gnu_hash)) return error("GNU-hash-only ELF shared objects are unsupported (need DT_HASH)");
    return error("ELF shared object is missing DT_HASH");
  }
  if (!tags.contains(dt_strtab) || !tags.contains(dt_strsz) || !tags.contains(dt_symtab) || !tags.contains(dt_syment) ||
      tags.at(dt_syment) != symbol_size)
    return error("missing or invalid ELF shared object string or symbol table metadata");
  if (!tags.contains(dt_soname)) return error("ELF shared object is missing DT_SONAME");

  const auto mapped = [&](std::uint64_t address, std::uint64_t size) -> std::optional<byte_view> {
    const auto offset = mapped_offset(loads, address, size);
    if (!offset) return std::nullopt;
    return file.subspan(*offset, size);
  };
  const auto strings = mapped(tags.at(dt_strtab), tags.at(dt_strsz));
  if (!strings || strings->empty() || strings->front() != 0 || strings->back() != 0)
    return error("invalid mapped ELF shared object string table");
  const std::string_view strtab(reinterpret_cast<const char*>(strings->data()), strings->size());
  const auto string_at = [&](std::size_t offset) {
    return strtab.substr(offset, strtab.find('\0', offset) - offset);
  };
  const auto soname_offset = tags.at(dt_soname);
  if (soname_offset >= strtab.size()) return error("ELF shared object DT_SONAME is outside the string table");
  const auto soname = string_at(soname_offset);
  if (soname.empty() || soname.contains('/')) return error("ELF shared object DT_SONAME must be nonempty and slash-free");

  const auto hash_header = mapped(tags.at(dt_hash), 8);
  if (!hash_header) return error("invalid mapped ELF shared object DT_HASH header");
  const std::uint64_t bucket_count = field<std::uint32_t>(*hash_header, 0);
  const std::uint64_t symbol_count = field<std::uint32_t>(*hash_header, 4);
  const auto hash = mapped(tags.at(dt_hash), 8 + 4 * (bucket_count + symbol_count));
  if (!hash || bucket_count == 0 || symbol_count == 0) return error("invalid ELF shared object DT_HASH dimensions");
  const auto symbols = mapped(tags.at(dt_symtab), symbol_count * symbol_size);
  if (!symbols || !std::ranges::all_of(symbols->first(symbol_size), [](auto byte) { return byte == 0; }))
    return error("invalid mapped ELF shared object symbol table");
  for (std::uint64_t i = 0; i < bucket_count + symbol_count; ++i) {
    if (field<std::uint32_t>(*hash, 8 + 4 * i) >= symbol_count)
      return error("invalid ELF shared object DT_HASH symbol index");
  }
  for (std::uint64_t i = 1; i < symbol_count; ++i) {
    if (field<std::uint32_t>(*symbols, i * symbol_size) >= strtab.size())
      return error("ELF shared object symbol name is outside the string table");
  }

  shared_object result{std::string(name), std::string(soname), {}, fingerprint128(file)};
  std::vector<std::uint8_t> visited(symbol_count);
  for (std::uint64_t bucket = 0; bucket < bucket_count; ++bucket) {
    auto index = field<std::uint32_t>(*hash, 8 + 4 * bucket);
    for (; index != 0; index = field<std::uint32_t>(*hash, 8 + 4 * (bucket_count + index))) {
      if (std::exchange(visited[index], 1)) return error("invalid ELF shared object DT_HASH chain");
      const auto symbol = symbols->subspan(std::uint64_t{index} * symbol_size, symbol_size);
      const auto symbol_name = string_at(field<std::uint32_t>(symbol, 0));
      if (sysv_hash(symbol_name) % bucket_count != bucket)
        return error("ELF shared object symbol is in the wrong DT_HASH bucket");
      const auto bind = symbol[4] >> 4;
      const auto type = symbol[4] & 0xf;
      const auto visibility = symbol[5] & 3;
      const auto section = field<std::uint16_t>(symbol, 6);
      if (symbol_name.empty() || section == 0 || (bind != stb_global && bind != stb_weak && bind != stb_gnu_unique) ||
          (visibility != stv_default && visibility != stv_protected)) continue;
      const bool function = bind == stb_global && type == stt_func && visibility == stv_default && section < shn_loreserve;
      if (function && !mapped_offset(loads, field<std::uint64_t>(symbol, 8),
                                     std::max<std::uint64_t>(field<std::uint64_t>(symbol, 16), 1), pf_x))
        return error("ELF shared object function is outside file-backed executable memory");
      result.exports.push_back({std::string(symbol_name), function});
    }
  }
  std::ranges::stable_sort(result.exports, {}, &shared_symbol::name);
  return result;
}

}  // namespace ycxx::link::elf
