// The c++-compatible request (daemon design §3.2, §7.1): a command line in; stdout, stderr, the
// exit status and the output files out. The daemon and the in-process path run this same handler,
// so `ycxx cxx` behaves the same with or without a daemon. Files are read through a `pp::Host`
// (the daemon's file table, or the disk); the client writes the outputs (`write_outputs`).
export module ycxx.compile;

import std;
import ycxx.obj.macho;
import ycxx.pipeline;
import ycxx.engine;
import ycxx.pp;

export namespace ycxx::compile {

// The environment variables the driver reads, in one table both paths read (§3.2). All of them
// are configuration variables today; none only renders.
inline constexpr std::array<std::string_view, 5> environment = {"SDKROOT", "DEVELOPER_DIR", "CPATH", "CPLUS_INCLUDE_PATH",
                                                                  "SOURCE_DATE_EPOCH"};

// One request. `argv` follows the front's name, with response files expanded by the client;
// relative paths in it are relative to `cwd`.
struct Request {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> env;  // the set variables of `environment`
  std::int64_t build_time = 0;                           // the client's clock, seconds since the epoch
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
};

struct Done {
  std::int32_t exit_status = 0;
  std::string out;  // stdout
  std::string err;  // stderr
  std::vector<Output> outputs;
  Report report;
};

// Called before each translation unit; true stops the request, whose answer is then meaningless.
using Stop = std::function<bool()>;

// The variables of `environment` this process has.
std::vector<std::pair<std::string, std::string>> environment_now();

// Runs a request in `session` (#63), reading files through `host`. `stopped` is set when `stop`
// stopped it.
Done run(const Request& r, pp::Host& host, pipeline::Session& session, const Stop& stop, bool& stopped);

// The path a request's scope is found from (§2): its first source file, else its first input,
// else nothing (the working directory). Relative to the request's `cwd`.
std::optional<std::string> first_input(std::span<const std::string> argv);

// Writes the outputs and prints stdout and stderr; the exit status. A failed write is reported on
// stderr after the request's own diagnostics, with status 1.
int finish(Done& d);

}  // namespace ycxx::compile

