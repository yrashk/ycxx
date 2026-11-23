// The c++-compatible request (daemon design §3.2, §7.1): a command line in; stdout, stderr, the
// exit status and the output files out. The daemon and the in-process path run this same handler,
// so `ycxx cxx` behaves the same with or without a daemon. Files are read through a `pp::Host`
// (the daemon's file table, or the disk); the client writes the outputs (`write_outputs`).
module;
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
export module ycxx.compile;

import std;
import ycxx.obj.macho;
import ycxx.obj.elf;
import ycxx.pipeline;
import ycxx.engine;
import ycxx.pp;
import ycxx.ast.target;

export namespace ycxx::compile {

// The environment variables the driver reads, in one table both paths read (§3.2). All of them
// are configuration variables today; none only renders.
inline constexpr std::array<std::string_view, 5> environment = {"SDKROOT", "DEVELOPER_DIR", "CPATH", "CPLUS_INCLUDE_PATH",
                                                                  "SOURCE_DATE_EPOCH"};

// One request. `argv` follows the front's name, with response files expanded by the client;
// relative paths in it are relative to `cwd`.
// Whether a request's engine events go back to its client (#245): none, events, or events with
// the values of the kinds that can show theirs.
// What a request's trace is for (#245): none, the stream (events), the browser (values too), or a
// browser that shows the build while it runs (`live`, values too; #939).
enum class Trace : std::uint8_t { off, events, values, live };

struct Request {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> env;  // the set variables of `environment`
  std::int64_t build_time = 0;                           // the client's clock, seconds since the epoch
  Trace trace = Trace::off;
};

struct DependencyFile {
  std::string path;
  std::string rule;
};

// A file the client writes, at a path relative to its working directory.
struct Output {
  std::string path;
  std::vector<std::uint8_t> bytes;
  bool executable = false;
  std::optional<DependencyFile> dependency_file = std::nullopt;  // written after this object
};

// What a request cost and where it was answered (§8.1). The in-process path answers at revision 0.
struct Report {
  std::uint64_t revision = 0;        // the file table's revision the answer reflects
  std::uint32_t restarts = 0;        // times the request was stopped by a batch and run again
  std::uint64_t inputs_changed = 0;  // file-table entries changed by the batches it ran through
  std::uint64_t regions_computed = 0;  // pp regions computed and reused (#85's session counters)
  std::uint64_t regions_reused = 0;
  std::uint64_t executed = 0;   // engine cells executed and validated in this request's session
  std::uint64_t validated = 0;
  std::uint64_t started_at = 0;  // the revision it started from: a later batch may not be in it (`--watch`)
};

struct Done {
  std::int32_t exit_status = 0;
  std::string out;  // stdout
  std::string err;  // stderr
  std::vector<Output> outputs;
  Report report;
};

// Answers a driver query without a project or a build session, if argv asks for one.
std::optional<Done> driver_query(std::span<const std::string> argv);

// Polled while the request runs (every 50 ms or so, on the calling thread); true cancels it, and
// its answer is then meaningless. The daemon also applies batches from it, which restart the
// request where it stopped (daemon design §4.1).
using Stop = std::function<bool()>;

// The variables of `environment` this process has.
std::vector<std::pair<std::string, std::string>> environment_now();

// The worker pool a session runs on (daemon design §8.2), the same in-process and in the daemon:
// `YCXX_THREADS` workers (default: the hardware's concurrency) and the chaos seed
// `YCXX_CHAOS_SEED`, which perturbs the schedule for tests. Outputs do not depend on either
// (DECISIONS §4).
struct Threads {
  unsigned count = 1;
  std::optional<std::uint64_t> chaos_seed;
};
Threads threads();

// Where a request's units are (daemon design §6.1): the view their files are read through. A unit
// is named by what it is (`pipeline::Session::unit`), view included, so the editor view's units
// are apart from the disk view's and a keystroke never turns a build's unit red.
struct Units {
  pp::View view = pp::View::disk;
};

// Runs a request in `session` (#63), reading files through `host` (which reads the view `units`
// names). `stopped` is set when `stop` stopped it. `tracer` gets the events of its engine requests.
Done run(const Request& r, pp::Host& host, pipeline::Session& session, const Stop& stop, bool& stopped,
         Units units = {}, engine::Tracer* tracer = nullptr);

// The path a request's scope is found from (§2): its first source file, else its first input,
// else nothing (the working directory). Relative to the request's `cwd`.
std::optional<std::string> first_input(std::span<const std::string> argv);

// The translation units a request compiles, as absolute, lexically normal paths (the editor's
// names for them); none when its command line is malformed.
std::vector<std::string> sources_of(const Request& r);

// The `-fsyntax-only` request for one of `r`'s units keeps preprocessing and language options
// and warning controls. It spells `source` (absolute and lexically normal) as `r` spells it.
// Returns nothing when `r` does not compile `source`.
std::optional<Request> check_one(const Request& r, std::string_view source);

// The check of `source` (absolute) with another compiler's command line (`compile_commands.json`'s,
// daemon design §6.4; `args` without the compiler's name), relative to `cwd`. It keeps supported
// preprocessing options, language modes and warning controls, as for `check_one`.
Request check_command(std::span<const std::string> args, std::string cwd, std::string_view source,
                      std::vector<std::pair<std::string, std::string>> env);

// Splits a response file's text into arguments: whitespace separates them, and quotes and
// backslashes escape as in a shell's words.
std::vector<std::string> split_words(std::string_view text);

// Writes the requested files, adding any write errors to the reply.
void write_outputs(Done& d);

// Writes the outputs and prints stdout and stderr; the exit status. A failed write is reported on
// stderr after the request's own diagnostics, with status 1.
int finish(Done& d);

// Go-to-definition (#170): in the unit of `r`'s first source, the place where the entity named at
// byte `offset` of `file` (absolute: the source or a header it includes) is first declared.
std::optional<pipeline::FileOffset> definition(const Request& r, pp::Host& host, pipeline::Session& session,
                                               const std::string& file, std::uint32_t offset, Units units = {});

}  // namespace ycxx::compile

