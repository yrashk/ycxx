// [dcl.init.string]: direct and braced character array initialization and zero padding.
// EXIT: 0
static constexpr char16_t text[] = u"é😀";
static constexpr char32_t letters[6]{U"xé"};
static_assert(sizeof(text) / sizeof(char16_t) == 4);
int main() {
  char16_t a[6] = u"é😀";
  char32_t b[] = {U"xé"};
  a[0] = text[1];
  return a[0] == 0xd83d && a[1] == 0xd83d && a[2] == 0xde00 && a[3] == 0 &&
         a[5] == 0 && letters[2] == 0 && letters[5] == 0 && b[0] == 'x' && b[1] == 0xe9 && b[2] == 0 ? 0 : 1;
}
