// ζ c2 (#88): a class template whose access-specifiers come before a constructor, a destructor
// and member functions; instantiating it checks each member against the earlier ones, which
// the access-specifiers are among ([class.access.spec], [class.mem.general]/5, [temp.inst]).
// EXIT: 0
template<class T>
class Counter {
public:
  Counter(T start) : n(start) {}
  ~Counter() {}
private:
  T n;
public:
  T get() const { return n; }
protected:
  T twice() const { return n + n; }
public:
  T both() const { return get() + twice(); }
};

int main() {
  Counter<int> c(4);
  if (c.get() != 4) return 1;
  if (c.both() != 12) return 2;
  Counter<long> l(-1L);
  if (l.both() != -3L) return 3;
  return 0;
}