namespace ycxx::compile {
namespace {

enum class WarningFlag : std::uint8_t { all, extra, multichar, attributes, sized_deallocation, count };

struct WarningSelection {
  WarningFlag flag;
  bool enabled;
};

std::optional<WarningSelection> warning_option(std::string_view arg) {
  if (!arg.starts_with("-W")) return std::nullopt;
  arg.remove_prefix(2);
  const bool enabled = !arg.starts_with("no-");
  if (!enabled) arg.remove_prefix(3);
  constexpr std::pair<std::string_view, WarningFlag> names[] = {
      {"all", WarningFlag::all}, {"extra", WarningFlag::extra}, {"multichar", WarningFlag::multichar},
      {"attributes", WarningFlag::attributes}, {"sized-deallocation", WarningFlag::sized_deallocation}};
  for (const auto& [name, flag] : names)
    if (arg == name) return WarningSelection{flag, enabled};
  return std::nullopt;
}

// Warning flags affect presentation only. Requests with different selections can reuse the
// same preprocessing and semantic results.
class WarningOptions {
 public:
  void select(WarningSelection selection) { flags_[std::to_underlying(selection.flag)] = selection.enabled; }

  bool emits(bool error, lex::Warning warning) const {
    if (error) return true;
    const auto selected = [this](WarningFlag flag) { return flags_[std::to_underlying(flag)]; };
    // The last selection of a flag wins. A specific warning overrides its group in either order.
    switch (warning) {
      case lex::Warning::uncontrolled: return true;
      case lex::Warning::multichar:
        return selected(WarningFlag::multichar).value_or(selected(WarningFlag::all).value_or(true));
      case lex::Warning::attributes: return selected(WarningFlag::attributes).value_or(true);
      case lex::Warning::sized_deallocation:
        return selected(WarningFlag::sized_deallocation).value_or(selected(WarningFlag::extra).value_or(false));
    }
    std::unreachable();
  }

