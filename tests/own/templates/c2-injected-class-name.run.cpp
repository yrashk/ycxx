// ζ c2 (#88): inside a class template its own name without arguments is the injected-class-name
// and means the current specialization: Box is Box<T> as a parameter type, a pointer type and a
// return type, and is interchangeable with Box<T> spelt out ([temp.local], [class.pre]).
// EXIT: 0
template<class T>
struct Box {
  T v;
  bool same(const Box& o) const { return v == o.v; }
  bool same_spelt(const Box<T>& o) const { return same(o); }
  Box* self() { return this; }
  Box<T>* self_spelt() { return self(); }
  Box& assign(const Box& o) { v = o.v; return *this; }
  void link(Box*& out) { out = this; }
};

int main() {
  Box<int> a{1};
  Box<int> b{1};
  Box<int> c{2};
  if (!a.same(b)) return 1;
  if (a.same(c)) return 2;
  if (!a.same_spelt(b)) return 3;
  if (a.self() != &a) return 4;
  if (a.self_spelt() != &a) return 5;
  if (a.assign(c).v != 2) return 6;
  if (&a.assign(b) != &a) return 7;
  Box<int>* p = nullptr;
  c.link(p);
  if (p != &c) return 8;
  Box<char> d{'d'};
  Box<char> e{'d'};
  if (!d.same(e)) return 9;
  return 0;
}
