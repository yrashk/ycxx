// ycxx.pp: translation phases 1–6 of a translation unit (#85). Phases 1–3 and 5–6 are ycxx.lex's;
// phase 4 ([cpp]) is here, as query-engine.md §B9 has it: each file occurrence is a region whose
// value is a function of the file, the configuration and the answers to the questions in its
// trace, reused wherever those answers are the same (:unit, :region).
//
//   :spell   spellings shared by every region of a session
//   :files   files, search paths and include resolution ([cpp.include])
//   :region  FileRegion (one file occurrence), questions, macros, Region (the TU flattened)
//   :unit    the preprocessor of one TU and the session's region cache
//   :tu      flattening, phases 5–6, and `-E` output
//   :system  the build's system include directories and ycxx's version
//   :predefined  the predefined macros ([cpp.predefined], DECISIONS §1.5)
//   :cond    #if expressions ([cpp.cond])
//
// `pp(file)` is an engine query over the main file's text and its `Source` (its name, working
// directory and configuration). Every disk file it includes is read as a `FileContent` input, provided
// on first read (daemon design §5.1, #93), so editing a header invalidates the `pp` cells that read
// it. The region cache of the process's session for the configuration still shares regions between
// units, where their traces verify; contextual queries (#16) will replace it.
export module ycxx.pp;

import std;
import ycxx.engine;
export import ycxx.lex;
export import :spell;
export import :files;
export import :region;
export import :unit;
export import :tu;
export import :system_os;
export import :system;
export import :predefined;
export import :cond;

namespace {
// Preserve the bit values of earlier source configurations.
constexpr std::uint8_t freestanding_bit = 1, no_exceptions_bit = 2, no_rtti_bit = 4, pic_bit = 0x08, small_pic_bit = 0x10;
constexpr std::uint8_t compatibility_shift = 5, compatibility_mask = 0x60;
constexpr std::uint8_t host_overlay_bit = 0x80;
}

