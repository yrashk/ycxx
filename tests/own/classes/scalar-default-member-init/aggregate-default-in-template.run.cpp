// [class.mem.general], [temp.res]: a non-dependent default keeps its completed-class lookup.
constexpr int seed = 5;

struct Record {
  int value = seed;
  static constexpr int seed = 42;
};

template<int Offset> int read_constants() {
  Record named{};
  return named.value + Record{}.value + Offset;
}

int next(int value) noexcept { return value + 1; }
struct Receiver {
  int first;
  int value = next(first);
};

template<int Input> int read_receiver() {
  Receiver named{Input};
  return named.value * 100 + Receiver{Input + 1}.value;
}

constexpr int constant_seed() { return 42; }
struct ConstantRecord { int value = constant_seed(); };

template<int Offset> int read_constexpr() {
  constexpr ConstantRecord named{};
  return named.value + Offset;
}

int main() {
  return read_constants<2>() != 86 || read_constants<3>() != 87 || read_receiver<5>() != 607 ||
         read_constexpr<2>() != 44;
}
