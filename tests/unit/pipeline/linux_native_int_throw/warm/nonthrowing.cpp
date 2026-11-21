extern "C" int edited_throw(int value) noexcept {
  if (value == 0) return 62;
  throw value;
}
extern "C" int edited_throw(int value) noexcept;