export namespace ycxx::pp {

// The process keeps one session per configuration, shared by every thread under its `mutex`.
// `session()` is the one configured last (single-threaded callers and tests).
Session& session();
// The session of `cfg`, reading the disk outside `Pp`.
Session& configure(Config cfg);
// The session of `cfg`, reading through `host` outside `Pp` (the daemon's file table): kept with
// its region cache and spellings, starting a new generation of file contents (the traces verify
// what changed).
Session& configure(Config cfg, Host& host);

// An absolute, lexically normal path, interned for the process. Its fingerprint is its spelling,
// never its number, so nothing that depends on the order paths were first seen reaches a key's
// fingerprint.
enum class PathId : u32 {};
PathId path_id(std::string_view absolute);
const std::string& path_of(PathId p);
void hash_into(Hasher& h, PathId p) { h.bytes(path_of(p)).uint(u8(0)); }

// Names in engine traces (#245): a path by its last two components, a driver's file by the name
// it was given (`name_file`, process-wide), else its number.
void name_file(FileId f, std::string name);
void trace_describe(std::string& out, PathId p);
void trace_describe(std::string& out, FileId f);

// Phases 1–6 of one translation unit, as a function: `text` is the main file's, `path` its name
// as given (it locates `#include "..."` and is its presumed name).
Preprocessed preprocess(Session& s, std::string_view text, std::string_view path);
Preprocessed preprocess(std::string_view text, std::string_view path = "<stdin>");
// A diagnostic ready to print: the presumed file name, line and column (1-based, columns in
// bytes), severity, message and warning category.
struct Message {
  std::string file;
  u32 line = 0;
  u32 column = 0;
  bool error = true;
  std::string text;
  lex::Warning warning = lex::Warning::uncontrolled;
};
Message message(Session& s, FileId file, std::string_view presumed, const Diag& d);
// The same, from the text of the file the diagnostic is in and the spellings of the session that
// preprocessed it.
Message message(const Spellings& sp, std::string_view text, std::string_view presumed, const Diag& d);
// Render a diagnostic emitted by Pp, whose directive text lives in the returned region.
std::string render_diagnostic(const Region& region, lex::LexDiag kind, std::span<const u64> args);
// The process's session for a configuration (the one `Pp` uses for a unit with that `SourceOf`).
// Sessions live as long as the process: the regions `Pp` returns hold their spellings' numbers.
Session& session_for(const Config& cfg);

// `-E`: the text Clang's `-E` would print, and the diagnostics.
std::pair<std::string, std::vector<Message>> preprocess_to_text(Session& s, std::string_view text, std::string_view path);

// Takes a preprocessing or language-mode option at `args[i]`. Advances `i` for a separate
// argument. Returns an error for a missing argument.
std::expected<bool, std::string> take_option(std::span<const std::string_view> args, std::size_t& i, Config& cfg);
// Consumes the include suppression options before appending automatic search paths.
// A command-line directory that names an automatic one (relative to `cwd`) is searched only
// at its automatic place when present, so `#include_next` goes past it.
Config with_system_dirs(Config cfg, std::string_view cwd = {}, std::string_view sdkroot = {});

// The item of a file-level diagnostic location in the region's file `file` (lex::file_level for
// the main file). Seam for #34: diagnostics before items exist are located by byte offset.
constexpr u32 file_loc(u16 file) { return lex::file_level - file; }

// The text of a file: an input, set by the driver.
struct [[=engine::input{engine::Durability::low}]] FileText {
  using Key = FileId;
  using Value = std::string;
  static engine::SourceSpan trace_span(const engine::Db& db, FileId f);
};

// The bytes of the regular file at a path, or nothing: an input read on demand through the
// provider its `Db` was given (the disk, or the daemon's file table, daemon design §5.1), and set by
// the daemon when a batch changes the file (§5.3).
struct [[=engine::input{engine::Durability::low}]] FileContent {
  using Key = PathId;
  using Value = std::optional<std::string>;
  // For traces (#245): an include search's probe often finds nothing.
  static std::string show(const Value& v) { return v ? std::format("{} bytes", v->size()) : std::string("absent"); }
};

// The text of an editor's open buffer at a path, or nothing when none is open (daemon design §6.1):
// an input at the lowest durability, so a keystroke leaves every disk-view cell green by the
// shortcut (§6.3). Only the editor view reads it; its provider is `no_buffer` unless the owner
// gives another.
struct [[=engine::input{engine::Durability::editor}]] Buffer {
  using Key = PathId;
  using Value = std::optional<std::string>;
};
engine::provided<std::optional<std::string>> no_buffer(PathId);

// A generated path in the build view. Read and negative include probes record the
// individual artifact. The build coordinator sets these inputs only at quiescence.
struct [[=engine::input{engine::Durability::low}]] GeneratedFile {
  using Key = PathId;
  using Value = std::optional<std::string>;
  static std::string show(const Value& v) { return FileContent::show(v); }
};
engine::provided<std::optional<std::string>> no_generated_file(PathId);

// A provider of `FileContent` that reads the disk, for a `Db` with no daemon behind it.
engine::provided<std::optional<std::string>> read_disk(PathId p);

// How a driver's file is preprocessed: its name as given (its presumed name, and where
// `#include "..."` starts), the directory relative paths are relative to, and the configuration.
struct Source {
  std::string path;
  std::string cwd;
  Config config;
};
// A null pointer leaves the path to the file table. A disengaged value is a miss
// in the immutable host overlay. An engaged empty string is an empty file.
const std::optional<std::string>* host_overlay_content(const Config& config, std::string_view cwd,
                                                       std::string_view name);
// Read a path in the source's view. A build view reserves a virtual directory for
// generated contents under the workspace's selected build state directory. A disk
// view with a host overlay serves its reserved directory from the configuration.
const std::optional<std::string>& source_content(engine::SyncCtx& cx, const Source& source, std::string_view name);
const std::string* peek_source_content(const engine::Db& db, const Source& source, std::string_view name);

// The configuration's target is a pointer to a known target: a `Source` is fingerprinted by the
// configuration's fingerprint and stored with the target's triple.
void hash_into(Hasher& h, const Source& s) { h.bytes(s.path).uint(u8(0)).bytes(s.cwd).uint(u8(0)).fp(s.config.fingerprint()); }
void serialize_into(engine::Sink& out, const Source& s) {
  engine::serialize(out, s.path);
  engine::serialize(out, s.cwd);
  engine::serialize(out, s.config.quote_dirs);
  engine::serialize(out, s.config.angle_dirs);
  engine::serialize(out, static_cast<std::uint64_t>(s.config.system_from));
  engine::serialize(out, s.config.macros);
  engine::serialize(out, std::string(s.config.target->triple));
  engine::serialize(out, static_cast<std::uint64_t>(s.config.view));
  engine::serialize(out, static_cast<std::uint64_t>(s.config.template_depth));
  engine::serialize(out, static_cast<std::uint64_t>(s.config.call_triviality ? 1 + std::to_underlying(*s.config.call_triviality) : 0));
  std::uint8_t modes = std::to_underlying(s.config.compatibility) << compatibility_shift;
  if (s.config.freestanding) modes |= freestanding_bit;
  if (!s.config.exceptions_enabled) modes |= no_exceptions_bit;
  if (!s.config.rtti_enabled) modes |= no_rtti_bit;
  if (s.config.pic == 2) modes |= pic_bit;
  if (s.config.pic == 1) modes |= small_pic_bit;
  if (s.config.host_overlay) modes |= host_overlay_bit;
  engine::serialize(out, modes);
  if (s.config.host_overlay) {
    const auto& files = s.config.host_overlay->files();
    engine::serialize(out, static_cast<std::uint64_t>(files.size()));
    for (const auto& [path, contents] : files) {
      engine::serialize(out, path);
      engine::serialize(out, *contents);
    }
  }
}
Source deserialize_from(engine::Source& in, std::type_identity<Source>) {
  Source s;
  s.path = engine::deserialize<std::string>(in);
  s.cwd = engine::deserialize<std::string>(in);
  s.config.quote_dirs = engine::deserialize<std::vector<std::string>>(in);
  s.config.angle_dirs = engine::deserialize<std::vector<std::string>>(in);
  s.config.system_from = static_cast<std::size_t>(engine::deserialize<std::uint64_t>(in));
  s.config.macros = engine::deserialize<std::vector<std::pair<std::string, std::optional<std::string>>>>(in);
  if (auto target = ast::target_for_triple(engine::deserialize<std::string>(in))) s.config.target = target;
  else in.fail();
  const auto view = engine::deserialize<std::uint64_t>(in);
  if (view > std::to_underlying(View::explicit_target_build)) in.fail();
  s.config.view = static_cast<View>(view);
  s.config.template_depth = static_cast<std::uint32_t>(engine::deserialize<std::uint64_t>(in));
  const auto calls = engine::deserialize<std::uint64_t>(in);
  if (calls > 1 + std::to_underlying(ast::CallTriviality::apple)) in.fail();
  if (calls != 0) s.config.call_triviality = static_cast<ast::CallTriviality>(calls - 1);
  const auto modes = engine::deserialize<std::uint8_t>(in);
  const auto compatibility = (modes & compatibility_mask) >> compatibility_shift;
  if (compatibility > std::to_underlying(ast::Compatibility::clang) ||
      (modes & (pic_bit | small_pic_bit)) == (pic_bit | small_pic_bit))
    in.fail();
  s.config.freestanding = (modes & freestanding_bit) != 0;
  s.config.exceptions_enabled = (modes & no_exceptions_bit) == 0;
  s.config.rtti_enabled = (modes & no_rtti_bit) == 0;
  s.config.pic = (modes & pic_bit) != 0 ? 2 : (modes & small_pic_bit) != 0 ? 1 : 0;
  s.config.compatibility = static_cast<ast::Compatibility>(compatibility);
  if (modes & host_overlay_bit) {
    HostOverlay::Files files;
    const auto count = in.length();
    for (std::size_t i = 0; i < count && !in.failed(); ++i) {
      auto path = engine::deserialize<std::string>(in);
      auto contents = engine::deserialize<std::string>(in);
      if (!files.emplace(std::move(path), std::move(contents)).second) in.fail();
    }
    if (!in.failed()) s.config.host_overlay = std::make_shared<const HostOverlay>(std::move(files));
  }
  return s;
}

struct [[=engine::input{engine::Durability::low}]] SourceOf {
  using Key = FileId;
  using Value = Source;
  static engine::SourceSpan trace_span(const engine::Db& db, FileId f);
};
// For traces (#638): a file's cells are the whole file, named by its path.
engine::SourceSpan file_span(const engine::Db& db, FileId f);

// `pp(file)` (§B9). Its diagnostics are at `Loc{file_loc(file index), byte offset}`.
struct Pp {
  using Key = FileId;
  using Value = Region;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
  static std::string errors(const Value& v) { return v.error_details; }
  static engine::SourceSpan trace_span(const engine::Db& db, Key file) { return file_span(db, file); }
};

}  // namespace ycxx::pp

