// Baseline System V AMD64 code generation. Put each scalar L2 value in a frame slot and emit
// instructions in block order, using only caller-saved scratch registers and a saved RBP.
export module ycxx.target.x86_64;

import :encode;
import std;
import ycxx.execution;
import ycxx.ir;
import ycxx.ir.l2;
import ycxx.ir.lower;

namespace ycxx::x86_64 {

export enum class RelocKind : std::uint8_t { pc32, plt32, rex_gotpcrelx };

export struct Reloc {
  std::uint32_t offset;  // disp32 field, relative to the function's first byte
  RelocKind kind;
  ir::EntityId target;
  std::int64_t addend;
  bool operator==(const Reloc&) const = default;
};

export enum class FrameState : std::uint8_t { entry, saved_rbp, frame_pointer };

export struct FrameRow {
  std::uint32_t offset;  // state starting at this instruction offset
  FrameState state;
  bool operator==(const FrameRow&) const = default;
};

export struct UnwindCallSite {
  std::uint32_t begin;        // first byte of the call
  std::uint32_t end;          // first byte after the call
  std::uint32_t landing_pad;  // function-relative, or zero for a call without a pad
  bool cleanup_only = false;
  bool operator==(const UnwindCallSite&) const = default;
};

export struct MachineCode {
  std::vector<std::uint8_t> bytes;
  std::vector<Reloc> relocs;
  std::vector<FrameRow> frame_rows;
  std::optional<std::vector<UnwindCallSite>> unwind_sites{};
  bool operator==(const MachineCode&) const = default;
};

export struct DivisionPair {
  ir::NodeId first;
  ir::NodeId second;
  bool operator==(const DivisionPair&) const = default;
};

export struct DivisionSite {
  ir::NodeId node;
  bool quotient;
  std::uint32_t saved_bytes;  // if this is the later node of a pair
  bool operator==(const DivisionSite&) const = default;
};

// In verified L2, the earlier node can produce both results of the same unsigned division.
export bool compatible_division_pair(const l2::Function& body, DivisionPair pair);
// Each group has identical operands and contains both quotients and remainders in block order.
export std::expected<std::vector<std::vector<DivisionSite>>, std::string>
group_divisions(const l2::Function& body, ycxx::exec::inplace_stop_token stop);

// Compile well-formed L2 with INTEGER ABI values and direct scalar SSE values.
// The producer checks original signatures and ellipsis types and flattens INTEGER records.
// L2 can erase the distinction between scalar and class arguments or results. Pass
// the entities whose addresses may be resolved directly by a signed RIP-relative displacement
// (e.g. local definitions in the small code model) in `direct_symbols`. Pass default-visible
// symbols in `got_symbols` so their addresses load through the ELF GOT.
export std::expected<MachineCode, std::string> compile_function(const l2::Function& body,
                                                                std::span<const ir::EntityId> direct_symbols = {},
                                                                std::span<const ir::EntityId> got_symbols = {},
                                                                std::span<const DivisionPair> division_pairs = {});

export struct EhFrameCies {
  std::optional<std::uint32_t> ordinary, handler;
};

export struct EhFrameFields {
  std::uint32_t initial_location;
  std::optional<std::uint32_t> personality;  // only when this function emits the handler CIE
  std::optional<std::uint32_t> lsda;
};

// Append one FDE and its CIE if absent. Offsets identify signed PC-relative relocation fields.
export std::expected<EhFrameFields, std::string> append_eh_frame(std::vector<std::uint8_t>& section,
                                                                EhFrameCies& cies, const MachineCode& code);

// Encode one hosted handler or cleanup LSDA from the final instruction and landing-pad offsets.
// For an int handler the last four bytes are the relocation field for its type entry.
export std::expected<std::vector<std::uint8_t>, std::string> exception_lsda(const MachineCode& code,
                                                                         bool catch_int = false);

namespace {

using l2::Op;
using l2::Type;
using ir::Amd64ArgumentKind;
using ir::Amd64ArgumentPlan;
using ir::BlockId;
using ir::NodeId;
using ir::ValueId;
using ir::raw;
using enum Reg;

// AMD64 psABI §3.2.2–3.2.3: INTEGER and SSE register sequences, then eightbyte stack slots.
constexpr Reg argument_regs[] = {rdi, rsi, rdx, rcx, r8, r9};
constexpr std::size_t register_args = std::size(argument_regs);
static_assert(register_args == Amd64ArgumentPlan::integer_registers);

// AMD64 psABI §3.5.7: the va_list fields and the entry register save area.
constexpr std::int32_t gp_offset_field = 0, fp_offset_field = 4;
constexpr std::int32_t overflow_arg_area_field = 8, reg_save_area_field = 16;
constexpr std::int32_t gp_slot = 8, xmm_slot = 16, xmm_register_args = Amd64ArgumentPlan::sse_registers;
constexpr std::int32_t gp_save_size = register_args * gp_slot;
constexpr std::int32_t reg_save_size = gp_save_size + xmm_register_args * xmm_slot;
constexpr std::int32_t stack_args_base = 16, stack_arg_slot = 8;

constexpr std::uint64_t align_up(std::uint64_t size, std::uint64_t align) {
  return (size + align - 1) & ~(align - 1);
}

constexpr std::optional<std::uint8_t> condition(ir::Pred pred) {
  using enum ir::Pred;
  switch (pred) {
    case eq: return 0x94;
    case ne: return 0x95;
    case slt: return 0x9c;
    case sle: return 0x9e;
    case sgt: return 0x9f;
    case sge: return 0x9d;
    case ult: case lt: return 0x92;
    case ule: case le: return 0x96;
    case ugt: case gt: return 0x97;
    case uge: case ge: return 0x93;
  }
  return std::nullopt;
}

std::unexpected<std::string> at_node(NodeId n, Op op, std::string_view message) {
  return std::unexpected(std::format("node {} `{}`: {}", raw(n), l2::info(op).name, message));
}

struct Frame {
  std::vector<std::int32_t> slots;
  std::vector<std::int32_t> objects;
  std::uint64_t size = 0;
  std::int32_t variadic_save = 0;  // RBP-relative, present only for an executing va_start
};

bool supports_float(Op op) {
  switch (op) {
    case Op::const_: case Op::load: case Op::store: case Op::select: case Op::fpext: case Op::fptrunc:
    case Op::fadd: case Op::fsub: case Op::fmul: case Op::fdiv: case Op::fneg: case Op::fabs: case Op::fcmp:
    case Op::fptosi: case Op::fptoui: case Op::sitofp: case Op::uitofp:
    case Op::call: case Op::call_ind: case Op::ret: case Op::va_arg: return true;
    default: return false;
  }
}

bool is_invoke(Op op) { return op == Op::invoke || op == Op::invoke_cleanup; }

enum class OperandKind { value, constant, load };
using DivisionOperand = std::tuple<Type, OperandKind, std::uint64_t, std::uint64_t, std::uint64_t>;

DivisionOperand division_operand(const l2::Function& body, ValueId id) {
  const auto& value = body.value(id);
  const DivisionOperand identity{value.type, OperandKind::value, raw(id), 0, 0};
  if (value.param) return identity;
  const NodeId node{value.def};
  const auto& def = body.node(node);
  if (def.flags != 0) return identity;
  if (def.op == Op::const_)
    return {value.type, OperandKind::constant, body.imms_of(node).front(), 0, 0};
  if (def.op != Op::load) return identity;
  // Ordinary loads must have the same state, address and alias information. The emitter
  // still evaluates both loads in their original positions.
  const auto operands = body.operands_of(node);
  return {value.type, OperandKind::load, raw(operands[0]), raw(operands[1]), body.imms_of(node).front()};
}

bool unsigned_division(const l2::Function& body, NodeId id) {
  const auto& node = body.node(id);
  if (node.flags || (node.op != Op::udiv && node.op != Op::urem)) return false;
  const auto type = body.type(body.result(id));
  return type == Type::i32 || type == Type::i64;
}

template <bool Poll = false>
std::expected<Frame, std::string> layout(const l2::Function& body, ycxx::exec::inplace_stop_token stop = {}) {
  // A frame over a page is probed page by page in the prologue (`allocate_frame`). The limit
  // matches the AArch64 back end's and keeps every RBP-relative displacement in 32 bits.
  constexpr std::uint64_t max_frame = std::uint64_t{16} << 20;
  const bool float_result = std::ranges::any_of(body.results, l2::is_float);
  // A class result in memory: `lower` makes its address the first argument (RDI), and the
  // function returns that address in RAX (AMD64 psABI §3.2.3).
  if ((body.indirect_result && (body.indirect_result != 1 || !body.results.empty())) ||
      body.results.size() > (float_result ? 1u : 2u))
    return std::unexpected("only a scalar SSE result or up to two INTEGER result registers are supported");
  if (std::ranges::contains(body.results, Type::state)) return std::unexpected("unsupported scalar ABI result type");
  auto is_float = [&](ValueId v) { return l2::is_float(body.type(v)); };

  Frame frame;
  frame.slots.resize(body.values.size());
  frame.objects.resize(body.nodes.size());
  for (std::size_t i = 0; i < body.values.size(); ++i) {
    if constexpr (Poll)
      if ((i % 256 == 0) && stop.stop_requested()) return std::unexpected("model cancelled");
    Type t = body.values[i].type;
    if (t == Type::state) continue;
    frame.size += 8;
    if (frame.size > max_frame) return std::unexpected("a frame larger than 16 MiB is not supported");
    frame.slots[i] = -std::int32_t(frame.size);
  }

  std::uint64_t scratch_words = 0;
  for (BlockId b : body.block_ids()) {
    if constexpr (Poll)
      if (stop.stop_requested()) return std::unexpected("model cancelled");
    const ir::Block& block = body.block(b);
    if (!block.nnodes) return std::unexpected("a block needs a terminator");
    auto params = body.params_of(b);
    if (params.empty() || body.type(params.front()) != Type::state)
      return std::unexpected("a block needs a leading state parameter");
    for (ValueId p : params | std::views::drop(1))
      if (body.type(p) == Type::state) return std::unexpected("a block has more than one state parameter");
    if (raw(b) != 0) scratch_words = std::max(scratch_words, std::uint64_t(block.nparams - 1));

    for (NodeId n : body.nodes_of(b)) {
      if constexpr (Poll)
        if ((raw(n) % 256 == 0) && stop.stop_requested()) return std::unexpected("model cancelled");
      const l2::Node& nd = body.node(n);
      auto imms = body.imms_of(n);
      auto operands = body.operands_of(n);
      const bool uses_float_result = std::ranges::any_of(std::views::iota(0u, nd.nres),
          [&](std::uint32_t i) { return is_float(body.result(n, i)); });
      if (!supports_float(nd.op) && (uses_float_result || std::ranges::any_of(operands, is_float)))
        return at_node(n, nd.op, "floating-point operation is unsupported");
      if (nd.op == Op::alloca) {
        auto size = imms[0], align = imms[1];
        if (!std::has_single_bit(align) || align > 16)
          return at_node(n, nd.op, "frame object alignment must be a power of two up to 16");
        if (size == 0 || size > max_frame) return at_node(n, nd.op, "unsupported frame object size");
        frame.size = align_up(frame.size + size, align);
        if (frame.size > max_frame) return at_node(n, nd.op, "a frame larger than 16 MiB is not supported");
        frame.objects[raw(n)] = -std::int32_t(frame.size);
      }
      if (nd.op == Op::va_start && !frame.variadic_save) {
        frame.size = align_up(frame.size + reg_save_size, xmm_slot);
        if (frame.size > max_frame) return at_node(n, nd.op, "a frame larger than 16 MiB is not supported");
        frame.variadic_save = -std::int32_t(frame.size);
      }
      if (nd.op == Op::call || nd.op == Op::call_ind || is_invoke(nd.op)) {
        const bool indirect = nd.op == Op::call_ind;
        const bool variadic = nd.flags & ir::bit(ir::Flag::variadic);
        if (indirect && body.type(operands[1]) != Type::ptr)
          return at_node(n, nd.op, "an indirect callee must be a pointer");
        auto args = operands.subspan(indirect ? 2 : 1);
        const auto ellipsis = imms[indirect ? 1 : 2];
        if (ellipsis && !variadic)
          return at_node(n, nd.op, indirect ? "ellipsis arguments on a nonvariadic indirect call"
                                         : "ellipsis arguments on a nonvariadic direct call");
        if (ellipsis > args.size()) return at_node(n, nd.op, "more ellipsis arguments than arguments");
        // A class result in memory is the first argument (`lower`) and leaves no result value.
        const auto result_in_memory = imms[indirect ? 2 : 3];
        if (result_in_memory > 1 || (result_in_memory && nd.nres > 1) ||
            nd.nres > (variadic || uses_float_result ? 2u : 3u))
          return at_node(n, nd.op, "unsupported ABI result placement");
        for (ValueId arg : args.last(ellipsis)) {
          Type type = body.type(arg);
          if (type != Type::i32 && type != Type::i64 && type != Type::ptr && type != Type::f64)
            return at_node(n, nd.op, "unsupported promoted ellipsis argument type");
        }
        Amd64ArgumentPlan plan;
        for (ValueId arg : args) plan.place(body.type(arg));
        scratch_words = std::max(scratch_words, std::uint64_t(plan.stack_slots()));
      }
    }
  }

  // Share the bottom of the frame between the outgoing stack arguments and parallel edge copies.
  frame.size = align_up(frame.size + 8 * scratch_words, 16);
  if (frame.size > max_frame) return std::unexpected("a frame larger than 16 MiB is not supported");
  return frame;
}

class Emitter {
 public:
  Emitter(const l2::Function& body, const Frame& frame, std::span<const ir::EntityId> direct_symbols,
          std::span<const ir::EntityId> got_symbols,
          std::span<const std::optional<NodeId>> division_peers)
      : body_(body), frame_(frame), direct_symbols_(direct_symbols), got_symbols_(got_symbols),
        division_peers_(division_peers), block_offsets_(body.blocks.size()) {}

