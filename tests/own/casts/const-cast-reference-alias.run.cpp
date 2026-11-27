// #1424, [expr.const.cast]: select the requested glvalue category without a temporary.
template<class T>
T& writable(const T& view) {
  return const_cast<T&>(view);
}

int value = 3;
int calls = 0;

const int& observe() {
  ++calls;
  return value;
}

int category(int&) { return 1; }
int category(int&&) { return 2; }

static_assert(__is_same(decltype(const_cast<int&>(observe())), int&));
static_assert(__is_same(decltype(const_cast<int&&>(observe())), int&&));
static_assert(__is_same(decltype(const_cast<const volatile int&>(value)), const volatile int&));

int main() {
  int& lvalue = writable(observe());
  int&& xvalue = const_cast<int&&>(observe());
  int&& from_xvalue = const_cast<int&&>(static_cast<const int&&>(observe()));
  if (calls != 3 || &lvalue != &value || &xvalue != &value || &from_xvalue != &value)
    return 1;
  lvalue = 7;
  xvalue = 11;
  if (value != 11) return 2;
  return category(const_cast<int&>(observe())) == 1 &&
         category(const_cast<int&&>(observe())) == 2 && calls == 5 ? 0 : 3;
}
