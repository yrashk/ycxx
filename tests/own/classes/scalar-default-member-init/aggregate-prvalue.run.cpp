// [class.mem.general], [dcl.init.aggr]: defaults see the complete class in every destination.
constexpr int seed = 5;

struct Record {
  int value = seed;
  static constexpr int seed = 42;
};

constexpr Record folded{};

constexpr int constant_call() { return 13; }
struct FoldedCall { int value = constant_call(); };
constexpr FoldedCall constant_context{};

Record prvalue_return() { return Record{}; }
Record braced_return() { return {}; }
int by_value(Record record) { return record.value; }

int main() {
  auto inferred = Record{};
  Record named{};
  return Record{}.value != 42 || Record().value != 42 || inferred.value != 42 ||
         named.value != 42 || folded.value != 42 || prvalue_return().value != 42 ||
         braced_return().value != 42 || by_value({}) != 42 || by_value({9}) != 9 ||
         constant_context.value != 13 || FoldedCall{}.value != 13;
}
