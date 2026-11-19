// The LSP front (#99, daemon design §6): one language-server session over a byte channel, run in
// the daemon for each `ycxx lsp` connection (§6.4). This first part serves the editor view's
// diagnostics: documents are opened, changed (whole text), saved and closed; after a change the
// session asks `Diagnose` about every open document, with every open buffer overlaid on the disk
// (§6.1: unsaved text drives the editor's answers and never a build), and publishes the
// diagnostics of each. Changes that arrive together are diagnosed once (§6.4, debounced).
//
// The session knows nothing of compilers: `Diagnose` is the daemon's, and runs the document's
// translation unit through the editor view.
export module ycxx.lsp;

export import :json;
import std;

export namespace ycxx::lsp {

// The bytes between the editor and the server.
class Channel {
public:
  virtual ~Channel() = default;
  // Up to `buf.size()` bytes; 0 at the end of the stream.
  virtual std::size_t read(std::span<char> buf) = 0;
  // Whether a `read` would return without waiting.
  virtual bool pending() = 0;
  virtual bool write(std::string_view bytes) = 0;
};

// One diagnostic of a translation unit, in the compiler's terms: a file, a 1-based line and byte
// column.
struct Diagnostic {
  std::string path;  // absolute
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  int severity = 1;  // LSP's: 1 error, 2 warning, 3 information, 4 hint
  std::string message;
};

// The open documents: absolute path -> text.
using Buffers = std::map<std::string, std::string>;

// The diagnostics of the translation units of `paths`, with `open` overlaid on the disk: one call
// per batch of changes, so the daemon synchronizes with the disk and sets the buffers once.
using Diagnose = std::function<std::vector<Diagnostic>(std::span<const std::string> paths, const Buffers& open)>;

// A place in a file: its absolute path, a 1-based line and byte column.
struct Location {
  std::string path;
  std::uint32_t line = 1;
  std::uint32_t column = 1;
};
// Go-to-definition (#170): where the entity named at `at` in an open document is declared, with
// `open` overlaid on the disk; nothing when no entity is named there.
using Define = std::function<std::optional<Location>(const Location& at, const Buffers& open)>;

struct ServerInfo {
  std::string name = "ycxx";
  std::string version;
};

// Runs the session until the editor sends `exit` or closes the channel; the exit status LSP
// prescribes (0 after `shutdown`, 1 otherwise).
int serve(Channel& io, const Diagnose& diagnose, const Define& define = {}, const ServerInfo& info = {});

// A `file://` URI's path (absolute and lexically normal: the engine's name for the file), and back.
std::optional<std::string> path_of(std::string_view uri);
std::string uri_of(std::string_view path);

// The diagnostics in a compiler's stderr, one per `path:line:col: error|warning: message` line
// (`path: error: message` at 1:1; DECISIONS §4.7), relative paths made absolute against `cwd`. A
// message about no file (`ycxx: error: ...`) has an empty path; other lines are skipped.
std::vector<Diagnostic> parse_messages(std::string_view err, std::string_view cwd);

// One framed message (`Content-Length: n\r\n\r\n` and n bytes), or nothing at the end of the
// stream or on a malformed header.
std::optional<std::string> read_message(Channel& io);
bool write_message(Channel& io, const Json& message);

}  // namespace ycxx::lsp

