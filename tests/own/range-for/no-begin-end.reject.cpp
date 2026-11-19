// ι (#298): a range of class type with neither member begin/end nor non-member begin/end
// found by argument-dependent lookup, and a range of scalar type, are ill-formed
// ([stmt.ranged]/1.3: `begin(__range)` finds no function). A member begin without a member end
// is ill-formed too ([stmt.ranged]/1.2: the members are used if either is found).
struct Empty {
  int v[2];
};

struct OnlyBegin {
  int v[2];
  int* begin() { return v; }
};

int main() {
  Empty e{};
  int s = 0;
  for (int x : e) s += x; //! error
  int n = 3;
  for (int x : n) s += x; //! error
  OnlyBegin ob{};
  for (int x : ob) s += x; //! error
  return s;
}
