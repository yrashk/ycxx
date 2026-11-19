#include "linux_narrow_abi.hpp"

int main() {
  constexpr S64 sentinel = 0x1122334455667788LL;
  ObservedArguments observed{
      sentinel, sentinel, sentinel, sentinel, sentinel, sentinel,
      sentinel, sentinel, sentinel, sentinel, sentinel, sentinel, sentinel, sentinel,
  };
  observe_arguments(-103, 0xe9, -23457, 0xd9a7, Tag::register_value, 0x81234567U,
                    -49, 0xb5, -17321, 0x93d6, Tag::stack_value, -0x71234567,
                    -0x123456789abcdefLL, 0xfedcba9876543210ULL, &observed);

  if (observed.register_s8 != -103) return 1;
  if (observed.register_u8 != 0xe9) return 2;
  if (observed.register_s16 != -23457) return 3;
  if (observed.register_u16 != 0xd9a7) return 4;
  if (observed.register_tag != 0xc123) return 5;
  if (observed.register_u32 != 0x81234567ULL) return 6;
  if (observed.stack_s8 != -49) return 7;
  if (observed.stack_u8 != 0xb5) return 8;
  if (observed.stack_s16 != -17321) return 9;
  if (observed.stack_u16 != 0x93d6) return 10;
  if (observed.stack_tag != 0xa567) return 11;
  if (observed.stack_s32 != -0x71234567LL) return 12;
  if (observed.stack_s64 != -0x123456789abcdefLL) return 13;
  if (observed.stack_u64 != 0xfedcba9876543210ULL) return 14;

  const S64 s8 = return_s8(0x9070605040302091ULL);
  const U64 u8 = return_u8(0x10203040506070e7ULL);
  const S64 s16 = return_s16(0x8172635445369abcULL);
  const U64 u16 = return_u16(0xf1e2d3c4b5a6def1ULL);
  const U64 tag = static_cast<U64>(return_tag(0x753159263748f2b3ULL));
  const U64 u32 = return_u32(0x1020304089abcdefULL);
  const S64 s64 = return_s64(-0x1234567889abcdeLL);
  return check_returns(s8, u8, s16, u16, tag, u32, s64);
}
