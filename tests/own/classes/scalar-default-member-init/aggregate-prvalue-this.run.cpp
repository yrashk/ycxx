int evaluations;
int transform(int value) noexcept { ++evaluations; return value + 1; }

struct Record {
  int first;
  int value = transform(first);
};

struct Outer { Record record; };
int by_value(Record record) { return record.value; }

struct MemberCall {
  int first;
  int transformed() const noexcept { return first + 2; }
  int value = transformed();
};

static_assert(noexcept(MemberCall{40}));

int main() {
  if (Record{41}.value != 42 || evaluations != 1) return 1;
  if (by_value({5}) != 6 || evaluations != 2) return 2;
  if (Outer{{7}}.record.value != 8 || evaluations != 3) return 3;
  if (by_value({0, 9}) != 9 || evaluations != 3) return 4;
  return MemberCall{40}.value != 42;
}