 private:
  std::array<std::optional<bool>, std::to_underlying(WarningFlag::count)> flags_{};
};

// What the c++-compatible command line asks for.
struct Options {
  enum class Stop { link, object, syntax, preprocess } stop = Stop::link;
  enum class Query { none, include, machine } query = Query::none;
  pp::Config pp;  // include paths, macros and target
  WarningOptions warnings;
  bool nostdlib = false;
  bool nostdlibxx = false;  // `-nostdlib++`: no libycxx runtime in a Darwin link
  std::optional<std::string> output, interpreter;
  bool dependencies = false;  // `-MD`
  std::optional<std::string> dependency_path;  // `-MF`
  std::vector<std::string> dependency_targets;  // `-MT`, in command-line order
  std::vector<std::string> sources;  // translation units
  std::vector<std::string> objects;  // objects named on the command line
  std::vector<std::string> archives;
  std::size_t explicit_archive_count = 0;  // subsequent archives are appended by the driver
  std::vector<pipeline::LinkOperand> operands;
  std::vector<std::string> libraries;  // shared objects in command-line order
  std::vector<std::string> undefined;  // names an archive link starts with as referenced
};

Options::Query query_option(std::string_view arg) {
  if (arg == "-dumpmachine") return Options::Query::machine;
  if (arg == "-print-file-name=include") return Options::Query::include;
  return Options::Query::none;
}

bool is_source(std::string_view f) {
  for (std::string_view ext : {".cpp", ".cc", ".cxx", ".c++", ".C", ".cppm", ".ixx"})
    if (f.ends_with(ext)) return true;
  return false;
}

bool is_shared_object(std::string_view f) {
  const auto slash = f.rfind('/');
  if (slash != std::string_view::npos) f.remove_prefix(slash + 1);
  return f.ends_with(".so") || f.contains(".so.");
}

// Parses the arguments, or says why it cannot.
std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
  Options o;
  o.pp.target = pp::native_target;
  std::vector<std::string_view> other_inputs;
  using Kind = pipeline::LinkOperand::Kind;
  const auto add_operand = [&](Kind kind, std::vector<std::string>& paths, std::string_view path) {
    o.operands.push_back({kind, paths.size()});
    paths.emplace_back(path);
  };
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    auto pp_option = pp::take_option(args, i, o.pp);
    if (!pp_option) return std::unexpected(pp_option.error());
    if (*pp_option) continue;
    // This spelling explicitly selects the existing automatic-variable initialization behavior.
    if (a == "-ftrivial-auto-var-init=uninitialized") continue;
    if (auto query = query_option(a); query != Options::Query::none) {
      if (o.query != Options::Query::machine) o.query = query;
    } else if (a == "-c") o.stop = Options::Stop::object;
    else if (a == "-fsyntax-only") o.stop = Options::Stop::syntax;
    else if (a == "-E") o.stop = Options::Stop::preprocess;
    else if (a == "-MD") o.dependencies = true;
    else if (a.starts_with("-MF") || a.starts_with("-MT")) {
      const auto option = a.substr(0, 3);
      if (a == option && i + 1 == args.size()) return std::unexpected(std::format("argument to '{}' is missing", option));
      const auto value = a == option ? args[++i] : a.substr(option.size());
      if (value.empty()) return std::unexpected(std::format("argument to '{}' is empty", option));
      if (option == "-MF") o.dependency_path = value;
      else o.dependency_targets.emplace_back(value);
    } else if (a == "-nostdlib") o.nostdlib = true;
    else if (a == "-nostdlib++") o.nostdlibxx = true;
    else if (a == "--dynamic-linker" || a.starts_with("--dynamic-linker=")) {
      if (a == "--dynamic-linker" && (i + 1 == args.size() || args[i + 1].starts_with('-')))
        return std::unexpected("argument to '--dynamic-linker' is missing");
      if (o.interpreter) return std::unexpected("duplicate '--dynamic-linker' option");
      constexpr std::string_view prefix = "--dynamic-linker=";
      o.interpreter = a == "--dynamic-linker" ? args[++i] : a.substr(prefix.size());
    } else if (a == "--target" || a.starts_with("--target=")) {
      if (a == "--target" && i + 1 == args.size()) return std::unexpected("argument to '--target' is missing");
      auto triple = a == "--target" ? args[++i] : a.substr(9);
      const ast::Target* target = ast::target_for_triple(triple);
      if (!target) return std::unexpected(std::format("unsupported target '{}'", triple));
      o.pp.target = target;
    } else if (a == "-o") {
      if (i + 1 == args.size()) return std::unexpected("argument to '-o' is missing");
      o.output = std::string(args[++i]);
    } else if (a.starts_with("-o")) o.output = std::string(a.substr(2));
    else if (a.starts_with("-ftemplate-depth=")) {
      // The limit on nested template instantiations ([temp.inst]/16), as Clang and GCC spell it.
      std::uint32_t n = 0;
      auto v = a.substr(17);
      auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
      if (ec != std::errc{} || p != v.data() + v.size() || n == 0) return std::unexpected(std::format("invalid value in '{}'", a));
      o.pp.template_depth = n;
    } else if (a.starts_with("-fclass-call-abi=")) {
      // Which classes pass in registers (DECISIONS §1.6): `itanium`, as upstream Clang and GCC,
      // or `apple`, as Apple Clang on arm64 Darwin.
      auto v = a.substr(17);
      if (v == "itanium") o.pp.call_triviality = ast::CallTriviality::itanium;
      else if (v == "apple") o.pp.call_triviality = ast::CallTriviality::apple;
      else return std::unexpected(std::format("invalid value in '{}'", a));
    } else if (a.starts_with("-std=")) {
      // The language is the C++26 working draft (DECISIONS §1.1).
      if (a != "-std=c++26" && a != "-std=c++2c") return std::unexpected(std::format("unsupported language standard '{}'; ycxx compiles C++26", a.substr(5)));
    }
    // Reflection and contracts are part of C++26, and ycxx's diagnostics are always plain:
    // what other compilers need these flags for is always on here.
    else if (a == "-pedantic-errors" || a == "-freflection" || a == "-fcontracts" || a == "-fdiagnostics-plain-output" ||
             a == "-fno-color-diagnostics")
      continue;
    else if (auto warning = warning_option(a)) o.warnings.select(*warning);
    else if (a.starts_with("-") && a != "-") return std::unexpected(std::format("unsupported option '{}'", a));
    else if (a.ends_with(".o")) add_operand(Kind::object, o.objects, a);
    else if (a.ends_with(".a")) add_operand(Kind::archive, o.archives, a);
    else if (is_source(a)) add_operand(Kind::source, o.sources, a);
    else other_inputs.push_back(a);
  }
  if (!o.pp.target)
    return std::unexpected("no default target for this build. Use '--target=x86_64-pc-linux-gnu' or '--target=arm64-apple-darwin'");
  if (o.query != Options::Query::none) return o;
  if (o.pp.pic && o.pp.target != &ast::x86_64_linux)
    return std::unexpected("'-fPIC' requires the x86-64 Linux target");
  const bool linux_link = o.pp.target == &ast::x86_64_linux && o.stop == Options::Stop::link;
  const bool darwin_link = o.pp.target == &ast::aarch64_darwin && o.stop == Options::Stop::link;
  if (o.interpreter && !linux_link)
    return std::unexpected("'--dynamic-linker' requires an x86-64 Linux executable link");
  for (std::string_view path : other_inputs) {
    if (is_shared_object(path) || o.interpreter) o.libraries.emplace_back(path);
    else return std::unexpected(std::format("'{}': unknown kind of input file", path));
  }
  if (!o.archives.empty()) {
    if (!linux_link && !darwin_link) return std::unexpected("archive inputs require an executable link");
    if (o.interpreter || !o.libraries.empty())
      return std::unexpected("archive inputs cannot be combined with explicit Linux dynamic linking");
    if (o.sources.empty() && o.objects.empty())
      return std::unexpected("an archive link requires at least one source or ordinary object");
  }
  if (!o.libraries.empty() && !linux_link)
    return std::unexpected("shared object inputs require an x86-64 Linux executable link");
  if (o.interpreter || !o.libraries.empty()) {
    if (!o.nostdlib) return std::unexpected("explicit Linux dynamic linking requires '-nostdlib'");
    if (!o.interpreter || !o.interpreter->starts_with('/') || o.interpreter->contains('\0'))
      return std::unexpected("Linux dynamic linking requires an absolute '--dynamic-linker' path");
    if (o.libraries.empty()) return std::unexpected("Linux dynamic linking requires an explicit shared object");
  }
  if (o.pp.target != &ast::x86_64_linux && o.nostdlib)
    return std::unexpected("unsupported option '-nostdlib'");
  if (o.pp.call_triviality == ast::CallTriviality::apple && !o.pp.target->apple_calls)
    return std::unexpected("invalid value in '-fclass-call-abi=apple'");
  if (o.sources.empty() && o.objects.empty()) return std::unexpected("no input files");
  o.explicit_archive_count = o.archives.size();
  // An executable for arm64 macOS links libycxx's runtime after the operands (#842).
  if (darwin_link && !o.nostdlibxx) {
    for (const auto& archive : pipeline::darwin_runtime().archives) add_operand(Kind::archive, o.archives, archive);
    o.undefined = pipeline::darwin_runtime().undefined;
  }
  if (linux_link && !o.nostdlib)
    return std::unexpected("hosted Linux linking is not supported. Use '-nostdlib' for a freestanding executable");
  if (o.output && o.stop == Options::Stop::object && o.sources.size() > 1)
    return std::unexpected("cannot specify '-o' with '-c' and several input files");
  if (!o.dependencies && (o.dependency_path || !o.dependency_targets.empty()))
    return std::unexpected("'-MF' and '-MT' require '-MD'");
  if (o.dependencies && (o.stop != Options::Stop::object || o.sources.empty()))
    return std::unexpected("'-MD' requires an object-producing '-c' invocation");
  if (o.dependency_path && o.sources.size() > 1)
    return std::unexpected("cannot specify '-MF' with several input files");
  return o;
}

