// [expr.type], [expr.cond], [basic.type.qualifier]
int main() {
  const int arr[2] = {20, 22};
  const int (*p)[2] = &arr;
  const void* erased = &arr;
  void* empty = nullptr;
  static_assert(__is_same(decltype(true ? p : empty), const void*));
  return p == erased ? 0 : 1;
}
