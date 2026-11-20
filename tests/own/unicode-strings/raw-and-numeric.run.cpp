// [lex.string]: raw characters and numeric escapes that each contribute one code unit.
// EXIT: 0
int main() {
  const char16_t* raw = uR"==(é😀\n)==";
  const char32_t* high = U"\xFFFFFFFF\0\U0010FFFF";
  const char16_t* units = u"\xD800\xFFFF\0z";
  return raw[0] == 0xe9 && raw[1] == 0xd83d && raw[2] == 0xde00 && raw[3] == '\\' &&
         raw[4] == 'n' && raw[5] == 0 && high[0] == 0xffffffffu && high[1] == 0 &&
         high[2] == 0x10ffff && high[3] == 0 && units[0] == 0xd800 && units[1] == 0xffff &&
         units[2] == 0 && units[3] == 'z' && units[4] == 0 ? 0 : 1;
}
