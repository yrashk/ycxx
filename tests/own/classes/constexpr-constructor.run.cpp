// [class.ctor.general], [dcl.constexpr]: runtime construction with a constexpr specifier.
struct Inline {
  int value;

  constexpr Inline(int input) noexcept : value(input + 2) {
    value += 3;
  }
};

struct Separate {
  int value;
  constexpr Separate(int input);
};

constexpr Separate::Separate(int input) : value(input * 3) {}

int main(int argc, char**) {
  const int input = argc % 7;
  Inline first(input);
  Separate second(input);
  return first.value == input + 5 && second.value == input * 3 ? 0 : 1;
}
