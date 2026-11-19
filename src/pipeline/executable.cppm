// Linking a program (#43) through ycxx's linkers. x86-64 Linux produces a static ELF executable.
// arm64 macOS links the objects and libSystem into a signed Mach-O executable. Imports are checked
// against the export list of the SDK's `usr/lib/libSystem.B.tbd`. The SDK is `$SDKROOT`, else what
// `xcrun --show-sdk-path` prints, else the Command Line Tools' SDK. The SDK is located and its
// export list read once per process.
//
// `link(P)` is the engine query over a program's translation units: their `tu(TU)` objects, in
// command-line order, then the objects given as files. The SDK is read outside the engine, once
// per process, as configuration the daemon's file table will provide (#97).
module;
#include <stdio.h>
export module ycxx.pipeline:executable;

import std;
import ycxx.ast;
import ycxx.engine;
export import ycxx.link;
import ycxx.link.elf;
import :back;

export namespace ycxx::pipeline {

// The SDK directory, if one is found.
const std::optional<std::filesystem::path>& sdk_path();

// The names libSystem exports, from the SDK; without an SDK, none is known and any name is
// accepted (the loader then reports a missing one).
const link::dylib& libsystem();

// The executable's bytes, or the linker's errors. `identifier` names the Mach-O code signature.
// Linux objects define `_start` or use the pipeline's freestanding startup with `main`.
std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(std::span<const obj::object> objects,
                                                                                   std::string identifier);

// Writes bytes to `path`, replacing a regular file or a symbolic link, and makes a new file
// executable when `executable` is set; an error message on failure.
std::optional<std::string> write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                      bool executable);

// A program, numbered by the driver.
struct ProgramId {
  std::uint32_t n = 0;
  bool operator==(const ProgramId&) const = default;
};
// A program in engine traces (#245): its output's name, once a session has named it.
void trace_describe(std::string& out, ProgramId p);
// What a program is linked from: translation units, in command-line order, then objects read from
// files; `identifier` names the code signature.
struct ProgramInputs {
  std::vector<TuId> tus;
  std::vector<obj::object> objects;
  std::string identifier;
};
struct [[=engine::input{engine::Durability::low}]] ProgramIn {
  using Key = ProgramId;
  using Value = ProgramInputs;
};

struct Linked {
  std::vector<std::uint8_t> image;  // empty when there are errors
  std::vector<std::string> errors;
};
struct Link {
  using Key = ProgramId;
  using Value = Linked;
  static engine::query_task<Value> run(engine::Ctx& cx, ProgramId p);
  static std::string errors(const Value& v) {  // for traces (#245)
    std::string out;
    for (const std::string& e : v.errors) out += e + "\n";
    return out;
  }
};

}  // namespace ycxx::pipeline

