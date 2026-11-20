// [lex.string]: common encoding and the grouping of adjacent literal contents.
// EXIT: 0
static_assert(sizeof("a" u"\u00E9" u"😀" "z") == 6 * sizeof(char16_t));
static_assert(sizeof(U"\xA" "B" U"é") == 4 * sizeof(char32_t));
int main() {
  const char16_t* a = "a" u"\u00E9" u"😀" "z";
  const char32_t* b = U"\xA" "B" U"é";
  return a[1] == 0xe9 && a[2] == 0xd83d && a[3] == 0xde00 && a[4] == 'z' &&
         a[5] == 0 && b[0] == 0x0a && b[1] == 'B' && b[2] == 0xe9 && b[3] == 0 ? 0 : 1;
}
