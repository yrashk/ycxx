// ζ c2 (#88): a class template with an integral non-type parameter, used as an array bound of a
// data member and as a value in member functions; arguments are constant expressions, and a
// constexpr variable counts as one ([temp.param], [temp.arg.nontype], [temp.class]).
// EXIT: 0
template<int N>
struct Arr {
  int data[N];
  int size() const { return N; }
  int sum() const {
    int s = 0;
    for (int i = 0; i < N; ++i) s += data[i];
    return s;
  }
  void fill(int v) {
    for (int i = 0; i < N; ++i) data[i] = v;
  }
};

constexpr int three = 3;

int main() {
  Arr<4> a{{1, 2, 3, 4}};
  if (a.size() != 4) return 1;
  if (a.sum() != 10) return 2;
  a.fill(5);
  if (a.sum() != 20) return 3;
  Arr<three> b{};
  if (b.size() != 3) return 4;
  if (b.sum() != 0) return 5;
  Arr<2 * three + 1> c{};
  c.fill(2);
  if (c.size() != 7) return 6;
  if (c.sum() != 14) return 7;
  Arr<1> d{{9}};
  if (d.sum() != 9) return 8;
  return 0;
}
