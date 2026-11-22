// Stage A fixture for #583. Source try lowering stays closed. This small instruction sequence
// follows the System V AMD64 ABI and uses the production unwind encoders and ELF writer.
import std;
import ycxx.ir;
import ycxx.obj.elf;
import ycxx.target.x86_64;

namespace ir = ycxx::ir;
namespace obj = ycxx::obj;
namespace x64 = ycxx::x86_64;

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

struct Function {
  x64::MachineCode code;

  std::uint32_t here() const { return static_cast<std::uint32_t>(code.bytes.size()); }
  void emit(std::initializer_list<std::uint8_t> bytes) { code.bytes.insert(code.bytes.end(), bytes); }
  void frame(x64::FrameState state) { code.frame_rows.push_back({here(), state}); }
  void enter() {
    emit({0x55});  // push rbp
    frame(x64::FrameState::saved_rbp);
    emit({0x48, 0x89, 0xe5});  // mov rbp, rsp
    frame(x64::FrameState::frame_pointer);
  }
  void ret() {
    emit({0xc9});  // leave
    frame(x64::FrameState::entry);
    emit({0xc3});
  }
  x64::UnwindCallSite call(std::uint32_t symbol) {
    const auto start = here();
    emit({0xe8});
    code.relocs.push_back({here(), x64::RelocKind::plt32, ir::EntityId(symbol), -4});
    emit({0, 0, 0, 0});
    return {start, here(), 0};
  }
};

Function forwarding(obj::object& o) {
  Function f;
  f.enter();
  f.call(o.reference("peer"));
  f.emit({0x83, 0xc0, 3});  // add eax, 3
  f.ret();
  return f;
}

Function catching(obj::object& o) {
  Function f;
  f.enter();
  auto protected_call = f.call(o.reference("peer"));
  f.ret();

  protected_call.landing_pad = f.here();
  f.frame(x64::FrameState::frame_pointer);
  f.emit({0x48, 0x89, 0xc7});        // mov rdi, rax, the original unwind header
  f.emit({0x48, 0x83, 0xfa, 1});     // cmp rdx, 1, the catch-all selector
  f.emit({0x0f, 0x85});              // jne resume
  const auto resume_branch = f.here();
  f.emit({0, 0, 0, 0});

  const auto begin = f.call(o.reference("__cxa_begin_catch"));
  const auto end = f.call(o.reference("__cxa_end_catch"));
  f.emit({0xb8, 47, 0, 0, 0});      // mov eax, 47
  f.ret();

  const auto resume = f.here();
  f.frame(x64::FrameState::frame_pointer);
  const auto distance = resume - (resume_branch + 4);
  for (unsigned byte = 0; byte < 4; ++byte)
    f.code.bytes[resume_branch + byte] = static_cast<std::uint8_t>(distance >> (8 * byte));
  const auto propagate = f.call(o.reference("_Unwind_Resume"));
  f.emit({0x0f, 0x0b});  // ud2 if the unwinder returns
  f.code.unwind_sites = std::vector{protected_call, begin, end, propagate};
  return f;
}

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
  require(at <= bytes.size() && bytes.size() - at >= 4, "read a bounded frame field");
  std::uint32_t value = 0;
  for (unsigned byte = 0; byte < 4; ++byte) value |= std::uint32_t{bytes[at + byte]} << (8 * byte);
  return value;
}

std::uint32_t uleb(std::span<const std::uint8_t> bytes, std::size_t& at) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32 && at < bytes.size(); shift += 7) {
    auto part = bytes[at++];
    value |= std::uint32_t(part & 0x7f) << shift;
    if (part < 0x80) return value;
  }
  throw std::runtime_error("invalid ULEB128 in catch-all LSDA");
}

std::size_t check_lsda(std::span<const std::uint8_t> lsda, std::span<const x64::UnwindCallSite> expected,
                       bool cleanup_only) {
  require(lsda.size() >= 6 && lsda[0] == 0xff && lsda[1] == 0x1b, "LSDA pointer encodings");
  std::size_t at = 2;
  const auto type_offset = uleb(lsda, at);
  require(at < lsda.size() && type_offset == lsda.size() - at && lsda[at++] == 1,
          "LSDA type and call-site table encodings");
  const auto call_sites_size = uleb(lsda, at);
  const auto actions = at + call_sites_size;
  std::size_t protected_action = 0;
  for (const auto& site : expected) {
    require(uleb(lsda, at) == site.begin, "LSDA starts at the actual call instruction");
    require(uleb(lsda, at) == site.end - site.begin, "LSDA range covers the emitted call");
    require(uleb(lsda, at) == site.landing_pad, "LSDA points at the emitted landing instruction");
    if (site.landing_pad != 0) protected_action = at;
    require(uleb(lsda, at) == (site.landing_pad != 0 && !cleanup_only ? 1u : 0u), "LSDA call-site action index");
  }
  require(at == actions && lsda.size() - at == 6 && lsda[at] == 1 && lsda[at + 1] == 0 &&
          word(lsda, at + 2) == 0 && protected_action != 0, "one positive catch-all filter with a null type entry");
  return protected_action;
}

enum class Order { handler_first, plain_first };