namespace ycxx::pp {

struct Process {
  std::mutex m;  // the map and `current`; each session has its own mutex
  DiskHost disk;
  // One session per configuration fingerprint, each with the host it reads through outside `Pp`.
  struct Configured {
    std::unique_ptr<Session> session;
    Host* host = nullptr;
  };
  // Never dropped: the regions of `Pp` cells hold the session's spelling numbers, so a session
  // must outlive every engine that read through it (a bound on memory is #17's eviction).
  std::map<Fp128, Configured> sessions;
  Session* current = nullptr;

  Configured& slot(const Config& cfg) { return sessions[cfg.fingerprint()]; }
};
Process& process() {
  static Process p;
  return p;
}

// The session of a configuration, created reading the disk if there is none.
Session& session_for(const Config& cfg) {
  Process& p = process();
  std::lock_guard g(p.m);
  auto& c = p.slot(cfg);
  if (!c.session) {
    c.session = std::make_unique<Session>(p.disk, cfg);
    c.host = &p.disk;
  }
  return *c.session;
}

struct Paths {
  std::mutex m;
  std::deque<std::string> spelled;  // a deque: references stay valid as it grows
  std::map<std::string, PathId, std::less<>> ids;
};
Paths& paths() {
  static Paths p;
  return p;
}

std::string normalized_source_path(std::string_view cwd, std::string_view name) {
  std::filesystem::path path(name);
  if (path.is_relative() && !cwd.empty()) path = std::filesystem::path(cwd) / path;
  return path.lexically_normal().generic_string<char>();
}

bool is_generated_path(const Source& source, std::string_view path) {
  const auto view = source.config.view;
  if (view != View::build && view != View::explicit_target_build) return false;
  auto state = std::filesystem::path(source.cwd) / ".ycxx";
  if (view == View::explicit_target_build) state = state / "targets" / source.config.target->triple;
  const auto root = (state / "virtual").lexically_normal();
  const std::string prefix = root.generic_string<char>() + '/';
  return path.starts_with(prefix);
}

const std::optional<std::string>* host_overlay_at(const Config& config, std::string_view cwd, std::string_view path) {
  if (config.view != View::disk || !config.host_overlay) return nullptr;
  const auto root = (std::filesystem::path(cwd) / ".ycxx/host/virtual").lexically_normal();
  const auto prefix = root.generic_string<char>();
  if (!path.starts_with(prefix) || (path.size() > prefix.size() && path[prefix.size()] != '/'))
    return nullptr;
  const auto& files = config.host_overlay->files();
  const auto found = files.find(path);
  static const std::optional<std::string> absent;
  return found == files.end() ? &absent : &found->second;
}

// Each attempted include records the input that answered it, including misses.
class EngineHost final : public Host {
 public:
  EngineHost(engine::Ctx& cx, const Source& source) : cx_(cx), source_(source) {}
  std::optional<std::string> read(const std::string& path) override { return source_content(cx_, source_, path); }

