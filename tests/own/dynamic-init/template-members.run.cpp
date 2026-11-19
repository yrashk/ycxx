// ι (#94), from the review: members of a class template specialization that only a
// namespace-scope variable's initialization or destruction uses are instantiated ([temp.inst]/4).
// EXIT: 0
int destroyed = 0;
template <class T> struct D {
  T v;
  D() : v(7) {}
  ~D() { ++destroyed; }
  T get() const { return v; }
};
D<int> d;
int a = d.get();
int main() { return a == 7 && destroyed == 0 ? 0 : 1; }
