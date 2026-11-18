// [cpp.cond]/11: #if expressions are integral constant expressions evaluated in intmax_t (or
// uintmax_t for unsigned operands), so the operators follow [expr]: precedence,
// associativity, division truncating toward zero, %, shifts, bitwise, logical, comparison,
// conditional and comma operators; values beyond 32 bits are not truncated.
#if 1 + 2 * 3 == 7 && (1 + 2) * 3 == 9
precedence;
#endif
#if 10 - 4 - 3 == 3
left_assoc;
#endif
#if -7 / 2 == -3 && -7 % 2 == -1 && 7 / -2 == -3
truncation;
#endif
#if (1 << 40) == 1099511627776 && 1099511627776 >> 39 == 2
wide_shift;
#endif
#if 4294967296 * 2 == 8589934592
beyond_32_bits;
#endif
#if 9223372036854775807 > 0 && -9223372036854775807 - 1 < 0
intmax_range;
#endif
#if (0x0F & 0x3C) == 0x0C && (0x0F | 0x30) == 0x3F && (0x0F ^ 0xFF) == 0xF0
bitwise;
#endif
#if ~0 == -1 && -(-3) == 3 && +4 == 4 && !0 == 1 && !5 == 0
unary;
#endif
#if 1 < 2 && 2 <= 2 && 3 > 2 && 3 >= 3 && 1 != 2 && (1 < 2) == 1
comparisons;
#endif
#if (1 ? 2 : 3) == 2 && (0 ? 2 : 3) == 3 && (0 ? 1 : 0 ? 2 : 4) == 4
conditional;
#endif
#if 010 == 8 && 0x10 == 16 && 0b101 == 5 && 1'000'000 == 1000000
literal_forms;
#endif
#if 10L == 10 && 10LL == 10 && 10u == 10 && 10ULL == 10
suffixes;
#endif
end;