  std::expected<MachineCode, std::string> run() {
    for (std::uint32_t n = 0; n < body_.nodes.size(); ++n)
      if (is_invoke(body_.nodes[n].op)) {
        landing_ = body_.succs_of(NodeId(n))[1].target;
        out_.unwind_sites.emplace();
      }
    e_.emit({0x55});  // push rbp
    frame_row(FrameState::saved_rbp);
    e_.move(rbp, rsp);
    frame_row(FrameState::frame_pointer);
    allocate_frame();
    if (frame_.variadic_save && !save_variadic_registers())
      return std::unexpected("x86-64 register-save branch displacement out of range");
    parameters();
    for (BlockId b : body_.block_ids()) {
      // CFI follows byte order. Branches into a block after a return still have a live frame.
      if (out_.frame_rows.back().state == FrameState::entry) frame_row(FrameState::frame_pointer);
      block_offsets_[raw(b)] = e_.offset();
      if (landing_ == b) {
        // The personality arrives at the recorded address with these original machine
        // registers. Capture both before an instruction uses them as scratch.
        const auto params = body_.params_of(b);
        e_.store(rbp, frame_.slots[raw(params[1])], rax, 8);
        e_.store(rbp, frame_.slots[raw(params[2])], rdx, 8);
      }
      for (NodeId n : body_.nodes_of(b)) {
        if (auto result = node(n); !result) return std::unexpected(result.error());
      }
    }
    for (const auto& branch : branches_)
      if (!e_.patch_relative(branch.at, block_offsets_[raw(branch.target)]))
        return std::unexpected("x86-64 branch displacement exceeds signed 32 bits");
    if (!std::in_range<std::uint32_t>(e_.offset())) return std::unexpected("x86-64 function exceeds 4 GiB");
    if (landing_) {
      if (!protected_call_) return std::unexpected("missing protected machine call");
      (*out_.unwind_sites)[*protected_call_].landing_pad =
          static_cast<std::uint32_t>(block_offsets_[raw(*landing_)]);
    }
    return std::move(out_);
  }

