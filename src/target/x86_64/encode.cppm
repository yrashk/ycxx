// The integer forms used by the first x86-64 emitter (AMD64 Programmer's Manual, vol. 3,
// Chapter 1 and the instruction reference). Memory operands use ModR/M with disp32.
module ycxx.target.x86_64:encode;

import std;

namespace ycxx::x86_64 {

enum class Reg : std::uint8_t { rax, rcx, rdx, rsp = 4, rbp, rsi, rdi, r8, r9, r11 = 11 };

class Encoder {
 public:
  explicit Encoder(std::vector<std::uint8_t>& code) : code_(code) {}

  std::size_t offset() const { return code_.size(); }

  void emit(std::initializer_list<std::uint8_t> bytes) {
    code_.insert(code_.end(), bytes.begin(), bytes.end());
  }

  void immediate(std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) code_.push_back(std::uint8_t(value >> (8 * i)));
  }

  // `reg` supplies the ModR/M reg field. Opcode extensions (/n) use this field too.
  void reg(std::initializer_list<std::uint8_t> opcode, unsigned reg, Reg rm, bool wide = true) {
    unsigned r = number(rm);
    rex(wide, reg, r);
    emit(opcode);
    emit({std::uint8_t(0xc0 | ((reg & 7) << 3) | (r & 7))});
  }

  void reg(std::initializer_list<std::uint8_t> opcode, Reg reg, Reg rm, bool wide = true) {
    this->reg(opcode, number(reg), rm, wide);
  }

  void move(Reg dst, Reg src) { reg({0x89}, src, dst); }

  // DIV and IDIV take the dividend in EDX:EAX or RDX:RAX. They put the quotient in EAX
  // or RAX and the remainder in EDX or RDX (AMD64 Programmer's Manual, vol. 3).
  void divide(Reg divisor, bool is_signed, bool wide) {
    if (is_signed) {
      rex(wide, 0, 0);
      emit({0x99});  // cdq or cqo
    } else {
      reg({0x31}, Reg::rdx, Reg::rdx, false);  // xor edx, edx also clears the high 32 bits of RDX
    }
    reg({0xf7}, is_signed ? 7 : 6, divisor, wide);  // idiv or div
  }

  void load(Reg dst, Reg base, std::int32_t disp, unsigned width, bool sign_extend = false) {
    // Narrow loads extend into a 32-bit register, which also clears its high 32 bits.
    switch (width) {
      case 1: memory({0x0f, std::uint8_t(sign_extend ? 0xbe : 0xb6)}, dst, base, disp, false); break;
      case 2: memory({0x0f, std::uint8_t(sign_extend ? 0xbf : 0xb7)}, dst, base, disp, false); break;
      default: memory({0x8b}, dst, base, disp, width == 8); break;
    }
  }

  void store(Reg base, std::int32_t disp, Reg src, unsigned width) {
    // Byte stores in this emitter use AL. Register inputs are moved to RAX before narrowing.
    if (width == 2) emit({0x66});
    memory({std::uint8_t(width == 1 ? 0x88 : 0x89)}, src, base, disp, width == 8);
  }

  void store_immediate32(Reg base, std::int32_t disp, std::uint32_t value) {
    memory({0xc7}, 0, base, disp, false);
    immediate(value, 4);
  }

  // MOVSS or MOVSD transfers only a scalar's low 32 or 64 bits at the ABI boundary.
  void load_scalar_xmm(unsigned xmm, Reg base, std::int32_t disp, unsigned width) {
    emit({std::uint8_t(width == 4 ? 0xf3 : 0xf2)});
    memory({0x0f, 0x10}, xmm, base, disp, false);
  }

  void store_scalar_xmm(Reg base, std::int32_t disp, unsigned xmm, unsigned width) {
    emit({std::uint8_t(width == 4 ? 0xf3 : 0xf2)});
    memory({0x0f, 0x11}, xmm, base, disp, false);
  }

  // MOVAPS to a 16-byte-aligned variadic register-save slot.
  void store_xmm(Reg base, std::int32_t disp, unsigned xmm) { memory({0x0f, 0x29}, xmm, base, disp, false); }

  void address(Reg dst, Reg base, std::int32_t disp) { memory({0x8d}, dst, base, disp, true); }

  std::size_t displacement() {
    auto at = offset();
    immediate(0, 4);
    return at;
  }

  bool patch_relative(std::size_t at, std::size_t target) {
    auto disp = std::int64_t(target) - std::int64_t(at + 4);
    if (!std::in_range<std::int32_t>(disp)) return false;
    auto bits = std::uint32_t(disp);
    for (unsigned i = 0; i < 4; ++i) code_[at + i] = std::uint8_t(bits >> (8 * i));
    return true;
  }

 private:
  std::vector<std::uint8_t>& code_;

  static unsigned number(Reg reg) { return std::to_underlying(reg); }

  void rex(bool wide, unsigned reg, unsigned rm) {
    unsigned prefix = 0x40 | (wide ? 8 : 0) | ((reg >> 3) << 2) | (rm >> 3);
    if (prefix != 0x40) emit({std::uint8_t(prefix)});
  }

  void memory(std::initializer_list<std::uint8_t> opcode, Reg reg, Reg base, std::int32_t disp, bool wide) {
    memory(opcode, number(reg), base, disp, wide);
  }

  void memory(std::initializer_list<std::uint8_t> opcode, unsigned r, Reg base, std::int32_t disp, bool wide) {
    unsigned b = number(base);
    rex(wide, r, b);
    emit(opcode);
    emit({std::uint8_t(0x80 | ((r & 7) << 3) | (b & 7))});
    if ((b & 7) == 4) emit({0x24});  // SIB: no index, base RSP
    immediate(std::uint32_t(disp), 4);
  }
};

}  // namespace ycxx::x86_64
