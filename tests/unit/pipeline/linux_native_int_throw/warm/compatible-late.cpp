extern "C" int edited_throw(int value) {
  if (value == 0) return 62;
  throw value;
}
extern "C" int edited_throw(int value) noexcept(false);
