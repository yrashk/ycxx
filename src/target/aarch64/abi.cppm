// Register classes and the calling convention, as data (optimization design §5; #28): AAPCS64
// (Arm IHI 0055, §6) with Apple's arm64 deviations ("Writing ARM64 code for Apple platforms"):
//   - integer arguments go in x0–x7, then on the stack; an integer result comes back in x0;
//   - x9–x15 are caller-saved temporaries, x16/x17 the intra-procedure-call scratch registers,
//     x18 is reserved by the platform, x19–x28 are callee-saved, x29 is the frame pointer and x30
//     the link register;
//   - the stack pointer stays 16-byte aligned, and the frame record (x29, x30) is kept, so x29
//     always points at the caller's frame record;
//   - Apple: arguments on the stack take their natural size and alignment, not 8-byte slots;
//   - Apple: the caller extends an integer argument narrower than 32 bits to 32 bits when it goes
//     in a register (on the stack it keeps its size), and the callee extends a narrow result
//     (`lower` makes that explicit, LowerConfig::min_result_bits).
// Variadic calls (Apple passes every variadic argument on the stack) are not in the α subset.
export module ycxx.target.aarch64:abi;

import std;
import :isa;

namespace ycxx::aarch64 {

export struct RegClass {
  std::string_view name;
  std::span<const R> regs;
};

export struct CallingConvention {
  std::array<R, 8> int_args{R::x0, R::x1, R::x2, R::x3, R::x4, R::x5, R::x6, R::x7};
  R int_result = R::x0;
  std::uint32_t max_results = 1;
  std::uint32_t stack_align = 16;
  bool natural_stack_args = true;     // Apple: stack arguments packed by their own size
  std::uint32_t reg_arg_bits = 32;    // narrower register arguments are extended to this
  std::uint32_t frame_record = 16;    // x29, x30 saved at the frame's top
  // The allocator's registers: values live in stack slots and pass through these.
  std::array<R, 4> scratch{R::x9, R::x10, R::x11, R::x12};
  R ip0 = R::x16;  // address arithmetic for far frame offsets
};

export constexpr CallingConvention apple_arm64{};

constexpr R gpr_regs[] = {R::x0,  R::x1,  R::x2,  R::x3,  R::x4,  R::x5,  R::x6,  R::x7,  R::x8,  R::x9,
                          R::x10, R::x11, R::x12, R::x13, R::x14, R::x15, R::x16, R::x17, R::x19, R::x20,
                          R::x21, R::x22, R::x23, R::x24, R::x25, R::x26, R::x27, R::x28};
constexpr R callee_saved_regs[] = {R::x19, R::x20, R::x21, R::x22, R::x23,
                                   R::x24, R::x25, R::x26, R::x27, R::x28};
constexpr R caller_saved_regs[] = {R::x0,  R::x1,  R::x2,  R::x3,  R::x4,  R::x5,  R::x6,  R::x7, R::x8,
                                   R::x9,  R::x10, R::x11, R::x12, R::x13, R::x14, R::x15, R::x16, R::x17};

// `gpr` is what an allocator may hand out (x18, x29, x30 and sp excluded); the 32-bit view of a
// class is the same registers, used through their W names by 32-bit forms.
export constexpr RegClass gpr{"gpr", gpr_regs};
export constexpr RegClass callee_saved{"callee_saved", callee_saved_regs};
export constexpr RegClass caller_saved{"caller_saved", caller_saved_regs};

}  // namespace ycxx::aarch64
