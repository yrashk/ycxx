// #1006, [dcl.constexpr], [expr.const.imm]
struct Empty {
  constexpr unsigned value() const { return 7; }
};

consteval Empty unused_empty() { return {}; }

consteval Empty demanded_empty() {
  Empty result{};
  return result;
}

static_assert(demanded_empty().value() == 7);

constexpr int runtime_step(int input) { return input + 7; }

int main(int argc, char**) { return runtime_step(argc); }