std::vector<std::string_view> views(std::span<const std::string> argv) {
  return argv | std::views::transform([](const std::string& a) { return std::string_view(a); }) | std::ranges::to<std::vector>();
}

// A path of the request, as the daemon or this process opens it: relative to the request's `cwd`.
std::string in_cwd(const Request& r, const std::string& path) {
  if (path.starts_with('/') || r.cwd.empty()) return path;
  return (std::filesystem::path(r.cwd) / path).native_encoded_string();
}

pp::Config preprocessing_config(pp::Config config, const Request& r) {
  auto sdk = std::ranges::find_if(r.env, [](const auto& entry) { return entry.first == "SDKROOT"; });
  std::string sdkroot;
  if (sdk != r.env.end() && !sdk->second.empty()) sdkroot = in_cwd(r, sdk->second);
  return pp::with_system_dirs(std::move(config), r.cwd, sdkroot);
}

std::optional<std::string> read_disk(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(in), {}};
}

// Configured Darwin runtime inputs share snapshots while their size and mtime match.
// Explicit archive operands take a fresh snapshot per request (#951).
std::optional<obj::archive::input> read_archive(const std::string& name, const std::string& path,
                                               bool configured_runtime) {
  struct entry {
    std::uintmax_t size;
    std::filesystem::file_time_type modified;
    std::shared_ptr<const std::string> bytes;
  };
  static std::mutex mutex;
  static std::map<std::string, entry, std::less<>> cache;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return std::nullopt;
  std::filesystem::file_time_type modified{};
  if (configured_runtime) {
    modified = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(path); it != cache.end() && it->second.size == size && it->second.modified == modified)
      return obj::archive::input(name, it->second.bytes);
  }
  auto text = read_disk(path);
  if (!text) return std::nullopt;
  if (!configured_runtime) return obj::archive::input(name, std::move(*text));
  auto bytes = std::make_shared<const std::string>(std::move(*text));
  std::lock_guard lock(mutex);
  cache.insert_or_assign(path, entry{size, modified, bytes});
  return obj::archive::input(name, std::move(bytes));
}

// Renders diagnostics as `path:line:col: error: message` (DECISIONS §4.7; the form tools/test
// reads).
class Renderer {
 public:
  Renderer(std::string& err, std::string_view path, const WarningOptions& warnings)
      : err_(&err), path_(path), warnings_(warnings) {}
  void diagnostic(const pipeline::Message& m) {
    if (!warnings_.emits(m.error, m.warning)) return;
    std::format_to(std::back_inserter(*err_), "{}: {}: {}\n", m.where.empty() ? path_ : std::string_view(m.where),
                   m.error ? "error" : "warning", m.text);
  }
  void file_level(std::string_view message) { std::format_to(std::back_inserter(*err_), "{}: error: {}\n", path_, message); }

 private:
  std::string* err_;
  std::string_view path_;
  const WarningOptions& warnings_;
};

// The object a `-c` without `-o` writes: the source's file name with `.o`.
std::filesystem::path object_name(const std::string& source) {
  return std::filesystem::path(source).filename().replace_extension(".o");
}

// Preprocesses one source to `d.out` (`-E`), with no engine work; false after reporting errors.
bool preprocess_unit(Done& d, const pp::Config& config, const WarningOptions& warnings, const std::string& path,
                     const std::string& text) {
  pp::Session& ps = pp::session_for(config);
  std::unique_lock lock(ps.mutex);
  auto [out, messages] = pp::preprocess_to_text(ps, text, path);
  lock.unlock();
  d.out += out;
  bool errors = false;
  for (const pp::Message& m : messages) {
    if (!warnings.emits(m.error, m.warning)) continue;
    std::format_to(std::back_inserter(d.err), "{}:{}:{}: {}: {}\n", m.file, m.line, m.column, m.error ? "error" : "warning",
                   m.text);
    errors = errors || m.error;
  }
  return !errors;
}

std::int32_t fail(Done& d, std::string_view message) {
  std::format_to(std::back_inserter(d.err), "ycxx: error: {}\n", message);
  return d.exit_status = 1;
}

