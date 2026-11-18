// [cpp.cond]/11, [expr.arith.conv]: an unsigned operand makes the operation uintmax_t, so -1
// converts to the largest unsigned value; a signed-only expression stays signed.
#if -1 < 0
signed_negative;
#endif
#if -1 > 0u
minus_one_unsigned_is_large;
#endif
#if -1 == 18446744073709551615u
minus_one_is_max;
#endif
#if (0u - 1) / 2 == 9223372036854775807
unsigned_wraps;
#endif
#if (2u > -1) == 0
two_less_than_max;
#endif
#if (-1 ? 1u : 0) && (0 ? -1 : 0u) - 1 > 0
conditional_unsigned;
#endif
end;
