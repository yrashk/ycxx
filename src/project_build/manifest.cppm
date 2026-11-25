// Declarative workspace definitions and filesystem-independent glob expansion (#1253).
export module ycxx.build.manifest;

import std;

export namespace ycxx::build {

struct SourceLocation {
  std::size_t line = 1;
  std::size_t column = 1;  // one-based byte column
};

struct ManifestError {
  SourceLocation location;
  std::string message;
};

struct Glob {
  std::string pattern;
  std::vector<std::string> exclusions;
};

struct ArtifactRef {
  std::string target;
  std::string output;
};

struct InputArg {
  std::string name;
};

struct OutputArg {
  std::string name;
};

struct CxxTool {
  std::optional<std::string> compiler;
};

struct ProbeRef {
  std::string package;
  std::string probe;
};

struct Value;
using List = std::vector<Value>;
using Record = std::vector<std::pair<std::string, Value>>;

struct Value {
  SourceLocation location;
  std::variant<std::string, List, Record, Glob, ArtifactRef, InputArg, OutputArg, CxxTool, ProbeRef> data;
};

struct TargetDecl {
  SourceLocation location;
  std::string kind;
  std::string name;
  Record fields;
};

struct PluginUse {
  SourceLocation location;
  std::string package;
  std::string alias;
};

struct Manifest {
  std::vector<TargetDecl> targets;
  std::vector<PluginUse> plugins;
};

std::expected<Manifest, ManifestError> parse_manifest(std::string_view text);

enum class EntryKind { regular, directory, other };
struct DirectoryEntry {
  std::string name;
  EntryKind kind;
};

// Names are immediate directory entries. Missing directories return an empty list; an access
// failure returns an error. The caller can record the observations with the query engine.
using DirectoryReader =
    std::function<std::expected<std::vector<DirectoryEntry>, std::string>(std::string_view)>;

// The callback receives normalized paths relative to the manifest, with "." denoting its
// directory. Results are regular files in byte order. Symlinks are reported as EntryKind::other.
std::expected<std::vector<std::string>, std::string>
expand_glob(const Glob& glob, const DirectoryReader& read);

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

using Pattern = std::vector<std::string_view>;

std::expected<Pattern, std::string> split_pattern(std::string_view pattern) {
  Pattern parts;
  for (;;) {
    const std::size_t slash = pattern.find('/');
    const std::string_view part = pattern.substr(0, slash);
    if (part.empty() || part == "." || part == ".." || part.contains('\0'))
      return std::unexpected("glob patterns must be nonempty, normalized relative paths");
    if (part != "**" && part.contains("**"))
      return std::unexpected("** must occupy a whole glob path component");
    parts.push_back(part);
    if (slash == std::string_view::npos) return parts;
    pattern.remove_prefix(slash + 1);
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Manifest document() {
    Manifest result;
    std::set<std::string> names;
    std::set<std::string> aliases;
    skip_space();
    while (pos_ != text_.size()) {
      const auto at = location();
      std::string kind = identifier();
      if (kind == "use") {
        if (identifier() != "plugin") fail(location(), "expected plugin import");
        expect('(');
        auto package = string();
        expect(')');
        if (identifier() != "as") fail(location(), "expected 'as' in plugin import");
        const auto alias_at = next_location();
        auto alias = identifier();
        if (alias.contains('.')) fail(alias_at, "plugin alias must be a single name");
        if (!aliases.insert(alias).second) fail(alias_at, std::format("duplicate plugin alias '{}'", alias));
        result.plugins.push_back({at, std::move(package), std::move(alias)});
        skip_space();
        continue;
      }
      const auto name_at = next_location();
      std::string name = string();
      if (name.empty()) fail(name_at, "target name is empty");
      if (!names.insert(name).second) fail(name_at, std::format("duplicate target '{}'", name));
      expect('{');
      result.targets.push_back({at, std::move(kind), std::move(name), record(0)});
      skip_space();
    }
    return result;
  }

 private:
  static bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  SourceLocation location() const { return {line_, column_}; }
  SourceLocation next_location() {
    skip_space();
    return location();
  }

  char advance() {
    const char c = text_[pos_++];
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return c;
  }

  void skip_space() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        advance();
      } else if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n') advance();
      } else {
        return;
      }
    }
  }

  char peek() {
    skip_space();
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }

  bool consume(char c) {
    if (peek() != c) return false;
    advance();
    return true;
  }

  void expect(char c) {
    if (!consume(c)) fail(location(), std::format("expected '{}'", c));
  }

  [[noreturn]] static void fail(SourceLocation at, std::string why) {
    throw ManifestError{at, std::move(why)};
  }

  std::string identifier() {
    if (!letter(peek())) fail(location(), "expected a name");
    const std::size_t begin = pos_;
    do {
      advance();
      if (pos_ == text_.size()) break;
      const char c = text_[pos_];
      if (!letter(c) && !(c >= '0' && c <= '9') && c != '.' && c != '-') break;
    } while (true);
    return std::string(text_.substr(begin, pos_ - begin));
  }

  std::string string() {
    expect('"');
    std::string result;
    while (pos_ < text_.size()) {
      const auto at = location();
      const char c = advance();
      if (c == '"') return result;
      if (static_cast<unsigned char>(c) < 0x20) fail(at, "control character in string");
      if (c != '\\') {
        result += c;
        continue;
      }
      if (pos_ == text_.size()) fail(at, "unterminated escape");
      switch (advance()) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        default: fail(at, "invalid string escape");
      }
    }
    fail(location(), "unterminated string");
  }

  Record record(int depth) {
    Record result;
    std::set<std::string> fields;
    while (!consume('}')) {
      const auto at = location();
      std::string key = peek() == '"' ? string() : identifier();
      if (!fields.insert(key).second) fail(at, std::format("duplicate field '{}'", key));
      expect('=');
      result.emplace_back(std::move(key), value(depth + 1));
      consume(',');
    }
    return result;
  }

  List list(int depth) {
    List result;
    while (!consume(']')) {
      result.push_back(value(depth + 1));
      if (!consume(',')) {
        expect(']');
        break;
      }
    }
    return result;
  }

  std::string pattern() {
    const auto at = next_location();
    std::string result = string();
    const auto parts = split_pattern(result);
    if (!parts) fail(at, parts.error());
    return result;
  }

  Glob glob() {
    Glob result{.pattern = pattern(), .exclusions = {}};
    if (consume(',')) {
      const auto at = next_location();
      if (identifier() != "exclude") fail(at, "expected 'exclude'");
      expect('=');
      expect('[');
      while (!consume(']')) {
        result.exclusions.push_back(pattern());
        if (!consume(',')) {
          expect(']');
          break;
        }
      }
    }
    return result;
  }

  Value value(int depth) {
    const auto at = next_location();
    if (depth > 64) fail(at, "value nested too deeply");
    if (peek() == '"') return {at, string()};
    if (consume('[')) return {at, list(depth)};
    if (consume('{')) return {at, record(depth)};
    const auto name = identifier();
    if (name == "glob") {
      expect('(');
      auto spec = glob();
      expect(')');
      return {at, std::move(spec)};
    }
    if (name == "artifact") {
      expect('(');
      auto target = string();
      expect(',');
      auto output = string();
      expect(')');
      if (target.empty() || output.empty()) fail(at, "empty artifact target or output");
      return {at, ArtifactRef{std::move(target), std::move(output)}};
    }
    if (name == "cxx") {
      expect('(');
      if (consume(')')) return {at, CxxTool{}};
      const auto compiler_at = next_location();
      auto compiler = string();
      if (compiler.empty()) fail(compiler_at, "empty compiler selector");
      expect(')');
      return {at, CxxTool{std::move(compiler)}};
    }
    if (name == "probe") {
      expect('(');
      const auto package_at = next_location();
      auto package = string();
      if (package.empty()) fail(package_at, "empty probe package target");
      expect(',');
      const auto probe_at = next_location();
      auto probe = string();
      if (probe.empty()) fail(probe_at, "empty probe id");
      expect(')');
      return {at, ProbeRef{std::move(package), std::move(probe)}};
    }
    if (name == "input" || name == "output") {
      expect('(');
      auto binding = string();
      expect(')');
      if (binding.empty()) fail(at, "empty action argument binding");
      if (name == "input") return {at, InputArg{std::move(binding)}};
      return {at, OutputArg{std::move(binding)}};
    }
    fail(at, std::format("unknown value '{}'", name));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

// Match within one path component. A single wildcard never consumes a directory separator.
bool match_component(std::string_view pattern, std::string_view name) {
  std::size_t p = 0, n = 0, star = std::string_view::npos, retry = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      retry = n;
    } else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
      ++p;
      ++n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++retry;
    } else {
      return false;
    }
  }
  return std::ranges::all_of(pattern.substr(p), [](char c) { return c == '*'; });
}

