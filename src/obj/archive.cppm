// Regular ar containers, from ar(5): SVR4/GNU for ELF links, and BSD with Darwin's `__.SYMDEF`
// ranlib index (<mach-o/ranlib.h>) for Mach-O links. The reader leaves member payloads opaque.
// The writer produces GNU archives; it uses the x86-64 ELF object reader to validate members and
// build the symbol index.
export module ycxx.obj.archive;

import std;
import ycxx.engine;
import ycxx.hash;
import ycxx.obj;
import ycxx.obj.elf;

export namespace ycxx::obj::archive {

struct member {
  std::size_t offset;  // absolute offset of the member header
  std::string_view name, data;
};

// All views refer to the input bytes. The index maps each symbol to a physical member number.
struct contents {
  std::vector<member> members;
  std::multimap<std::string_view, std::size_t, std::less<>> symbols;
};

// The container a link expects. A GNU archive has a `/` index and `//` long names; a Darwin one
// has a `__.SYMDEF` (or `__.SYMDEF SORTED`, or their `_64` forms) index as its first member and
// BSD `#1/<length>` names stored before the member's contents.
enum class flavor : std::uint8_t { gnu, darwin };

std::expected<contents, std::string> read(std::string_view bytes, std::string_view name, flavor kind = flavor::gnu);

// An archive operand of a link: its name for diagnostics and its bytes, read at driver ingress.
// The bytes are shared and immutable, and their fingerprint is computed once, so the libycxx
// runtime every Darwin link names (#842) is held once per process, not once per program.
class input {
public:
  input(std::string name, std::shared_ptr<const std::string> bytes)
    pre(bytes != nullptr)
    : name(std::move(name)), bytes_(std::move(bytes)), content_(fingerprint128(*bytes_)) {}
  input(std::string name, std::string bytes) : input(std::move(name), std::make_shared<const std::string>(std::move(bytes))) {}

  std::string name;
  std::string_view bytes() const noexcept { return *bytes_; }
  // The fingerprint of the bytes, which with the name is the input's identity.
  Fp128 content() const noexcept { return content_; }

private:
  std::shared_ptr<const std::string> bytes_;
  Fp128 content_;
};

void hash_into(Hasher& h, const input& in) {
  h.uint(static_cast<std::uint64_t>(in.name.size())).bytes(in.name).fp(in.content());
}
void serialize_into(engine::Sink& out, const input& in) {
  engine::serialize(out, in.name);
  engine::serialize(out, std::string(in.bytes()));
}
input deserialize_from(engine::Source& in, std::type_identity<input>) {
  auto name = engine::deserialize<std::string>(in);
  return input(std::move(name), engine::deserialize<std::string>(in));
}

inline constexpr std::uint64_t max_member_size = 9'999'999'999;  // 10 decimal digits

struct member_input {
  std::string_view path, data;
};

std::expected<std::string, std::string> write(std::span<const member_input> inputs);

}  // namespace ycxx::obj::archive

