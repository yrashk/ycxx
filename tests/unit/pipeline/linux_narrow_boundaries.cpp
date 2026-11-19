enum class SignedOctet : signed char {};
using Peer = int(SignedOctet, unsigned short, long, long, short, unsigned char);

extern "C" int narrow_registers(SignedOctet, unsigned short, long, long, short, unsigned char);
extern "C" short narrow_dirty_s16();
extern "C" unsigned char narrow_dirty_u8();

extern "C" SignedOctet narrow_return(SignedOctet value) { return value; }

extern "C" int narrow_boundaries(SignedOctet s8, unsigned short u16, long, long, short s16,
                                 unsigned char u8, short stack_s16, unsigned char stack_u8, Peer* peer) {
  if (static_cast<int>(s8) != -127 || u16 != 0xd9a7 || s16 != -23457 || u8 != 0xe9) return 1;
  if (stack_s16 != -32767 || stack_u8 != 0xe7) return 2;
  if (narrow_dirty_s16() != -32767 || narrow_dirty_u8() != 0xe7) return 3;
  if (narrow_registers(s8, u16, 0, 0, stack_s16, stack_u8) != 0) return 4;
  return peer(s8, u16, 0, 0, stack_s16, stack_u8);
}
