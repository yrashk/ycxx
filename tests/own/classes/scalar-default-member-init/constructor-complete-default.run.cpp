// [class.mem.general]: the written default is a complete-class context.
constexpr int seed = 5;

struct Record {
  int value = seed;
  static constexpr int seed = 42;

  Record() noexcept;
  explicit Record(int replacement) noexcept : value(replacement) {}
};

Record::Record() noexcept {}

struct Aggregate {
  int value = seed;
  static constexpr int seed = 42;
};

int main() {
  Record defaults;
  Record explicit_value(17);
  return defaults.value != 42 || explicit_value.value != 17 || Aggregate{}.value != 42;
}
