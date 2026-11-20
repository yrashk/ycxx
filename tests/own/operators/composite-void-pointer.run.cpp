// [expr.type], [expr.eq], [expr.rel], [expr.cond], [conv.ptr]
int main() {
  int values[3] = {10, 20, 30};
  const int* first = values;
  volatile void* last = values + 2;
  const void* same = values;
  if (!(first == same && same == first && first != last && last != first)) return 1;
  if (!(first < last && last > first && first <= last && last >= first)) return 2;

  bool pick_first = values[0] == 10;
  static_assert(__is_same(decltype(pick_first ? first : last), const volatile void*));
  static_assert(__is_same(decltype(pick_first ? last : first), const volatile void*));
  auto selected = pick_first ? first : last;
  auto reverse = pick_first ? last : first;
  if (selected != same || reverse != last) return 3;

  int* mutable_first = values;
  void* erased = mutable_first;
  if (static_cast<int*>(erased) != mutable_first || mutable_first == nullptr) return 4;
  return 0;
}