namespace ycxx::obj::archive {
namespace {

constexpr std::string_view magic = "!<arch>\n";
constexpr std::size_t header_size = 60, name_size = 16, word_size = 4;

std::string_view trim_padding(std::string_view field) {
  while (field.ends_with(' ')) field.remove_suffix(1);
  return field;
}

std::optional<std::size_t> decimal(std::string_view field) {
  std::size_t value = 0;
  const auto end = field.data() + field.size();
  const auto [parsed, ec] = std::from_chars(field.data(), end, value);
  if (ec != std::errc{} || parsed != end) return std::nullopt;
  return value;
}

std::uint32_t big_word(std::string_view field) {
  std::uint32_t value = 0;
  for (unsigned char byte : field) value = (value << 8) | byte;
  return value;
}

bool symdef_name(std::string_view name) {
  return name == "__.SYMDEF" || name == "__.SYMDEF SORTED" || name == "__.SYMDEF_64" || name == "__.SYMDEF_64 SORTED";
}

bool bsd_member(std::string_view name) {
  if (name.starts_with("#1/")) return true;
  if (name.ends_with('/')) name.remove_suffix(1);
  return symdef_name(name);
}

template <std::unsigned_integral T>
T little(std::string_view field) {
  T value = 0;
  for (std::size_t i = field.size(); i-- > 0;) value = static_cast<T>(value << 8) | static_cast<unsigned char>(field[i]);
  return value;
}

// One member header and its contents, after the alignment checks both formats share.
struct raw_member {
  std::size_t offset;
  std::string_view field, data;
};

// Calls `visit(raw_member)` for each member in physical order; stops at its first error.
template <class Visit>
std::expected<void, std::string> each_member(std::string_view bytes, Visit visit) {
  for (std::size_t offset = magic.size(); offset < bytes.size();) {
    if (bytes.size() - offset < header_size) return std::unexpected("truncated archive member header");
    const auto header = bytes.substr(offset, header_size);
    if (header.substr(58) != "`\n") return std::unexpected("invalid archive member header trailer");
    const auto size = decimal(trim_padding(header.substr(48, 10)));
    if (!size) return std::unexpected("invalid archive member size");
    const auto start = offset + header_size;
    if (*size > bytes.size() - start) return std::unexpected("archive member contents exceed the file");
    const auto end = start + *size;
    if (*size % 2 != 0) {
      if (end == bytes.size()) return std::unexpected("missing archive member alignment padding");
      if (bytes[end] != '\n') return std::unexpected("invalid archive member alignment padding");
    }
    if (auto visited = visit(raw_member{offset, trim_padding(header.substr(0, name_size)), bytes.substr(start, *size)});
        !visited)
      return visited;
    offset = end + *size % 2;
  }
  return {};
}

// A Darwin archive: BSD names, and a ranlib index first. Index entries name member headers by
// their offset in the file, in the archive's byte order (little-endian for arm64).
std::expected<contents, std::string> read_darwin(std::string_view bytes) {
  contents result;
  std::optional<std::string_view> index;
  bool wide = false;
  auto walked = each_member(bytes, [&](raw_member m) -> std::expected<void, std::string> {
    auto member_name = m.field;
    if (m.field.starts_with("#1/")) {
      const auto length = decimal(m.field.substr(3));
      if (!length || *length > m.data.size()) return std::unexpected("invalid BSD archive member name length");
      member_name = m.data.substr(0, *length);
      while (member_name.ends_with('\0')) member_name.remove_suffix(1);
      m.data.remove_prefix(*length);
    } else if (m.field.starts_with('/')) {
      return std::unexpected("GNU archive indexes and long names are unsupported in Darwin links");
    }
    if (member_name.empty() || member_name.contains('\0')) return std::unexpected("invalid archive member name");
    if (symdef_name(member_name)) {
      if (index || !result.members.empty()) return std::unexpected("the archive symbol index is not the first member");
      index = m.data;
      wide = member_name.starts_with("__.SYMDEF_64");
      return {};
    }
    result.members.push_back({m.offset, member_name, m.data});
    return {};
  });
  if (!walked) return std::unexpected(std::move(walked.error()));
  if (!index) {
    if (!result.members.empty()) return std::unexpected("nonempty archive has no symbol index");
    return result;
  }

  // struct ranlib { uint32 ran_strx, ran_off; } or ranlib_64's 64-bit fields, after the array's
  // size in bytes; then the string table's size and the table.
  const std::size_t word = wide ? 8 : 4, entry = 2 * word;
  const auto take = [&](std::string_view field) {
    return wide ? little<std::uint64_t>(field) : std::uint64_t{little<std::uint32_t>(field)};
  };
  const auto table = *index;
  if (table.size() < word) return std::unexpected("truncated archive symbol index");
  const auto array_size = take(table.substr(0, word));
  if (array_size % entry != 0 || array_size > table.size() - word || table.size() - word - array_size < word)
    return std::unexpected("invalid archive symbol index size");
  const auto strings_at = word + array_size + word;
  const auto strings_size = take(table.substr(word + array_size, word));
  if (strings_size > table.size() - strings_at) return std::unexpected("invalid archive symbol string table size");
  const auto strings = table.substr(strings_at, strings_size);
  for (std::size_t at = word; at < word + array_size; at += entry) {
    const auto strx = take(table.substr(at, word));
    const auto offset = take(table.substr(at + word, word));
    if (strx >= strings.size()) return std::unexpected("archive index symbol name lies outside the string table");
    const auto symbol = strings.substr(strx, strings.find('\0', strx) - strx);
    if (strx + symbol.size() == strings.size()) return std::unexpected("unterminated archive index symbol");
    if (symbol.empty()) return std::unexpected("empty archive index symbol");
    const auto found = std::ranges::lower_bound(result.members, offset, {}, &member::offset);
    if (found == result.members.end() || found->offset != offset) return std::unexpected("invalid archive index member offset");
    result.symbols.emplace(symbol, static_cast<std::size_t>(found - result.members.begin()));
  }
  return result;
}

}  // namespace

std::expected<contents, std::string> read(std::string_view bytes, std::string_view name, flavor kind) {
  const auto fail = [&](std::string_view reason) {
    return std::unexpected(std::format("{}: {}", name, reason));
  };
  if (bytes.starts_with("!<thin>\n")) return fail("thin archives are unsupported");
  if (kind == flavor::darwin) {
    if (!bytes.starts_with(magic)) return fail("expected a Darwin archive");
    auto result = read_darwin(bytes);
    if (!result) return fail(result.error());
    return result;
  }
  if (!bytes.starts_with(magic)) return fail("expected a regular GNU archive");

  contents result;
  // BOOTSTRAP: -Wmaybe-uninitialized: `index` is a view and a flag, not std::optional. At -O3
  // GCC 16.2 reports the optional's payload as possibly uninitialized after `if (!index)`
  // (#748; GCC's known class of std::optional false positives, as PR 114253). Without the bug:
  //   std::optional<std::string_view> index, long_names;   // `if (!index)`, `index->size()`
  std::optional<std::string_view> long_names;
  std::string_view index;
  bool has_index = false;
  auto walked = each_member(bytes, [&](raw_member m) -> std::expected<void, std::string> {
    if (m.field == "/") {
      if (std::exchange(has_index, true)) return std::unexpected("duplicate archive symbol index");
      index = m.data;
    } else if (m.field == "//") {
      if (long_names) return std::unexpected("duplicate archive long-name table");
      long_names = m.data;
    } else if (m.field == "/SYM64/") {
      return std::unexpected("64-bit archive indexes are unsupported");
    } else if (bsd_member(m.field)) {
      return std::unexpected("BSD/Darwin archive indexes and inline names are unsupported");
    } else {
      result.members.push_back({m.offset, m.field, m.data});
    }
    return {};
  });
  if (!walked) return fail(walked.error());

  std::map<std::size_t, std::string_view> long_entries;
  if (long_names) {
    auto rest = *long_names;
    // GNU ar can include a final newline in the declared table size to make it even.
    while (!rest.empty() && rest != "\n") {
      const auto end = rest.find("/\n");
      if (end == 0 || end == std::string_view::npos || rest.substr(0, end).contains('\0'))
        return fail("invalid archive long-name table entry");
      long_entries.emplace(long_names->size() - rest.size(), rest.substr(0, end));
      rest.remove_prefix(end + 2);
    }
  }
  for (auto& member : result.members) {
    if (member.name.starts_with('/')) {
      const auto offset = decimal(member.name.substr(1));
      const auto entry = offset ? long_entries.find(*offset) : long_entries.end();
      if (entry == long_entries.end()) return fail("invalid archive long-name table offset");
      member.name = entry->second;
    } else {
      if (member.name.size() < 2 || !member.name.ends_with('/') || member.name.contains('\0'))
        return fail("invalid archive member name");
      member.name.remove_suffix(1);
    }
  }

  if (!has_index) {
    if (!result.members.empty()) return fail("nonempty archive has no symbol index");
    return result;
  }
  if (index.size() < word_size) return fail("truncated archive symbol count");
  const auto count = big_word(index.substr(0, word_size));
  if (count > (index.size() - word_size) / (word_size + 1)) return fail("invalid archive symbol count");
  auto names = index.substr(word_size + std::size_t{count} * word_size);
  for (std::size_t i = 0; i < count; ++i) {
    const auto offset = big_word(index.substr(word_size + i * word_size, word_size));
    const auto found = std::ranges::lower_bound(result.members, offset, {}, &member::offset);
    if (found == result.members.end() || found->offset != offset)
      return fail("invalid archive index member offset");
    const auto end = names.find('\0');
    if (end == std::string_view::npos) return fail("unterminated archive index symbol");
    if (end == 0) return fail("empty archive index symbol");
    result.symbols.emplace(names.substr(0, end), static_cast<std::size_t>(found - result.members.begin()));
    names.remove_prefix(end + 1);
  }
  if (names.find_first_not_of('\0') != std::string_view::npos) return fail("invalid archive index padding");
  return result;
}

namespace {

struct indexed_member {
  std::string field;
  std::string_view data;
  std::vector<symbol_name> symbols;
  std::uint32_t offset = 0;
};

constexpr std::uint64_t packed_size(std::uint64_t size) { return header_size + size + size % 2; }

void append_header(std::string& out, std::string_view field, std::uint64_t size, std::string_view mode)
  pre(field.size() <= name_size && size <= max_member_size)
{
  out.append(field).append(name_size - field.size(), ' ');
  std::format_to(std::back_inserter(out), "{:<12}{:<6}{:<6}{:<8}{:<10}`\n", 0, 0, 0, mode, size);
}

void append_word(std::string& out, std::uint32_t word) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(word >> shift));
}

