extern "C" int pointer_variable();
extern "C" int typed_callback(int (*)(int), int);
extern "C" int pointer_default();
extern "C" int reference_callback(int& (*)(int), int);
extern "C" int variadic_callback(int (*)(int, ...), int);
extern "C" int guarded_callback(int (*)(int) noexcept, int);

int seven() { return 7; }
int throw_seven() { throw 7; }
int succeed(int value) noexcept { return value * 3 + 1; }
int fail(int value) { throw value; }
int& reference(int value) { static int slot; slot = value; return slot; }
int variadic(int value, ...) { return value - 1; }

int (*exception_source)() = seven;
int destroyed;

void note(int value) noexcept { destroyed += value; }

int main() {
  if (pointer_variable() != 7 || typed_callback(succeed, 5) != 16 || pointer_default() != 7 ||
      reference_callback(reference, 6) != 6 || variadic_callback(variadic, 8) != 7 ||
      guarded_callback(succeed, 10) != 31 || destroyed != 10) return 1;

  exception_source = throw_seven;
  try {
    pointer_variable();
    return 2;
  } catch (int caught) {
    if (caught != 7) return 3;
  }
  try {
    typed_callback(fail, 11);
    return 4;
  } catch (int caught) {
    if (caught != 11) return 5;
  }
  try {
    pointer_default();
    return 6;
  } catch (int caught) {
    return caught != 7;
  }
}