namespace ycxx::lsp {

namespace {

// The messages this part reads and writes, as plain aggregates (the codec is in :json).
struct Position {
  std::uint32_t line = 0;       // 0-based
  std::uint32_t character = 0;  // UTF-16 code units
};
struct Range {
  Position start;
  Position end;
};
struct LspDiagnostic {
  Range range;
  int severity = 1;
  std::string source = "ycxx";
  std::string message;
};
struct PublishDiagnostics {
  std::string uri;
  std::optional<std::int64_t> version;
  std::vector<LspDiagnostic> diagnostics;
};
struct TextDocumentItem {
  std::string uri;
  std::optional<std::string> languageId;
  std::optional<std::int64_t> version;
  std::string text;
};
struct DidOpen {
  TextDocumentItem textDocument;
};
struct VersionedId {
  std::string uri;
  std::optional<std::int64_t> version;
};
struct Change {
  std::optional<Json> range;  // a range means an incremental change, which this server does not ask for
  std::string text;
};
struct DidChange {
  VersionedId textDocument;
  std::vector<Change> contentChanges;
};
struct DocumentId {
  std::string uri;
};
struct DidSaveOrClose {
  DocumentId textDocument;
};
struct SaveOptions {
  bool includeText = false;
};
struct SyncOptions {
  bool openClose = true;
  int change = 1;  // full text
  SaveOptions save;
};
struct Capabilities {
  SyncOptions textDocumentSync;
  bool definitionProvider = false;
};
struct TextDocumentPosition {
  DocumentId textDocument;
  Position position;
};
struct LspLocation {
  std::string uri;
  Range range;
};
struct InitializeResult {
  Capabilities capabilities;
  ServerInfo serverInfo;
};

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A file's text from the disk, for positions in a file the editor has not opened.
std::optional<std::string> read_disk(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(in), {}};
}

// A UTF-16 column of `line` (0-based, of `text`) as a 1-based byte column: the inverse of
// `utf16_column`.
std::uint32_t byte_column(std::string_view text, std::uint32_t line, std::uint32_t character) {
  std::size_t at = 0;
  for (std::uint32_t l = 0; l < line && at != std::string_view::npos; ++l) {
    at = text.find('\n', at);
    if (at != std::string_view::npos) ++at;
  }
  if (at == std::string_view::npos) return character + 1;
  const std::string_view rest = text.substr(at, text.find('\n', at) - at);
  std::size_t i = 0;
  for (std::uint32_t units = 0; i < rest.size() && units < character;) {
    const auto c = static_cast<unsigned char>(rest[i]);
    const std::size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    units += len == 4 ? 2 : 1;
    i += len;
  }
  return static_cast<std::uint32_t>(i + 1);
}

// A byte column of `line` (1-based, of `text`) as UTF-16 code units, for LSP's positions.
std::uint32_t utf16_column(std::string_view text, std::uint32_t line, std::uint32_t column) {
  std::size_t at = 0;
  for (std::uint32_t l = 1; l < line && at != std::string_view::npos; ++l) {
    at = text.find('\n', at);
    if (at != std::string_view::npos) ++at;
  }
  if (at == std::string_view::npos || column == 0) return column ? column - 1 : 0;
  const std::string_view rest = text.substr(at, text.find('\n', at) - at);
  std::uint32_t units = 0;
  for (std::size_t i = 0; i < rest.size() && i + 1 < column;) {
    const auto c = static_cast<unsigned char>(rest[i]);
    const std::size_t len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    units += len == 4 ? 2 : 1;
    i += len;
  }
  return units;
}

class Session {
public:
  Session(Channel& io, const Diagnose& diagnose, const Define& define, const ServerInfo& info)
      : io_(io), diagnose_(diagnose), define_(define), info_(info) {}

  int run() {
    while (auto text = read_message(io_)) {
      auto msg = parse(*text);
      if (!msg || !msg->object()) {
        reply_error(Json(nullptr), -32700, msg ? "not an object" : msg.error());
        continue;
      }
      if (const auto exit = handle(*msg)) return *exit;
      // Changes that arrived together are diagnosed once (§6.4).
      if (dirty_ && !io_.pending()) {
        publish();
        dirty_ = false;
      }
    }
    return shut_down_ ? 0 : 1;
  }

private:
  // The exit status when the message ends the session.
  std::optional<int> handle(const Json& msg) {
    const Json* id = msg["id"];
    const bool request = id != nullptr;
    if (!msg["method"]) return std::nullopt;  // a response to a request of ours: none yet
    const std::string* method = msg["method"]->string();
    if (!method) {
      if (request) reply_error(*id, -32600, "the method is not a string");
      return std::nullopt;
    }
    const Json params = msg["params"] ? *msg["params"] : Json(nullptr);
    if (*method == "exit") return shut_down_ ? 0 : 1;
    // Before `initialize` only `exit` is heard; after `shutdown`, nothing but `exit` is served.
    if (!initialized_ && *method != "initialize") {
      if (request) reply_error(*id, -32002, "the server is not initialized");
      return std::nullopt;
    }
    if (shut_down_) {
      if (request) reply_error(*id, -32600, "the server is shut down");
      return std::nullopt;
    }
    if ((*method == "initialize" || *method == "shutdown") && !request) return std::nullopt;
    if (*method == "initialize") {
      initialized_ = true;
      InitializeResult r{{}, info_};
      r.capabilities.definitionProvider = static_cast<bool>(define_);
      reply(*id, to_json(r));
    } else if (*method == "shutdown") {
      shut_down_ = true;
      reply(*id, Json(nullptr));
    } else if (*method == "textDocument/didOpen") {
      if (auto p = from_json<DidOpen>(params); p) set(p->textDocument.uri, p->textDocument.text, p->textDocument.version);
    } else if (*method == "textDocument/didChange") {
      if (auto p = from_json<DidChange>(params); p && !p->contentChanges.empty() && !p->contentChanges.back().range) {
        set(p->textDocument.uri, p->contentChanges.back().text, p->textDocument.version);
      }
    } else if (*method == "textDocument/didSave") {
      dirty_ = true;  // the disk changed: a document's unsaved neighbours may see it now
    } else if (*method == "textDocument/didClose") {
      if (auto p = from_json<DidSaveOrClose>(params); p) {
        if (auto path = path_of(p->textDocument.uri); path && open_.contains(*path)) {
          send_diagnostics(*path, {});
          open_.erase(*path);
          versions_.erase(*path);
          uris_.erase(*path);
          dirty_ = true;
        }
      }
    } else if (*method == "textDocument/definition" && request && define_) {
      reply(*id, definition(params));
    } else if (request) {
      reply_error(*id, -32601, "method not found: " + *method);
    }
    return std::nullopt;
  }

