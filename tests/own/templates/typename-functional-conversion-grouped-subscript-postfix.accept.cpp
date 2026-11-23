// [stmt.ambig], [expr.type.conv], [expr.sub]
int observed;
struct Result {
  int value;
  Result(int seed) : value(seed) {}
  Result& operator[](int amount) { value += amount; return *this; }
  void mark() const { observed = value; }
};
struct Traits { using type = Result; };
template<class T> void check(int seed) {
  typename T::type(seed)[1].mark();
}
int main() {
  check<Traits>(4);
  return observed != 5;
}