// Match a path against one exclusion with a bounded set of positions in the pattern. Processing
// ** in order permits zero-component matches and consumes arbitrary additional components.
bool match_path(const Pattern& pattern, std::string_view path) {
  std::vector<std::uint8_t> current(pattern.size() + 1);
  current.front() = 1;
  auto consume_empty = [&] {
    for (std::size_t i = 0; i < pattern.size(); ++i)
      if (current[i] && pattern[i] == "**") current[i + 1] = 1;
  };
  consume_empty();
  while (!path.empty()) {
    const std::size_t slash = path.find('/');
    const std::string_view component = path.substr(0, slash);
    std::vector<std::uint8_t> next(current.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      if (!current[i]) continue;
      if (pattern[i] == "**") next[i] = 1;
      else if (match_component(pattern[i], component)) next[i + 1] = 1;
    }
    current = std::move(next);
    consume_empty();
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return current.back() != 0;
}

}  // namespace

std::expected<Manifest, ManifestError> parse_manifest(std::string_view text) {
  try {
    return Parser(text).document();
  } catch (ManifestError& error) {
    return std::unexpected(std::move(error));
  }
}

std::expected<std::vector<std::string>, std::string>
expand_glob(const Glob& glob, const DirectoryReader& read) {
  const auto pattern = split_pattern(glob.pattern);
  if (!pattern) return std::unexpected(pattern.error());
  std::vector<Pattern> exclusions;
  for (const auto& exclusion : glob.exclusions) {
    auto parts = split_pattern(exclusion);
    if (!parts) return std::unexpected(parts.error());
    exclusions.push_back(std::move(*parts));
  }

  using State = std::pair<std::string, std::size_t>;
  std::set<State> pending{{"", 0}};
  std::set<State> visited;
  std::map<std::string, std::vector<DirectoryEntry>> listings;
  std::set<std::string> matches;
  while (!pending.empty()) {
    auto [dir, i] = *pending.begin();
    pending.erase(pending.begin());
    if (!visited.emplace(dir, i).second) continue;
    auto listing = listings.find(dir);
    if (listing == listings.end()) {
      auto entries = read(dir.empty() ? "." : std::string_view(dir));
      if (!entries) return std::unexpected(entries.error());
      listing = listings.emplace(dir, std::move(*entries)).first;
    }
    const auto component = (*pattern)[i];
    const bool recursive = component == "**";
    const bool last = i + 1 == pattern->size();
    if (recursive && !last) pending.emplace(dir, i + 1);
    for (const auto& [name, kind] : listing->second) {
      if (!recursive && !match_component(component, name)) continue;
      std::string path = dir.empty() ? name : dir + '/' + name;
      if (kind == EntryKind::directory) {
        if (recursive) pending.emplace(std::move(path), i);
        else if (!last) pending.emplace(std::move(path), i + 1);
      } else if (kind == EntryKind::regular && last) {
        if (!std::ranges::any_of(exclusions, [&](const Pattern& exclusion) {
              return match_path(exclusion, path);
            }))
          matches.insert(std::move(path));
      }
    }
  }
  return std::vector<std::string>(matches.begin(), matches.end());
}

}  // namespace ycxx::build
