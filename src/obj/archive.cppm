// Regular SVR4/GNU ar containers, from ar(5). The reader leaves member payloads opaque.
export module ycxx.obj.archive;

import std;

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

std::expected<contents, std::string> read(std::string_view bytes, std::string_view name);

}  // namespace ycxx::obj::archive

namespace ycxx::obj::archive {
namespace {

constexpr std::size_t header_size = 60, word_size = 4;

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

bool bsd_member(std::string_view name) {
  if (name.starts_with("#1/")) return true;
  if (name.ends_with('/')) name.remove_suffix(1);
  return name == "__.SYMDEF" || name == "__.SYMDEF SORTED" ||
         name == "__.SYMDEF_64" || name == "__.SYMDEF_64 SORTED";
}

}  // namespace

std::expected<contents, std::string> read(std::string_view bytes, std::string_view name) {
  const auto fail = [&](std::string_view reason) {
    return std::unexpected(std::format("{}: {}", name, reason));
  };
  constexpr std::string_view magic = "!<arch>\n";
  if (bytes.starts_with("!<thin>\n")) return fail("thin archives are unsupported");
  if (!bytes.starts_with(magic)) return fail("expected a regular GNU archive");

  contents result;
  std::optional<std::string_view> index, long_names;
  for (std::size_t offset = magic.size(); offset < bytes.size();) {
    if (bytes.size() - offset < header_size) return fail("truncated archive member header");
    const auto header = bytes.substr(offset, header_size);
    if (header.substr(58) != "`\n") return fail("invalid archive member header trailer");
    const auto field = trim_padding(header.substr(0, 16));
    const auto size = decimal(trim_padding(header.substr(48, 10)));
    if (!size) return fail("invalid archive member size");
    const auto start = offset + header_size;
    if (*size > bytes.size() - start) return fail("archive member contents exceed the file");
    const auto end = start + *size;
    if (*size % 2 != 0) {
      if (end == bytes.size()) return fail("missing archive member alignment padding");
      if (bytes[end] != '\n') return fail("invalid archive member alignment padding");
    }
    const auto data = bytes.substr(start, *size);
    if (field == "/") {
      if (index) return fail("duplicate archive symbol index");
      index = data;
    } else if (field == "//") {
      if (long_names) return fail("duplicate archive long-name table");
      long_names = data;
    } else if (field == "/SYM64/") {
      return fail("64-bit archive indexes are unsupported");
    } else if (bsd_member(field)) {
      return fail("BSD/Darwin archive indexes and inline names are unsupported");
    } else {
      result.members.push_back({offset, field, data});
    }
    offset = end + *size % 2;
  }

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

  if (!index) {
    if (!result.members.empty()) return fail("nonempty archive has no symbol index");
    return result;
  }
  if (index->size() < word_size) return fail("truncated archive symbol count");
  const auto count = big_word(index->substr(0, word_size));
  if (count > (index->size() - word_size) / (word_size + 1)) return fail("invalid archive symbol count");
  auto names = index->substr(word_size + std::size_t{count} * word_size);
  for (std::size_t i = 0; i < count; ++i) {
    const auto offset = big_word(index->substr(word_size + i * word_size, word_size));
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

}  // namespace ycxx::obj::archive
