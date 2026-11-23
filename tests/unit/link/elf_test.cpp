// A libc-free Linux process, linked from two objects whose instructions are hand-encoded
// from the Intel SDM. ELF fields and relocations follow the ELF gABI and AMD64 psABI.
import std;
import ycxx.link.elf;
import ycxx.unit;

using ycxx::unit::check;
namespace obj = ycxx::obj;
namespace elf = ycxx::link::elf;
using obj::binding;
using obj::reloc_kind;
using obj::standard_section;

namespace {

std::array<obj::object, 2> fixture() {
  obj::object startup;
  startup.name = "startup.o";
  startup.arch = obj::target::x86_64_linux;
  const auto start_text = startup.add_section(standard_section::text);
  const auto constants = startup.add_section(standard_section::data_const);
  const auto start_rodata = startup.add_section(standard_section::rodata);
  auto& start = startup.sections[start_text];
  startup.define("_start", binding::global, start_text);
  const auto fallback = startup.define("compute", binding::global, start_text, 14);
  startup.symbols[fallback].weak = true;
  start.emit(std::array<std::uint8_t, 5>{0xe8, 0, 0, 0, 0});              // call compute
  start.relocate(reloc_kind::x86_64_plt32, fallback, -4);
  start.emit(std::array<std::uint8_t, 9>{0x89, 0xc7,                       // mov edi, eax
                                         0xb8, 60, 0, 0, 0,              // mov eax, 60 (exit)
                                         0x0f, 0x05});                   // syscall
  start.emit(std::array<std::uint8_t, 6>{0xb8, 99, 0, 0, 0, 0xc3});       // fallback returns 99

  startup.define("table", binding::hidden, constants);
  startup.sections[constants].emit64(0);
  startup.sections[constants].relocate(reloc_kind::abs64, startup.reference("values"), 4);
  startup.define("optional_value", binding::hidden, constants);
  const auto missing = startup.reference("optional");
  startup.symbols[missing].weak = true;
  startup.sections[constants].emit64(99);
  startup.sections[constants].relocate(reloc_kind::abs64, missing);
  startup.define("bonus", binding::local, start_rodata);
  startup.sections[start_rodata].emit32(99);

  obj::object compute;
  compute.name = "compute.o";
  compute.arch = obj::target::x86_64_linux;
  const auto text = compute.add_section(standard_section::text);
  const auto rodata = compute.add_section(standard_section::rodata);
  const auto data = compute.add_section(standard_section::data);
  const auto bss = compute.add_section(standard_section::bss);
  const auto table = compute.reference("table");
  const auto optional_value = compute.reference("optional_value");
  const auto bonus = compute.define("bonus", binding::local, rodata);
  const auto scratch = compute.define("scratch", binding::local, bss, 8192);
  compute.define("values", binding::hidden, data);
  compute.define("compute", binding::global, text);
  compute.sections[rodata].align(4);
  compute.sections[rodata].emit32(12);
  compute.sections[data].align(2);
  compute.sections[data].emit32(18);
  compute.sections[data].emit32(30);
  compute.sections[bss].align(4);
  compute.sections[bss].reserve_zerofill(8196);

  auto& code = compute.sections[text];
  code.emit(std::array<std::uint8_t, 7>{0x48, 0x8b, 0x05, 0, 0, 0, 0});  // mov rax, [rip + table]
  code.relocate(reloc_kind::x86_64_pc32, table, -4);
  code.emit(std::array<std::uint8_t, 2>{0x8b, 0x00});                      // mov eax, [rax]
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + bonus]
  code.relocate(reloc_kind::x86_64_pc32, bonus, -4);
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + scratch]
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x89, 0x05, 0, 0, 0, 0});          // mov [rip + scratch], eax
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x8b, 0x05, 0, 0, 0, 0});          // mov eax, [rip + scratch]
  code.relocate(reloc_kind::x86_64_pc32, scratch, -4);
  code.emit(std::array<std::uint8_t, 6>{0x03, 0x05, 0, 0, 0, 0});          // add eax, [rip + optional_value]
  code.relocate(reloc_kind::x86_64_pc32, optional_value, -4);
  code.emit(std::array<std::uint8_t, 1>{0xc3});                            // ret
  return {std::move(startup), std::move(compute)};
}

