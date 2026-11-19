// The c++-compatible request (daemon design §3.2, §7.1): a command line in; stdout, stderr, the
// exit status and the output files out. The daemon and the in-process path run this same handler,
// so `ycxx cxx` behaves the same with or without a daemon. Files are read through a `pp::Host`
// (the daemon's file table, or the disk); the client writes the outputs (`write_outputs`).
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
enum class Trace : std::uint8_t { off, events, values };

struct Request {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> env;  // the set variables of `environment`
  std::int64_t build_time = 0;                           // the client's clock, seconds since the epoch
  Trace trace = Trace::off;
};

// A file the client writes, at a path relative to its working directory.
struct Output {
  std::string path;
  std::vector<std::uint8_t> bytes;
  bool executable = false;
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

// The request that checks one of `r`'s units: `-fsyntax-only` with `r`'s preprocessing and
// language options, `source` (absolute, lexically normal) spelled as `r` spells it. Nothing when
// `r` does not compile `source`.
std::optional<Request> check_one(const Request& r, std::string_view source);

// The check of `source` (absolute) with another compiler's command line (`compile_commands.json`'s,
// daemon design §6.4; `args` without the compiler's name), relative to `cwd`: its preprocessing
// options and template depth, as for `check_one`; its other options are that compiler's.
Request check_command(std::span<const std::string> args, std::string cwd, std::string_view source,
                      std::vector<std::pair<std::string, std::string>> env);

// Splits a response file's text into arguments: whitespace separates them, and quotes and
// backslashes escape as in a shell's words.
std::vector<std::string> split_words(std::string_view text);

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

// What the c++-compatible command line asks for.
struct Options {
  enum class Stop { link, object, syntax, preprocess } stop = Stop::link;
  pp::Config pp;  // include paths, macros and target
  bool nostdlib = false;
  std::optional<std::string> output;
  std::vector<std::string> sources;  // translation units
  std::vector<std::string> objects;  // objects named on the command line
};

bool is_source(std::string_view f) {
  for (std::string_view ext : {".cpp", ".cc", ".cxx", ".c++", ".C", ".cppm", ".ixx"})
    if (f.ends_with(ext)) return true;
  return false;
}

// Parses the arguments, or says why it cannot.
std::expected<Options, std::string> parse_options(std::span<const std::string_view> args) {
  Options o;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    auto pp_option = pp::take_option(args, i, o.pp);
    if (!pp_option) return std::unexpected(pp_option.error());
    if (*pp_option) continue;
    if (a == "-c") o.stop = Options::Stop::object;
    else if (a == "-fsyntax-only") o.stop = Options::Stop::syntax;
    else if (a == "-E") o.stop = Options::Stop::preprocess;
    else if (a == "-nostdlib") o.nostdlib = true;
    else if (a == "--target" || a.starts_with("--target=")) {
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
    else if (a.starts_with("-") && a != "-") return std::unexpected(std::format("unsupported option '{}'", a));
    else if (a.ends_with(".o")) o.objects.emplace_back(a);
    else o.sources.emplace_back(a);
  }
  if (o.pp.target != &ast::x86_64_linux && o.nostdlib)
    return std::unexpected("unsupported option '-nostdlib'");
  if (o.pp.call_triviality == ast::CallTriviality::apple && !o.pp.target->apple_calls)
    return std::unexpected("invalid value in '-fclass-call-abi=apple'");
  if (o.sources.empty() && o.objects.empty()) return std::unexpected("no input files");
  if (o.pp.target == &ast::x86_64_linux && o.stop == Options::Stop::link && !o.nostdlib)
    return std::unexpected("hosted Linux linking is not supported. Use '-nostdlib' for a freestanding executable");
  if (o.output && o.stop == Options::Stop::object && o.sources.size() > 1)
    return std::unexpected("cannot specify '-o' with '-c' and several input files");
  for (const std::string& s : o.sources)
    if (!is_source(s)) return std::unexpected(std::format("'{}': unknown kind of input file", s));
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

std::optional<std::string> read_disk(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(in), {}};
}

// Renders diagnostics as `path:line:col: error: message` (DECISIONS §4.7; the form tools/test
// reads).
class Renderer {
 public:
  Renderer(std::string& err, std::string_view path) : err_(&err), path_(path) {}
  void diagnostic(const pipeline::Message& m) {
    std::format_to(std::back_inserter(*err_), "{}: {}: {}\n", m.where.empty() ? path_ : std::string_view(m.where),
                   m.error ? "error" : "warning", m.text);
  }
  void file_level(std::string_view message) { std::format_to(std::back_inserter(*err_), "{}: error: {}\n", path_, message); }