void emit_object(const std::filesystem::path& output, Order order, bool cleanup_only = false) {
  obj::object o;
  o.name = output.filename().display_string();
  o.arch = obj::target::x86_64_linux;
  const auto text = o.add_section(obj::standard_section::text);
  const auto eh = o.add_section(obj::standard_section::eh_frame);
  const auto table = o.add_section(obj::standard_section::gcc_except_table);
  const auto pointers = o.add_section(obj::standard_section::data_const);
  const auto personality_cell = o.define({}, obj::binding::local, pointers);
  o.sections[pointers].emit64(0);
  o.sections[pointers].relocate(obj::reloc_kind::abs64, o.reference("__gxx_personality_v0"));
  auto handler = catching(o);
  auto ordinary = forwarding(o);
  x64::EhFrameCies cies;

  auto append = [&](std::string name, const Function& f) {
    auto& code = o.sections[text];
    code.align(4);
    const auto base = static_cast<std::uint32_t>(code.size());
    const auto definition = o.define(std::move(name), obj::binding::global, text);
    o.symbols[definition].function_size = f.code.bytes.size();
    const auto code_anchor = o.define({}, obj::binding::local, text);
    code.emit(f.code.bytes);
    for (const auto& r : f.code.relocs)
      code.relocate(obj::reloc_kind::x86_64_plt32, ir::raw(r.target), r.addend, base + r.offset);

    auto& frame = o.sections[eh];
    auto fields = x64::append_eh_frame(frame.bytes, cies, f.code);
    require(fields.has_value(), fields ? "" : fields.error());
    if (fields->personality) frame.relocate(obj::reloc_kind::x86_64_pc32, personality_cell, 0, *fields->personality);
    frame.relocate(obj::reloc_kind::x86_64_pc32, code_anchor, 0, fields->initial_location);
    if (f.code.unwind_sites) {
      require(fields->lsda.has_value(), "handler FDE has an LSDA field");
      auto lsda = x64::exception_lsda(f.code);
      require(lsda.has_value(), lsda ? "" : lsda.error());
      const auto action = check_lsda(*lsda, *f.code.unwind_sites, false);
      // A separate test-only cleanup entry lets the hosted personality provide selector zero.
      // Reuse the exact emitted landing path and keep the production catch-all encoder intact.
      if (cleanup_only) (*lsda)[action] = 0;
      const auto lsda_anchor = o.define({}, obj::binding::local, table);
      o.sections[table].emit(*lsda);
      frame.relocate(obj::reloc_kind::x86_64_pc32, lsda_anchor, 0, *fields->lsda);
    } else {
      require(!fields->lsda && !fields->personality, "ordinary FDE keeps zR without handler data");
    }
  };

  if (order == Order::handler_first) {
    append("chosen", handler);
    append("transit", ordinary);
  } else {
    append("transit", ordinary);
    append("chosen", handler);
  }

  auto bytes = obj::elf::write_object(o);
  require(bytes.has_value(), bytes ? "" : bytes.error());
  auto parsed = obj::elf::read_object(*bytes, o.name);
  require(parsed.has_value(), parsed ? "" : parsed.error());
  auto rewritten = obj::elf::write_object(*parsed);
  require(rewritten && *bytes == *rewritten, "own ELF reader and writer preserve handler object bytes");
  check_lsda(parsed->sections[table].bytes, *handler.code.unwind_sites, cleanup_only);

  const auto& frames = parsed->sections[eh];
  for (const auto& name : {"chosen", "transit"}) {
    const auto f = std::ranges::find_if(parsed->symbols, [&](const obj::symbol& s) { return s.name_view() == name; });
    require(f != parsed->symbols.end() && f->section == text, "find the serialized function");
    const auto location = std::ranges::find_if(frames.relocs, [&](const obj::reloc& r) {
      const auto& s = parsed->symbols[r.symbol];
      return s.bind == obj::binding::local && s.section == text && s.offset == f->offset;
    });
    require(location != frames.relocs.end(), "function has a local FDE code anchor");
    const auto field = location->offset - 4;
    const auto cie = field - word(frames.bytes, field);
    require(cie + 13 < frames.bytes.size(), "FDE refers backwards to a bounded CIE");
    const auto aug = reinterpret_cast<const char*>(frames.bytes.data() + cie + 9);
    require(std::string_view(aug, std::string_view(name) == "chosen" ? 4 : 2) ==
                (std::string_view(name) == "chosen" ? "zPLR" : "zR"), "FDE uses its actual handler or ordinary CIE");
  }

  auto malformed = o;
  malformed.sections[eh].relocs.front().addend = -4;
  require(!obj::elf::write_object(malformed), "reject an instruction addend on handler frame relocations");
  malformed = o;
  malformed.sections[pointers].relocs.clear();
  require(!obj::elf::write_object(malformed), "reject a personality cell without an absolute pointer relocation");

  std::ofstream file(output, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
  require(static_cast<bool>(file), "write the authored ELF fixture");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "object fixture requires one output directory");
    const std::filesystem::path output(argv[1]);
    emit_object(output / "handler-first.o", Order::handler_first);
    emit_object(output / "plain-first.o", Order::plain_first);
    emit_object(output / "cleanup-only.o", Order::handler_first, true);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
