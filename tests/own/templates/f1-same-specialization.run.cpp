// ζ f1 (#88): the same specialization named several times: by deduction from int lvalues,
// rvalues and const ints, by an explicit argument, from main and from another template, and
// for a non-type parameter by two constant expressions with the same value. Every use calls one
// function, which counts its calls through a reference ([temp.inst], [temp.type],
// [temp.deduct.call]).
// EXIT: 0
int calls = 0;

template<class T>
T add(T a, T b, int& counter) {
  counter = counter + 1;
  return a + b;
}

template<class T>
T add3(T a, T b, T c, int& counter) { return add(add(a, b, counter), c, counter); }

template<int N>
int offset(int x) { return x + N; }

int main() {
  int i = 1;
  const int ci = 2;
  if (add(i, ci, calls) != 3) return 1;
  if (add(3, 4, calls) != 7) return 2;
  if (add<int>(5, 6, calls) != 11) return 3;
  if (add3(1, 2, 3, calls) != 6) return 4;
  if (calls != 5) return 5;
  if (add('\1', '\2', calls) != 3) return 6;
  if (add(1L, 2L, calls) != 3L) return 7;
  if (calls != 7) return 8;
  if (offset<4>(1) != 5) return 9;
  if (offset<2 + 2>(2) != 6) return 10;
  if (offset<4>(3) != 7) return 11;
  return 0;
}
