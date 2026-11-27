int value = 17;
int* f() {
  constexpr int& ref = value;
  return [] { int& alias = ref; return &alias; }();
}
