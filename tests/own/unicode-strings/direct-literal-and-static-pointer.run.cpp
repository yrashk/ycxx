// [lex.string], [expr.const]: constant reads and interior pointers to literal objects.
// EXIT: 0
static_assert(u"a\U0001F600z"[1] == 0xd83d);
static_assert(u"a\U0001F600z"[2] == 0xde00);
static_assert(u"a\U0001F600z"[4] == 0);
static_assert(U"a\U0001F600z"[1] == 0x1f600);
static_assert(U"a\U0001F600z"[3] == 0);
static constexpr const char16_t* suffix16 = &u"a\U0001F600z"[1];
static constexpr const char32_t* suffix32 = &U"a\U0001F600z"[1];
static_assert(suffix16[1] == 0xde00 && suffix32[1] == 'z');
int main() {
  return suffix16[0] == 0xd83d && suffix16[2] == 'z' &&
         suffix32[0] == 0x1f600 && suffix32[2] == 0 ? 0 : 1;
}
