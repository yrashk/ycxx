// The daemon's clients (DECISIONS §4.7): a c++-compatible command line, an LSP server and a
// control interface. Each will connect to the daemon, starting it if needed; until the daemon
// exists (#63), `cxx` runs the pipeline in-process (ycxx.pipeline, #43).
export module ycxx.client;

import std;
import ycxx.obj.macho;
import ycxx.pipeline;
import ycxx.pp;

export namespace ycxx::client {
int cxx(std::span<const std::string_view> args);
int lsp(std::span<const std::string_view> args);
int control(std::span<const std::string_view> args);
}

namespace ycxx::client {
namespace {

int not_yet(std::string_view front) {
  std::println(std::cerr, "ycxx: the {} front is not implemented yet", front);
  return 2;
}

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

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string{std::istreambuf_iterator<char>(in), {}};
}

// Renders diagnostics as `path:line:col: error: message` (DECISIONS §4.7; the form tools/test
// reads).
class Renderer {
 public:
  Renderer(std::string_view path, std::string_view text) : path_(path), lines_(text) {}
  void at(std::uint32_t offset, bool error, std::string_view message) {
    pipeline::LineColumn lc = lines_.line_column(offset);
    std::println(std::cerr, "{}:{}:{}: {}: {}", path_, lc.line, lc.column, error ? "error" : "warning", message);
  }
  void file_level(std::string_view message) {
    std::println(std::cerr, "{}: error: {}", path_, message);
  }

 private:
  std::string_view path_;
  pipeline::LineTable lines_;
};

// The object a `-c` without `-o` writes: the source's file name with `.o`.
std::filesystem::path object_name(const std::string& source) {
  return std::filesystem::path(source).filename().replace_extension(".o");
}

// Compiles one translation unit; nullopt after reporting its errors.
std::optional<obj::object> compile(const std::string& path, Options::Stop stop) {
  auto text = read_file(path);
  if (!text) {
    std::println(std::cerr, "ycxx: error: cannot read '{}'", path);
    return std::nullopt;
  }
  if (stop == Options::Stop::preprocess) {
    auto [out, messages] = pp::preprocess_to_text(pp::session(), *text, path);
    std::print("{}", out);
    bool errors = false;
    for (const pp::Message& m : messages) {
      std::println(std::cerr, "{}:{}:{}: {}: {}", m.file, m.line, m.column, m.error ? "error" : "warning", m.text);
      errors = errors || m.error;
    }
    if (errors) return std::nullopt;
    return obj::object{};
  }
  Renderer out(path, *text);
  pipeline::Front front(path, *text);
  for (const pipeline::Message& m : front.messages()) {
    if (m.where.empty()) out.at(m.offset, m.error, m.text);
    else std::println(std::cerr, "{}: {}: {}", m.where, m.error ? "error" : "warning", m.text);
  }
  if (front.has_errors()) return std::nullopt;
  if (stop == Options::Stop::syntax) return obj::object{};
  pipeline::TranslationUnit tu = front.unit(object_name(path).native_encoded_string());
  auto o = pipeline::compile(tu, front.interners(), front.constants());
  if (!o) {
    for (const pipeline::BackError& e : o.error()) {
      if (e.at) out.at(front.byte_offset(*e.at), true, e.message);
      else out.file_level(e.message);
    }
    return std::nullopt;
  }
  return std::move(*o);
}

}  // namespace

// The c++-compatible front: `ycxx cxx [-c|-fsyntax-only] [-o out] [-std=c++26] a.cpp...`. Exits
// 0 on success and 1 on any error in the input or the command line.
int cxx(std::span<const std::string_view> args) {
  auto opts = parse_options(args);
  if (!opts) {
    std::println(std::cerr, "ycxx: error: {}", opts.error());
    return 1;
  }
  pp::configure(pp::with_system_dirs(opts->pp));
  bool failed = false;
  std::vector<obj::object> objects;
  for (const std::string& s : opts->sources) {
    auto o = compile(s, opts->stop);
    if (!o) {
      failed = true;
      continue;
    }
    if (opts->stop == Options::Stop::object) {
      std::filesystem::path out = opts->output ? std::filesystem::path(*opts->output) : object_name(s);
      if (auto err = pipeline::write_file(out, obj::macho::write_object(*o), false)) {
        std::println(std::cerr, "ycxx: error: {}", *err);
        failed = true;
      }
    } else if (opts->stop == Options::Stop::link) {
      objects.push_back(std::move(*o));
    }
  }
  if (failed) return 1;
  if (opts->stop != Options::Stop::link) return 0;

  for (const std::string& path : opts->objects) {
    auto text = read_file(path);
    if (!text) {
      std::println(std::cerr, "ycxx: error: cannot read '{}'", path);
      return 1;
    }
    std::vector<std::uint8_t> bytes(text->begin(), text->end());
    auto o = obj::macho::read_object(bytes, path);
    if (!o) {
      std::println(std::cerr, "ycxx: error: {}", o.error());
      return 1;
    }
    objects.push_back(std::move(*o));
  }
  std::filesystem::path out = opts->output.value_or("a.out");
  auto image = pipeline::link_executable(objects, out.filename().native_encoded_string());
  if (!image) {
    for (const link::error& e : image.error()) std::println(std::cerr, "ycxx: error: {}", e.message);
    return 1;
  }
  if (auto err = pipeline::write_file(out, *image, true)) {
    std::println(std::cerr, "ycxx: error: {}", *err);
    return 1;
  }
  return 0;
}

int lsp(std::span<const std::string_view>) { return not_yet("lsp"); }
int control(std::span<const std::string_view>) { return not_yet("ctl"); }
}  // namespace ycxx::client
