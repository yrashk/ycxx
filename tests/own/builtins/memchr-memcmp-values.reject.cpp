// The bytes __builtin_memchr and __builtin_memcmp read in a constant expression must be usable in
// constant expressions ([expr.const]). Indeterminate bytes are in memchr-memcmp-indeterminate.
char mutable_text[] = "abc";
static_assert(__builtin_memchr(mutable_text, 'b', 3) != nullptr); //! error
static_assert(__builtin_memcmp(mutable_text, "abc", 3) == 0); //! error
