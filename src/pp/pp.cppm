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
// directory and configuration). Every file it includes is read as a `FileContent` input, provided
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
export import :system;
export import :predefined;
export import :cond;

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

// Phases 1–6 of one translation unit, as a function: `text` is the main file's, `path` its name
// as given (it locates `#include "..."` and is its presumed name).
Preprocessed preprocess(Session& s, std::string_view text, std::string_view path);
Preprocessed preprocess(std::string_view text, std::string_view path = "<stdin>");
// A diagnostic ready to print: the presumed file name, line and column (1-based, columns in
// bytes), severity and message.
struct Message {
  std::string file;
  u32 line = 0;
  u32 column = 0;
  bool error = true;
  std::string text;
};
Message message(Session& s, FileId file, std::string_view presumed, const Diag& d);
// The same, from the text of the file the diagnostic is in and the spellings of the session that
// preprocessed it.
Message message(const Spellings& sp, std::string_view text, std::string_view presumed, const Diag& d);
// The process's session for a configuration (the one `Pp` uses for a unit with that `SourceOf`).
// Sessions live as long as the process: the regions `Pp` returns hold their spellings' numbers.
Session& session_for(const Config& cfg);

// `-E`: the text Clang's `-E` would print, and the diagnostics.
std::pair<std::string, std::vector<Message>> preprocess_to_text(Session& s, std::string_view text, std::string_view path);

// Takes a preprocessor option at `args[i]` (`-I dir`, `-Idir`, `-isystem`, `-iquote`, `-D`,
// `-U`), advancing `i` past its argument: true if it was one, an error if its argument is missing.
std::expected<bool, std::string> take_option(std::span<const std::string_view> args, std::size_t& i, Config& cfg);
// The configuration with the system directories after those the options gave.
// The system directories after the command line's. A `-I` or `-isystem` directory that names one
// of them (relative to `cwd`) is dropped, as GCC and Clang do, so it is searched once, at its
// system place, and `#include_next` from it goes on past it.
Config with_system_dirs(Config cfg, std::string_view cwd = {});

// The item of a file-level diagnostic location in the region's file `file` (lex::file_level for
// the main file). Seam for #34: diagnostics before items exist are located by byte offset.
constexpr u32 file_loc(u16 file) { return lex::file_level - file; }

// The text of a file: an input, set by the driver.
struct [[=engine::input{engine::Durability::low}]] FileText {
  using Key = FileId;
  using Value = std::string;
};

// The bytes of the regular file at a path, or nothing: an input read on demand through the
// provider its `Db` was given (the disk, or the daemon's file table, daemon design §5.1), and set by
// the daemon when a batch changes the file (§5.3).
struct [[=engine::input{engine::Durability::low}]] FileContent {
  using Key = PathId;
  using Value = std::optional<std::string>;
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
// A provider of `FileContent` that reads the disk, for a `Db` with no daemon behind it.
engine::provided<std::optional<std::string>> read_disk(PathId p);

// How a driver's file is preprocessed: its name as given (its presumed name, and where
// `#include "..."` starts), the directory relative paths are relative to, and the configuration.
struct Source {
  std::string path;
  std::string cwd;
  Config config;
};
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
}
Source deserialize_from(engine::Source& in, std::type_identity<Source>) {
  Source s;
  s.path = engine::deserialize<std::string>(in);
  s.cwd = engine::deserialize<std::string>(in);
  s.config.quote_dirs = engine::deserialize<std::vector<std::string>>(in);
  s.config.angle_dirs = engine::deserialize<std::vector<std::string>>(in);
  s.config.system_from = static_cast<std::size_t>(engine::deserialize<std::uint64_t>(in));
  s.config.macros = engine::deserialize<std::vector<std::pair<std::string, std::optional<std::string>>>>(in);
  if (engine::deserialize<std::string>(in) != ast::aarch64_darwin.triple) in.fail();  // the one target there is
  const auto view = engine::deserialize<std::uint64_t>(in);
  if (view > std::to_underlying(View::editor)) in.fail();
  s.config.view = static_cast<View>(view);
  s.config.template_depth = static_cast<std::uint32_t>(engine::deserialize<std::uint64_t>(in));
  return s;
}

struct [[=engine::input{engine::Durability::low}]] SourceOf {
  using Key = FileId;
  using Value = Source;
};

