// Local classes (#117), spec-only: a local class in a member function of a class template uses the
// template parameter and calls a member of the enclosing class through a reference member from
// its destructor; two specializations give two local classes ([temp.inst], [class.local],
// [class.dtor]).
template <class T> struct Acc {
  T total{};
  int closed = 0;
  void close() { ++closed; }
  T add_scoped(T a, T b) {
    struct Scope {
      Acc& owner;
      T sum;
      ~Scope() {
        owner.total = owner.total + sum;
        owner.close();
      }
    };
    {
      Scope s{*this, a + b};
    }
    return total;
  }
};

int main() {
  Acc<int> ai;
  if (ai.add_scoped(2, 3) != 5) return 1;
  if (ai.add_scoped(10, 0) != 15) return 2;
  if (ai.closed != 2) return 3;
  Acc<double> ad;
  if (ad.add_scoped(0.25, 0.5) != 0.75) return 4;
  if (ad.closed != 1) return 5;
  return 0;
}
