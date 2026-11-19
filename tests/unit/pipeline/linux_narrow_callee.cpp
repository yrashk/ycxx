#include "linux_narrow_abi.hpp"

extern "C" void observe_arguments(
  S8 register_s8, U8 register_u8, S16 register_s16, U16 register_u16,
  Tag register_tag, U32 register_u32,
  S8 stack_s8, U8 stack_u8, S16 stack_s16, U16 stack_u16, Tag stack_tag,
  S32 stack_s32, S64 stack_s64, U64 stack_u64, ObservedArguments* observed) {
  observed->register_s8 = register_s8;
  observed->register_u8 = register_u8;
  observed->register_s16 = register_s16;
  observed->register_u16 = register_u16;
  observed->register_tag = static_cast<U64>(register_tag);
  observed->register_u32 = register_u32;
  observed->stack_s8 = stack_s8;
  observed->stack_u8 = stack_u8;
  observed->stack_s16 = stack_s16;
  observed->stack_u16 = stack_u16;
  observed->stack_tag = static_cast<U64>(stack_tag);
  observed->stack_s32 = stack_s32;
  observed->stack_s64 = stack_s64;
  observed->stack_u64 = stack_u64;
}

extern "C" S8 return_s8(U64 carrier) { return static_cast<S8>(carrier); }
extern "C" U8 return_u8(U64 carrier) { return static_cast<U8>(carrier); }
extern "C" S16 return_s16(U64 carrier) { return static_cast<S16>(carrier); }
extern "C" U16 return_u16(U64 carrier) { return static_cast<U16>(carrier); }
extern "C" Tag return_tag(U64 carrier) { return static_cast<Tag>(carrier); }
extern "C" U32 return_u32(U64 carrier) { return static_cast<U32>(carrier); }
extern "C" S64 return_s64(S64 value) { return value; }

extern "C" int check_returns(S64 s8, U64 u8, S64 s16, U64 u16, U64 tag, U64 u32, S64 s64) {
  if (s8 != -111) return 21;
  if (u8 != 0xe7) return 22;
  if (s16 != -25924) return 23;
  if (u16 != 0xdef1) return 24;
  if (tag != 0xf2b3) return 25;
  if (u32 != 0x89abcdefULL) return 26;
  if (s64 != -0x1234567889abcdeLL) return 27;
  return 0;
}
