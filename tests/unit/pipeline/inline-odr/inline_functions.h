#ifndef YCXX_INLINE_ODR_422_FUNCTIONS_H
#define YCXX_INLINE_ODR_422_FUNCTIONS_H

inline int inline_first(int value);
int inline_first(int value) { return value + 11; }

int inline_later(int value);
inline int inline_later(int value) { return value + 17; }

struct Operation {
    static int member(int value) { return value + 23; }
};

struct DeclaredMember {
    inline static int member(int value);
};

int DeclaredMember::member(int value) { return value + 31; }

constexpr int implicit_inline(int value) { return value + 29; }

using Function = int (*)(int);

Function main_first_address();
Function main_later_address();
Function main_member_address();
Function main_constexpr_address();
int main_direct_calls(int value);

Function peer_first_address();
Function peer_later_address();
Function peer_member_address();
Function peer_constexpr_address();
int peer_direct_calls(int value);

#endif
