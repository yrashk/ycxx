// [lex.string]: UTF-16 and UTF-32 array types, code units, and terminators.
// EXIT: 0
using U16 = const char16_t (&)[5];
using U32 = const char32_t (&)[4];
static_assert(__is_same(decltype(u"aé😀"), U16));
static_assert(__is_same(decltype(U"aé😀"), U32));
static_assert(sizeof(u"") == sizeof(char16_t));
static_assert(sizeof(U"") == sizeof(char32_t));
int main() {
  const char16_t* a = u"aé😀";
  const char32_t* b = U"a\u00E9\U0001F600";
  return a[0] == 0x61 && a[1] == 0xe9 && a[2] == 0xd83d && a[3] == 0xde00 &&
         a[4] == 0 && b[0] == 0x61 && b[1] == 0xe9 && b[2] == 0x1f600 && b[3] == 0 ? 0 : 1;
}