 private:
  engine::Ctx& cx_;
  const Source& source_;
};

// Points a session's files at a host for a scope, starting and ending with a new generation, so
// no content read through one host is served through the other.
class HostScope {
 public:
  HostScope(Files& f, Host& h) : f_(&f), old_(f.swap_host(h)) { f_->new_generation(); }
  ~HostScope() {
    f_->swap_host(*old_);
    f_->new_generation();
  }
  HostScope(const HostScope&) = delete;

 private:
  Files* f_;
  Host* old_;
};

// The names of the driver's files, for traces (#245).
struct FileNames {
  std::mutex m;
  std::map<FileId, std::string> names;
};
FileNames& file_names() {
  static FileNames n;
  return n;
}

bool has_directive_text(lex::LexDiag kind) {
  return kind == lex::LexDiag::error_directive || kind == lex::LexDiag::warning_directive;
}

}  // namespace ycxx::pp

export namespace ycxx::pp {

Session& session() {
  Session* current;
  {
    Process& p = process();
    std::lock_guard g(p.m);
    current = p.current;
  }
  if (!current) {
    current = &session_for(Config{});
    Process& p = process();
    std::lock_guard g(p.m);
    if (!p.current) p.current = current;
  }
  return *current;
}

Session& configure(Config cfg) { return configure(std::move(cfg), process().disk); }

Session& configure(Config cfg, Host& host) {
  Process& p = process();
  std::lock_guard g(p.m);
  auto& c = p.slot(cfg);
  if (!c.session) {
    c.session = std::make_unique<Session>(host, std::move(cfg));
  } else {
    // Never a new session for an existing configuration: `Pp`'s regions hold its spellings.
    std::lock_guard s(c.session->mutex);
    if (c.host != &host) c.session->files.swap_host(host);
    c.session->files.new_generation();
  }
  c.host = &host;
  p.current = c.session.get();
  return *c.session;
}

PathId path_id(std::string_view absolute) {
  Paths& p = paths();
  std::lock_guard g(p.m);
  if (auto it = p.ids.find(absolute); it != p.ids.end()) return it->second;
  PathId id{static_cast<u32>(p.spelled.size())};
  p.spelled.emplace_back(absolute);
  p.ids.emplace(std::string(absolute), id);
  return id;
}

void name_file(FileId f, std::string name) {
  FileNames& n = file_names();
  std::lock_guard lock(n.m);
  n.names.insert_or_assign(f, std::move(name));
}

engine::SourceSpan file_span(const engine::Db& db, FileId f) {
  if (const Source* s = db.peek<SourceOf>(f)) return {s->path};
  return {};
}
engine::SourceSpan SourceOf::trace_span(const engine::Db& db, FileId f) { return file_span(db, f); }
engine::SourceSpan FileText::trace_span(const engine::Db& db, FileId f) { return file_span(db, f); }

void trace_describe(std::string& out, FileId f) {
  FileNames& n = file_names();
  std::lock_guard lock(n.m);
  if (const auto it = n.names.find(f); it != n.names.end()) out += it->second;
  else std::format_to(std::back_inserter(out), "file {}", std::to_underlying(f));
}

// The whole path, so the include search's probes of one name in several directories differ,
// with its long prefixes named: an SDK (`<sdk>/usr/include/stdio.h`), a toolchain, the home.
void trace_describe(std::string& out, PathId p) {
  std::string_view path = path_of(p);
  if (const std::size_t sdk = path.find(".sdk/"); sdk != std::string_view::npos) {
    out += "<sdk>";
    out += path.substr(sdk + 4);
    return;
  }
  for (std::string_view tc : {".xctoolchain/", "/CommandLineTools/"}) {
    if (const std::size_t at = path.find(tc); at != std::string_view::npos) {
      out += "<toolchain>/";
      out += path.substr(at + tc.size());
      return;
    }
  }
  if (const char* home = std::getenv("HOME"); home && *home && path.starts_with(home) && path.size() > std::strlen(home) &&
                                               path[std::strlen(home)] == '/') {
    out += "~";
    out += path.substr(std::strlen(home));
    return;
  }
  out += path;
}

const std::string& path_of(PathId id) {
  Paths& p = paths();
  std::lock_guard g(p.m);
  return p.spelled[std::to_underlying(id)];
}

engine::provided<std::optional<std::string>> read_disk(PathId p) {
  DiskHost disk;
  return {disk.read(path_of(p)), engine::Durability::low};
}

engine::provided<std::optional<std::string>> no_buffer(PathId) { return {std::nullopt, engine::Durability::editor}; }

engine::provided<std::optional<std::string>> no_generated_file(PathId) { return {std::nullopt, engine::Durability::low}; }

const std::optional<std::string>* host_overlay_content(const Config& config, std::string_view cwd,
                                                       std::string_view name) {
  if (!config.host_overlay) return nullptr;
  return host_overlay_at(config, cwd, normalized_source_path(cwd, name));
}

const std::optional<std::string>& source_content(engine::SyncCtx& cx, const Source& source, std::string_view name) {
  const auto path = normalized_source_path(source.cwd, name);
  if (const auto* overlay = host_overlay_at(source.config, source.cwd, path)) return *overlay;
  const PathId id = path_id(path);
  if (source.config.view == View::editor)
    if (const auto& open = cx.fetch<Buffer>(id)) return open;
  if (is_generated_path(source, path)) return cx.fetch<GeneratedFile>(id);
  return cx.fetch<FileContent>(id);
}

const std::string* peek_source_content(const engine::Db& db, const Source& source, std::string_view name) {
  const auto path = normalized_source_path(source.cwd, name);
  if (const auto* overlay = host_overlay_at(source.config, source.cwd, path)) return *overlay ? &**overlay : nullptr;
  const PathId id = path_id(path);
  if (source.config.view == View::editor)
    if (const auto* open = db.peek<Buffer>(id); open && *open) return &**open;
  const auto* content = is_generated_path(source, path) ? db.peek<GeneratedFile>(id) : db.peek<FileContent>(id);
  return content && *content ? &**content : nullptr;
}

Preprocessed preprocess(Session& s, std::string_view text, std::string_view path) {
  FileId main = s.files.id(path);
  s.files.set_content(main, std::string(text));
  Tree t = Unit(s).run(main, path);
  Preprocessed out = to_region(s, flatten(s, t));
  if (!t.main) return out;
  out.region.dependencies.push_back(s.files.path(main));
  // The main region's trace includes each child's content reads, including those replayed
  // from cached regions. An include search or __has_include probe alone records no such read.
  for (const Question& q : t.main->trace) {
    if (q.ask != Ask::content || q.answer == Fp128{} || FileId{q.a} == main) continue;
    out.region.dependencies.push_back(s.files.path(FileId{q.a}));
  }
  return out;
}

Preprocessed preprocess(std::string_view text, std::string_view path) {
  Session& s = session();
  std::lock_guard g(s.mutex);
  return preprocess(s, text, path);
}

std::string render_diagnostic(const Region& region, lex::LexDiag kind, std::span<const u64> args) {
  if (has_directive_text(kind)) {
    const auto it = std::ranges::find(region.directive_messages, Fp128{args[0], args[1]},
                                      &std::pair<Fp128, std::string>::first);
    if (it != region.directive_messages.end()) return it->second;
  }
  return lex::render(kind, args);
}

Message message(Session& s, FileId file, std::string_view presumed, const Diag& d) {
  const Files::Content* c = s.files.content(file);
  return message(s.spellings, c ? std::string_view(c->text) : std::string_view(), presumed, d);
}

Message message(const Spellings& s, std::string_view file_text, std::string_view presumed, const Diag& d) {
  std::vector<u32> starts = line_starts(file_text);
  auto at = std::ranges::upper_bound(starts, d.offset);
  std::string text = lex::render(d.kind, d.args);
  // #error and #warning say their line's text, a spelling of the session.
  if (has_directive_text(d.kind))
    text = s.name(static_cast<u32>(d.args[0]));
  u32 line = static_cast<u32>(at - starts.begin());
  std::string name(presumed);
  // [cpp.line]: the presumed line and name its region resolved.
  if (d.presumed_line != 0) line = d.presumed_line;
  if (d.presumed_name) name = s.name(*d.presumed_name);
  const auto spec = lex::message_of(d.kind);
  return Message{std::move(name), line, d.offset - *(at - 1) + 1, spec.severity == lex::Severity::error,
                 std::move(text), spec.warning};
}

std::pair<std::string, std::vector<Message>> preprocess_to_text(Session& s, std::string_view text, std::string_view path) {
  FileId main = s.files.id(path);
  s.files.set_content(main, std::string(text));
  Flat flat = flatten(s, Unit(s).run(main, path));
  std::string out = print(s, flat);
  std::vector<Message> msgs;
  // Phases 1–4 only: -E stops before phases 5–6 ([lex.phases]).
  for (const auto& [at, d] : flat.diags)
    msgs.push_back(message(s, flat.files[d.file], s.spellings.name(flat.file_names[d.file]), d));
  return {std::move(out), std::move(msgs)};
}

std::expected<bool, std::string> take_option(std::span<const std::string_view> args, std::size_t& i, Config& cfg) {
  std::string_view a = args[i];
  constexpr std::string_view compat = "-fcompat=";
  if (a.starts_with(compat)) {
    const auto mode = a.substr(compat.size());
    if (mode == "spec") cfg.compatibility = ast::Compatibility::spec;
    else if (mode == "gcc") cfg.compatibility = ast::Compatibility::gcc;
    else if (mode == "clang") cfg.compatibility = ast::Compatibility::clang;
    else return std::unexpected(std::format("invalid value in '{}'; expected spec, gcc, or clang", a));
    return true;
  }
  if (a == "-fcompat") return std::unexpected("'-fcompat' requires '=spec', '=gcc', or '=clang'");
  if (a == "-nostdinc") {
    cfg.nostdinc = true;
    return true;
  }
  if (a == "-nostdinc++") {
    cfg.nostdincxx = true;
    return true;
  }
  for (std::string_view opt : {"-isystem", "-iquote", "-I", "-D", "-U"}) {
    if (!a.starts_with(opt)) continue;
    std::string value(a.substr(opt.size()));
    if (value.empty()) {
      if (i + 1 == args.size()) return std::unexpected(std::format("argument to '{}' is missing", opt));
      value = args[++i];
    }
    if (opt == "-I") cfg.angle_dirs.insert(cfg.angle_dirs.begin() + std::ptrdiff_t(cfg.system_from++), value);
    else if (opt == "-isystem") cfg.angle_dirs.push_back(value);
    else if (opt == "-iquote") cfg.quote_dirs.push_back(value);
    else if (opt == "-D") {
      auto eq = value.find('=');
      if (eq == value.npos) cfg.macros.emplace_back(value, "1");
      else cfg.macros.emplace_back(value.substr(0, eq), value.substr(eq + 1));
    } else cfg.macros.emplace_back(value, std::nullopt);
    return true;
  }
  if (a == "-ffreestanding") {
    cfg.freestanding = true;
    return true;
  }
  if (a == "-fno-exceptions") {
    cfg.exceptions_enabled = false;
    return true;
  }
  if (a == "-frtti" || a == "-fno-rtti") {
    cfg.rtti_enabled = a == "-frtti";
    return true;
  }
  // The last of `-fpic`, `-fPIC` and `-fno-pic` wins, as with Clang and GCC.
  if (a == "-fpic" || a == "-fPIC" || a == "-fno-pic" || a == "-fno-PIC") {
    cfg.pic = a == "-fPIC" ? 2 : a == "-fpic" ? 1 : 0;
    return true;
  }
  return false;
}

Config with_system_dirs(Config cfg, std::string_view cwd, std::string_view sdkroot) {
  auto normal = [&](std::string_view d) {
    std::filesystem::path p(d);
    if (p.is_relative() && !cwd.empty()) p = std::filesystem::path(cwd) / p;
    std::string s = p.lexically_normal().generic_string<char>();
    while (s.size() > 1 && s.ends_with('/')) s.pop_back();
    return s;
  };
  std::vector<std::string> dirs;
  const bool include_libycxx = !std::exchange(cfg.nostdincxx, false);
  std::string resource_dir = std::exchange(cfg.resource_dir, {});
  std::string target_include_dir = std::exchange(cfg.target_include_dir, {});
  if (!resource_dir.empty()) resource_dir = normal(resource_dir);
  if (!target_include_dir.empty()) target_include_dir = normal(target_include_dir);
  if (!std::exchange(cfg.nostdinc, false))
    dirs = system_dirs(*cfg.target, sdkroot, include_libycxx, resource_dir, target_include_dir);
  auto system = dirs | std::views::transform(normal) | std::ranges::to<std::vector>();
  // A `-I` or `-isystem` directory that is a system directory is searched at the system
  // directory's place only. -isystem directories are system directories too: a `-I` directory
  // that is also an `-isystem` one is searched there, as a system directory (as Clang and GCC do).
  std::vector<std::string> isystem;
  for (u32 i = cfg.system_from; i < cfg.angle_dirs.size(); ++i) isystem.push_back(normal(cfg.angle_dirs[i]));
  std::vector<std::string> kept;
  u32 from = cfg.system_from;
  for (u32 i = 0; i < cfg.angle_dirs.size(); ++i) {
    if (std::ranges::contains(system, normal(cfg.angle_dirs[i])) ||
        (i < cfg.system_from && std::ranges::contains(isystem, normal(cfg.angle_dirs[i])))) {
      if (i < cfg.system_from) --from;
      continue;
    }
    kept.push_back(std::move(cfg.angle_dirs[i]));
  }
  cfg.angle_dirs = std::move(kept);
  cfg.system_from = from;
  for (std::string& d : dirs) cfg.angle_dirs.push_back(std::move(d));
  return cfg;
}

engine::query_task<Region> Pp::run(engine::Ctx& cx, FileId file) {
  const Source& src = cx.fetch<SourceOf>(file);
  Session& s = session_for(src.config);
  EngineHost host(cx, src);
  // The editor and build views read the main file through their path inputs too. A unit the
  // view cannot read is empty here: its request checks readability before fetching Pp.
  std::string_view text;
  if (src.config.view == View::disk) text = cx.fetch<FileText>(file);
  else if (const auto& own = source_content(cx, src, src.path)) text = *own;
  Preprocessed p = [&] {
    std::lock_guard g(s.mutex);
    HostScope scope(s.files, host);
    Preprocessed result = preprocess(s, text, src.path);
    // Trace the errors at the same physical positions as the compiler's diagnostics (#404).
    struct FileLines {
      u32 size = 0;
      std::vector<u32> starts;
    };
    std::map<u16, FileLines> files;
    for (Diag& d : result.diags) {
      const lex::message info = lex::message_of(d.kind);
      if (has_directive_text(d.kind)) {
        const std::string_view text = s.spellings.name(static_cast<u32>(d.args[0]));
        const Fp128 key = fingerprint128(text);
        auto& messages = result.region.directive_messages;
        if (std::ranges::find(messages, key, &std::pair<Fp128, std::string>::first) == messages.end())
          messages.emplace_back(key, text);
        d.args = {key.lo, key.hi};
      }
      if (info.severity != lex::Severity::error || info.text[0] == 0) continue;
      auto [it, fresh] = files.try_emplace(d.file);
      if (fresh) {
        const Files::Content* content = s.files.content(s.files.id(result.region.files[d.file]));
        if (content) it->second = {static_cast<u32>(content->text.size()), line_starts(content->text)};
      }
      const auto& [size, starts] = it->second;
      if (!starts.empty()) {
        const u32 offset = std::min(d.offset, size);
        const auto line = std::ranges::upper_bound(starts, offset);
        const std::string& name = d.file == 0 ? src.path : result.region.file_names[d.file];
        std::format_to(std::back_inserter(result.region.error_details), "{}:{}:{}: ", name,
                       line - starts.begin(), offset - *(line - 1) + 1);
      }
      result.region.error_details += render_diagnostic(result.region, d.kind, d.args);
      result.region.error_details += '\n';
    }
    return result;
  }();
  for (const Diag& d : p.diags) cx.diag(engine::Loc{file_loc(d.file), d.offset}, lex::diag_id(d.kind), d.args[0], d.args[1]);
  co_return std::move(p.region);
}

}  // namespace ycxx::pp