Done answer_query(const Options& options) {
  Done d;
  std::string_view answer = options.pp.target->triple;
  if (options.query == Options::Query::include) {
    answer = pp::resource_include_dir(*options.pp.target);
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(answer), ec)) {
      fail(d, std::format("compiler resource include directory is unavailable: '{}'", answer));
      return d;
    }
  }
  d.out = std::format("{}\n", answer);
  return d;
}

}  // namespace

std::optional<Done> driver_query(std::span<const std::string> argv) {
  if (!std::ranges::any_of(argv, [](std::string_view arg) { return query_option(arg) != Options::Query::none; }))
    return std::nullopt;
  auto opts = parse_options(views(argv));
  if (!opts) {
    Done d;
    fail(d, opts.error());
    return d;
  }
  if (opts->query == Options::Query::none) return std::nullopt;
  return answer_query(*opts);
}

std::vector<std::pair<std::string, std::string>> environment_now() {
  std::vector<std::pair<std::string, std::string>> env;
  for (std::string_view name : environment)
    if (const char* v = std::getenv(std::string(name).c_str())) env.emplace_back(name, v);
  return env;
}

Threads threads() {
  // A value that is not a decimal number is ignored, with a warning: a test that asks for chaos
  // must not silently run without it.
  auto number = [](const char* name) -> std::optional<std::uint64_t> {
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    std::uint64_t n = 0;
    auto [p, ec] = std::from_chars(v, v + std::strlen(v), n);
    if (ec != std::errc{} || *p != '\0') {
      std::println(std::cerr, "ycxx: warning: ignoring {}='{}': not a decimal number", name, v);
      return std::nullopt;
    }
    return n;
  };
  Threads t;
  t.count = std::max(1u, std::thread::hardware_concurrency());
  if (auto n = number("YCXX_THREADS"); n && *n > 0) t.count = static_cast<unsigned>(std::min<std::uint64_t>(*n, 256));
  t.chaos_seed = number("YCXX_CHAOS_SEED");
  return t;
}

std::optional<std::string> first_input(std::span<const std::string> argv) {
  auto opts = parse_options(views(argv));
  if (!opts || opts->query != Options::Query::none) return std::nullopt;
  if (!opts->sources.empty()) return opts->sources.front();
  if (!opts->objects.empty()) return opts->objects.front();
  return std::nullopt;
}

namespace {

std::string absolute_in(const Request& r, std::string_view path) {
  std::filesystem::path p(path);
  if (p.is_relative() && !r.cwd.empty()) p = std::filesystem::path(r.cwd) / p;
  return p.lexically_normal().generic_native_encoded_string();
}

// Quote a file name in a make rule. A semicolon is read during both recipe splitting and
// file-name parsing. Ninja's gcc depfile reader uses a smaller grammar for unusual names.
// An explicit `-MT` supplies make syntax and is written literally.
std::optional<std::size_t> append_make_path(std::string& rule, std::string_view path, bool target = false) {
  if (path.find_first_of("\r\n") != path.npos) return std::nullopt;
  std::size_t backslashes = 0;
  for (char c : path) {
    if (c == ';') rule.append(3 * backslashes + 3, '\\');
    else if (c == ' ' || c == '\t' || c == '#' || c == ':' || c == '*' || c == '?' || c == '[' ||
             (target ? c == '%' : c == '|'))
      rule.append(backslashes + 1, '\\');
    else if (c == '$') rule += '$';
    rule += c;
    backslashes = c == '\\' ? backslashes + 1 : 0;
  }
  return backslashes;
}

std::expected<DependencyFile, std::string> make_dependencies(
    const Request& r, const Options& opts, const std::string& object, const std::vector<std::string>& dependencies) {
  DependencyFile out;
  out.path = opts.dependency_path
                 ? *opts.dependency_path
                 : std::filesystem::path(object).replace_extension(".d").native_encoded_string();
  const auto bad_path = std::unexpected("a dependency path cannot contain a newline");
  if (opts.dependency_targets.empty()) {
    const auto trailing = append_make_path(out.rule, object, true);
    if (!trailing) return bad_path;
    out.rule.append(*trailing, '\\');
  }
  for (const std::string& target : opts.dependency_targets) {
    if (target.find_first_of("\r\n") != target.npos) return std::unexpected("'-MT' cannot contain a newline");
    if (!out.rule.empty()) out.rule += ' ';
    out.rule += target;
  }
  out.rule += ':';
  // Use the preprocessor's physical main path and each header's first observed spelling.
  std::set<std::string> seen;
  std::size_t trailing = 0;
  for (const std::string& path : dependencies) {
    if (!seen.insert(absolute_in(r, path)).second) continue;
    out.rule.append(trailing, '\\');
    out.rule += ' ';
    const auto slashes = append_make_path(out.rule, path);
    if (!slashes) return bad_path;
    trailing = *slashes;
  }
  // Keep a final backslash separate from the newline so it cannot continue the rule.
  if (trailing != 0) out.rule += ' ';
  out.rule += '\n';
  return out;
}

// The writers replace the leaf, including a symlink there. Resolve the parent as the
// filesystem does so paths through directory aliases name the same destination.
std::optional<std::filesystem::path> output_destination(const std::filesystem::path& path) {
  std::error_code ec;
  const auto parent = path.parent_path();
  auto directory = std::filesystem::canonical(parent.empty() ? std::filesystem::path(".") : parent, ec);
  if (ec) return std::nullopt;
  return directory / path.filename();
}

std::optional<struct stat> output_entry(const std::filesystem::path& path) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) return std::nullopt;
  return status;
}

