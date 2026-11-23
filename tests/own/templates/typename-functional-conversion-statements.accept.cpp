// #917 [expr.type.conv], [temp.res.general], [stmt.ambig].
struct Reading {
  int value;
  Reading() : value(7) {}
  Reading(int& total, int amount) : value(amount) { total += amount; }
  void mark(int& total) const { ++total; }
};

struct Source { using type = Reading; };

template<class Provider>
int measure(int& total, int amount) {
  typename Provider::type (baseline);
  typename Provider::type(total, amount);
  typename Provider::type{total, amount};
  typename Provider::type(total, amount).mark(total);
  return baseline.value;
}

int main() {
  int total = 1;
  if (measure<Source>(total, 3) != 7) return 1;
  return total != 11;
}