// `pp(file)` (§B9). Its diagnostics are at `Loc{file_loc(file index), byte offset}`.
struct Pp {
  using Key = FileId;
  using Value = Region;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
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

// Inside `Pp`: every file read is a `FileContent` fetch, relative paths against the source's
// working directory, so the cell records each one (negatives included). In the editor view an
// open buffer comes first (§6.1); a disk-view read never fetches a `Buffer`, so no disk-view cell
// takes the `editor` durability.
class EngineHost final : public Host {
 public:
  EngineHost(engine::Ctx& cx, std::string_view cwd, View view) : cx_(&cx), cwd_(cwd), view_(view) {}
  std::optional<std::string> read(const std::string& path) override {
    std::filesystem::path p(path);
    if (p.is_relative() && !cwd_.empty()) p = std::filesystem::path(cwd_) / p;
    const PathId id = path_id(p.lexically_normal().generic_native_encoded_string());
    if (view_ == View::editor)
      if (const auto& open = cx_->fetch<Buffer>(id)) return *open;
    return cx_->fetch<FileContent>(id);
  }

 private:
  engine::Ctx* cx_;
  std::string_view cwd_;
  View view_;
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

Preprocessed preprocess(Session& s, std::string_view text, std::string_view path) {
  FileId main = s.files.id(path);
  s.files.set_content(main, std::string(text));
  Tree t = Unit(s).run(main, path);
  return to_region(s, flatten(s, t));
}

Preprocessed preprocess(std::string_view text, std::string_view path) {
  Session& s = session();
  std::lock_guard g(s.mutex);
  return preprocess(s, text, path);
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
  if (d.kind == lex::LexDiag::error_directive || d.kind == lex::LexDiag::warning_directive)
    text = s.name(static_cast<u32>(d.args[0]));
  u32 line = static_cast<u32>(at - starts.begin());
  std::string name(presumed);
  // [cpp.line]: the presumed line and name its region resolved.
  if (d.presumed_line != 0) line = d.presumed_line;
  if (d.presumed_name) name = s.name(*d.presumed_name);
  return Message{std::move(name), line, d.offset - *(at - 1) + 1, lex::message_of(d.kind).severity == lex::Severity::error,
                 std::move(text)};
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
  return false;
}

Config with_system_dirs(Config cfg, std::string_view cwd) {
  auto normal = [&](std::string_view d) {
    std::filesystem::path p(d);
    if (p.is_relative() && !cwd.empty()) p = std::filesystem::path(cwd) / p;
    std::string s = p.lexically_normal().generic_native_encoded_string();
    while (s.size() > 1 && s.ends_with('/')) s.pop_back();
    return s;
  };
  std::vector<std::string> system;
  for (const std::string& d : system_dirs()) system.push_back(normal(d));
  // A `-I` or `-isystem` directory that is a system directory is searched at the system
  // directory's place only. -isystem directories are system directories too.
  std::vector<std::string> kept;
  u32 from = cfg.system_from;
  for (u32 i = 0; i < cfg.angle_dirs.size(); ++i) {
    if (std::ranges::contains(system, normal(cfg.angle_dirs[i]))) {
      if (i < cfg.system_from) --from;
      continue;
    }
    kept.push_back(std::move(cfg.angle_dirs[i]));
  }
  cfg.angle_dirs = std::move(kept);
  cfg.system_from = from;
  for (std::string& d : system_dirs()) cfg.angle_dirs.push_back(std::move(d));
  return cfg;
}

engine::query_task<Region> Pp::run(engine::Ctx& cx, FileId file) {
  const Source& src = cx.fetch<SourceOf>(file);
  Session& s = session_for(src.config);
  EngineHost host(cx, src.cwd, src.config.view);
  // The editor view reads the unit's own text through the view too (§6.1), so a keystroke in it
  // changes an `editor` input, never the `low` `FileText` (which it leaves unset). A unit the view
  // cannot read is empty: its driver reports that before asking.
  std::string own;
  if (src.config.view == View::editor) own = host.read(src.path).value_or(std::string());
  Preprocessed p = [&] {
    std::lock_guard g(s.mutex);
    HostScope scope(s.files, host);
    return preprocess(s, src.config.view == View::editor ? std::string_view(own) : cx.fetch<FileText>(file), src.path);
  }();
  for (const Diag& d : p.diags) cx.diag(engine::Loc{file_loc(d.file), d.offset}, lex::diag_id(d.kind), d.args[0], d.args[1]);
  co_return std::move(p.region);
}

}  // namespace ycxx::pp
