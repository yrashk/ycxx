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
//   :system  the build's system include directories
//
// `pp(file)` is an engine query over the main file's text. The files it includes and the
// configuration are read through the process's session (`session()`) until the engine has inputs
// read on demand (#93) and contextual queries (#16); the regions' traces carry those dependencies.
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

export namespace ycxx::pp {

// The session of this process: the disk, and the configuration the driver sets before the first
// translation unit.
Session& session();
void configure(Config cfg);
// The path of a file the driver numbered itself (a `FileText` input).
void name_file(FileId driver_file, std::string path);

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

// `-E`: the text Clang's `-E` would print, and the diagnostics.
std::pair<std::string, std::vector<Message>> preprocess_to_text(Session& s, std::string_view text, std::string_view path);

// Takes a preprocessor option at `args[i]` (`-I dir`, `-Idir`, `-isystem`, `-iquote`, `-D`,
// `-U`), advancing `i` past its argument: true if it was one, an error if its argument is missing.
std::expected<bool, std::string> take_option(std::span<const std::string_view> args, std::size_t& i, Config& cfg);
// The configuration with the system directories after those the options gave.
Config with_system_dirs(Config cfg);

// The item of a file-level diagnostic location in the region's file `file` (lex::file_level for
// the main file). Seam for #34: diagnostics before items exist are located by byte offset.
constexpr u32 file_loc(u16 file) { return lex::file_level - file; }

// The text of a file: an input, set by the driver.
struct [[=engine::input{engine::Durability::low}]] FileText {
  using Key = FileId;
  using Value = std::string;
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
  DiskHost disk;
  std::unique_ptr<Session> session;
  std::map<FileId, std::string> driver_names;
};
Process& process() {
  static Process p;
  return p;
}

// The driver numbers its own files below this; the session numbers files from 0 too, so a
// driver's number is translated by name.
std::pair<FileId, std::string> main_file(Session& s, FileId driver_file) {
  auto& names = process().driver_names;
  auto it = names.find(driver_file);
  std::string path = it == names.end() ? std::format("<file {}>", std::to_underlying(driver_file)) : it->second;
  return {s.files.id(path), path};
}

}  // namespace ycxx::pp

export namespace ycxx::pp {

Session& session() {
  Process& p = process();
  if (!p.session) p.session = std::make_unique<Session>(p.disk);
  return *p.session;
}

void configure(Config cfg) {
  Process& p = process();
  p.session = std::make_unique<Session>(p.disk, std::move(cfg));
}

void name_file(FileId driver_file, std::string path) { process().driver_names.insert_or_assign(driver_file, std::move(path)); }

Preprocessed preprocess(Session& s, std::string_view text, std::string_view path) {
  FileId main = s.files.id(path);
  s.files.set_content(main, std::string(text));
  Tree t = Unit(s).run(main, path);
  return to_region(s, flatten(s, t));
}

Preprocessed preprocess(std::string_view text, std::string_view path) { return preprocess(session(), text, path); }

Message message(Session& s, FileId file, std::string_view presumed, const Diag& d) {
  const Files::Content* c = s.files.content(file);
  std::vector<u32> starts = line_starts(c ? std::string_view(c->text) : std::string_view());
  auto at = std::ranges::upper_bound(starts, d.offset);
  return Message{std::string(presumed), static_cast<u32>(at - starts.begin()), d.offset - *(at - 1) + 1,
                 lex::message_of(d.kind).severity == lex::Severity::error, lex::render(d.kind, d.args)};
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

Config with_system_dirs(Config cfg) {
  // -isystem directories are system directories too.
  for (std::string& d : system_dirs()) cfg.angle_dirs.push_back(std::move(d));
  return cfg;
}

engine::query_task<Region> Pp::run(engine::Ctx& cx, FileId file) {
  Session& s = session();
  auto [main, path] = main_file(s, file);
  Preprocessed p = preprocess(s, cx.fetch<FileText>(file), path);
  for (const Diag& d : p.diags) cx.diag(engine::Loc{file_loc(d.file), d.offset}, lex::diag_id(d.kind), d.args[0], d.args[1]);
  co_return std::move(p.region);
}

}  // namespace ycxx::pp