  // The text of a file as the editor sees it: an open document's, else the disk's.
  std::optional<std::string> text_of(const std::string& path) const {
    if (auto it = open_.find(path); it != open_.end()) return it->second;
    return read_disk(path);
  }

  // `textDocument/definition`: a `Location`, or null.
  Json definition(const Json& params) {
    auto p = from_json<TextDocumentPosition>(params);
    if (!p) return Json(nullptr);
    auto path = path_of(p->textDocument.uri);
    if (!path) return Json(nullptr);
    const std::optional<std::string> text = text_of(*path);
    if (!text) return Json(nullptr);
    const Location at{*path, p->position.line + 1, byte_column(*text, p->position.line, p->position.character)};
    const std::optional<Location> found = define_(at, open_);
    if (!found) return Json(nullptr);
    const std::uint32_t line = found->line ? found->line - 1 : 0;
    const std::optional<std::string> target = text_of(found->path);
    const std::uint32_t character = target ? utf16_column(*target, found->line, found->column) : found->column - 1;
    auto uri = uris_.find(found->path);
    return to_json(LspLocation{uri == uris_.end() ? uri_of(found->path) : uri->second, {{line, character}, {line, character}}});
  }

  void set(const std::string& uri, std::string text, std::optional<std::int64_t> version) {
    auto path = path_of(uri);
    if (!path) return;
    open_[*path] = std::move(text);
    versions_[*path] = version;
    uris_[*path] = uri;  // the editor's spelling, which its answers carry back
    dirty_ = true;
  }

  // Every open document's diagnostics: an unsaved header changes its includers' answers. Those
  // located in a file the editor has not opened (a header the document includes, #170) are
  // published under that file, and cleared once no document reports them.
  void publish() {
    const std::vector<std::string> paths = open_ | std::views::keys | std::ranges::to<std::vector>();
    std::map<std::string, std::vector<LspDiagnostic>> found;
    std::map<std::string, std::optional<std::string>> texts;  // of files not open
    for (const Diagnostic& d : diagnose_(paths, open_)) {
      if (d.path.empty()) continue;  // about no file
      const std::uint32_t line = d.line ? d.line - 1 : 0;
      std::uint32_t character = d.column ? d.column - 1 : 0;
      if (const auto doc = open_.find(d.path); doc != open_.end()) {
        character = utf16_column(doc->second, d.line, d.column);
      } else {
        auto [t, fresh] = texts.try_emplace(d.path);
        if (fresh) t->second = read_disk(d.path);
        if (t->second) character = utf16_column(*t->second, d.line, d.column);
      }
      found[d.path].push_back({{{line, character}, {line, character}}, d.severity, "ycxx", d.message});
    }
    for (const std::string& path : paths) send_diagnostics(path, std::move(found[path]));
    std::set<std::string> elsewhere;
    for (auto& [path, diags] : found)
      if (!open_.contains(path)) {
        elsewhere.insert(path);
        send_diagnostics(path, std::move(diags));
      }
    for (const std::string& path : elsewhere_)
      if (!elsewhere.contains(path) && !open_.contains(path)) send_diagnostics(path, {});
    elsewhere_ = std::move(elsewhere);
  }

  void send_diagnostics(const std::string& path, std::vector<LspDiagnostic> diags) {
    const auto v = versions_.find(path);
    const auto uri = uris_.find(path);
    notify("textDocument/publishDiagnostics",
           to_json(PublishDiagnostics{uri == uris_.end() ? uri_of(path) : uri->second,
                                      v == versions_.end() ? std::nullopt : v->second, std::move(diags)}));
  }

  void notify(std::string_view method, Json params) {
    write_message(io_, Json(Object{{"jsonrpc", "2.0"}, {"method", std::string(method)}, {"params", std::move(params)}}));
  }
  void reply(const Json& id, Json result) {
    write_message(io_, Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}));
  }
  void reply_error(const Json& id, int code, std::string message) {
    write_message(io_, Json(Object{{"jsonrpc", "2.0"},
                                   {"id", id},
                                   {"error", Json(Object{{"code", code}, {"message", std::move(message)}})}}));
  }

  Channel& io_;
  const Diagnose& diagnose_;
  const Define& define_;
  const ServerInfo& info_;
  std::set<std::string> elsewhere_;  // files not open that the last publish reported on
  Buffers open_;
  std::map<std::string, std::optional<std::int64_t>> versions_;
  std::map<std::string, std::string> uris_;
  bool initialized_ = false;
  bool dirty_ = false;
  bool shut_down_ = false;
};

}  // namespace

