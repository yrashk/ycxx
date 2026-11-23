// #917 [expr.type.conv], [temp.res.general], [stmt.ambig].
struct Reading {
  int value;
  Reading() : value(4) {}
  explicit Reading(int tens, int units) : value(tens * 10 + units) {}
};

struct Source { using type = Reading; };

template<class Provider>
int difference(int tens, int units) {
  typename Provider::type (baseline);
  return typename Provider::type(tens, units).value - baseline.value;
}

int main() { return difference<Source>(3, 7) != 33; }
