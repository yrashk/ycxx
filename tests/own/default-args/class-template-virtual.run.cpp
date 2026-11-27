// #356: a virtual call uses the default arguments of the declaration found through the static
// type of the object expression, while the overrider's body runs ([dcl.fct.default]: "a virtual
// function call uses the default arguments in the declaration of the virtual function
// determined by the static type"; [class.virtual]). Base and derived are class templates whose
// defaults depend on their parameters.
// EXIT: 0
template<class T>
struct Base {
  virtual int f(int x = sizeof(T)) { return 1000 + x; }
  virtual ~Base() = default;
};

template<class T, int N>
struct Derived : Base<T> {
  int f(int x = N) override { return 2000 + x; }
};

int main() {
  Derived<char, 40> d;
  Base<char>& br = d;
  Base<char>* bp = &d;
  if (br.f() != 2001) return 1;
  if (bp->f() != 2001) return 2;
  if (d.f() != 2040) return 3;
  if (br.f(5) != 2005) return 4;
  Base<int> b;
  if (b.f() != 1000 + static_cast<int>(sizeof(int))) return 5;
  Derived<int, 9> object;
  Derived<int, 9>* dp = &object;
  Base<int>* ip = dp;
  if (ip->f() != 2000 + static_cast<int>(sizeof(int)) || dp->f() != 2009) return 7;
  return 0;
}
