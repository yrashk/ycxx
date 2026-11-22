// Check the deduced parameter of each placeholder, before substituting its declarator.
template<class T> concept Int = __is_same(T, int);
template<class T> concept IntRef = __is_same(T, int&);

int main() {
  int value = 17;
  const Int auto& readonly = value;
  IntRef auto&& alias = value;
  static_assert(__is_same(decltype(readonly), const int&));
  static_assert(__is_same(decltype(alias), int&));
  alias = 42;
  return readonly == 42 ? 0 : 1;
}
