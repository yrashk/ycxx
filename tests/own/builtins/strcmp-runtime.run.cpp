// [cstring.syn]: strcmp compares the first differing characters as unsigned char.
#if !__has_builtin(__builtin_strcmp)
#error __builtin_strcmp is unavailable
#endif

int compare(const char* first, const char* second) {
  static_assert(__is_same(decltype(__builtin_strcmp(first, second)), int));
  return __builtin_strcmp(first, second);
}

int main(int argc, char**) {
  char text[] = "a19\0x";
  char equal[] = "a19\0y";
  const char digit = argc > 1 ? '2' : '1';
  text[1] = digit;
  equal[1] = digit;

  if (compare(text, equal) != 0) return 1;  // Different bytes after the first NUL.
  if (compare("a", text) >= 0) return 2;
  if (compare(text, "a") <= 0) return 3;
  if (compare(text, "a30") >= 0) return 4;  // The first difference decides.
  if (compare("a30", text) <= 0) return 5;
  if (compare("a\xff", "a\x01") <= 0) return 6;
  if (compare("a\x01", "a\xff") >= 0) return 7;
  return 0;
}