bool same_inode(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Stage a complete rule beside its destination, using a unique temporary file for each writer.
std::expected<std::filesystem::path, std::string> stage_dependencies(
    const DependencyFile& dep, const std::filesystem::path& path) {
  std::error_code ec;
  using std::filesystem::file_type;
  const auto type = std::filesystem::symlink_status(path, ec).type();
  if (type != file_type::regular && type != file_type::symlink && type != file_type::not_found)
    return std::unexpected(std::format("cannot write dependency file '{}'", dep.path));
  auto staging = (path.parent_path() / ".ycxx-dep-XXXXXX").native_encoded_string();
  const int fd = ::mkstemp(staging.data());
  if (fd < 0) return std::unexpected(std::format("cannot open dependency file '{}' for writing", dep.path));
  if (std::FILE* stream = ::fdopen(fd, "wb")) {
    const auto written = std::fwrite(dep.rule.data(), 1, dep.rule.size(), stream);
    if (std::fclose(stream) == 0 && written == dep.rule.size()) return std::filesystem::path(staging);
  } else ::close(fd);
  std::filesystem::remove(staging, ec);
  return std::unexpected(std::format("cannot write dependency file '{}'", dep.path));
}

// Publish a complete regular object with the usual creation permissions. Devices and pipes
// still use the ordinary writer and have no newly installed file to retire.
std::expected<std::optional<struct stat>, std::string> write_paired_object(
    const Output& output, const std::filesystem::path& path) {
  std::error_code ec;
  using std::filesystem::file_type;
  const auto type = std::filesystem::symlink_status(path, ec).type();
  if (type != file_type::regular && type != file_type::symlink && type != file_type::not_found) {
    if (auto error = pipeline::write_file(path, output.bytes, output.executable)) return std::unexpected(*error);
    return std::nullopt;
  }

  auto name = (path.parent_path() / ".ycxx-obj-XXXXXX").native_encoded_string();
  if (!::mkdtemp(name.data())) return std::unexpected(std::format("cannot stage object file '{}'", output.path));
  const std::filesystem::path directory(name);
  const auto staged = directory / "object";
  const auto entry = pipeline::write_file(staged, output.bytes, output.executable) ? std::nullopt : output_entry(staged);
  const bool ready = entry && S_ISREG(entry->st_mode);
  std::error_code install;
  if (ready) std::filesystem::rename(staged, path, install);
  if (!ready || install) std::filesystem::remove(staged, ec);
  std::filesystem::remove(directory, ec);
  if (!ready) return std::unexpected(std::format("cannot stage object file '{}'", output.path));
  if (install) return std::unexpected(std::format("cannot write '{}': {}", output.path, install.message()));
  return entry;
}

std::optional<std::string> write_object_and_dependencies(const Output& output, const DependencyFile& dep) {
  const auto object_path = output_destination(output.path);
  if (!object_path) return std::format("cannot open '{}' for writing", output.path);
  const auto dep_path = output_destination(dep.path);
  if (!dep_path) return std::format("cannot open dependency file '{}' for writing", dep.path);
  constexpr auto collision = "the dependency file and object output must be different paths";
  if (*object_path == *dep_path) return collision;
  // A single link proves two spellings name one existing entry. Distinct links can share an inode.
  if (const auto before = output_entry(*object_path); before && before->st_nlink == 1) {
    if (const auto existing = output_entry(*dep_path); existing && same_inode(*before, *existing)) return collision;
  }
  auto staged = stage_dependencies(dep, *dep_path);
  if (!staged) return staged.error();

  std::error_code ec;
  const auto published = write_paired_object(output, *object_path);
  if (!published) {
    std::filesystem::remove(*staged, ec);
    return published.error();
  }
  std::string error;
  const auto existing = output_entry(*dep_path);
  if (*published && existing && same_inode(**published, *existing)) {
    // The second spelling can resolve to the fresh object on a case-insensitive filesystem.
    error = collision;
  } else {
    std::filesystem::rename(*staged, *dep_path, ec);
    if (!ec) return std::nullopt;
    error = std::format("cannot write dependency file '{}': {}", dep.path, ec.message());
  }
  // A fresh object with an old depfile could make the next build miss required work.
  const auto current = output_entry(*object_path);
  if (*published && current && same_inode(**published, *current)) {
    std::filesystem::remove(*object_path, ec);
    if (ec) error += std::format(" (cannot remove newly written object '{}': {})", output.path, ec.message());
  }
  std::filesystem::remove(*staged, ec);
  return error;
}

}  // namespace

std::vector<std::string> sources_of(const Request& r) {
  auto opts = parse_options(views(r.argv));
  if (!opts || opts->query != Options::Query::none) return {};
  return opts->sources | std::views::transform([&](const std::string& s) { return absolute_in(r, s); }) |
         std::ranges::to<std::vector>();
}

std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> out;
  std::string cur;
  bool in_word = false;
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
      else if (c == '\\' && quote == '"' && i + 1 < text.size()) cur += text[++i];
      else cur += c;
    } else if (c == '\'' || c == '"') {
      quote = c;
      in_word = true;
    } else if (c == '\\' && i + 1 < text.size()) {
      cur += text[++i];
      in_word = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) out.push_back(std::exchange(cur, {}));
      in_word = false;
    } else {
      cur += c;
      in_word = true;
    }
  }
  if (in_word) out.push_back(cur);
  return out;
}

