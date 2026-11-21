// Baseline System V AMD64 code generation. Put each scalar L2 value in a frame slot and emit
// instructions in block order, using only caller-saved scratch registers and a saved RBP.
export module ycxx.target.x86_64;

import :encode;
import std;
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

export struct MachineCode {
  std::vector<std::uint8_t> bytes;
  std::vector<Reloc> relocs;
  bool operator==(const MachineCode&) const = default;
};

// Compile well-formed L2 with INTEGER ABI values and direct scalar SSE values.
// The producer checks original signatures and ellipsis types and flattens INTEGER records.
// L2 can erase the distinction between scalar and class arguments or results. Pass
// the entities whose addresses may be resolved directly by a signed RIP-relative displacement
// (e.g. local definitions in the small code model) in `direct_symbols`. Pass default-visible
// symbols in `got_symbols` so their addresses load through the ELF GOT.
export std::expected<MachineCode, std::string> compile_function(const l2::Function& body,
                                                                std::span<const ir::EntityId> direct_symbols = {},
                                                                std::span<const ir::EntityId> got_symbols = {});

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

// Floating value motion and binary32-to-binary64 widening use these instructions.
bool supports_float(Op op) {
  switch (op) {
    case Op::const_: case Op::load: case Op::store: case Op::select: case Op::fpext:
    case Op::call: case Op::call_ind: case Op::ret: return true;
    default: return false;
  }
}