std::vector<Diagnostic> parse_messages(std::string_view err, std::string_view cwd) {
  std::vector<Diagnostic> out;
  // A trailing `:digits` of `where`, removed; nothing if there is none.
  auto number = [](std::string_view& where) -> std::optional<std::uint32_t> {
    const std::size_t colon = where.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    std::uint32_t n = 0;
    const std::string_view digits = where.substr(colon + 1);
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    where = where.substr(0, colon);
    return n;
  };
  for (auto part : err | std::views::split('\n')) {
    const std::string_view line(part.begin(), part.end());
    Diagnostic d;
    std::size_t at = std::string_view::npos;
    for (auto [kind, severity] : {std::pair{std::string_view(": error: "), 1}, {": warning: ", 2}})
      if (const std::size_t k = line.find(kind); k < at) {
        at = k;
        d.severity = severity;
        d.message = std::string(line.substr(k + kind.size()));
      }
    if (at == std::string_view::npos) continue;
    std::string_view where = line.substr(0, at);
    d.line = d.column = 1;
    std::string_view rest = where;
    if (auto column = number(rest))
      if (auto row = number(rest)) {
        d.line = *row;
        d.column = *column;
        where = rest;
      }
    if (where == "ycxx") {
      out.push_back(std::move(d));
      continue;
    }
    std::filesystem::path p(where);
    if (p.is_relative() && !cwd.empty()) p = std::filesystem::path(cwd) / p;
    d.path = p.lexically_normal().generic_native_encoded_string();
    out.push_back(std::move(d));
  }
  return out;
}

std::optional<std::string> path_of(std::string_view uri) {
  if (!uri.starts_with("file://")) return std::nullopt;
  std::string_view rest = uri.substr(7);
  if (!rest.starts_with('/')) {
    const auto slash = rest.find('/');  // a host: only the local one
    if (slash == std::string_view::npos || rest.substr(0, slash) != "localhost") return std::nullopt;
    rest = rest.substr(slash);
  }
  std::string out;
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
      out += static_cast<char>(hex(rest[i + 1]) * 16 + hex(rest[i + 2]));
      i += 2;
    } else {
      out += rest[i];
    }
  }
  return std::filesystem::path(out).lexically_normal().generic_native_encoded_string();
}

std::string uri_of(std::string_view path) {
  std::string out = "file://";
  for (const char c : path) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else out += std::format("%{:02X}", static_cast<unsigned>(u));
  }
  return out;
}

std::optional<std::string> read_message(Channel& io) {
  // Header lines up to an empty one; only Content-Length matters.
  std::string header;
  char c = 0;
  while (!header.ends_with("\r\n\r\n")) {
    if (io.read(std::span(&c, 1)) != 1) return std::nullopt;
    header += c;
    if (header.size() > 4096) return std::nullopt;
  }
  std::optional<std::size_t> length;
  for (std::string_view rest = header; !rest.empty();) {
    const auto eol = rest.find("\r\n");
    const std::string_view line = rest.substr(0, eol);
    rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 2);
    constexpr std::string_view key = "Content-Length:";
    if (line.size() > key.size() && std::ranges::equal(line.substr(0, key.size()), key, [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        })) {
      std::string_view n = line.substr(key.size());
      while (!n.empty() && n.front() == ' ') n.remove_prefix(1);
      std::size_t v = 0;
      const auto r = std::from_chars(n.data(), n.data() + n.size(), v);
      if (r.ec != std::errc{} || r.ptr != n.data() + n.size() || v > (64u << 20)) return std::nullopt;
      length = v;
    }
  }
  if (!length) return std::nullopt;
  std::string body(*length, '\0');
  for (std::size_t got = 0; got < *length;) {
    const std::size_t n = io.read(std::span(body.data() + got, *length - got));
    if (n == 0) return std::nullopt;
    got += n;
  }
  return body;
}

bool write_message(Channel& io, const Json& message) {
  const std::string body = dump(message);
  return io.write(std::format("Content-Length: {}\r\n\r\n", body.size())) && io.write(body);
}

int serve(Channel& io, const Diagnose& diagnose, const Define& define, const ServerInfo& info) {
  return Session(io, diagnose, define, info).run();
}

}  // namespace ycxx::lsp