namespace ycxx::compile {
namespace {

// What the c++-compatible command line asks for.
struct Options {
  enum class Stop { link, object, syntax, preprocess } stop = Stop::link;
  pp::Config pp;  // -I, -isystem, -iquote, -D, -U
  std::optional<std::string> output;
  std::vector<std::string> sources;  // translation units
  std::vector<std::string> objects;  // Mach-O objects, linked as they are
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
    else if (a == "-o") {
      if (i + 1 == args.size()) return std::unexpected("argument to '-o' is missing");
      o.output = std::string(args[++i]);
    } else if (a.starts_with("-o")) o.output = std::string(a.substr(2));
    else if (a.starts_with("-std=")) {
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
  if (o.sources.empty() && o.objects.empty()) return std::unexpected("no input files");
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
  Renderer(std::string& err, std::string_view path, std::string_view text) : err_(&err), path_(path), lines_(text) {}
  void at(std::uint32_t offset, bool error, std::string_view message) {
    pipeline::LineColumn lc = lines_.line_column(offset);
    std::format_to(std::back_inserter(*err_), "{}:{}:{}: {}: {}\n", path_, lc.line, lc.column, error ? "error" : "warning",
                   message);
  }
  void file_level(std::string_view message) { std::format_to(std::back_inserter(*err_), "{}: error: {}\n", path_, message); }

 private:
  std::string* err_;
  std::string_view path_;
  pipeline::LineTable lines_;
};

// The object a `-c` without `-o` writes: the source's file name with `.o`.
std::filesystem::path object_name(const std::string& source) {
  return std::filesystem::path(source).filename().replace_extension(".o");
}

// Compiles one translation unit through the session's queries; nullopt after reporting its
// errors. With `-fsyntax-only` or `-E`, an object with no content.
std::optional<obj::object> compile_unit(Done& d, pp::Host& host, pipeline::Session& session, pipeline::TuId tu,
                                        const std::string& path, Options::Stop stop) {
  auto text = host.read(path);
  if (!text) {
    std::format_to(std::back_inserter(d.err), "ycxx: error: cannot read '{}'\n", path);
    return std::nullopt;
  }
  if (stop == Options::Stop::preprocess) {
    auto [out, messages] = pp::preprocess_to_text(pp::session(), *text, path);
    d.out += out;
    bool errors = false;
    for (const pp::Message& m : messages) {
      std::format_to(std::back_inserter(d.err), "{}:{}:{}: {}: {}\n", m.file, m.line, m.column, m.error ? "error" : "warning",
                     m.text);
      errors = errors || m.error;
    }
    if (errors) return std::nullopt;
    return obj::object{};
  }
  session.set_source(tu, path, *text, object_name(path).native_encoded_string());
  Renderer out(d.err, path, *text);
  const pipeline::FrontResult& front = session.get<pipeline::TuFront>(tu);
  for (const pipeline::Message& m : front.messages) {
    if (m.where.empty()) out.at(m.offset, m.error, m.text);
    else std::format_to(std::back_inserter(d.err), "{}: {}: {}\n", m.where, m.error ? "error" : "warning", m.text);
  }
  if (front.has_errors()) return std::nullopt;
  if (stop == Options::Stop::syntax) return obj::object{};
  const pipeline::TuValue& v = session.get<pipeline::Tu>(tu);
  for (const pipeline::BackError& e : v.errors) {
    if (e.at) out.at(session.byte_offset(tu, *e.at), true, e.message);
    else out.file_level(e.message);
  }
  if (!v.object) return std::nullopt;
  return *v.object;
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

std::optional<std::string> first_input(std::span<const std::string> argv) {
  auto opts = parse_options(views(argv));
  if (!opts) return std::nullopt;
  if (!opts->sources.empty()) return opts->sources.front();
  if (!opts->objects.empty()) return opts->objects.front();
  return std::nullopt;
}

// The c++-compatible front: `[-c|-fsyntax-only|-E] [-o out] [-std=c++26] a.cpp...`. Exits 0 on
// success and 1 on any error in the input or the command line.
Done run(const Request& r, pp::Host& host, pipeline::Session& session, const Stop& stop, bool& stopped) {
  Done d;
  stopped = false;
  auto opts = parse_options(views(r.argv));
  if (!opts) {
    fail(d, opts.error());
    return d;
  }
  pp::configure(pp::with_system_dirs(opts->pp), host);
  const pp::Session::Stats before = pp::session().stats;
  const engine::Stats engine_before = session.db().stats();
  bool failed = false;
  std::vector<pipeline::TuId> tus;
  for (std::uint32_t k = 0; k < opts->sources.size(); ++k) {
    const std::string& s = opts->sources[k];
    if (stop && stop()) {
      stopped = true;
      return d;
    }
    const pipeline::TuId tu{k};
    auto o = compile_unit(d, host, session, tu, s, opts->stop);
    if (!o) {
      failed = true;
      continue;
    }
    if (opts->stop == Options::Stop::object) {
      std::string out = opts->output ? *opts->output : object_name(s).native_encoded_string();
      d.outputs.push_back({std::move(out), obj::macho::write_object(*o), false});
    } else if (opts->stop == Options::Stop::link) {
      tus.push_back(tu);
    }
  }
  auto count = [&] {
    d.report.regions_computed = pp::session().stats.computed - before.computed;
    d.report.regions_reused = pp::session().stats.reused - before.reused;
    const engine::Stats now = session.db().stats();
    d.report.executed = now.executed - engine_before.executed;
    d.report.validated = now.validated - engine_before.validated;
  };
  if (failed || opts->stop != Options::Stop::link) {
    count();
    if (failed) d.exit_status = 1;
    return d;
  }

  std::vector<obj::object> objects;
  // Objects named on the command line are read as they are, never through the file table (§5.2).
  for (const std::string& path : opts->objects) {
    auto text = read_disk(in_cwd(r, path));
    if (!text) {
      fail(d, std::format("cannot read '{}'", path));
      return d;
    }
    std::vector<std::uint8_t> bytes(text->begin(), text->end());
    auto o = obj::macho::read_object(bytes, path);
    if (!o) {
      fail(d, o.error());
      return d;
    }
    objects.push_back(std::move(*o));
  }
  std::filesystem::path out = opts->output.value_or("a.out");
  session.set_program(pipeline::ProgramId{0}, {std::move(tus), std::move(objects), out.filename().native_encoded_string()});
  const pipeline::Linked& image = session.get<pipeline::Link>(pipeline::ProgramId{0});
  count();
  if (!image.errors.empty()) {
    for (const std::string& e : image.errors) std::format_to(std::back_inserter(d.err), "ycxx: error: {}\n", e);
    d.exit_status = 1;
    return d;
  }
  d.outputs.push_back({out.native_encoded_string(), image.image, true});
  return d;
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