std::uint64_t field(std::span<const std::uint8_t> image, std::size_t offset, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{image[offset + i]} << (8 * i);
  return value;
}

void inspect(std::span<const std::uint8_t> image) {
  constexpr std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  constexpr std::size_t header_size = 64, phdr_size = 56, phdrs = 4;
  constexpr std::size_t headers_size = header_size + phdrs * phdr_size;
  check(image.size() >= headers_size, "ELF and program headers fit the image");
  if (image.size() < headers_size) return;
  check(std::ranges::equal(image.first(ident.size()), ident) && field(image, 16, 2) == 2 && field(image, 18, 2) == 62,
        "little-endian ELF64 ET_EXEC for x86-64");
  check(field(image, 32, 8) == header_size && field(image, 54, 2) == phdr_size && field(image, 56, 2) == phdrs,
        "program header table");
  check(field(image, 40, 8) == 0 && field(image, 60, 2) == 0, "no section header table");
  const auto phdr = [&](std::size_t index, std::size_t offset, std::size_t width) {
    return field(image, header_size + index * phdr_size + offset, width);
  };
  constexpr std::array flags{5u, 4u, 6u};
  for (std::size_t i = 0; i < flags.size(); ++i) {
    const auto offset = phdr(i, 8, 8);
    const auto address = phdr(i, 16, 8);
    const auto size = phdr(i, 32, 8);
    check(phdr(i, 0, 4) == 1 && phdr(i, 4, 4) == flags[i] && phdr(i, 48, 8) == 4096,
          "page-aligned load segment with the expected permissions");
    check(offset % 4096 == 0 && address % 4096 == 0 && offset <= image.size() && size <= image.size() - offset,
          "congruent load segment within the image");
    if (i > 0) check(offset >= phdr(i - 1, 8, 8) + phdr(i - 1, 32, 8) && address > phdr(i - 1, 16, 8),
                     "load segments have independent pages in ascending order");
  }
  check(phdr(3, 0, 4) == 0x6474e551 && phdr(3, 4, 4) == 6, "non-executable GNU stack");
  check(phdr(0, 8, 8) == 0 && field(image, 24, 8) == phdr(0, 16, 8) + headers_size,
        "entry immediately follows the mapped headers");
  check(phdr(2, 32, 8) == 8 && phdr(2, 40, 8) == 8212, "writable segment zero-fills aligned BSS beyond the file");

  const auto read_only = phdr(1, 8, 8);
  if (read_only > image.size() || image.size() - read_only < 36) {
    check(false, "read-only data fits the image");
    return;
  }
  check(field(image, read_only, 8) == phdr(2, 16, 8) + 4, "absolute relocation with a nonzero addend");
  check(field(image, read_only + 8, 8) == 0, "unresolved weak symbol has value zero");
  check(field(image, read_only + 16, 4) == 99 && field(image, read_only + 32, 4) == 12,
        "read-only contributions preserve input order and alignment");
}

void alternate_layouts(const std::array<obj::object, 2>& objects) {
  const std::array reversed{objects[1], objects[0]};
  const auto image = elf::link(reversed);
  check(image && image->size() >= 64 + 4 * 56 + 5, "link the two objects in the opposite order");
  if (image && image->size() >= 64 + 4 * 56 + 5) {
    const auto base = field(*image, 64 + 16, 8);
    const auto entry = field(*image, 24, 8);
    if (entry >= base && entry - base <= image->size() - 5) {
      const auto bits = static_cast<std::uint32_t>(field(*image, entry - base + 1, 4));
      const auto displacement = std::bit_cast<std::int32_t>(bits);
      check(displacement < 0 && static_cast<std::int64_t>(entry + 5) + displacement ==
                                    static_cast<std::int64_t>(base + 64 + 4 * 56),
            "backwards branch resolves to the earlier strong definition");
    } else {
      check(false, "reordered entry is inside the executable segment");
    }
  }

  obj::object small;
  small.arch = obj::target::x86_64_linux;
  const auto text = small.add_section(standard_section::text);
  small.define("_start", binding::global, text);
  small.sections[text].emit(std::array<std::uint8_t, 1>{0xcc});  // int3, only the layout is tested
  const auto text_only = elf::link(std::span(&small, 1));
  check(text_only && text_only->size() == 64 + 2 * 56 + 1 && field(*text_only, 56, 2) == 2,
        "text-only executable has one load segment and a stack header");

  const auto bss = small.add_section(standard_section::bss);
  small.sections[bss].reserve_zerofill(8192);
  const auto bss_only = elf::link(std::span(&small, 1));
  check(bss_only && bss_only->size() == 4096 && field(*bss_only, 56, 2) == 3 &&
        field(*bss_only, 64 + 56 + 32, 8) == 0 && field(*bss_only, 64 + 56 + 40, 8) == 8192,
        "a writable load segment can consist entirely of BSS");
}

