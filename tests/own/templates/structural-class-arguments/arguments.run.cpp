// #317: class arguments identify a specialization by type and field values
// ([temp.arg.nontype], [temp.type]).
// EXIT: 0
struct Offset {
  int x;
  int y;
};

using OffsetAlias = Offset;
constexpr Offset named{3, -2};

template<Offset Value>
int sum() {
  return Value.x + Value.y;
}

int main() {
  if (sum<Offset{1, -1}>() != 0) return 1;

  int (*from_name)() = &sum<named>;
  int (*from_braces)() = &sum<OffsetAlias{3, -2}>;
  int (*other)() = &sum<Offset{4, -3}>;
  if (from_name != from_braces || from_name == other) return 2;
  return from_name() == 1 && other() == 1 ? 0 : 3;
}
