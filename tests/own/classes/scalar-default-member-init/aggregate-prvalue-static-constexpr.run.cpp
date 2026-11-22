// [class.static.data], [dcl.constexpr]: these initializers require constant evaluation.
constexpr int compute() { return 9; }
struct Record { int value = compute(); };

struct Owner {
  static constexpr Record copied = Record{};
  static constexpr Record braced{};
  static constexpr auto inferred = Record{};
};

int main() {
  return Owner::copied.value != 9 || Owner::braced.value != 9 || Owner::inferred.value != 9;
}