void rejects(std::span<const obj::object> objects, std::string_view reason, std::string_view entry = "_start") {
  const auto result = elf::link(objects, entry);
  check(!result && result.error().contains(reason), reason);
}

void initializer_arrays(std::array<obj::object, 2> input) {
  auto& [first, second] = input;
  first.add_section(standard_section::init_array);
  second.add_section(standard_section::init_array);
  first.sections.back().emit64(0);
  first.sections.back().relocate(reloc_kind::abs64, 1, 1);  // weak compute resolves to second's definition
  const auto inner = second.define("inner", binding::local, 0, 1);
  second.sections.back().emit64(0);
  second.sections.back().relocate(reloc_kind::abs64, inner, -1);
  const auto image = elf::link(input);
  check(image.has_value(), image ? "link initializer arrays" : image.error());
  if (image) {
    const auto read_only = field(*image, 64 + 56 + 8, 8);
    const auto compute = field(*image, 24, 8) + first.sections[0].size();
    check(read_only + 56 <= image->size() && field(*image, read_only + 40, 8) == compute + 1 &&
          field(*image, read_only + 48, 8) == compute, "array slots are contiguous and use selected definitions and addends");
  }

  const auto change = [&](auto mutation, std::string_view reason) {
    auto bad = input;
    mutation(bad[0], bad[1]);
    rejects(bad, reason);
  };
  change([](auto& a, auto&) { a.sections.back().align_log2 = 4; }, "invalid .init_array alignment or size");
  change([](auto& a, auto&) { a.sections.back().bytes.pop_back(); }, "invalid .init_array alignment or size");
  change([](auto& a, auto&) { a.sections.back().relocs.clear(); }, ".init_array requires one relocation per slot");
  change([](auto& a, auto&) { a.sections.back().relocs.front().kind = reloc_kind::x86_64_pc32; },
         ".init_array requires an aligned R_X86_64_64 relocation per slot");
  change([](auto& a, auto&) { a.sections.back().relocs.front().offset = 1; },
         ".init_array requires an aligned R_X86_64_64 relocation per slot");
  change([](auto& a, auto&) {
    auto& s = a.sections.back();
    s.emit64(0);
    s.relocs.push_back(s.relocs.front());
  }, "overlapping ELF relocations are unsupported");
  change([](auto& a, auto&) { a.sections.back().relocs.front().symbol = a.reference("optional"); },
         ".init_array relocation has no defined target");
  const auto outside = ".init_array relocation target is outside its executable input section";
  change([](auto&, auto& b) {
    const auto compute = std::ranges::find(b.symbols, std::string_view("compute"), &obj::symbol::name_view);
    compute->section = 2;  // strong data definition overrides a weak text definition
  }, outside);
  change([](auto& a, auto& b) { a.sections.back().relocs.front().addend = b.sections[0].size(); }, outside);
  change([](auto& a, auto&) { a.sections.back().relocs.front().addend = std::numeric_limits<std::int64_t>::min(); }, outside);
  change([](auto& a, auto&) { a.sections.back().relocs.front().addend = std::numeric_limits<std::int64_t>::max(); }, outside);
}