namespace ycxx::pipeline {

// The first line a command prints, if it succeeds.
std::optional<std::string> first_line_of(const char* command) {
  FILE* p = ::popen(command, "r");
  if (!p) return std::nullopt;
  std::array<char, 4096> buf{};
  std::string line;
  if (::fgets(buf.data(), int(buf.size()), p)) line = buf.data();
  int status = ::pclose(p);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (status != 0 || line.empty()) return std::nullopt;
  return line;
}

const std::optional<std::filesystem::path>& sdk_path() {
  static const std::optional<std::filesystem::path> sdk = []() -> std::optional<std::filesystem::path> {
    if (const char* env = std::getenv("SDKROOT"); env && *env) return std::filesystem::path(env);
    if (auto p = first_line_of("xcrun --show-sdk-path 2>/dev/null")) return std::filesystem::path(*p);
    std::filesystem::path clt = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
    std::error_code ec;
    if (std::filesystem::is_directory(clt, ec)) return clt;
    return std::nullopt;
  }();
  return sdk;
}

const link::dylib& libsystem() {
  static const link::dylib lib = [] {
    if (const auto& sdk = sdk_path()) {
      std::ifstream in(*sdk / "usr/lib/libSystem.B.tbd", std::ios::binary);
      if (in) {
        std::string text{std::istreambuf_iterator<char>(in), {}};
        return link::libsystem(link::tbd_exports(text));
      }
    }
    return link::libsystem();
  }();
  return lib;
}

namespace {

bool defines_global(const obj::symbol& symbol) {
  return symbol.defined() && (symbol.bind == obj::binding::global || symbol.bind == obj::binding::hidden);
}

bool defines_symbol(const obj::object& object, std::string_view name) {
  return std::ranges::any_of(object.symbols, [&](const obj::symbol& symbol) {
    return defines_global(symbol) && symbol.name == name;
  });
}

// The parser checks the ordinary C++ main. An asm label can give another function that symbol.
bool has_valid_linux_main(const TranslationUnit& tu, ast::Interners& in) {
  for (const Entity& entity : tu.entities) {
    if (entity.asm_label != "main" || entity.linkage == ast::Linkage::internal) continue;
    const auto def = std::ranges::find(tu.functions, entity.id, &FunctionDef::entity);
    if (def == tu.functions.end()) continue;
    const ast::QualType type = in.canonical(def->body.deduced ? def->body.deduced_type : def->decl.type);
    if (!in.types.is<ast::TFunction>(type) || def->this_type) return false;
    const auto f = in.types.get<ast::TFunction>(type);
    if (f.variadic || in.canonical(f.ret) != ast::builtin(in, ast::Builtin::int_)) return false;
    const auto params = in.seqs.elements(f.params);
    if (params.empty()) return true;
    const auto charpp = ast::pointer_to(in, ast::pointer_to(in, ast::builtin(in, ast::Builtin::char_)));
    return params.size() == 2 && params[0] == ast::builtin(in, ast::Builtin::int_) && params[1] == charpp;
  }
  return true;
}

obj::object linux_startup() {
  obj::object startup;
  startup.name = "<ycxx Linux startup>";
  startup.arch = obj::target::x86_64_linux;
  const auto text = startup.add_section(obj::standard_section::text);
  startup.define("_start", obj::binding::hidden, text);
  auto& code = startup.sections[text];
  // The AMD64 psABI initial stack has 16-byte alignment, then argc and argv at offsets 0 and 8.
  // Leave %rsp in place so the call also meets the System V function-entry alignment rule.
  code.emit(std::array<std::uint8_t, 15>{
      0x31, 0xed,                // xor ebp, ebp
      0x8b, 0x3c, 0x24,          // mov edi, [rsp]
      0x48, 0x8d, 0x74, 0x24, 8, // lea rsi, [rsp + 8]
      0xe8, 0, 0, 0, 0});       // call main
  code.relocate(obj::reloc_kind::x86_64_plt32, startup.reference("main"), -4);
  code.emit(std::array<std::uint8_t, 9>{
      0x89, 0xc7,        // mov edi, eax
      0xb8, 60, 0, 0, 0, // mov eax, 60 (Linux exit)
      0x0f, 0x05});      // syscall
  return startup;
}

std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_linux_executable(
    std::span<const obj::object> objects) {
  const auto link_elf = [](std::span<const obj::object> input, std::string_view entry) {
    return link::elf::link(input, entry).transform_error([](std::string message) {
      return std::vector<link::error>{{std::move(message)}};
    });
  };
  const obj::symbol* main = nullptr;
  const obj::object* main_object = nullptr;
  for (const auto& object : objects) {
    for (const auto& symbol : object.symbols) {
      if (!defines_global(symbol)) continue;
      if (symbol.name == "_start") return link_elf(objects, "_start");
      // Match the static linker's choice of the first weak definition or the strong definition.
      if (symbol.name == "main" && (!main || (main->weak && !symbol.weak))) {
        main = &symbol;
        main_object = &object;
      }
    }
  }

  // Let the ELF linker diagnose missing or invalid definitions, along with any other input errors.
  if (!main || *main->section >= main_object->sections.size()) return link_elf(objects, "main");
  const auto& section = main_object->sections[*main->section];
  if (section.segment != "__TEXT" || section.name != "__text" || main->offset >= section.size())
    return link_elf(objects, "main");

  // The object model only describes the code symbol. The source caller checks main's type.
  std::vector<obj::object> with_startup(objects.begin(), objects.end());
  with_startup.push_back(linux_startup());
  return link_elf(with_startup, "_start");
}

}  // namespace

// /usr/lib/libc++abi.dylib, the C++ ABI runtime (`__cxa_guard_acquire`, Itanium C++ ABI §3.3.3),
// with its exports from the SDK's stub; none without one.
const std::optional<link::dylib>& libcxxabi() {
  static const std::optional<link::dylib> lib = []() -> std::optional<link::dylib> {
    if (const auto& sdk = sdk_path()) {
      std::ifstream in(*sdk / "usr/lib/libc++abi.tbd", std::ios::binary);
      if (in) {
        std::string text{std::istreambuf_iterator<char>(in), {}};
        return link::dylib{"/usr/lib/libc++abi.dylib", link::tbd_exports(text), 0x08981b00, 0x00010000};  // 2200.27, 1.0
      }
    }
    return std::nullopt;
  }();
  return lib;
}

std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(std::span<const obj::object> objects,
                                                                                   std::string identifier) {
  const auto target = objects.empty() ? obj::target::aarch64_macos : objects.front().arch;
  for (const auto& object : objects) {
    if (object.arch != obj::target::aarch64_macos && object.arch != obj::target::x86_64_linux)
      return std::unexpected(std::vector<link::error>{{std::format("{}: unsupported executable object target", object.name)}});
    if (object.arch != target)
      return std::unexpected(std::vector<link::error>{{
          std::format("{}: cannot mix arm64 Mach-O and x86-64 Linux objects", object.name)}});
  }
  if (target == obj::target::x86_64_linux) return link_linux_executable(objects);

  link::options opts;
  opts.dylibs = {libsystem()};
  // libc++abi when the objects need a name only it exports.
  const auto& abi = libcxxabi();
  const auto& sys = libsystem().exports;
  if (abi && sys && std::ranges::any_of(objects, [&](const obj::object& o) {
        return std::ranges::any_of(o.symbols, [&](const obj::symbol& s) {
          return !s.defined() && !sys->contains(s.name) && abi->exports->contains(s.name);
        });
      }))
    opts.dylibs.push_back(*abi);
  opts.identifier = std::move(identifier);
  return link::link(objects, opts);
}

std::optional<std::string> write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                      bool executable) {
  // A new file, not the old one rewritten: the kernel caches a signed executable's pages by
  // vnode, and rewriting one in place makes the next run of it fail its signature check. Only a
  // regular file or a symbolic link is replaced; a directory is an error, and anything else (a
  // device such as /dev/null, a pipe) is written to as it is.
  std::error_code ec;
  auto st = std::filesystem::symlink_status(path, ec);
  if (st.type() == std::filesystem::file_type::directory)
    return std::format("cannot write '{}': it is a directory", path.display_string());
  bool regular = st.type() == std::filesystem::file_type::regular || st.type() == std::filesystem::file_type::symlink ||
                 st.type() == std::filesystem::file_type::not_found;
  if (st.type() == std::filesystem::file_type::regular || st.type() == std::filesystem::file_type::symlink)
    std::filesystem::remove(path, ec);
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return std::format("cannot open '{}' for writing", path.display_string());
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) return std::format("cannot write '{}'", path.display_string());
  }
  if (executable && regular) {
    using enum std::filesystem::perms;
    std::filesystem::permissions(path, owner_exec | group_exec | others_exec, std::filesystem::perm_options::add, ec);
    if (ec) return std::format("cannot make '{}' executable: {}", path.display_string(), ec.message());
  }
  return std::nullopt;
}

