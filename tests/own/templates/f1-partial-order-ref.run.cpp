// ζ f1 (#88): r(T&) against r(const T&): for a non-const lvalue the binding to the less
// cv-qualified reference is the better conversion; for a const lvalue both bind the same way and
// partial ordering makes r(const T&) more specialized; an rvalue binds only to r(const T&)
// ([over.ics.rank], [temp.func.order], [temp.deduct.partial]).
// EXIT: 0
template<class T>
int r(T&) { return 1; }
template<class T>
int r(const T&) { return 2; }

struct S {
  int v;
};

int main() {
  int i = 0;
  const int ci = 0;
  S s{1};
  const S cs{2};
  if (r(i) != 1) return 1;
  if (r(ci) != 2) return 2;
  if (r(5) != 2) return 3;
  if (r(s) != 1) return 4;
  if (r(cs) != 2) return 5;
  if (r(i + 1) != 2) return 6;
  return 0;
}