namespace {

// A check keeps the preprocessing options, template depth, class call ABI and warning controls.
// With `std` it also keeps the language standard. Another compiler may name one ycxx does not take.
void checking_options(std::span<const std::string_view> args, bool std, std::vector<std::string>& out) {
  pp::Config scratch;
  scratch.target = pp::native_target;
  for (std::size_t i = 0; i < args.size(); ++i) {
    // Dependency-output operands are not options of the syntax check, even if they start with '-'.
    if (args[i] == "-MF" || args[i] == "-MT") {
      ++i;
      continue;
    }
    const std::size_t from = i;
    if (auto taken = pp::take_option(args, i, scratch); taken && *taken) {
      if (args[from] == "-fPIC") continue;  // Forward it only after the final target is known.
      for (std::size_t k = from; k <= i; ++k) out.emplace_back(args[k]);
    } else if (args[i] == "--target" && i + 1 < args.size()) {
      if (const auto* target = ast::target_for_triple(args[i + 1])) {
        scratch.target = target;
        out.emplace_back(args[i]);
        out.emplace_back(args[i + 1]);
      }
      ++i;
    } else if (args[i].starts_with("--target=")) {
      if (const auto* target = ast::target_for_triple(args[i].substr(9))) {
        scratch.target = target;
        out.emplace_back(args[i]);
      }
    } else if (args[i] == "-o" || args[i] == "--dynamic-linker") {
      ++i;
    } else if ((std && args[i].starts_with("-std=")) || args[i].starts_with("-ftemplate-depth=") ||
               args[i].starts_with("-fclass-call-abi=") || warning_option(args[i])) {
      out.emplace_back(args[i]);
    }
  }
  if (scratch.pic && scratch.target == &ast::x86_64_linux) out.emplace_back("-fPIC");
}

}  // namespace

Request check_command(std::span<const std::string> args, std::string cwd, std::string_view source,
                      std::vector<std::pair<std::string, std::string>> env) {
  Request one{{}, std::move(cwd), std::move(env), 0};
  checking_options(views(std::vector<std::string>(args.begin(), args.end())), false, one.argv);
  one.argv.emplace_back("-fsyntax-only");
  one.argv.emplace_back(source);
  return one;
}

std::optional<Request> check_one(const Request& r, std::string_view source) {
  auto opts = parse_options(views(r.argv));
  if (!opts || opts->query != Options::Query::none) return std::nullopt;
  auto spelled = std::ranges::find_if(opts->sources, [&](const std::string& s) { return absolute_in(r, s) == source; });
  if (spelled == opts->sources.end()) return std::nullopt;
  Request one{{}, r.cwd, r.env, r.build_time};
  // The preprocessing and language options, as given; the action, the output and the other
  // inputs are this request's own.
  checking_options(views(r.argv), true, one.argv);
  one.argv.emplace_back("-fsyntax-only");
  one.argv.push_back(*spelled);
  return one;
}

