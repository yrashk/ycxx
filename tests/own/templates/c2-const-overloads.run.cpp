// ζ c2 (#88): member functions of a class template overloaded on the object's const
// qualification: at(i) returns T& on a modifiable object and const T& on a const one, chosen by
// the implicit object parameter; also an overload on the parameter's type T versus char, which
// for Box<char> would declare one signature twice, so only Box<long> is used
// ([temp.class], [over.match.funcs], [class.mfct.non.static], [over.ics.rank]).
// EXIT: 0
template<class T, int N>
struct Arr {
  T data[N];
  T& at(int i) { return data[i]; }
  const T& at(int i) const { return data[i]; }
  int which() { return 1; }
  int which() const { return 2; }
};

template<class T>
struct Box {
  T v;
  int put(T) { return 1; }
  int put(char) { return 2; }
};

int read_const(const Arr<int, 2>& a) { return a.at(1); }

int main() {
  Arr<int, 2> a{{1, 2}};
  a.at(0) = 5;
  if (a.at(0) != 5) return 1;
  if (read_const(a) != 2) return 2;
  if (a.which() != 1) return 3;
  const Arr<int, 2>& ca = a;
  if (ca.which() != 2) return 4;
  if (&ca.at(1) != &a.data[1]) return 5;
  Box<long> b{0L};
  if (b.put(3L) != 1) return 6;
  if (b.put('c') != 2) return 7;
  return 0;
}