void diagnostics(const std::array<obj::object, 2>& good) {
  rejects(good, "entry symbol 'missing' is undefined", "missing");
  rejects(good, "entry symbol 'values' is not inside executable code", "values");
  auto bad = good;
  bad[0].symbols[0].offset = bad[0].sections[0].size();
  rejects(bad, "entry symbol '_start' is not inside executable code");
  bad = good;
  bad[1].arch = obj::target::aarch64_macos;
  rejects(bad, "compute.o: expected an x86-64 Linux object");
  bad = good;
  bad[0].symbols[1].weak = false;
  rejects(bad, "compute.o: duplicate symbol 'compute'");
  bad = good;
  bad[1].reference("undefined");
  rejects(bad, "compute.o: undefined symbol 'undefined'");
  bad = good;
  bad[0].add_symbol({"local_undef", binding::local, std::nullopt, 0, false});
  rejects(bad, "startup.o: invalid local symbol 'local_undef'");
  bad = good;
  bad[0].symbols[0].section = bad[0].sections.size();
  rejects(bad, "startup.o: symbol '_start' is outside its section");
  bad = good;
  bad[1].sections[3].name = "__tls";
  rejects(bad, "compute.o: __DATA,__tls: unsupported ELF section");
  bad = good;
  bad[1].sections[3].flags = obj::section_flags::thread_local_zerofill;
  rejects(bad, "unsupported ELF section flags");
  bad = good;
  bad[1].sections[3].emit32(1);
  rejects(bad, "invalid zero-fill section contents");
  bad = good;
  bad[1].sections[3].align_log2 = 23;
  rejects(bad, "section alignment exceeds 4 MiB");
  bad = good;
  bad[1].sections[3].zerofill_size = std::numeric_limits<std::uint64_t>::max();
  rejects(bad, "section exceeds the ELF image address limit");
  bad = good;
  bad[0].sections[0].relocs[0].kind = reloc_kind::branch26;
  rejects(bad, "unsupported x86-64 ELF relocation kind");
  bad = good;
  bad[0].sections[1].relocs[0].minus = 0;
  rejects(bad, "ELF symbol-difference relocations are unsupported");
  bad = good;
  bad[0].sections[0].relocs[0].offset = 19;
  rejects(bad, "ELF relocation is outside its section");
  bad = good;
  bad[0].sections[0].relocs[0].symbol = bad[0].symbols.size();
  rejects(bad, "invalid ELF relocation symbol index");
  bad = good;
  bad[0].sections[1].relocs[1].offset = 7;
  rejects(bad, "overlapping ELF relocations are unsupported");
  bad = good;
  bad[0].sections[0].relocs[0].addend = std::numeric_limits<std::int64_t>::max();
  rejects(bad, "R_X86_64_PLT32 displacement overflows for 'compute'");
  bad = good;
  bad[1].sections[0].relocs[0].addend = std::numeric_limits<std::int64_t>::min();
  rejects(bad, "R_X86_64_PC32 displacement overflows for 'table'");
}