 private:
  std::string* err_;
  std::string_view path_;
};

// The object a `-c` without `-o` writes: the source's file name with `.o`.
std::filesystem::path object_name(const std::string& source) {
  return std::filesystem::path(source).filename().replace_extension(".o");
}

// Preprocesses one source to `d.out` (`-E`), with no engine work; false after reporting errors.
bool preprocess_unit(Done& d, const pp::Config& config, const std::string& path, const std::string& text) {
  pp::Session& ps = pp::session_for(config);
  std::unique_lock lock(ps.mutex);
  auto [out, messages] = pp::preprocess_to_text(ps, text, path);
  lock.unlock();
  d.out += out;
  bool errors = false;
  for (const pp::Message& m : messages) {
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

}  // namespace

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
  if (!opts) return std::nullopt;
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

}  // namespace

std::vector<std::string> sources_of(const Request& r) {
  auto opts = parse_options(views(r.argv));
  if (!opts) return {};
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

// The arguments of `args` a check keeps: the preprocessing options, the template depth and the
// class call ABI, and with `std` the language standard (ycxx's own command lines; another
// compiler's may name one ycxx does not take).
void checking_options(std::span<const std::string_view> args, bool std, std::vector<std::string>& out) {
  pp::Config scratch;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::size_t from = i;
    if (auto taken = pp::take_option(args, i, scratch); taken && *taken) {
      for (std::size_t k = from; k <= i; ++k) out.emplace_back(args[k]);
    } else if (args[i] == "--target" && i + 1 < args.size()) {
      if (ast::target_for_triple(args[i + 1])) {
        out.emplace_back(args[i]);
        out.emplace_back(args[i + 1]);
      }
      ++i;
    } else if (args[i].starts_with("--target=") && ast::target_for_triple(args[i].substr(9))) {
      out.emplace_back(args[i]);
    } else if (args[i] == "-o") {
      ++i;
    } else if ((std && args[i].starts_with("-std=")) || args[i].starts_with("-ftemplate-depth=") ||
               args[i].starts_with("-fclass-call-abi=")) {
      out.emplace_back(args[i]);
    }
  }
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
  if (!opts) return std::nullopt;
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
  pp::Config config = pp::with_system_dirs(opts->pp, r.cwd);
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
      failed = !preprocess_unit(d, config, s, *text) || failed;
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
  for (const std::string& s : opts->sources)
    key.units.push_back(session.unit({s, r.cwd, config, object_name(s).native_encoded_string()}));
  std::filesystem::path out = opts->output.value_or("a.out");
  // A command-line object that cannot be used is reported after the units' diagnostics, and only
  // when they compiled; the units are then built without the link.
  std::optional<std::string> bad_object;
  if (key.until == pipeline::Until::link) {
    std::vector<obj::object> objects;
    const bool linux_target = opts->pp.target == &ast::x86_64_linux;
    // Objects named on the command line are read as they are, never through the file table (§5.2).
    for (const std::string& path : opts->objects) {
      auto text = read_disk(in_cwd(r, path));
      if (!text) {
        bad_object = std::format("cannot read '{}'", path);
        break;
      }
      std::vector<std::uint8_t> bytes(text->begin(), text->end());
      auto o = linux_target ? obj::elf::read_object(bytes, path) : obj::macho::read_object(bytes, path);
      if (!o) {
        bad_object = o.error();
        break;
      }
      objects.push_back(std::move(*o));
    }
    if (bad_object) key.until = pipeline::Until::object;
    else key.program = session.program({key.units, std::move(objects), out.filename().native_encoded_string()});
  }
  const pipeline::Until until = bad_object ? pipeline::Until::link : key.until;
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
    Renderer render(d.err, path);
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
      std::string o = opts->output ? *opts->output : object_name(path).native_encoded_string();
      if (u.object->arch == obj::target::x86_64_linux) {
        auto bytes = obj::elf::write_object(*u.object);
        if (!bytes) {
          render.file_level(bytes.error());
          failed = true;
          continue;
        }
        d.outputs.push_back({std::move(o), std::move(*bytes), false});
      } else d.outputs.push_back({std::move(o), obj::macho::write_object(*u.object), false});
    }
  }
  if (failed || until != pipeline::Until::link) {
    if (failed) d.exit_status = 1;
    return d;
  }
  if (bad_object) {
    fail(d, *bad_object);
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
  if (!opts || opts->sources.empty()) return std::nullopt;
  pp::Config config = pp::with_system_dirs(opts->pp);
  config.view = units.view;
  pp::configure(config, host);
  const std::string& s = opts->sources.front();
  const pipeline::TuId tu = session.unit({s, r.cwd, config, object_name(s).native_encoded_string()});
  auto found = session.run<pipeline::DefinitionAt>({tu, pp::path_id(file), offset});
  return found ? *found : std::nullopt;
}

int finish(Done& d) {
  for (const Output& o : d.outputs) {
    if (auto err = pipeline::write_file(o.path, o.bytes, o.executable)) fail(d, *err);
  }
  std::cout << d.out << std::flush;
  std::cerr << d.err << std::flush;
  return d.exit_status;
}

}  // namespace ycxx::compile