// The c++-compatible front: `[-c|-fsyntax-only|-E] [-o out] [-std=c++26] a.cpp...`. Exits 0 on
// success and 1 on any error in the input or the command line.
Done run(const Request& r, pp::Host& host, pipeline::Session& session, const Stop& stop, bool& stopped,
         Units units, engine::Tracer* tracer) {
  Done d;
  stopped = false;
  auto opts = parse_options(views(r.argv));
  if (!opts) {
    fail(d, opts.error());
    return d;
  }
  if (opts->query != Options::Query::none) return answer_query(*opts);
  pp::Config config = preprocessing_config(std::move(opts->pp), r);
  config.view = units.view;
  pp::Session& ps = pp::configure(config, host);
  auto stats = [&] {
    std::lock_guard lock(ps.mutex);
    return ps.stats;
  };
  const pp::Session::Stats before = stats();
  auto count_regions = [&] {
    const pp::Session::Stats after = stats();
    d.report.regions_computed = after.computed - before.computed;
    d.report.regions_reused = after.reused - before.reused;
  };
  bool failed = false;
  if (opts->stop == Options::Stop::preprocess) {
    for (const std::string& s : opts->sources) {
      auto text = host.read(absolute_in(r, s));  // never through a host's working directory: requests run at once
      if (!text) {
        std::format_to(std::back_inserter(d.err), "ycxx: error: cannot read '{}'\n", s);
        failed = true;
        continue;
      }
      failed = !preprocess_unit(d, config, opts->warnings, s, *text) || failed;
    }
    count_regions();
    if (failed) d.exit_status = 1;
    return d;
  }

  // The units, named by what they are (#139), and the whole request as one root.
  pipeline::BuildKey key;
  key.until = opts->stop == Options::Stop::syntax ? pipeline::Until::syntax
              : opts->stop == Options::Stop::object ? pipeline::Until::object
                                                    : pipeline::Until::link;
  key.want_dependencies = opts->dependencies;
  for (const std::string& s : opts->sources)
    key.units.push_back(session.unit({s, r.cwd, config, object_name(s).native_encoded_string()}));
  std::filesystem::path out = opts->output.value_or("a.out");
  // An unusable link input is reported after the units' diagnostics, and only when they compiled.
  std::optional<std::string> bad_input;
  if (key.until == pipeline::Until::link) {
    std::vector<obj::object> objects;
    std::vector<obj::archive::input> archives;
    const bool linux_target = config.target == &ast::x86_64_linux;
    // Snapshot disk inputs at ingress, outside the file table (§5.2).
    using Kind = pipeline::LinkOperand::Kind;
    for (const auto [kind, index] : opts->operands) {
      if (kind == Kind::source) continue;
      const auto& path = kind == Kind::archive ? opts->archives[index] : opts->objects[index];
      if (kind == Kind::archive) {
        const bool configured_runtime = index >= opts->explicit_archive_count;
        auto archive = read_archive(path, in_cwd(r, path), configured_runtime);
        if (!archive) {
          bad_input = configured_runtime
                          ? std::format("cannot read libycxx's runtime '{}': rebuild ycxx, or link without it "
                                        "using '-nostdlib++'", path)
                          : std::format("cannot read '{}'", path);
          break;
        }
        archives.push_back(std::move(*archive));
        continue;
      }
      auto text = read_disk(in_cwd(r, path));
      if (!text) {
        bad_input = std::format("cannot read '{}'", path);
        break;
      }
      const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text->data()), text->size());
      auto o = linux_target ? obj::elf::read_object(bytes, path) : obj::macho::read_object(bytes, path);
      if (!o) {
        bad_input = o.error();
        break;
      }
      objects.push_back(std::move(*o));
    }
    std::optional<link::elf::dynamic_options> dynamic;
    if (!bad_input && opts->interpreter) {
      dynamic.emplace(link::elf::dynamic_options{*opts->interpreter, {}});
      for (const std::string& path : opts->libraries) {
        auto text = read_disk(in_cwd(r, path));
        if (!text) {
          bad_input = std::format("cannot read '{}'", path);
          break;
        }
        const std::span bytes(reinterpret_cast<const std::uint8_t*>(text->data()), text->size());
        auto library = link::elf::read_shared_object(bytes, path);
        if (!library) {
          bad_input = library.error();
          break;
        }
        dynamic->libraries.push_back(std::move(*library));
      }
    }
    std::optional<pipeline::ArchiveLinkInputs> archive_link;
    if (!archives.empty())
      archive_link.emplace(pipeline::ArchiveLinkInputs{
          linux_target ? obj::target::x86_64_linux : obj::target::aarch64_macos, std::move(opts->operands),
          std::move(archives), std::move(opts->undefined)});
    if (bad_input) key.until = pipeline::Until::object;
    else key.program = session.program(
        {key.units, std::move(objects), out.filename().native_encoded_string(), std::move(dynamic), std::move(archive_link)});
  }
  const pipeline::Until until = bad_input ? pipeline::Until::link : key.until;
  pipeline::RunStats rs;
  std::optional<pipeline::Built> built = session.run<pipeline::Build>(session.request(std::move(key)), stop, &rs, tracer);
  if (!built) {
    stopped = true;
    return d;
  }
  d.report.restarts = static_cast<std::uint32_t>(rs.restarts);
  d.report.executed = rs.engine.executed;
  d.report.validated = rs.engine.validated;
  count_regions();

  for (std::size_t i = 0; i < opts->sources.size(); ++i) {
    const pipeline::UnitBuilt& u = built->units[i];
    const std::string& path = opts->sources[i];
    if (!u.readable) {
      std::format_to(std::back_inserter(d.err), "ycxx: error: cannot read '{}'\n", path);
      failed = true;
      continue;
    }
    Renderer render(d.err, path, opts->warnings);
    for (const pipeline::Message& m : u.messages) render.diagnostic(m);
    if (u.front_failed) {
      failed = true;
      continue;
    }
    if (until == pipeline::Until::syntax) continue;
    for (const pipeline::Message& m : u.errors) render.diagnostic(m);
    if (!u.object) {
      failed = true;
      continue;
    }
    if (until == pipeline::Until::object) {
      Output output;
      output.path = opts->output ? *opts->output : object_name(path).native_encoded_string();
      if (opts->dependencies) {
        auto dep = make_dependencies(r, *opts, output.path, u.dependencies);
        if (!dep) {
          render.file_level(dep.error());
          failed = true;
          continue;
        }
        output.dependency_file = std::move(*dep);
      }
      if (u.object->arch == obj::target::x86_64_linux) {
        auto bytes = obj::elf::write_object(*u.object);
        if (!bytes) {
          render.file_level(bytes.error());
          failed = true;
          continue;
        }
        output.bytes = std::move(*bytes);
      } else output.bytes = obj::macho::write_object(*u.object);
      d.outputs.push_back(std::move(output));
    }
  }
  if (failed || until != pipeline::Until::link) {
    if (failed) d.exit_status = 1;
    return d;
  }
  if (bad_input) {
    fail(d, *bad_input);
    return d;
  }
  const pipeline::Linked& image = *built->linked;
  if (!image.errors.empty()) {
    for (const std::string& e : image.errors) std::format_to(std::back_inserter(d.err), "ycxx: error: {}\n", e);
    d.exit_status = 1;
    return d;
  }
  d.outputs.push_back({out.native_encoded_string(), image.image, true});
  return d;
}

std::optional<pipeline::FileOffset> definition(const Request& r, pp::Host& host, pipeline::Session& session,
                                               const std::string& file, std::uint32_t offset, Units units) {
  auto opts = parse_options(views(r.argv));
  if (!opts || opts->query != Options::Query::none || opts->sources.empty()) return std::nullopt;
  pp::Config config = preprocessing_config(std::move(opts->pp), r);
  config.view = units.view;
  pp::configure(config, host);
  const std::string& s = opts->sources.front();
  const pipeline::TuId tu = session.unit({s, r.cwd, config, object_name(s).native_encoded_string()});
  auto found = session.run<pipeline::DefinitionAt>({tu, pp::path_id(file), offset});
  return found ? *found : std::nullopt;
}

void write_outputs(Done& d) {
  for (const Output& o : d.outputs) {
    auto err = o.dependency_file ? write_object_and_dependencies(o, *o.dependency_file)
                                 : pipeline::write_file(o.path, o.bytes, o.executable);
    if (err) fail(d, *err);
  }
}

int finish(Done& d) {
  write_outputs(d);
  std::cout << d.out << std::flush;
  std::cerr << d.err << std::flush;
  return d.exit_status;
}

}  // namespace ycxx::compile
