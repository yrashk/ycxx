// A lambda can capture a local after the constraint is checked and its type is deduced.
template<class T> concept Int = __is_same(T, int);
int main() {
  Int auto value = 17;
  auto read = [&value] { return value; };
  return read() - 17;
}