void relax_data_addresses() {
  std::array<obj::object, 2> input;
  auto& [user, provider] = input;
  user.arch = provider.arch = obj::target::x86_64_linux;
  const auto text = user.add_section(standard_section::text);
  const auto weak_data = user.add_section(standard_section::data);
  user.define("_start", binding::global, text);
  const auto value = user.define("value", binding::global, weak_data);
  user.symbols[value].weak = true;
  user.sections[weak_data].emit32(1);
  auto& code = user.sections[text];
  code.emit64(0x9090909090909090);
  code.emit(std::array<std::uint8_t, 7>{0x4c, 0x8b, 0x15, 0, 0, 0, 0});  // mov r10, [rip + GOT slot]
  code.relocate(reloc_kind::x86_64_rex_gotpcrelx, value, -4);
  code.emit32(0x90909090);
  const auto data = provider.add_section(standard_section::data);
  provider.sections[data].emit32(0);
  provider.define("value", binding::global, data);
  provider.sections[data].emit32(2);

  const auto image = elf::link(input);
  check(image.has_value(), image ? "link a canonical GOT load" : image.error());
  if (image) {
    const auto entry = field(*image, 24, 8);
    const auto text_base = field(*image, 64 + 16, 8);
    const auto data_base = field(*image, 64 + 56 + 16, 8);
    const auto start = entry - text_base;
    const auto displacement = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(field(*image, start + 11, 4)));
    check(field(*image, start + 8, 3) == 0x158d4c, "GOT load becomes LEA to the same extended register");
    check(entry + 15 + displacement == data_base + 8, "relaxed displacement uses the winning strong definition");
  }

  const auto change = [&](auto mutation, std::string_view reason) {
    auto bad = input;
    mutation(bad[0], bad[1]);
    rejects(bad, reason);
  };
  constexpr auto shape = "R_X86_64_REX_GOTPCRELX for 'value' requires a canonical 64-bit RIP-relative MOV with addend -4";
  change([](auto& o, auto&) { o.sections[0].bytes[8] = 0x44; }, shape);
  change([](auto& o, auto&) { o.sections[0].bytes[9] = 0x8d; }, shape);
  change([](auto& o, auto&) { o.sections[0].bytes[10] = 0x14; }, shape);
  change([](auto& o, auto&) { o.sections[0].relocs.front().offset = 2; }, shape);
  change([](auto& o, auto&) { o.sections[0].relocs.front().addend = 0; }, shape);
  change([=](auto& o, auto&) { o.sections[0].relocate(reloc_kind::x86_64_pc32, value, -4, 7); },
         "overlapping ELF relocations are unsupported for R_X86_64_REX_GOTPCRELX of 'value'");
  change([=](auto& o, auto&) { o.sections[0].relocate(reloc_kind::x86_64_pc32, value, -4, 13); },
         "overlapping ELF relocations are unsupported for R_X86_64_REX_GOTPCRELX of 'value'");
  auto adjacent = input;
  adjacent[0].sections[0].relocate(reloc_kind::x86_64_pc32, value, -4, 4);
  check(elf::link(adjacent).has_value(), "a fixup ending at the MOV prefix is adjacent");
  auto undefined_weak = input;
  undefined_weak[0].symbols[value].section.reset();
  undefined_weak[1].symbols.clear();
  const auto weak_image = elf::link(undefined_weak);
  check(weak_image.has_value(),
        weak_image ? "link a canonical GOT load of an undefined weak symbol" : weak_image.error());
  undefined_weak[0].sections[text].bytes[9] = 0x8d;
  rejects(undefined_weak, shape);
  change([=](auto& o, auto& p) {
    o.symbols[value].section.reset();
    o.symbols[value].weak = false;
    p.symbols.clear();
  }, "undefined symbol 'value'");
  change([](auto&, auto& p) {
    constexpr auto far = std::uint64_t{1} << 31;
    const auto bss = p.add_section(standard_section::bss);
    p.sections[bss].reserve_zerofill(far + 4);
    p.symbols.front().section = bss;
    p.symbols.front().offset = far;
  }, "R_X86_64_REX_GOTPCRELX displacement overflows for 'value'");

  const elf::dynamic_options options{"/ld.so", {{"data.so", "data.so", {{"value", false}}, {}}}};
  const auto dynamic = elf::link(input, "_start", &options);
  check(!dynamic && dynamic.error().contains("native dynamic ELF GOT address for 'value' is unsupported"),
        "native dynamic data addresses require data binding rules");
  auto imported = input;
  imported[0].symbols[value].section.reset();
  imported[0].symbols[value].weak = false;
  imported[1].symbols.clear();
  const auto dso_data = elf::link(imported, "_start", &options);
  check(!dso_data && dso_data.error().contains("native dynamic ELF GOT address for 'value' is unsupported"),
        "native dynamic data imports through the GOT are unsupported");
}