 private:
  const l2::Function& body_;
  const Frame& frame_;
  std::span<const ir::EntityId> direct_symbols_;
  std::span<const ir::EntityId> got_symbols_;
  std::span<const std::optional<NodeId>> division_peers_;
  MachineCode out_;
  Encoder e_{out_.bytes};
  std::vector<std::size_t> block_offsets_;
  Amd64ArgumentPlan incoming_;
  std::optional<BlockId> landing_;
  std::optional<std::size_t> protected_call_;

  struct Branch {
    std::size_t at;
    BlockId target;
  };
  std::vector<Branch> branches_;

  // Lower RSP by the frame size without moving it more than a page past memory already
  // touched, so a guard page below the stack faults instead of being skipped. The pushed RBP
  // touched the word below the incoming RSP. R11 is caller-saved and carries no argument, and
  // RAX still holds a variadic callee's vector count here.
  void allocate_frame() {
    constexpr std::uint32_t page = 4096;
    auto lower = [&](std::uint32_t bytes) {
      e_.reg({0x81}, 5, rsp);  // sub rsp, imm32
      e_.immediate(bytes, 4);
    };
    // A frame of a whole page or more is probed: after an unprobed `sub` of a full page, the
    // first call would push its return address more than a page below the saved RBP.
    const auto size = static_cast<std::uint32_t>(frame_.size);
    if (size >= page) {
      e_.emit({0x41, 0xbb});  // mov r11d, imm32: the number of whole pages
      e_.immediate(size / page, 4);
      const auto loop = e_.offset();
      lower(page);
      e_.emit({0x48, 0x83, 0x0c, 0x24, 0x00});  // or qword [rsp], 0
      e_.emit({0x49, 0xff, 0xcb});              // dec r11
      e_.emit({0x75, std::uint8_t(std::int8_t(loop - (e_.offset() + 2)))});  // jnz loop
    }
    if (const auto rest = size >= page ? size % page : size) lower(rest);
  }

  void frame_row(FrameState state) {
    // `run` checks the final byte extent before returning any offsets.
    out_.frame_rows.push_back({static_cast<std::uint32_t>(e_.offset()), state});
  }

  unsigned home_width(ValueId v) const {
    Type type = body_.type(v);
    return l2::is_float(type) ? l2::bytes(type) : 8;
  }

  void read(ValueId v, Reg dst = rax) { e_.load(dst, rbp, frame_.slots[raw(v)], home_width(v)); }
  void write(ValueId v) {
    Type type = body_.type(v);
    if (l2::is_float(type)) {
      e_.store(rbp, frame_.slots[raw(v)], rax, l2::bytes(type));
      return;
    }
    // Keep high bits clear for later unsigned operations and narrow ABI values. The psABI
    // leaves bits outside an INTEGER value's memory representation unspecified at the boundary.
    // A valid bool has a low byte of 0 or 1 (§3.2.3), which fits in the internal i1.
    unsigned bits = l2::bits(type);
    if (bits < 32) {
      e_.emit({0x25});  // and eax, imm32
      e_.immediate((1u << bits) - 1, 4);
    } else if (bits == 32) {
      e_.reg({0x89}, rax, rax, false);  // mov eax, eax
    }
    e_.store(rbp, frame_.slots[raw(v)], rax, 8);
  }

  void sign_extend(Reg reg, Type t) {
    unsigned bits = l2::bits(t);
    if (bits == 64) return;
    auto shift = std::uint8_t(64 - bits);
    e_.reg({0xc1}, 4, reg);  // shl
    e_.emit({shift});
    e_.reg({0xc1}, 7, reg);  // sar
    e_.emit({shift});
  }

  bool save_variadic_registers() {
    for (std::size_t i = 0; i < register_args; ++i)
      e_.store(rbp, frame_.variadic_save + std::int32_t(i) * gp_slot, argument_regs[i], gp_slot);
    // AL may overestimate the count (AMD64 psABI §3.2.3). A nonzero value can be
    // handled by saving all eight registers for a va_list consumed by an external callee.
    e_.emit({0x84, 0xc0, 0x0f, 0x84});  // test al, al; jz rel32
    auto skip = e_.displacement();
    for (std::int32_t i = 0; i < xmm_register_args; ++i)
      e_.store_xmm(rbp, frame_.variadic_save + gp_save_size + i * xmm_slot, i);
    return e_.patch_relative(skip, e_.offset());
  }

  void va_start(ValueId cursor) {
    read(cursor, rcx);
    e_.store_immediate32(rcx, gp_offset_field, incoming_.used_integer_registers() * gp_slot);
    e_.store_immediate32(rcx, fp_offset_field, gp_save_size + incoming_.used_sse_registers() * xmm_slot);
    // Skip the saved frame pointer, the return address and every named stack slot.
    e_.address(rax, rbp, stack_args_base + std::int32_t(incoming_.stack_slots()) * stack_arg_slot);
    e_.store(rcx, overflow_arg_area_field, rax, 8);
    e_.address(rax, rbp, frame_.variadic_save);
    e_.store(rcx, reg_save_area_field, rax, 8);
  }

  bool va_arg(ValueId cursor, ValueId result) {
    const Type type = body_.type(result);
    const bool sse = type == Type::f64;
    const auto offset_field = sse ? fp_offset_field : gp_offset_field;
    const auto save_end = sse ? reg_save_size : gp_save_size;
    const auto reg_slot = sse ? xmm_slot : gp_slot;
    read(cursor, rdx);
    e_.load(rax, rdx, offset_field, 4);
    e_.emit({0x3d});  // cmp eax, imm32. The FP limit of 176 does not fit a signed imm8.
    e_.immediate(save_end, 4);
    e_.emit({0x0f, 0x83});  // jae rel32
    auto overflow = e_.displacement();

    e_.load(rcx, rdx, reg_save_area_field, 8);
    e_.reg({0x01}, rax, rcx);  // add rcx, rax
    e_.reg({0x83}, 0, rax, false);  // add eax, imm8
    e_.immediate(reg_slot, 1);
    e_.store(rdx, offset_field, rax, 4);
    e_.emit({0xe9});  // jmp rel32
    auto fetched = e_.displacement();

    if (!e_.patch_relative(overflow, e_.offset())) return false;
    e_.load(rcx, rdx, overflow_arg_area_field, 8);
    e_.address(rax, rcx, stack_arg_slot);
    e_.store(rdx, overflow_arg_area_field, rax, 8);
    if (!e_.patch_relative(fetched, e_.offset())) return false;

    e_.load(rax, rcx, 0, l2::bytes(type));
    write(result);
    return true;
  }

