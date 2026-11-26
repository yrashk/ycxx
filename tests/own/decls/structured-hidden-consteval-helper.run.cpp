// REQUIRES: run linux
// PARITY: by design: .hidden sets ELF symbol visibility
// [dcl.asm]/1, [stmt.if]/5, [expr.const.core]
namespace {
struct AssemblyView {
  const char* text;
  unsigned count;

  constexpr const char* data() const noexcept { return text; }
  constexpr unsigned size() const noexcept { return count; }
};

class Table {
  static inline constexpr int dormant_table_entry(int seed) {
    int length = 2 + (seed & 3);
    int* entries = new int[length];
    entries[length - 1] = seed * 2;
    int result = entries[length - 1];
    delete[] entries;
    return result;
  }

public:
  static constexpr int value(int seed) {
    if consteval {
      return dormant_table_entry(seed);
    } else {
      return seed + 5;
    }
  }
};
}

asm((AssemblyView{".hidden separate_target", sizeof(".hidden separate_target") - 1}));

extern "C" int separate_target(int input) {
  return input + 19;
}

int runtime_value(int input) {
  return Table::value(input);
}

int main(int argc, char**) {
  int input = argc > 1 ? 6 : 3;
  int ordinary = runtime_value(input);
  int hidden = separate_target(input);
  if (ordinary != input + 5) return 1;
  if (hidden != input + 19) return 2;
  return 0;
}