void dynamic_calls() {
  obj::object input;
  input.arch = obj::target::x86_64_linux;
  const auto text = input.add_section(standard_section::text);
  const auto data = input.add_section(standard_section::data);
  input.define("_start", binding::global, text);
  const auto function = input.reference("provided");
  auto& code = input.sections[text];
  code.emit32(0);  // padding for a test that patches the call opcode
  code.emit(std::array<std::uint8_t, 5>{0xe8, 0, 0, 0, 0});
  code.relocate(reloc_kind::x86_64_plt32, function, -4);
  code.emit(std::array<std::uint8_t, 5>{0xe8, 0, 0, 0, 0});
  code.relocate(reloc_kind::x86_64_plt32, function, -4);
  input.sections[data].emit64(0);

  const elf::dynamic_options options{"/target/ld.so", {{"leaf.so", "leaf.so.1", {{"provided", true}}, {}}}};
  const auto image = elf::link(std::span(&input, 1), "_start", &options);
  check(image.has_value(), image ? "link direct dynamic calls" : image.error());
  if (image) {
    const auto base = field(*image, 64 + 2 * 56 + 16, 8);
    const auto entry = field(*image, 24, 8) - base;
    const auto target = [&](std::uint64_t offset) {
      return offset + 4 + std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(field(*image, offset, 4)));
    };
    check(target(entry + 5) == entry + code.size() && target(entry + 10) == entry + code.size(),
          "two calls to the same import resolve to one thunk");
  }

  const auto reject = [](const obj::object& o, const elf::dynamic_options& dyn, std::string_view reason) {
    const auto result = elf::link(std::span(&o, 1), "_start", &dyn);
    check(!result && result.error().contains(reason), reason);
  };
  const auto change = [&](auto mutation, std::string_view reason) {
    auto bad = input;
    mutation(bad.sections[text], bad.sections[data]);
    reject(bad, options, reason);
  };
  constexpr std::string_view direct = "ELF import 'provided' requires a direct call with R_X86_64_PLT32";
  change([](auto& c, auto&) { c.relocs.front().kind = reloc_kind::x86_64_pc32; }, direct);
  change([](auto& c, auto&) { c.relocs.front().addend = 0; }, direct);
  change([](auto& c, auto&) { c.bytes[4] = 0xe9; }, direct);  // jmp rel32
  change([](auto& c, auto&) { c.relocs.front().offset = 0; }, direct);
  change([](auto& c, auto&) { c.relocs.back().offset += 1; }, "ELF relocation is outside its section");
  change([&](auto&, auto& d) { d.relocate(reloc_kind::abs64, function); }, direct);
  change([&](auto& c, auto& d) {
    d.bytes[0] = 0xe8;
    d.relocs.push_back(c.relocs.front());
    d.relocs.back().offset = 1;
  }, direct);
  change([](auto& c, auto&) { c.relocate(reloc_kind::x86_64_pc32, 0, 0, 1); },
         "overlapping ELF relocations are unsupported");
  change([](auto& c, auto&) { c.relocs.clear(); }, "has no direct call reference");

  auto weak = input;
  weak.symbols[function].weak = true;
  reject(weak, options, "weak ELF import 'provided' is unsupported");
  auto unused_weak = weak;
  unused_weak.sections[text].relocs.clear();
  check(elf::link(std::span(&unused_weak, 1), "_start", &options).has_value(),
        "a weak name with no reference does not require dynamic binding");
  auto no_definition = options;
  no_definition.libraries[0].exports.clear();
  check(elf::link(std::span(&weak, 1), "_start", &no_definition).has_value(),
        "a weak name missing from explicit providers keeps the unresolved zero value");

  auto incompatible = options;
  incompatible.libraries.insert(incompatible.libraries.begin(),
                                {"data.so", "data.so.1", {{"provided", false}}, {}});
  reject(input, incompatible, "unsupported ELF import 'provided'");
  auto local = input;
  local.symbols[function].section = text;
  local.symbols[function].offset = 0;
  local.sections[text].relocs.front().kind = reloc_kind::x86_64_pc32;
  check(elf::link(std::span(&local, 1), "_start", &incompatible).has_value(),
        "an object definition takes precedence over supplied libraries");
}

}  // namespace

int main(int argc, char** argv) {
  const auto objects = fixture();
  const auto image = elf::link(objects);
  check(image.has_value(), image ? "linked a static ELF executable" : image.error());
  if (image) {
    inspect(*image);
    check(elf::link(objects) == image, "ELF link is byte identical");
    check(elf::link(objects, "compute").has_value(), "caller can supply a different executable entry symbol");
    if (argc == 2) {
      std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(image->data()), image->size());
      out.close();
      check(bool(out), "write ELF executable fixture");
    }
  }
  diagnostics(objects);
  alternate_layouts(objects);
  initializer_arrays(objects);
  relax_data_addresses();
  dynamic_calls();
  return ycxx::unit::result("unit.elf_link");
}
