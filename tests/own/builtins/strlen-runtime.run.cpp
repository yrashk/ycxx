using Size = decltype(sizeof(0));

int main(int argc, char** argv) {
  char text[] = "xabc";
  text[2] = argc > 1 ? argv[1][0] : '\0';

  const Size start = argc < 3 ? 1 : 2;
  const Size stop = text[2] == '\0' ? 2 : 4;
  Size offset = start;
  const Size length = __builtin_strlen(text + offset++);
  return length == stop - start && offset == start + 1 ? 0 : 1;
}
