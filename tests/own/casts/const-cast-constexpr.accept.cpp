// #1424, [expr.const.cast], [expr.const.core]: a cv-qualified view can alias a mutable object.
constexpr bool aliasing() {
  int value = 1;
  const volatile int* pointer_view = &value;
  int* pointer = const_cast<int*>(pointer_view);
  *pointer = 3;
  const volatile int& reference_view = value;
  int& reference = const_cast<int&>(reference_view);
  reference = 9;
  return pointer == &value && &reference == &value && value == 9;
}

static_assert(aliasing());

volatile int external_view = 1;
constexpr int* external_address = const_cast<int*>(&external_view);
constexpr int& external_reference = const_cast<int&>(external_view);
static_assert(external_address == &external_reference);

constexpr bool local_address() {
  volatile int value = 2;
  return &const_cast<int&>(value) == &value;
}
static_assert(local_address());
