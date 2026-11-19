// Register classes and the calling convention, as data (optimization design §5; #28): AAPCS64
// (Arm IHI 0055, §6) with Apple's arm64 deviations ("Writing ARM64 code for Apple platforms"):
//   - integer arguments go in x0–x7, then on the stack; an integer result comes back in x0;
//   - floating arguments go in v0–v7 (their S or D view), counted apart from the integer ones,
//     then on the stack; a floating result comes back in v0, and a homogeneous floating-point
//     aggregate's members in v0–v3, one each (`lower` splits it into its members; AAPCS64 C.2,
//     §6.9). v8–v15 are callee-saved (their low 64 bits), v16–v31 caller-saved temporaries;
//   - x9–x15 are caller-saved temporaries, x16/x17 the intra-procedure-call scratch registers,
//     x18 is reserved by the platform, x19–x28 are callee-saved, x29 is the frame pointer and x30
//     the link register;
//   - the stack pointer stays 16-byte aligned, and every function keeps a frame record (x29, x30):
//     x29 points at this function's record, which holds the caller's x29;
//   - the stack grows a page at a time: a frame larger than a page touches each page as it goes
//     down, so it cannot step over the guard page;
//   - Apple: arguments on the stack take their natural size and alignment, not 8-byte slots;
//   - Apple: the caller extends an integer argument narrower than 32 bits to 32 bits when it goes
//     in a register (on the stack it keeps its size). A narrow result is returned extended to 32
//     bits (`lower`, LowerConfig::min_result_bits), and the caller reads only its own bits, so
//     either side's rule for results is met.
//   - Apple: every argument a callee's ellipsis matches goes on the stack, after the named stack
//     arguments, each in an 8-byte slot of its own, 8-byte aligned; named arguments are placed as
//     in any call. (AAPCS64 places them as named ones.) Their default argument promotions
//     ([expr.call]/12) happened before, so a floating one is a double; the caller extends an
//     integer to its slot by its signedness.
// Defining a variadic function (va_start) is not in the α subset.
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
  // Results in x0 and x1, a trivially copyable class of up to 16 bytes taking both (AAPCS64
  // §6.9); a larger or non-trivial one goes to memory whose address the caller passes in x8.
  std::array<R, 2> int_results{R::x0, R::x1};
  std::uint32_t max_results = 2;
  std::array<R, 8> fp_args{R::v0, R::v1, R::v2, R::v3, R::v4, R::v5, R::v6, R::v7};
  std::array<R, 4> fp_results{R::v0, R::v1, R::v2, R::v3};
  R indirect_result = R::x8;
  std::uint32_t stack_align = 16;
  bool natural_stack_args = true;     // Apple: stack arguments packed by their own size
  std::uint32_t reg_arg_bits = 32;    // narrower register arguments are extended to this
  // Apple: the arguments an ellipsis matches go on the stack in slots of this many bytes; 0
  // places them as named arguments (AAPCS64).
  std::uint32_t variadic_slot = 8;
  std::uint32_t frame_record = 16;    // x29, x30 saved at the frame's top
  // The allocator's registers: values live in stack slots and pass through these; floating ones
  // through `fp_scratch`.
  std::array<R, 4> scratch{R::x9, R::x10, R::x11, R::x12};
  std::array<R, 4> fp_scratch{R::v16, R::v17, R::v18, R::v19};
  R ip0 = R::x16;  // address arithmetic for far frame offsets; a floating constant's bits
  std::uint32_t page = 16384;  // stack probing step (the arm64 macOS page size)
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