std::expected<Frame, std::string> layout(const l2::Function& body) {
  // Keep the frame and the saved frame and return addresses within one page. Larger frames
  // need stack probing to avoid skipping a guard page.
  constexpr std::uint64_t max_frame = 4096 - 16;
  const bool float_result = std::ranges::any_of(body.results, l2::is_float);
  if (body.indirect_result || body.results.size() > (float_result ? 1u : 2u))
    return std::unexpected("only a scalar SSE result or up to two INTEGER result registers are supported");
  if (std::ranges::contains(body.results, Type::state)) return std::unexpected("unsupported scalar ABI result type");
  auto is_float = [&](ValueId v) { return l2::is_float(body.type(v)); };

  Frame frame;
  frame.slots.resize(body.values.size());
  frame.objects.resize(body.nodes.size());
  for (std::size_t i = 0; i < body.values.size(); ++i) {
    Type t = body.values[i].type;
    if (t == Type::state) continue;
    frame.size += 8;
    if (frame.size > max_frame) return std::unexpected("x86-64 frame exceeds the single-page limit");
    frame.slots[i] = -std::int32_t(frame.size);
  }

  std::uint64_t scratch_words = 0;
  for (BlockId b : body.block_ids()) {
    const ir::Block& block = body.block(b);
    if (!block.nnodes) return std::unexpected("a block needs a terminator");
    auto params = body.params_of(b);
    if (params.empty() || body.type(params.front()) != Type::state)
      return std::unexpected("a block needs a leading state parameter");
    for (ValueId p : params | std::views::drop(1))
      if (body.type(p) == Type::state) return std::unexpected("a block has more than one state parameter");
    if (raw(b) != 0) scratch_words = std::max(scratch_words, std::uint64_t(block.nparams - 1));

    for (NodeId n : body.nodes_of(b)) {
      const l2::Node& nd = body.node(n);
      auto imms = body.imms_of(n);
      auto operands = body.operands_of(n);
      const bool uses_float_result = std::ranges::any_of(std::views::iota(0u, nd.nres),
          [&](std::uint32_t i) { return is_float(body.result(n, i)); });
      if (!supports_float(nd.op) && (uses_float_result || std::ranges::any_of(operands, is_float)))
        return at_node(n, nd.op, "floating-point arithmetic, comparisons and conversions are unsupported");
      if (nd.op == Op::alloca) {
        auto size = imms[0], align = imms[1];
        if (!std::has_single_bit(align) || align > 16)
          return at_node(n, nd.op, "frame object alignment must be a power of two up to 16");
        if (size == 0 || size > max_frame) return at_node(n, nd.op, "unsupported frame object size");
        frame.size = align_up(frame.size + size, align);
        if (frame.size > max_frame) return at_node(n, nd.op, "x86-64 frame exceeds the single-page limit");
        frame.objects[raw(n)] = -std::int32_t(frame.size);
      }
      if (nd.op == Op::va_start && !frame.variadic_save) {
        if (float_result || std::ranges::any_of(body.params_of(BlockId(0)), is_float))
          return at_node(n, nd.op, "floating-point variadic parameters and results are unsupported");
        frame.size = align_up(frame.size + reg_save_size, xmm_slot);
        if (frame.size > max_frame) return at_node(n, nd.op, "x86-64 frame exceeds the single-page limit");
        frame.variadic_save = -std::int32_t(frame.size);
      }
      if (nd.op == Op::call || nd.op == Op::call_ind) {
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
        if (imms[indirect ? 2 : 3] || nd.nres > (indirect || variadic || uses_float_result ? 2u : 3u))
          return at_node(n, nd.op, "unsupported ABI result placement");
        if (indirect && (uses_float_result || std::ranges::any_of(args, is_float)))
          return at_node(n, nd.op, "floating-point indirect calls are unsupported");
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
  if (frame.size > max_frame) return std::unexpected("x86-64 frame exceeds the single-page limit");
  return frame;
}

class Emitter {
 public:
  Emitter(const l2::Function& body, const Frame& frame, std::span<const ir::EntityId> direct_symbols,
          std::span<const ir::EntityId> got_symbols)
      : body_(body), frame_(frame), direct_symbols_(direct_symbols), got_symbols_(got_symbols),
        block_offsets_(body.blocks.size()) {}

  std::expected<MachineCode, std::string> run() {
    e_.emit({0x55});  // push rbp
    e_.move(rbp, rsp);
    if (frame_.size) {
      e_.reg({0x81}, 5, rsp);  // sub rsp, imm32
      e_.immediate(frame_.size, 4);
    }
    if (frame_.variadic_save && !save_variadic_registers())
      return std::unexpected("x86-64 register-save branch displacement out of range");
    parameters();
    for (BlockId b : body_.block_ids()) {
      block_offsets_[raw(b)] = e_.offset();
      for (NodeId n : body_.nodes_of(b)) {
        if (auto result = node(n); !result) return std::unexpected(result.error());
      }
    }
    for (const auto& branch : branches_)
      if (!e_.patch_relative(branch.at, block_offsets_[raw(branch.target)]))
        return std::unexpected("x86-64 branch displacement exceeds signed 32 bits");
    if (!std::in_range<std::uint32_t>(e_.offset())) return std::unexpected("x86-64 function exceeds 4 GiB");
    return std::move(out_);
  }

 private:
  const l2::Function& body_;
  const Frame& frame_;
  std::span<const ir::EntityId> direct_symbols_;
  std::span<const ir::EntityId> got_symbols_;
  MachineCode out_;
  Encoder e_{out_.bytes};
  std::vector<std::size_t> block_offsets_;

  struct Branch {
    std::size_t at;
    BlockId target;
  };
  std::vector<Branch> branches_;

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
    // By this point each named parameter occupies exactly one INTEGER slot. RBP+16
    // steps over the saved frame pointer and return address to the incoming stack slots.
    const auto named = std::size_t(body_.params_of(BlockId(0)).size()) - 1;
    const auto named_in_registers = std::min(named, register_args);
    e_.store_immediate32(rcx, gp_offset_field, named_in_registers * gp_slot);
    e_.store_immediate32(rcx, fp_offset_field, gp_save_size);
    e_.address(rax, rbp, 16 + std::int32_t(named - named_in_registers) * gp_slot);
    e_.store(rcx, overflow_arg_area_field, rax, 8);
    e_.address(rax, rbp, frame_.variadic_save);
    e_.store(rcx, reg_save_area_field, rax, 8);
  }

  bool va_arg(ValueId cursor, ValueId result) {
    read(cursor, rdx);
    e_.load(rax, rdx, gp_offset_field, 4);
    e_.reg({0x83}, 7, rax, false);  // cmp eax, imm8
    e_.immediate(gp_save_size, 1);
    e_.emit({0x0f, 0x83});  // jae rel32
    auto overflow = e_.displacement();

    e_.load(rcx, rdx, reg_save_area_field, 8);
    e_.reg({0x01}, rax, rcx);  // add rcx, rax
    e_.reg({0x83}, 0, rax, false);  // add eax, imm8
    e_.immediate(gp_slot, 1);
    e_.store(rdx, gp_offset_field, rax, 4);
    e_.emit({0xe9});  // jmp rel32
    auto fetched = e_.displacement();

    if (!e_.patch_relative(overflow, e_.offset())) return false;
    e_.load(rcx, rdx, overflow_arg_area_field, 8);
    e_.address(rax, rcx, gp_slot);
    e_.store(rdx, overflow_arg_area_field, rax, 8);
    if (!e_.patch_relative(fetched, e_.offset())) return false;

    e_.load(rax, rcx, 0, l2::bytes(body_.type(result)));
    write(result);
    return true;
  }

  void parameters() {
    Amd64ArgumentPlan plan;
    for (ValueId v : body_.params_of(BlockId(0)) | std::views::drop(1)) {
      auto [kind, index] = plan.place(body_.type(v));
      if (kind == Amd64ArgumentKind::sse) {
        e_.store_scalar_xmm(rbp, frame_.slots[raw(v)], index, home_width(v));
      } else {
        if (kind == Amd64ArgumentKind::integer) e_.move(rax, argument_regs[index]);
        else e_.load(rax, rbp, 16 + 8 * std::int32_t(index), home_width(v));
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
    if (indirect) {
      // R11 is caller-saved and holds no argument. Variadic calls reserve AL for the vector
      // register count (AMD64 psABI §3.2.3), so their callee address needs a separate register.
      Reg target = variadic ? r11 : rax;
      read(ops[1], target);
      if (variadic) e_.reg({0x31}, rax, rax, false);  // xor eax, eax
      e_.reg({0xff}, 2, target, false);  // call r/m64
    } else {
      // AL counts named and unnamed vector register arguments (AMD64 psABI §3.2.3).
      const auto vector_count = variadic ? plan.used_sse_registers() : 0u;
      if (vector_count) e_.emit({0xb0, std::uint8_t(vector_count)});  // mov al, imm8
      else e_.reg({0x31}, rax, rax, false);  // xor eax, eax
      e_.emit({0xe8});  // call rel32
      relocation(RelocKind::plt32, ir::EntityId(std::uint32_t(imms[0])));
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
    e_.emit({0xe9});  // jmp rel32
    branches_.push_back({e_.displacement(), succ.target});
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
    case Op::sdiv: case Op::udiv: case Op::srem: case Op::urem: {
      Type type = body_.type(body_.result(n));
      if (type != Type::i32 && type != Type::i64)
        return at_node(n, op, "operation is outside the x86-64 integer subset");
      read(ops[0]);
      read(ops[1], rcx);
      e_.divide(rcx, op == Op::sdiv || op == Op::srem, type == Type::i64);
      if (op == Op::srem || op == Op::urem) e_.move(rax, rdx);
      break;
    }
    case Op::neg:
      read(ops[0]);
      e_.reg({0xf7}, 3, rax);
      break;
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
    case Op::va_start:
      va_start(ops[1]);
      return {};
    case Op::va_arg:
      if (!va_arg(ops[1], body_.result(n, 1))) return at_node(n, op, "conditional branch displacement out of range");
      return {};
    case Op::call: case Op::call_ind:
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
      if (ops.size() > 2) read(ops[2], rdx);
      if (ops.size() > 1) {
        ValueId result = ops[1];
        if (l2::is_float(body_.type(result))) e_.load_scalar_xmm(0, rbp, frame_.slots[raw(result)], home_width(result));
        else read(result);
      }
      e_.emit({0xc9, 0xc3});  // leave, ret
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

std::expected<MachineCode, std::string> compile_function(const l2::Function& body,
                                                         std::span<const ir::EntityId> direct_symbols,
                                                         std::span<const ir::EntityId> got_symbols) {
  if (auto valid = l2::verify(body); !valid) return std::unexpected(valid.error());
  auto frame = layout(body);
  if (!frame) return std::unexpected(frame.error());
  return Emitter(body, *frame, direct_symbols, got_symbols).run();
}

}  // namespace ycxx::x86_64
