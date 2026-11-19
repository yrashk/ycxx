#pragma once

using S8 = signed char;
using U8 = unsigned char;
using S16 = short;
using U16 = unsigned short;
using S32 = int;
using U32 = unsigned int;
using S64 = long long;
using U64 = unsigned long long;

static_assert(sizeof(S8) == 1 && sizeof(S16) == 2);
static_assert(sizeof(S32) == 4 && sizeof(S64) == 8);

enum class Tag : U16 {
  register_value = 0xc123,
  stack_value = 0xa567,
};

struct ObservedArguments {
  S64 register_s8;
  U64 register_u8;
  S64 register_s16;
  U64 register_u16;
  U64 register_tag;
  U64 register_u32;
  S64 stack_s8;
  U64 stack_u8;
  S64 stack_s16;
  U64 stack_u16;
  U64 stack_tag;
  S64 stack_s32;
  S64 stack_s64;
  U64 stack_u64;
};

extern "C" {
void observe_arguments(
  S8 register_s8, U8 register_u8, S16 register_s16, U16 register_u16,
  Tag register_tag, U32 register_u32,
  S8 stack_s8, U8 stack_u8, S16 stack_s16, U16 stack_u16, Tag stack_tag,
  S32 stack_s32, S64 stack_s64, U64 stack_u64, ObservedArguments* observed);

S8 return_s8(U64 carrier);
U8 return_u8(U64 carrier);
S16 return_s16(U64 carrier);
U16 return_u16(U64 carrier);
Tag return_tag(U64 carrier);
U32 return_u32(U64 carrier);
S64 return_s64(S64 value);

int check_returns(S64 s8, U64 u8, S64 s16, U64 u16, U64 tag, U64 u32, S64 s64);
}
