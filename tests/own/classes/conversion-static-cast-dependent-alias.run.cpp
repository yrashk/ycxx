// [expr.static.cast], [over.match.conv], [class.conv.fct]
template<class T> using Result = T;

template<class T> struct Number {
  T value;
  operator Result<T>() const { return value; }
};

int main() {
  const Number<int> first{7};
  const Number<long> second{11};
  return static_cast<int>(first) != 7 || static_cast<long>(second) != 11;
}
