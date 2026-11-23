// [stmt.ambig], [expr.type.conv], [expr.sub]
int observed;
struct Result {
  int value;
  void mark() const { observed = value; }
};
struct Traits { using type = Result*; };
template<class T> void check(Result* seed) {
  typename T::type(seed)[1].mark();
}
int main() {
  Result values[] = {{4}, {5}};
  check<Traits>(values);
  return observed != 5;
}
