// θ step 2 (#280), spec-only: capturing an object of class type by copy copy-initializes the closure's
// member with the copy constructor, copying a closure copies it again, and destroying the closure object
// destroys the member; capture by reference copies nothing ([expr.prim.lambda.capture],
// [expr.prim.lambda.closure], [class.copy.ctor], [class.dtor]).
int copies, dtors;

struct T {
  int v;
  T(int x) : v(x) {}
  T(const T& o) : v(o.v) { ++copies; }
  ~T() { ++dtors; }
};

int main() {
  {
    T t(5);
    {
      auto f = [t] { return t.v; };
      if (copies != 1) return 1;
      if (f() != 5) return 2;
      if (copies != 1) return 3;
      {
        auto g = f;
        if (copies != 2) return 4;
        if (g() != 5) return 5;
      }
      if (dtors != 1) return 6;
    }
    if (dtors != 2) return 7;
    auto r = [&t] { return t.v; };
    if (r() != 5 || copies != 2) return 8;
    auto i = [u = t] { return u.v + 1; };
    if (copies != 3 || i() != 6) return 9;
  }
  if (dtors != 4) return 10;
  return 0;
}
