// [class.ctor.general], [dcl.constexpr]: constexpr constructors and a function pointer.
struct Empty {
  constexpr Empty() noexcept {}
  static constexpr Empty (*factory)() = nullptr;
};

struct Separate {
  int member;
  constexpr Separate(int input);
};

constexpr Separate::Separate(int input) : member(input) {}

void construct(int input) {
  Empty empty;
  Separate separate(input);
}
