import std;
import ycxx.pipeline;
import ycxx.unit;

using ycxx::unit::check;
namespace obj = ycxx::obj;
namespace pipeline = ycxx::pipeline;

namespace {

obj::object linux_object(std::string name) {
  obj::object object;
  object.name = std::move(name);
  object.arch = obj::target::x86_64_linux;
  return object;
}

obj::object main_with_arguments() {
  auto object = linux_object("main.o");
  const auto text = object.add_section(obj::standard_section::text);
  object.define("main", obj::binding::global, text);
  // Return the stack alignment remainder plus argc and the first byte of argv[1].
  object.sections[text].emit(std::array<std::uint8_t, 17>{
      0x89, 0xe0,          // mov eax, esp
      0x83, 0xe0, 15,      // and eax, 15
      0x01, 0xf8,          // add eax, edi
      0x48, 0x8b, 0x4e, 8, // mov rcx, [rsi + 8]
      0x0f, 0xb6, 0x11,    // movzx edx, byte [rcx]
      0x01, 0xd0,          // add eax, edx
      0xc3});              // ret
  return object;
}

obj::object main_in_data() {
  auto object = linux_object("data.o");
  const auto data = object.add_section(obj::standard_section::data);
  object.define("main", obj::binding::hidden, data);
  object.sections[data].emit32(0);
  return object;
}

void add_initialization(obj::object& object) {
  const auto text = object.add_section(obj::standard_section::text);
  const auto init = object.add_section(obj::standard_section::init_array);
  const auto fn = object.define("init", obj::binding::local, text);
  // Check the AMD64 function-entry stack alignment and clobber main's argument registers.
  object.sections[text].emit(std::array<std::uint8_t, 17>{
      0x89, 0xe0,      // mov eax, esp
      0x83, 0xe0, 15,  // and eax, 15
      0x83, 0xf8, 8,   // cmp eax, 8
      0x75, 5,         // jne invalid_alignment
      0x31, 0xff,      // xor edi, edi
      0x31, 0xf6,      // xor esi, esi
      0xc3,            // ret
      0x0f, 0x0b});    // invalid_alignment: ud2
  object.sections[init].emit64(0);
  object.sections[init].relocate(obj::reloc_kind::abs64, fn);
}

obj::object custom_start() {
  auto object = linux_object("start.o");
  const auto text = object.add_section(obj::standard_section::text);
  object.define("_start", obj::binding::global, text);
  object.sections[text].emit(std::array<std::uint8_t, 12>{
      0xbf, 23, 0, 0, 0, // mov edi, 23
      0xb8, 60, 0, 0, 0, // mov eax, 60 (Linux exit)
      0x0f, 0x05});      // syscall
  return object;
}

void rejects(std::span<const obj::object> objects, std::string_view reason) {
  const auto result = pipeline::link_executable(objects, "bad");
  check(!result && result.error().size() == 1 && result.error()[0].message.contains(reason), reason);
}

void diagnostics(const obj::object& main, const obj::object& start) {
  auto bad = linux_object("empty.o");
  rejects(std::span(&bad, 1), "ELF entry symbol 'main' is undefined");
  bad = main_in_data();
  rejects(std::span(&bad, 1), "ELF entry symbol 'main' is not inside executable code");

  bad = main;
  bad.symbols[0].offset = bad.sections[0].size();
  rejects(std::span(&bad, 1), "ELF entry symbol 'main' is not inside executable code");
  bad = main;
  bad.symbols[0].bind = obj::binding::local;
  rejects(std::span(&bad, 1), "ELF entry symbol 'main' is undefined");
  bad = main;
  bad.reference("missing");
  rejects(std::span(&bad, 1), "main.o: undefined symbol 'missing'");

  bad = main;
  bad.symbols[0].weak = true;
  const std::array with_strong_data{bad, main_in_data()};
  rejects(with_strong_data, "ELF entry symbol 'main' is not inside executable code");
  const std::array duplicate{main, main};
  rejects(duplicate, "main.o: duplicate symbol 'main'");

  bad = start;
  bad.symbols[0].offset = bad.sections[0].size();
  const std::array invalid_start{main, bad};
  rejects(invalid_start, "ELF entry symbol '_start' is not inside executable code");

  obj::object macho;
  macho.name = "macho.o";
  const std::array mixed{main, macho}, reversed{macho, main};
  rejects(mixed, "macho.o: cannot mix arm64 Mach-O and x86-64 Linux objects");
  rejects(reversed, "main.o: cannot mix arm64 Mach-O and x86-64 Linux objects");
  bad = linux_object("unknown.o");
  bad.arch = static_cast<obj::target>(255);
  rejects(std::span(&bad, 1), "unknown.o: unsupported executable object target");
}

}  // namespace

int main(int argc, char** argv) {
  const auto main_object = main_with_arguments();
  const auto start = custom_start();
  auto weak_main = main_in_data();
  weak_main.symbols[0].weak = true;
  auto local_start = start;
  local_start.symbols[0].bind = obj::binding::local;
  auto initializers = linux_object("initializers.o");
  add_initialization(initializers);
  const std::array automatic_objects{weak_main, local_start, main_object, initializers};
  const std::array explicit_objects{main_object, start};
  const auto automatic = pipeline::link_executable(automatic_objects, "automatic");
  const auto explicit_start = pipeline::link_executable(explicit_objects, "explicit");
  constexpr std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  check(automatic && std::ranges::starts_with(*automatic, ident), "link an ELF image with generated startup");
  check(explicit_start && std::ranges::starts_with(*explicit_start, ident), "link an ELF image with caller startup");
  check(automatic && pipeline::link_executable(automatic_objects, "other-name") == automatic,
        "Linux output is deterministic and independent of the Mach-O signature identifier");
  check(pipeline::link_executable(std::span(&start, 1), "start-only").has_value(), "caller startup can omit main");
  auto start_with_empty_array = start;
  start_with_empty_array.add_section(obj::standard_section::init_array);
  check(pipeline::link_executable(std::span(&start_with_empty_array, 1), "empty-init").has_value(),
        "caller startup can coexist with an empty initializer array");

  if (argc == 3 && automatic && explicit_start) {
    const auto auto_error = pipeline::write_file(argv[1], *automatic, true);
    check(!auto_error, auto_error.value_or("write ELF image with generated startup"));
    const auto explicit_error = pipeline::write_file(argv[2], *explicit_start, true);
    check(!explicit_error, explicit_error.value_or("write ELF image with caller startup"));
  }
  diagnostics(main_object, start);
  return ycxx::unit::result("unit.pipeline_elf_executable");
}