  // Convert an i32 or i64 to binary32 or binary64, rounding under MXCSR.
  bool int_to_float(bool is_signed, ValueId source, ValueId result) {
    const Type from = body_.type(source);
    const unsigned width = home_width(result);
    const std::int32_t slot = frame_.slots[raw(source)];
    if (is_signed || from == Type::i32) {
      // `write` zero-extends an i32 home slot, so its unsigned value is the signed i64.
      e_.integer_to_float(0, rbp, slot, width, !is_signed || from == Type::i64);
      e_.store_scalar_xmm(rbp, frame_.slots[raw(result)], 0, width);
      return true;
    }
    // An i64 with the top bit set: halve it, keeping the bit shifted out as a sticky low bit
    // so the signed conversion rounds the same way, then double the result exactly.
    read(source);
    e_.reg({0x85}, rax, rax);  // test rax, rax
    e_.emit({0x0f, 0x89});     // jns rel32
    auto non_negative = e_.displacement();
    e_.move(rcx, rax);
    e_.reg({0xd1}, 5, rcx);              // shr rcx, 1
    e_.emit({0x83, 0xe0, 0x01});         // and eax, 1
    e_.reg({0x09}, rax, rcx);            // or rcx, rax
    e_.integer_to_float(0, rcx, width);
    e_.double_xmm(0, width);
    e_.emit({0xe9});  // jmp rel32
    auto converted = e_.displacement();
    if (!e_.patch_relative(non_negative, e_.offset())) return false;
    e_.integer_to_float(0, rax, width);
    if (!e_.patch_relative(converted, e_.offset())) return false;
    e_.store_scalar_xmm(rbp, frame_.slots[raw(result)], 0, width);
    return true;
  }

  void parameters() {
    for (ValueId v : body_.params_of(BlockId(0)) | std::views::drop(1)) {
      auto [kind, index] = incoming_.place(body_.type(v));
      if (kind == Amd64ArgumentKind::sse) {
        e_.store_scalar_xmm(rbp, frame_.slots[raw(v)], index, home_width(v));
      } else {
        if (kind == Amd64ArgumentKind::integer) e_.move(rax, argument_regs[index]);
        else e_.load(rax, rbp, stack_args_base + stack_arg_slot * std::int32_t(index), home_width(v));
        write(v);
      }
    }
  }

  void relocation(RelocKind kind, ir::EntityId target) {
    auto at = e_.displacement();
    // x86 measures from the end of the instruction; ELF measures from the field.
    out_.relocs.push_back({std::uint32_t(at), kind, target, -4});
  }

  void call(NodeId n) {
    const auto& node = body_.node(n);
    const bool indirect = node.op == Op::call_ind;
    const bool variadic = node.flags & ir::bit(ir::Flag::variadic);
    auto ops = body_.operands_of(n);
    auto imms = body_.imms_of(n);
    auto args = ops.subspan(indirect ? 2 : 1);
    auto signed_args = imms[indirect ? 0 : 1];
    Amd64ArgumentPlan plan;
    for (std::size_t i = 0; i < args.size(); ++i) {
      ValueId arg = args[i];
      Type type = body_.type(arg);
      auto [kind, index] = plan.place(type);
      if (kind == Amd64ArgumentKind::integer) {
        // The psABI leaves excess bits unspecified, but optimized Clang callees can read
        // 32 bits for narrow register arguments. Extend to that width for interoperability.
        e_.load(argument_regs[index], rbp, frame_.slots[raw(arg)], l2::bytes(type), i < 64 && ((signed_args >> i) & 1));
      } else if (kind == Amd64ArgumentKind::sse) {
        e_.load_scalar_xmm(index, rbp, frame_.slots[raw(arg)], l2::bytes(type));
      } else {
        read(arg);
        e_.store(rsp, 8 * std::int32_t(index), rax, home_width(arg));
      }
    }
    std::size_t call_begin;
    if (indirect) {
      // R11 is caller-saved and holds no argument. Variadic calls reserve AL for the vector
      // register count (AMD64 psABI §3.2.3), so their callee address needs a separate register.
      Reg target = variadic ? r11 : rax;
      read(ops[1], target);
      if (variadic) {
        // AL counts the vector registers holding arguments (AMD64 psABI §3.2.3).
        if (const auto vector_count = plan.used_sse_registers()) e_.emit({0xb0, std::uint8_t(vector_count)});  // mov al, imm8
        else e_.reg({0x31}, rax, rax, false);  // xor eax, eax
      }
      call_begin = e_.offset();
      e_.reg({0xff}, 2, target, false);  // call r/m64
    } else {
      // AL counts named and unnamed vector register arguments (AMD64 psABI §3.2.3).
      const auto vector_count = variadic ? plan.used_sse_registers() : 0u;
      if (vector_count) e_.emit({0xb0, std::uint8_t(vector_count)});  // mov al, imm8
      else e_.reg({0x31}, rax, rax, false);  // xor eax, eax
      call_begin = e_.offset();
      e_.emit({0xe8});  // call rel32
      relocation(RelocKind::plt32, ir::EntityId(std::uint32_t(imms[0])));
    }
    const bool invoke = is_invoke(node.op);
    if (out_.unwind_sites) {
      if (invoke) protected_call_ = out_.unwind_sites->size();
      out_.unwind_sites->push_back({static_cast<std::uint32_t>(call_begin),
                                static_cast<std::uint32_t>(e_.offset()), 0, node.op == Op::invoke_cleanup});
    }
    if (invoke) {
      const auto normal = body_.succs_of(n)[0].target;
      if (node.op == Op::invoke) write(body_.params_of(normal)[1]);
      jump(normal);
      return;
    }
    if (node.nres > 1) {
      ValueId result = body_.result(n, 1);
      if (l2::is_float(body_.type(result))) e_.store_scalar_xmm(rbp, frame_.slots[raw(result)], 0, home_width(result));
      else write(result);
    }
    if (node.nres > 2) {
      e_.move(rax, rdx);
      write(body_.result(n, 2));
    }
  }

  void jump(BlockId target) {
    e_.emit({0xe9});  // jmp rel32
    branches_.push_back({e_.displacement(), target});
  }

  void edge(const ir::Succ& succ) {
    auto args = body_.args_of(succ);
    auto params = body_.params_of(succ.target);
    // Stage all sources before overwriting any destination, including on loop backedges.
    for (std::size_t i = 1; i < args.size(); ++i) {
      read(args[i]);
      e_.store(rsp, 8 * std::int32_t(i - 1), rax, home_width(args[i]));
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
      const auto width = home_width(params[i]);
      e_.load(rax, rsp, 8 * std::int32_t(i - 1), width);
      e_.store(rbp, frame_.slots[raw(params[i])], rax, width);
    }
    jump(succ.target);
  }

