static int observed = 1;
decltype(auto) by_reference() { return (observed); }
auto by_copy() { return observed; }
auto pointer() { return &observed; }
auto narrow(bool first) { return first ? static_cast<short>(11) : static_cast<short>(-1); }
static_assert(__is_same(decltype(by_reference()), int&));
static_assert(__is_same(decltype(by_copy()), int));
static_assert(__is_same(decltype(pointer()), int*));
static_assert(__is_same(decltype(narrow(true)), short));
int main() {
  int copy = by_copy();
  by_reference() = 42;
  return copy == 1 && pointer() == &observed && *pointer() == 42 &&
         &by_reference() == pointer() && narrow(true) == 11 && narrow(false) == -1 ? 0 : 1;
}