engine::query_task<Linked> Link::run(engine::Ctx& cx, ProgramId p) {
  const ProgramInputs& in = cx.fetch<ProgramIn>(p);
  std::vector<obj::object> objects;
  Linked out;
  const auto tus = co_await cx.par_map<Tu>(in.tus);
  for (std::size_t i = 0; i < in.tus.size(); ++i) {
    const TuValue& t = tus[i];
    if (!t.object) out.errors.push_back(std::format("translation unit {} did not compile", in.tus[i].n));
    else objects.push_back(*t.object);
  }
  if (!out.errors.empty()) co_return out;
  objects.append_range(in.objects);
  auto image = link_executable(objects, in.identifier);
  if (!image) {
    for (const link::error& e : image.error()) out.errors.push_back(e.message);
    co_return out;
  }
  if (!objects.empty() && objects.front().arch == obj::target::x86_64_linux &&
      !std::ranges::any_of(objects, [](const obj::object& o) { return defines_symbol(o, "_start"); })) {
    for (std::size_t i = 0; i < in.tus.size(); ++i) {
      if (!defines_symbol(objects[i], "main")) continue;
      const auto& tu = co_await cx.fetch<TuUnit>(in.tus[i]);
      if (!has_valid_linux_main(tu, *universe().in))
        out.errors.push_back(std::format("{}: Linux startup requires 'int main()' or 'int main(int, char**)'", tu.name));
    }
    if (!out.errors.empty()) co_return out;
  }
  out.image = std::move(*image);
  co_return out;
}

}  // namespace ycxx::pipeline