  std::expected<void, std::string> node(NodeId n);
};

std::expected<void, std::string> Emitter::node(NodeId n) {
  Op op = body_.node(n).op;
  auto ops = body_.operands_of(n);
  auto imms = body_.imms_of(n);
  auto two = [&](std::initializer_list<std::uint8_t> opcode) {
    read(ops[0]);
    read(ops[1], rcx);
    e_.reg(opcode, rcx, rax);
  };
  auto float_two = [&](std::uint8_t opcode) {
    ValueId result = body_.result(n);
    unsigned width = home_width(result);
    e_.load_scalar_xmm(0, rbp, frame_.slots[raw(ops[0])], width);
    e_.scalar_float_arithmetic(opcode, 0, rbp, frame_.slots[raw(ops[1])], width);
    e_.store_scalar_xmm(rbp, frame_.slots[raw(result)], 0, width);
  };
  switch (op) {
    case Op::const_:
      if (body_.type(body_.result(n)) == Type::f32) {
        e_.emit({0xb8});  // mov eax, imm32
        e_.immediate(imms[0], 4);
      } else {
        e_.emit({0x48, 0xb8});  // mov rax, imm64
        e_.immediate(imms[0], 8);
      }
      break;
    case Op::symbol: {
      auto target = ir::EntityId(std::uint32_t(imms[0]));
      if (std::ranges::contains(direct_symbols_, target)) {
        e_.emit({0x48, 0x8d, 0x05});  // lea rax, [rip + disp32]
        relocation(RelocKind::pc32, target);
      } else if (std::ranges::contains(got_symbols_, target)) {
        e_.emit({0x48, 0x8b, 0x05});  // mov rax, [rip + GOT slot]
        relocation(RelocKind::rex_gotpcrelx, target);
      } else {
        return at_node(n, op, "symbol address needs a direct or GOT-relative reference");
      }
      break;
    }
    case Op::alloca: e_.address(rax, rbp, frame_.objects[raw(n)]); break;
    case Op::ptradd: case Op::add: two({0x01}); break;
    case Op::sub: two({0x29}); break;
    case Op::and_: two({0x21}); break;
    case Op::or_: two({0x09}); break;
    case Op::xor_: two({0x31}); break;
    case Op::mul:
      read(ops[0]);
      read(ops[1], rcx);
      e_.reg({0x0f, 0xaf}, rax, rcx);  // imul
      break;
    case Op::umulh:
      read(ops[0]);
      read(ops[1], rcx);
      e_.reg({0xf7}, 4, rcx);  // mul rcx: RDX:RAX = RAX * RCX
      e_.move(rax, rdx);
      break;
    case Op::sdiv: case Op::udiv: case Op::srem: case Op::urem: {
      const auto peer = division_peers_.empty() ? std::nullopt : division_peers_[raw(n)];
      if (peer && raw(*peer) < raw(n)) return {};
      Type type = body_.type(body_.result(n));
      if (type != Type::i32 && type != Type::i64)
        return at_node(n, op, "operation is outside the x86-64 integer subset");
      read(ops[0]);
      read(ops[1], rcx);
      e_.divide(rcx, op == Op::sdiv || op == Op::srem, type == Type::i64);
      if (peer) {
        // Unsigned 32-bit DIV zero-extends EAX and EDX. Store the later result in its own
        // home now, so intervening instructions may overwrite both scratch registers.
        const auto result = body_.result(*peer);
        e_.store(rbp, frame_.slots[raw(result)], op == Op::udiv ? rdx : rax, 8);
      }
      if (op == Op::srem || op == Op::urem) e_.move(rax, rdx);
      break;
    }
    case Op::neg:
      read(ops[0]);
      e_.reg({0xf7}, 3, rax);
      break;
    case Op::ctz:
      read(ops[0]);
      e_.reg({0x0f, 0xbc}, rax, rax, body_.type(ops[0]) == Type::i64);  // BSF (Intel SDM vol. 2)
      break;
    case Op::clz: {
      const Type type = body_.type(ops[0]);
      read(ops[0]);
      e_.reg({0x0f, 0xbd}, rax, rax, type == Type::i64);  // BSR (Intel SDM vol. 2)
      // For a 32- or 64-bit operand, XOR with the maximum bit index subtracts the BSR index.
      e_.reg({0x83}, 6, rax, false);  // xor eax, imm8
      e_.immediate(l2::bits(type) - 1, 1);
      break;
    }
    case Op::fadd: float_two(0x58); return {};
    case Op::fsub: float_two(0x5c); return {};
    case Op::fmul: float_two(0x59); return {};
    case Op::fdiv: float_two(0x5e); return {};
    case Op::fneg:
      // Flip only the IEEE sign bit. Integer XOR preserves the payload and MXCSR status.
      read(ops[0]);
      if (body_.type(body_.result(n)) == Type::f32) {
        e_.emit({0x35});  // xor eax, imm32
        e_.immediate(std::uint32_t{1} << 31, 4);
      } else {
        e_.emit({0x48, 0xb9});  // mov rcx, imm64
        e_.immediate(std::uint64_t{1} << 63, 8);
        e_.reg({0x31}, rcx, rax);  // xor rax, rcx
      }
      break;
    case Op::fabs:
      // Clear only the IEEE sign bit, as for negation.
      read(ops[0]);
      if (body_.type(body_.result(n)) == Type::f32) {
        e_.emit({0x25});  // and eax, imm32
        e_.immediate(~(std::uint32_t{1} << 31), 4);
      } else {
        e_.emit({0x48, 0xb9});  // mov rcx, imm64
        e_.immediate(~(std::uint64_t{1} << 63), 8);
        e_.reg({0x21}, rcx, rax);  // and rax, rcx
      }
      break;
    case Op::fptrunc:
      e_.narrow_float(0, rbp, frame_.slots[raw(ops[0])]);
      e_.store_scalar_xmm(rbp, frame_.slots[raw(body_.result(n))], 0, 4);
      return {};
    case Op::sitofp: case Op::uitofp:
      if (!int_to_float(op == Op::sitofp, ops[0], body_.result(n)))
        return at_node(n, op, "conditional branch displacement out of range");
      return {};
    case Op::fptosi: case Op::fptoui: {
      // Values outside the result type are poison (L2), so a truncation that is exact for
      // every value in range suffices.
      const Type type = body_.type(body_.result(n));
      const unsigned width = home_width(ops[0]);
      const std::int32_t source = frame_.slots[raw(ops[0])];
      if (op == Op::fptosi || type == Type::i32) {
        // A uint32 value fits the signed 64-bit conversion; `write` keeps its low 32 bits.
        e_.truncate_float(rax, rbp, source, width, op == Op::fptoui || type == Type::i64);
        break;
      }
      // Below 2^63 the signed conversion is exact. At or above it, convert x - 2^63, which is
      // exact there, and set the top bit. Comparing first keeps a valid conversion from raising
      // the invalid-operation flag (Intel SDM vol. 2, CVTTSD2SI).
      const std::int32_t scratch = frame_.slots[raw(body_.result(n))];
      if (width == 4) {
        e_.emit({0xb8});  // mov eax, imm32: 2^63 as binary32
        e_.immediate(0x5f000000, 4);
      } else {
        e_.emit({0x48, 0xb8});  // mov rax, imm64: 2^63 as binary64
        e_.immediate(0x43e0000000000000, 8);
      }
      e_.store(rbp, scratch, rax, width);
      e_.load_scalar_xmm(0, rbp, source, width);
      e_.compare_scalar_xmm(0, rbp, scratch, width);
      e_.emit({0x0f, 0x83});  // jae rel32: x >= 2^63 (unordered is poison and falls through)
      auto high = e_.displacement();
      e_.truncate_float(rax, rbp, source, width, true);
      e_.emit({0xe9});  // jmp rel32
      auto converted = e_.displacement();
      if (!e_.patch_relative(high, e_.offset())) return at_node(n, op, "conditional branch displacement out of range");
      e_.scalar_float_arithmetic(0x5c, 0, rbp, scratch, width);  // subtract 2^63
      e_.store_scalar_xmm(rbp, scratch, 0, width);
      e_.truncate_float(rax, rbp, scratch, width, true);
      e_.emit({0x48, 0xb9});  // mov rcx, imm64
      e_.immediate(std::uint64_t{1} << 63, 8);
      e_.reg({0x31}, rcx, rax);  // xor rax, rcx
      if (!e_.patch_relative(converted, e_.offset())) return at_node(n, op, "conditional branch displacement out of range");
      break;
    }
    case Op::shl: case Op::lshr: case Op::ashr:
      read(ops[0]);
      read(ops[1], rcx);
      if (op == Op::ashr) sign_extend(rax, body_.type(ops[0]));
      e_.reg({0xd3}, op == Op::shl ? 4 : op == Op::lshr ? 5 : 7, rax);  // shift by CL
      break;
    case Op::icmp: {
      if (imms[0] > std::to_underlying(ir::Pred::ge)) return at_node(n, op, "unknown integer predicate");
      auto pred = ir::Pred(imms[0]);
      read(ops[0]);
      read(ops[1], rcx);
      if (pred >= ir::Pred::slt && pred <= ir::Pred::sge) {
        sign_extend(rax, body_.type(ops[0]));
        sign_extend(rcx, body_.type(ops[1]));
      }
      e_.reg({0x39}, rcx, rax);  // cmp, then setcc into AL
      e_.reg({0x0f, *condition(pred)}, 0, rax, false);
      break;
    }
    case Op::fcmp: {
      using enum ir::Pred;
      if (imms[0] > std::to_underlying(ge)) return at_node(n, op, "unknown floating predicate");
      auto pred = ir::Pred(imms[0]);
      ValueId lhs = ops[0], rhs = ops[1];
      switch (pred) {
        case lt: case le:
          // The operands already have home slots. Reversal lets SETA and SETAE reject unordered.
          std::swap(lhs, rhs);
          pred = pred == lt ? gt : ge;
          break;
        case eq: case ne: case gt: case ge:
          break;
        default:
          return at_node(n, op, "unknown floating predicate");
      }
      unsigned width = home_width(lhs);
      e_.load_scalar_xmm(0, rbp, frame_.slots[raw(lhs)], width);
      e_.compare_scalar_xmm(0, rbp, frame_.slots[raw(rhs)], width);
      e_.reg({0x0f, *condition(pred)}, 0, rax, false);  // setcc al
      // Unordered sets both ZF and PF. Capture the parity test before changing flags.
      if (pred == eq) {
        e_.reg({0x0f, 0x9b}, 0, rdx, false);  // setnp dl
        e_.reg({0x20}, rdx, rax, false);     // and al, dl
      } else if (pred == ne) {
        e_.reg({0x0f, 0x9a}, 0, rdx, false);  // setp dl
        e_.reg({0x08}, rdx, rax, false);     // or al, dl
      }
      break;
    }
    case Op::select:
      read(ops[0]);
      e_.reg({0x85}, rax, rax);  // test condition, then select without changing flags
      read(ops[2]);
      read(ops[1], rcx);
      e_.reg({0x0f, 0x45}, rax, rcx);  // cmovne
      break;
    case Op::fpext:
      e_.widen_float(0, rbp, frame_.slots[raw(ops[0])]);
      e_.store_scalar_xmm(rbp, frame_.slots[raw(body_.result(n))], 0, 8);
      return {};
    case Op::sext:
      read(ops[0]);
      sign_extend(rax, body_.type(ops[0]));
      break;
    case Op::zext: case Op::trunc: case Op::ptr2int: case Op::int2ptr:
      read(ops[0]);
      break;
    case Op::load:
      read(ops[1], rcx);
      e_.load(rax, rcx, 0, l2::bytes(body_.type(body_.result(n))));
      break;
    case Op::store:
      read(ops[1], rcx);
      read(ops[2]);
      e_.store(rcx, 0, rax, l2::bytes(body_.type(ops[2])));
      return {};
    case Op::atomic_load:
      // Aligned 32-bit loads are indivisible. TSO provides acquire ordering, and
      // the locked sequentially consistent stores prevent reordering across those stores.
      read(ops[1], rcx);
      e_.load(rax, rcx, 0, 4);
      write(body_.result(n, 1));
      return {};
    case Op::atomic_store:
      read(ops[1], rcx);
      read(ops[2]);
      if (ir::MemOrder(imms[0]) == ir::MemOrder::seq_cst) e_.exchange32(rcx, rax);
      else e_.store(rcx, 0, rax, 4);
      return {};
    case Op::atomic_rmw: {
      read(ops[1], rcx);
      switch (ir::Rmw(imms[0])) {
        case ir::Rmw::xchg:
          read(ops[2]);
          e_.exchange32(rcx, rax);
          break;
        case ir::Rmw::add:
        case ir::Rmw::sub:
          read(ops[2]);
          if (ir::Rmw(imms[0]) == ir::Rmw::sub) e_.reg({0xf7}, 3, rax, false);  // neg eax
          e_.atomic_add32(rcx, rax);
          break;
        case ir::Rmw::and_:
        case ir::Rmw::or_:
        case ir::Rmw::xor_: {
          const auto rmw = ir::Rmw(imms[0]);
          const std::uint8_t opcode = rmw == ir::Rmw::and_ ? 0x21 : rmw == ir::Rmw::or_ ? 0x09 : 0x31;
          read(ops[2], rdi);
          e_.load(rax, rcx, 0, 4);
          const auto retry = e_.offset();
          e_.reg({0x89}, rax, rdx, false);  // mov edx, eax
          e_.reg({opcode}, rdi, rdx, false);
          e_.compare_exchange32(rcx, rdx);
          e_.emit({0x0f, 0x85});  // jne retry, with the newly observed value in EAX
          if (!e_.patch_relative(e_.displacement(), retry))
            return at_node(n, op, "atomic retry branch displacement out of range");
          break;
        }
        default: return at_node(n, op, "unsupported atomic read-modify-write operation");
      }
      write(body_.result(n, 1));
      return {};
    }
    case Op::atomic_cmpxchg:
      read(ops[1], rcx);
      read(ops[2]);
      read(ops[3], rdx);
      e_.compare_exchange32(rcx, rdx);
      write(body_.result(n, 1));  // the i32 write preserves ZF
      e_.emit({0x0f, 0x94, 0xc0});  // sete al
      write(body_.result(n, 2));
      return {};
    case Op::fence:
      // TSO already orders the acquire and release cases. The state dependency is
      // needed at every order, including when the fence has no machine instruction.
      if (ir::MemOrder(imms[0]) == ir::MemOrder::seq_cst) e_.emit({0x0f, 0xae, 0xf0});  // mfence
      return {};
    case Op::va_start:
      va_start(ops[1]);
      return {};
    case Op::va_arg:
      if (!va_arg(ops[1], body_.result(n, 1))) return at_node(n, op, "conditional branch displacement out of range");
      return {};
    case Op::call: case Op::call_ind: case Op::invoke: case Op::invoke_cleanup:
      call(n);
      return {};
    case Op::br:
      edge(body_.succs_of(n)[0]);
      return {};
    case Op::cond_br: {
      auto succs = body_.succs_of(n);
      read(ops[0]);
      e_.reg({0x85}, rax, rax);
      e_.emit({0x0f, 0x84});  // jz rel32, skip the true edge's copies and jump
      auto at = e_.displacement();
      edge(succs[0]);
      if (!e_.patch_relative(at, e_.offset())) return at_node(n, op, "conditional branch displacement out of range");
      edge(succs[1]);
      return {};
    }
    case Op::ret:
      if (body_.indirect_result) read(body_.params_of(BlockId(0))[body_.indirect_result]);
      if (ops.size() > 2) read(ops[2], rdx);
      if (ops.size() > 1) {
        ValueId result = ops[1];
        if (l2::is_float(body_.type(result))) e_.load_scalar_xmm(0, rbp, frame_.slots[raw(result)], home_width(result));
        else read(result);
      }
      e_.emit({0xc9});  // leave: restore RSP and the caller's RBP
      frame_row(FrameState::entry);
      e_.emit({0xc3});  // ret
      return {};
    case Op::compiler_memory_barrier:
      return {};
    case Op::int3:
      e_.emit({0xcc});  // INT3 (Intel SDM vol. 2)
      return {};
    case Op::unreachable:
      e_.emit({0x0f, 0x0b});  // ud2
      return {};
    default: return at_node(n, op, "operation is outside the x86-64 integer subset");
  }
  write(body_.result(n));
  return {};
}

}  // namespace

bool compatible_division_pair(const l2::Function& body, DivisionPair pair) {
  if (raw(pair.first) >= raw(pair.second) || raw(pair.second) >= body.nodes.size()) return false;
  if (!unsigned_division(body, pair.first) || !unsigned_division(body, pair.second)) return false;
  const auto& first = body.node(pair.first);
  const auto& second = body.node(pair.second);
  if (first.block != second.block || first.op == second.op) return false;
  const auto lhs = body.operands_of(pair.first);
  const auto rhs = body.operands_of(pair.second);
  return division_operand(body, lhs[0]) == division_operand(body, rhs[0]) &&
         division_operand(body, lhs[1]) == division_operand(body, rhs[1]);
}

std::expected<std::vector<std::vector<DivisionSite>>, std::string>
group_divisions(const l2::Function& body, ycxx::exec::inplace_stop_token stop) {
  using Operands = std::pair<DivisionOperand, DivisionOperand>;
  std::vector<std::vector<DivisionSite>> groups;
  for (const auto block : body.block_ids()) {
    if (stop.stop_requested()) return std::unexpected("model cancelled");
    std::map<Operands, std::vector<DivisionSite>> sites;
    for (const auto id : body.nodes_of(block)) {
      if ((raw(id) % 256 == 0) && stop.stop_requested()) return std::unexpected("model cancelled");
      if (!unsigned_division(body, id)) continue;
      const auto operands = body.operands_of(id);
      const auto divisor = division_operand(body, operands[1]);
      // The existing shift/mask path remains the policy for constant power-of-two divisors.
      if (std::get<1>(divisor) == OperandKind::constant) {
        auto value = std::get<2>(divisor);
        if (body.type(operands[1]) == Type::i32) value = std::uint32_t(value);
        if (std::has_single_bit(value)) continue;
      }
      sites[{division_operand(body, operands[0]), divisor}].push_back({id, body.node(id).op == Op::udiv, 0});
    }
    for (auto& [operand, group] : sites) {
      if (stop.stop_requested()) return std::unexpected("model cancelled");
      if (std::ranges::any_of(group, [&](const DivisionSite& s) { return s.quotient != group.front().quotient; }))
        groups.push_back(std::move(group));
    }
  }
  if (groups.empty()) return groups;
  if (stop.stop_requested()) return std::unexpected("model cancelled");
  std::ranges::sort(groups, {}, [](const auto& group) { return group.front().node; });
  if (stop.stop_requested()) return std::unexpected("model cancelled");
  const auto frame = layout<true>(body, stop);
  if (!frame) return std::unexpected(frame.error());
  std::vector<std::uint8_t> bytes;
  Encoder encoder(bytes);
  for (auto& group : groups) {
    for (auto& site : group) {
      if (stop.stop_requested()) return std::unexpected("model cancelled");
      const auto operands = body.operands_of(site.node);
      const bool wide = body.type(body.result(site.node)) == Type::i64;
      bytes.clear();
      encoder.load(rax, rbp, frame->slots[raw(operands[0])], 8);
      encoder.load(rcx, rbp, frame->slots[raw(operands[1])], 8);
      encoder.divide(rcx, false, wide);
      if (!site.quotient) encoder.move(rax, rdx);
      if (!wide) encoder.reg({0x89}, rax, rax, false);
      // The later result still gets the same store to its home, just earlier in the block.
      site.saved_bytes = std::uint32_t(bytes.size());
    }
  }
  return groups;
}

std::expected<MachineCode, std::string> compile_function(const l2::Function& body,
                                                         std::span<const ir::EntityId> direct_symbols,
                                                         std::span<const ir::EntityId> got_symbols,
                                                         std::span<const DivisionPair> division_pairs) {
  if (auto valid = l2::verify(body); !valid) return std::unexpected(valid.error());
  if (std::ranges::contains(body.values, Type::i128, &l2::Value::type))
    return std::unexpected("an i128 value was not legalized");
  std::vector<std::optional<NodeId>> division_peers;
  if (!division_pairs.empty()) division_peers.resize(body.nodes.size());
  for (const auto pair : division_pairs) {
    if (!compatible_division_pair(body, pair)) return std::unexpected("invalid unsigned division pair");
    auto& first = division_peers[raw(pair.first)];
    auto& second = division_peers[raw(pair.second)];
    if (first || second) return std::unexpected("overlapping unsigned division pairs");
    first = pair.second;
    second = pair.first;
  }
  auto frame = layout(body);
  if (!frame) return std::unexpected(frame.error());
  return Emitter(body, *frame, direct_symbols, got_symbols, division_peers).run();
}

std::expected<EhFrameFields, std::string> append_eh_frame(std::vector<std::uint8_t>& section,
                                                         EhFrameCies& cies, const MachineCode& code) {
  // AMD64 psABI §§3.6.2, 4.2.4 and DWARF §6.4. The return address is at CFA-8.
  // RBP initially has the caller's value.
  constexpr std::uint8_t dwarf_rbp = 6, dwarf_rsp = 7, dwarf_ra = 16;
  constexpr std::uint8_t def_cfa = 0x0c, same_value = 0x08, offset = 0x80, restore = 0xc0;
  constexpr std::uint8_t pcrel_sdata4 = 0x1b, indirect = 0x80;
  constexpr std::uint8_t initial_cfi[]{
      def_cfa, dwarf_rsp, 8, offset | dwarf_ra, 1, same_value, dwarf_rbp,
  };
  if (code.bytes.empty() || !std::in_range<std::int32_t>(code.bytes.size()))
    return std::unexpected("the unwind range is outside signed 32 bits");

  std::vector<std::uint8_t> instructions;
  Encoder cfi(instructions);
  std::uint32_t previous = 0;
  for (const FrameRow row : code.frame_rows) {
    if (row.offset <= previous || row.offset >= code.bytes.size())
      return std::unexpected("an unwind row is outside the function's instruction range");
    const auto advance = row.offset - previous;
    if (advance <= 0x3f) {
      cfi.emit({static_cast<std::uint8_t>(0x40 | advance)});  // DW_CFA_advance_loc
    } else {
      const unsigned width = advance <= 0xff ? 1 : advance <= 0xffff ? 2 : 4;
      cfi.emit({static_cast<std::uint8_t>(0x02 + std::countr_zero(width))});  // advance_loc1/2/4
      cfi.immediate(advance, width);
    }
    switch (row.state) {
      case FrameState::entry:
        cfi.emit({def_cfa, dwarf_rsp, 8, restore | dwarf_rbp});
        break;
      case FrameState::saved_rbp:
        cfi.emit({def_cfa, dwarf_rsp, 16, offset | dwarf_rbp, 2});
        break;
      case FrameState::frame_pointer:
        cfi.emit({def_cfa, dwarf_rbp, 16, offset | dwarf_rbp, 2});
        break;
    }
    previous = row.offset;
  }

  const bool handler = code.unwind_sites.has_value();
  auto& cie = handler ? cies.handler : cies.ordinary;
  constexpr std::size_t word = sizeof(std::uint32_t);
  const std::size_t cie_size = handler ? 32 : 24;  // length field included
  const std::size_t fde_header = 4 * word + 1 + (handler ? word : 0);
  if (section.size() % 8 != 0 || (cie && (*cie > section.size() || cie_size > section.size() - *cie)))
    return std::unexpected("invalid unwind CIE position");
  const auto fde_start = section.size() + (cie ? 0 : cie_size);
  const auto fde_size = align_up(fde_header + instructions.size(), 8);
  if (!std::in_range<std::uint32_t>(fde_start + fde_size))
    return std::unexpected("the unwind section exceeds the object offset limit");

  Encoder record(section);
  EhFrameFields fields{};
  if (!cie) {
    cie = static_cast<std::uint32_t>(record.offset());
    record.immediate(cie_size - word, word);
    record.immediate(0, word);  // CIE ID
    if (handler) {
      record.emit({1, 'z', 'P', 'L', 'R', 0, 1, 0x78, dwarf_ra, 7, indirect | pcrel_sdata4});
      fields.personality = static_cast<std::uint32_t>(record.offset());
      record.immediate(0, word);
      record.emit({pcrel_sdata4, pcrel_sdata4});  // LSDA and initial-location encodings
    } else {
      record.emit({1, 'z', 'R', 0, 1, 0x78, dwarf_ra, 1, pcrel_sdata4});
    }
    section.insert(section.end(), std::begin(initial_cfi), std::end(initial_cfi));
  }
  record.immediate(fde_size - word, word);
  record.immediate(fde_start + word - *cie, word);  // backwards from the CIE pointer field
  fields.initial_location = static_cast<std::uint32_t>(record.offset());
  record.immediate(0, word);  // S - P at this field, with no instruction-length addend
  record.immediate(code.bytes.size(), word);
  record.emit({static_cast<std::uint8_t>(handler ? word : 0)});
  if (handler) {
    fields.lsda = static_cast<std::uint32_t>(record.offset());
    record.immediate(0, word);
  }
  section.insert(section.end(), instructions.begin(), instructions.end());
  section.resize(fde_start + fde_size, 0);  // DW_CFA_nop padding belongs to the FDE
  return fields;
}

namespace {

void uleb128(std::vector<std::uint8_t>& bytes, std::size_t value) {
  do {
    const auto part = static_cast<std::uint8_t>(value & 0x7f);
    value >>= 7;
    bytes.push_back(part | (value ? 0x80 : 0));
  } while (value != 0);
}

}  // namespace

std::expected<std::vector<std::uint8_t>, std::string> exception_lsda(const MachineCode& code, bool catch_int) {
  if (!code.unwind_sites || code.bytes.empty() || !std::in_range<std::int32_t>(code.bytes.size()))
    return std::unexpected("missing or oversized hosted unwind machine code");

  std::vector<std::uint8_t> sites;
  std::uint32_t previous_end = 0;
  bool protected_call = false;
  bool cleanup_only = false;
  for (const UnwindCallSite& site : *code.unwind_sites) {
    if (site.begin < previous_end || site.begin >= site.end || site.end > code.bytes.size())
      return std::unexpected("an unwind call-site range is outside the function or out of order");
    previous_end = site.end;
    if (site.landing_pad != 0) {
      if (protected_call || site.landing_pad >= code.bytes.size() ||
          (site.landing_pad >= site.begin && site.landing_pad < site.end))
        return std::unexpected("invalid unwind landing pad");
      if (site.end - site.begin != 5 || code.bytes[site.begin] != 0xe8 ||
          !std::ranges::any_of(code.relocs, [&](const Reloc& r) {
            return r.offset == site.begin + 1 && r.addend == -4 &&
                   (r.kind == RelocKind::plt32 || r.kind == RelocKind::pc32);
          }))
        return std::unexpected("the unwind protected range must be one relocated direct call");
      protected_call = true;
      cleanup_only = site.cleanup_only;
    } else if (site.cleanup_only) {
      return std::unexpected("a cleanup call site needs a landing pad");
    }
    uleb128(sites, site.begin);
    uleb128(sites, site.end - site.begin);
    uleb128(sites, site.landing_pad);
    uleb128(sites, site.landing_pad != 0 && !site.cleanup_only ? 1 : 0);
  }
  if (!protected_call) return std::unexpected("missing unwind protected call");

  if (cleanup_only) {
    if (catch_int) return std::unexpected("a cleanup-only LSDA cannot have a handler type");
    // The cleanup entry needs neither a type table nor an action table.
    std::vector<std::uint8_t> lsda{0xff, 0xff, 0x01};  // omit LPStart and types, ULEB128 sites
    uleb128(lsda, sites.size());
    lsda.insert(lsda.end(), sites.begin(), sites.end());
    return lsda;
  }

  std::vector<std::uint8_t> tables{0x01};  // DW_EH_PE_uleb128 call-site encoding
  uleb128(tables, sites.size());
  tables.insert(tables.end(), sites.begin(), sites.end());
  tables.insert(tables.end(), {1, 0});  // SLEB128 filter 1, no next action
  tables.insert(tables.end(), {0, 0, 0, 0});  // type relocation field, or null type for catch (...)
  // Omit LPStart. The signed PC-relative int entry points to an indirect cell.
  std::vector<std::uint8_t> lsda{0xff, std::uint8_t(catch_int ? 0x9b : 0x1b)};
  uleb128(lsda, tables.size());  // from the end of the offset to the end of the type table
  lsda.insert(lsda.end(), tables.begin(), tables.end());
  return lsda;
}

}  // namespace ycxx::x86_64
