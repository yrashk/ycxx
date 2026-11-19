// Baseline System V AMD64 code generation. Put each scalar L2 value in a frame slot and emit
// instructions in block order, using only caller-saved scratch registers and a saved RBP.
export module ycxx.target.x86_64;

import :encode;
import std;
import ycxx.ir;
import ycxx.ir.l2;

namespace ycxx::x86_64 {

export enum class RelocKind : std::uint8_t { pc32, plt32 };

export struct Reloc {
  std::uint32_t offset;  // disp32 field, relative to the function's first byte
  RelocKind kind;        // R_X86_64_PC32 or R_X86_64_PLT32 in ELF
  ir::EntityId target;
  std::int64_t addend;
  bool operator==(const Reloc&) const = default;
};

export struct MachineCode {
  std::vector<std::uint8_t> bytes;
  std::vector<Reloc> relocs;
  bool operator==(const MachineCode&) const = default;
};

// Compile well-formed L2 with a scalar INTEGER ABI signature and scalar INTEGER calls. The
// producer must check the original signatures and ellipsis argument types. L2 can erase
// distinctions between scalar and class arguments or results. A variadic definition or
// indirect call with an empty ellipsis cannot be identified from L2 alone. Pass
// the entities whose addresses may be resolved directly by a signed RIP-relative displacement
// (e.g. local definitions in the small code model) in `direct_symbols`.
export std::expected<MachineCode, std::string> compile_function(const l2::Function& body,
                                                                std::span<const ir::EntityId> direct_symbols = {});

namespace {

using l2::Op;
using l2::Type;
using ir::BlockId;
using ir::NodeId;
using ir::ValueId;
using ir::raw;
using enum Reg;

// AMD64 psABI §3.2.2–3.2.3: scalar INTEGER argument registers, then eightbyte stack slots.
constexpr Reg argument_regs[] = {rdi, rsi, rdx, rcx, r8, r9};
constexpr std::size_t register_args = std::size(argument_regs);

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
};

std::expected<Frame, std::string> layout(const l2::Function& body) {
  // Keep the frame and the saved frame and return addresses within one page. Larger frames
  // need stack probing to avoid skipping a guard page.
  constexpr std::uint64_t max_frame = 4096 - 16;
  if (body.indirect_result || body.results.size() > 1)
    return std::unexpected("indirect and multiple results are outside the scalar ABI subset");
  for (Type t : body.results)
    if (t == Type::state || l2::is_float(t)) return std::unexpected("unsupported scalar ABI result type");

  Frame frame;
  frame.slots.resize(body.values.size());
  frame.objects.resize(body.nodes.size());
  for (std::size_t i = 0; i < body.values.size(); ++i) {
    Type t = body.values[i].type;
    if (t == Type::state) continue;
    if (l2::is_float(t)) return std::unexpected("floating-point values are outside the scalar INTEGER subset");
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
      if (nd.op == Op::alloca) {
        auto size = imms[0], align = imms[1];
        if (!std::has_single_bit(align) || align > 16)
          return at_node(n, nd.op, "frame object alignment must be a power of two up to 16");
        if (size == 0 || size > max_frame) return at_node(n, nd.op, "unsupported frame object size");
        frame.size = align_up(frame.size + size, align);
        if (frame.size > max_frame) return at_node(n, nd.op, "x86-64 frame exceeds the single-page limit");
        frame.objects[raw(n)] = -std::int32_t(frame.size);
      }
      if (nd.op == Op::call || nd.op == Op::call_ind) {
        bool indirect = nd.op == Op::call_ind;
        if (indirect && body.type(body.operands_of(n)[1]) != Type::ptr)
          return at_node(n, nd.op, "an indirect callee must be a pointer");
        if (indirect && imms[1]) return at_node(n, nd.op, "indirect variadic calls are outside the scalar ABI subset");
        if (imms[indirect ? 2 : 3] || nd.nres > 2)
          return at_node(n, nd.op, "indirect and multiple results are outside the scalar ABI subset");
        std::size_t args = nd.nopnd - (indirect ? 2 : 1);
        if (args > register_args) scratch_words = std::max(scratch_words, std::uint64_t(args - register_args));
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
  Emitter(const l2::Function& body, const Frame& frame, std::span<const ir::EntityId> direct_symbols)
      : body_(body), frame_(frame), direct_symbols_(direct_symbols), block_offsets_(body.blocks.size()) {}

  std::expected<MachineCode, std::string> run() {
    e_.emit({0x55});  // push rbp
    e_.move(rbp, rsp);
    if (frame_.size) {
      e_.reg({0x81}, 5, rsp);  // sub rsp, imm32
      e_.immediate(frame_.size, 4);
    }
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
  MachineCode out_;
  Encoder e_{out_.bytes};
  std::vector<std::size_t> block_offsets_;

  struct Branch {
    std::size_t at;
    BlockId target;
  };
  std::vector<Branch> branches_;

  void read(ValueId v, Reg dst = rax) { e_.load(dst, rbp, frame_.slots[raw(v)], 8); }
  void write(ValueId v) {
    // Keep high bits clear for later unsigned operations and narrow ABI values. The psABI
    // leaves bits outside an INTEGER value's memory representation unspecified at the boundary.
    unsigned bits = l2::bits(body_.type(v));
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

  void parameters() {
    std::size_t i = 0;
    for (ValueId v : body_.params_of(BlockId(0)) | std::views::drop(1)) {
      if (i < register_args) e_.move(rax, argument_regs[i]);
      else e_.load(rax, rbp, 16 + 8 * std::int32_t(i - register_args), 8);
      write(v);
      ++i;
    }
  }

  void relocation(RelocKind kind, ir::EntityId target) {
    auto at = e_.displacement();
    // x86 measures from the end of the instruction; ELF measures from the field.
    out_.relocs.push_back({std::uint32_t(at), kind, target, -4});
  }

  void call(NodeId n) {
    bool indirect = body_.node(n).op == Op::call_ind;
    auto ops = body_.operands_of(n);
    auto imms = body_.imms_of(n);
    auto args = ops.subspan(indirect ? 2 : 1);
    for (std::size_t i = 0; i < args.size(); ++i) {
      // The ABI leaves excess bits unspecified. Each frame slot already contains the low bits.
      if (i < register_args) read(args[i], argument_regs[i]);
      else {
        read(args[i]);
        e_.store(rsp, 8 * std::int32_t(i - register_args), rax, 8);
      }
    }
    if (indirect) {
      read(ops[1]);
      e_.reg({0xff}, 2, rax, false);  // call rax
    } else {
      // AL gives the vector register count for variadic calls (AMD64 psABI §3.2.3).
      // L2's ellipsis count can be zero for a variadic call, so set AL for every direct call.
      e_.reg({0x31}, rax, rax, false);  // xor eax, eax
      e_.emit({0xe8});  // call rel32
      relocation(RelocKind::plt32, ir::EntityId(std::uint32_t(imms[0])));
    }
    if (body_.node(n).nres > 1) write(body_.result(n, 1));
  }

  void edge(const ir::Succ& succ) {
    auto args = body_.args_of(succ);
    auto params = body_.params_of(succ.target);
    // Stage all sources before overwriting any destination, including on loop backedges.
    for (std::size_t i = 1; i < args.size(); ++i) {
      read(args[i]);
      e_.store(rsp, 8 * std::int32_t(i - 1), rax, 8);
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
      e_.load(rax, rsp, 8 * std::int32_t(i - 1), 8);
      e_.store(rbp, frame_.slots[raw(params[i])], rax, 8);
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
      e_.emit({0x48, 0xb8});  // mov rax, imm64
      e_.immediate(imms[0], 8);
      break;
    case Op::symbol: {
      auto target = ir::EntityId(std::uint32_t(imms[0]));
      if (!std::ranges::contains(direct_symbols_, target))
        return at_node(n, op, "symbol address needs a direct RIP-relative reference");
      e_.emit({0x48, 0x8d, 0x05});  // lea rax, [rip + disp32]
      relocation(RelocKind::pc32, target);
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
      if (ops.size() > 1) read(ops[1]);
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
                                                         std::span<const ir::EntityId> direct_symbols) {
  if (auto valid = l2::verify(body); !valid) return std::unexpected(valid.error());
  auto frame = layout(body);
  if (!frame) return std::unexpected(frame.error());
  return Emitter(body, *frame, direct_symbols).run();
}

}  // namespace ycxx::x86_64
