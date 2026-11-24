// #1153 [class.virtual], [temp.inst]: a class template that declares virtual functions.
// EXIT: 0

template <class C>
class Buffer {
public:
  virtual ~Buffer() {}
  void set(C v) { value = v; }
  C get() { return fetch(); }
  int through_this() { return this->kind() * 10; }

protected:
  virtual C fetch() { return value; }
  virtual int kind() const { return int(sizeof(C)); }
  C value{};
};

struct Upper : Buffer<char> {
  char fetch() override { return char(value - 'a' + 'A'); }
  int kind() const override { return 6 + Buffer<char>::kind(); }
};

template <class T> struct B {
  virtual ~B() {}
  virtual T f() { return 1; }
};
struct D : B<int> {
  int f() override { return 2; }
};

template <class C> class sb {
public:
  virtual ~sb() {}

protected:
  virtual int f() { return 1; }
};

int main() {
  sb<char> s;
  D d;
  B<int>& b = d;
  if (b.f() != 2) return 1;
  B<long> plain;
  B<long>* p = &plain;
  if (p->f() != 1L) return 2;
  Buffer<int> i;
  i.set(40);
  if (i.get() != 40 || i.through_this() != 40) return 3;
  Upper u;
  u.set('x');
  Buffer<char>& r = u;
  if (r.get() != 'X' || r.through_this() != 70) return 4;
  return sizeof(B<char>) == sizeof(void*) && sizeof(sb<char>) == sizeof(void*) ? 0 : 5;
}