void align_member(std::string& out, std::uint64_t size) {
  if (size % 2 != 0) out.push_back('\n');
}

void append_member(std::string& out, std::string_view field, std::string_view data, std::string_view mode) {
  append_header(out, field, data.size(), mode);
  out.append(data);
  align_member(out, data.size());
}

}  // namespace

std::expected<std::string, std::string> write(std::span<const member_input> inputs) {
  constexpr auto max_index_word = std::numeric_limits<std::uint32_t>::max();
  std::vector<indexed_member> members;
  members.reserve(inputs.size());
  std::string long_names;
  std::uint32_t count = 0;
  std::uint64_t index_size = word_size;
  for (const auto& input : inputs) {
    if (input.data.size() > max_member_size)
      return std::unexpected(std::format("{}: archive member size exceeds 10 decimal digits", input.path));
    auto name = input.path;
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    if (name.empty() || name.contains('\0') || name.contains('\n'))
      return std::unexpected(std::format("{}: cannot represent member basename in a GNU archive", input.path));

    indexed_member member{.field = {}, .data = input.data, .symbols = {}};
    if (name.size() >= name_size || name.find_first_of(" \t\r\v\f") != std::string_view::npos ||
        name == "#1" || bsd_member(name)) {
      const auto remaining = max_member_size - long_names.size();
      if (remaining < 2 || name.size() > remaining - 2)
        return std::unexpected("archive long-name table exceeds 10 decimal digits");
      member.field = std::format("/{}", long_names.size());
      long_names.append(name).append("/\n");
    } else {
      member.field = std::string(name).append("/");
    }

    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(input.data.data()), input.data.size());
    auto object = elf::read_object(bytes, std::string(input.path));
    if (!object) return std::unexpected(std::move(object.error()));
    if (!object->comdat_groups.empty())
      return std::unexpected(std::format("{}: ELF COMDAT groups are unsupported in native archive production", input.path));
    for (auto& symbol : object->symbols) {
      if (symbol.bind == binding::local) continue;
      const auto name_size = symbol.name_view().size();
      if (name_size == 0)
        return std::unexpected(std::format("{}: empty name for nonlocal ELF symbol", input.path));
      if (!symbol.defined()) continue;
      constexpr auto overhead = word_size + 1;  // offset and name terminator
      const auto remaining = max_member_size - index_size;
      if (count == max_index_word || remaining < overhead || name_size > remaining - overhead)
        return std::unexpected("archive symbol index exceeds the GNU 32-bit index format");
      ++count;
      index_size += overhead + name_size;
      member.symbols.push_back(std::move(symbol.name));
    }
    members.push_back(std::move(member));
  }

  // Layout includes every ordinary header, even for members with no index entry.
  // Each addend is bounded by the corresponding header's size field.
  auto size = magic.size() + packed_size(index_size);
  if (!long_names.empty()) size += packed_size(long_names.size());
  for (auto& member : members) {
    if (size > max_index_word) return std::unexpected("archive member header offset exceeds 32 bits");
    member.offset = static_cast<std::uint32_t>(size);
    size += packed_size(member.data.size());
  }

  std::string result;
  if (size > result.max_size()) return std::unexpected("archive exceeds the maximum output size");
  result.reserve(size);
  result.append(magic);
  append_header(result, "/", index_size, "0");
  append_word(result, count);
  for (const auto& member : members)
    for (std::size_t i = 0; i < member.symbols.size(); ++i) append_word(result, member.offset);
  for (const auto& member : members)
    for (const auto& symbol : member.symbols) result.append(symbol.view()).push_back('\0');
  align_member(result, index_size);
  if (!long_names.empty()) append_member(result, "//", long_names, "0");
  for (const auto& member : members) append_member(result, member.field, member.data, "644");
  return result;
}

}  // namespace ycxx::obj::archive
