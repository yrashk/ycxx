// Linking a program (#43) through ycxx's linkers. x86-64 Linux produces an ELF executable.
// arm64 macOS links the objects and libSystem into a signed Mach-O executable. Imports are checked
// against the export list of the SDK's `usr/lib/libSystem.B.tbd`. The SDK is `$SDKROOT`, else what
// `xcrun --show-sdk-path` prints, else the Command Line Tools' SDK. The SDK is located and its
// export list read once per process.
//
// `link(P)` is the engine query over a program's translation units and disk inputs. Static
// archive links follow operand order. The SDK is read outside the engine, once per process,
// as configuration the daemon's file table will provide (#97).
module;
#include <stdio.h>
export module ycxx.pipeline:executable;

import std;
import ycxx.ast;
import ycxx.engine;
export import ycxx.link;
export import ycxx.link.elf;
import :back;

export namespace ycxx::pipeline {

// The SDK directory, if one is found.
const std::optional<std::filesystem::path>& sdk_path();

// The names libSystem exports, from the SDK; without an SDK, none is known and any name is
// accepted (the loader then reports a missing one).
const link::dylib& libsystem();

// The executable's bytes, or the linker's errors. `identifier` names the Mach-O code signature.
// Linux uses the pipeline's startup with `main`, or a static executable can define `_start`.
std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(
    std::span<const obj::object> objects, std::string identifier, const link::elf::dynamic_options* dynamic = nullptr);

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
struct LinkOperand {
  enum class Kind : std::uint8_t { source, object, archive };
  Kind kind;
  std::size_t index;  // into the corresponding input vector
};

struct ArchiveLinkInputs {
  obj::target target;
  std::vector<LinkOperand> operands;
  std::vector<link::elf::archive_input> archives;
};

// Ordinarily, translation units precede disk objects. Archive links carry a positional manifest
// of all operands and the archive snapshots. Linux dynamic links carry their target interpreter
// and ordered shared inputs. `identifier` names the Mach-O code signature.
struct ProgramInputs {
  std::vector<TuId> tus;
  std::vector<obj::object> objects;
  std::string identifier;
  std::optional<link::elf::dynamic_options> dynamic = std::nullopt;
  std::optional<ArchiveLinkInputs> archive_link = std::nullopt;
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
    return defines_global(symbol) && symbol.name_view() == name;
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

obj::object linux_startup(std::int64_t init_array_size, bool dynamic) {
  obj::object startup;
  startup.name = "<ycxx Linux startup>";
  startup.arch = obj::target::x86_64_linux;
  const auto text = startup.add_section(obj::standard_section::text);
  std::optional<std::uint32_t> init_end;
  if (init_array_size != 0) {
    const auto marker = startup.add_section(obj::standard_section::init_array);
    init_end = startup.define("__ycxx_init_array_end", obj::binding::local, marker);
  }
  startup.define("_start", obj::binding::hidden, text);
  auto& code = startup.sections[text];
  // The AMD64 psABI initial stack has 16-byte alignment, then argc and argv at offsets 0 and 8.
  // Leave %rsp in place so every call meets the System V function-entry alignment rule.
  code.emit(std::array<std::uint8_t, 2>{0x31, 0xed});  // xor ebp, ebp
  if (dynamic) {
    // The AMD64 psABI passes the loader's termination callback in RDX. Keep it across calls.
    code.emit(std::array<std::uint8_t, 3>{0x49, 0x89, 0xd6});  // mov r14, rdx
  }
  if (init_end) {
    // The table is contiguous and this object's empty .init_array marks its end. R12 and R13
    // are callee-saved, so callbacks preserve the cursor and end address.
    code.emit(std::array<std::uint8_t, 7>{0x4c, 0x8d, 0x25, 0, 0, 0, 0});  // lea r12, [rip + begin]
    code.relocate(obj::reloc_kind::x86_64_pc32, *init_end, -4 - init_array_size);
    code.emit(std::array<std::uint8_t, 7>{0x4c, 0x8d, 0x2d, 0, 0, 0, 0});  // lea r13, [rip + end]
    code.relocate(obj::reloc_kind::x86_64_pc32, *init_end, -4);
    code.emit(std::array<std::uint8_t, 15>{
        0x4d, 0x39, 0xec,        // loop: cmp r12, r13
        0x74, 10,                // je done
        0x41, 0xff, 0x14, 0x24,  // call [r12]
        0x49, 0x83, 0xc4, 8,    // add r12, 8
        0xeb, 0xf1});            // jmp loop
  }
  // Initializers may clobber argument registers. Set them just before calling main.
  code.emit(std::array<std::uint8_t, 13>{
      0x8b, 0x3c, 0x24,          // mov edi, [rsp]
      0x48, 0x8d, 0x74, 0x24, 8, // lea rsi, [rsp + 8]
      0xe8, 0, 0, 0, 0});       // call main
  code.relocate(obj::reloc_kind::x86_64_plt32, startup.reference("main"), -4);
  if (dynamic) {
    // The callback takes no arguments. Preserve main's result in a callee-saved register.
    code.emit(std::array<std::uint8_t, 14>{
        0x41, 0x89, 0xc4,  // mov r12d, eax
        0x4d, 0x85, 0xf6,  // test r14, r14
        0x74, 3,           // jz done
        0x41, 0xff, 0xd6,  // call r14
        0x44, 0x89, 0xe7}); // done: mov edi, r12d
  } else {
    code.emit(std::array<std::uint8_t, 2>{0x89, 0xc7});  // mov edi, eax
  }
  code.emit(std::array<std::uint8_t, 7>{
      0xb8, 60, 0, 0, 0,  // mov eax, 60 (Linux exit)
      0x0f, 0x05});       // syscall
  return startup;
}

std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_linux_executable(
    std::span<const obj::object> objects, const link::elf::dynamic_options* dynamic) {
  const auto link_elf = [&](std::span<const obj::object> input, std::string_view entry) {
    return link::elf::link(input, entry, dynamic).transform_error([](std::string message) {
      return std::vector<link::error>{{std::move(message)}};
    });
  };
  const obj::symbol* main = nullptr;
  const obj::object* main_object = nullptr;
  bool custom_start = false;
  std::uint64_t init_array_size = 0;
  // The begin relocation also subtracts four bytes for PC32's instruction-relative displacement.
  constexpr std::uint64_t init_array_limit = std::numeric_limits<std::int64_t>::max() - 4;
  for (const auto& object : objects) {
    for (const auto& s : object.sections) {
      if (!s.is_init_array()) continue;
      if (s.size() > init_array_limit - init_array_size)
        return std::unexpected(std::vector<link::error>{{"ELF .init_array exceeds the startup address limit"}});
      init_array_size += s.size();
    }
    for (const auto& symbol : object.symbols) {
      if (!defines_global(symbol)) continue;
      const auto name = symbol.name_view();
      if (name == "_start") custom_start = true;
      // Match the static linker's choice of the first weak definition or the strong definition.
      if (name == "main" && (!main || (main->weak && !symbol.weak))) {
        main = &symbol;
        main_object = &object;
      }
    }
  }
  if (custom_start) {
    if (dynamic)
      return std::unexpected(std::vector<link::error>{{"a custom '_start' is unsupported for dynamic ELF linking"}});
    if (init_array_size != 0)
      return std::unexpected(std::vector<link::error>{{"a custom '_start' with a nonempty ELF .init_array is unsupported"}});
    return link_elf(objects, "_start");
  }

  // Let the ELF linker diagnose missing or invalid definitions, along with any other input errors.
  if (!main || *main->section >= main_object->sections.size()) return link_elf(objects, "main");
  const auto& section = main_object->sections[*main->section];
  if (section.segment != "__TEXT" || section.name != "__text" || main->offset >= section.size())
    return link_elf(objects, "main");

  // The object model only describes the code symbol. The source caller checks main's type.
  std::vector<obj::object> with_startup(objects.begin(), objects.end());
  with_startup.push_back(linux_startup(static_cast<std::int64_t>(init_array_size), dynamic != nullptr));
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

std::expected<std::vector<std::uint8_t>, std::vector<link::error>> link_executable(
    std::span<const obj::object> objects, std::string identifier, const link::elf::dynamic_options* dynamic) {
  const auto target = objects.empty() ? (dynamic ? obj::target::x86_64_linux : obj::target::aarch64_macos)
                                      : objects.front().arch;
  for (const auto& object : objects) {
    if (object.arch != obj::target::aarch64_macos && object.arch != obj::target::x86_64_linux)
      return std::unexpected(std::vector<link::error>{{std::format("{}: unsupported executable object target", object.name)}});
    if (object.arch != target)
      return std::unexpected(std::vector<link::error>{{
          std::format("{}: cannot mix arm64 Mach-O and x86-64 Linux objects", object.name)}});
  }
  if (target == obj::target::x86_64_linux) {
    if (std::ranges::none_of(objects, [](const obj::object& o) { return !o.comdat_groups.empty(); }))
      return link_linux_executable(objects, dynamic);
    std::vector<obj::object> selected(objects.begin(), objects.end());
    if (auto selection = link::elf::select_comdat_groups(selected, dynamic != nullptr); !selection)
      return std::unexpected(std::vector<link::error>{{std::move(selection.error())}});
    return link_linux_executable(selected, dynamic);
  }
  if (dynamic) return std::unexpected(std::vector<link::error>{{"dynamic ELF options require x86-64 Linux objects"}});

  link::options opts;
  opts.dylibs = {libsystem()};
  // libc++abi when the objects need a name only it exports.
  const auto& abi = libcxxabi();
  const auto& sys = libsystem().exports;
  if (abi && sys && std::ranges::any_of(objects, [&](const obj::object& o) {
        return std::ranges::any_of(o.symbols, [&](const obj::symbol& s) {
          return !s.defined() && !sys->contains(s.name_view()) && abi->exports->contains(s.name_view());
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
  Linked out;
  const auto tus = co_await cx.par_map<Tu>(in.tus);
  for (std::size_t i = 0; i < in.tus.size(); ++i) {
    if (!tus[i].get().object) out.errors.push_back(std::format("translation unit {} did not compile", in.tus[i].n));
  }
  if (!out.errors.empty()) co_return out;

  std::vector<obj::object> objects;
  if (const auto& archive_link = in.archive_link) {
    if (archive_link->target != obj::target::x86_64_linux || in.dynamic) {
      out.errors.push_back("archive inputs require a static x86-64 Linux executable link");
      co_return out;
    }
    std::vector<link::elf::static_operand> operands;
    operands.reserve(archive_link->operands.size());
    for (const auto [kind, index] : archive_link->operands) {
      switch (kind) {
        case LinkOperand::Kind::source: operands.emplace_back(std::cref(*tus[index].get().object)); break;
        case LinkOperand::Kind::object: operands.emplace_back(std::cref(in.objects[index])); break;
        case LinkOperand::Kind::archive: operands.emplace_back(std::cref(archive_link->archives[index])); break;
      }
    }
    auto selected = link::elf::select_archive_members(operands);
    if (!selected) {
      out.errors.push_back(std::move(selected.error()));
      co_return out;
    }
    objects = std::move(*selected);
  } else {
    objects.reserve(in.tus.size() + in.objects.size());
    for (const TuValue& t : tus) objects.push_back(*t.object);
    objects.append_range(in.objects);
  }
  if (auto selection = link::elf::select_comdat_groups(objects, in.dynamic.has_value()); !selection) {
    out.errors.push_back(std::move(selection.error()));
    co_return out;
  }
  auto image = link_executable(objects, in.identifier, in.dynamic ? &*in.dynamic : nullptr);
  if (!image) {
    for (const link::error& e : image.error()) out.errors.push_back(e.message);
    co_return out;
  }
  if (!objects.empty() && objects.front().arch == obj::target::x86_64_linux &&
      !std::ranges::any_of(objects, [](const obj::object& o) { return defines_symbol(o, "_start"); })) {
    for (std::size_t i = 0; i < in.tus.size(); ++i) {
      if (!defines_symbol(*tus[i].get().object, "main")) continue;
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
