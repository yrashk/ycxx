// #356: a constructor of a class template whose parameters all have dependent defaults is a
// default constructor: it initializes plain objects, array elements, new-expressions and a
// member through another class's implicit default constructor. A copy constructor may have
// further defaulted parameters ([class.default.ctor], [class.copy.ctor], [dcl.fct.default],
// [dcl.init.general], [expr.new]).
// EXIT: 0
template<class T>
struct Count {
  int v, w;
  Count(int x = sizeof(T), int y = T(3)) : v(x), w(y) {}
  Count(const Count& o, int extra = T(100)) : v(o.v + extra), w(o.w) {}
};

struct Holder { Count<short> c; int k = 0; };

int main() {
  Count<int> a;
  if (a.v != static_cast<int>(sizeof(int)) || a.w != 3) return 1;
  Count<char> arr[3];
  for (auto& e : arr)
    if (e.v != 1 || e.w != 3) return 2;
  Count<long>* p = new Count<long>;
  int pv = p->v;
  delete p;
  if (pv != static_cast<int>(sizeof(long))) return 3;
  Holder h;
  if (h.c.v != static_cast<int>(sizeof(short)) || h.c.w != 3) return 5;
  Count<int> copy = a;
  if (copy.v != a.v + 100 || copy.w != 3) return 6;
  Count<int> copy2(a, 1);
  if (copy2.v != a.v + 1) return 7;
  Count<int> value{};
  if (value.w != 3) return 8;
  return 0;
}
