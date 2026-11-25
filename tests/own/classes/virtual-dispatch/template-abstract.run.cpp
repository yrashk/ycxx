// #1153 [class.abstract]: pure virtual members of class templates, and their final overriders in
// a template, an ordinary class and a partial or explicit specialization.
// EXIT: 0

template <class T> struct Shape {
  virtual ~Shape() = default;
  virtual T area() const = 0;
  T doubled() const { return area() + area(); }
};
static_assert(__is_abstract(Shape<int>) && __is_polymorphic(Shape<int>));

template <class T> struct Square : Shape<T> {
  Square(T s) : side(s) {}
  T area() const override final { return side * side; }
  T side;
};
static_assert(!__is_abstract(Square<int>) && __has_virtual_destructor(Square<int>));

template <class T> struct Defaulted : Shape<T> {
  ~Defaulted() override = default;
  T area() const override { return 5; }
};

struct Unit : Shape<long> {
  long area() const override { return 1; }
};

template <class T> struct Tag {
  virtual int tag() { return 0; }
};
template <class T> struct Tag<T*> : Tag<T> {
  virtual int depth() { return this->tag() + 100; }
  int tag() override { return Tag<T>::tag() + 1; }
};
template <> struct Tag<char> {
  virtual ~Tag() {}
  virtual int tag() { return 40; }
};

int main() {
  Square<int> s(3);
  Shape<int>& r = s;
  if (r.doubled() != 18) return 1;
  Defaulted<int> dd;
  Shape<int>& rd = dd;
  if (rd.doubled() != 10) return 5;
  Unit u;
  Shape<long>* p = &u;
  if (p->doubled() != 2) return 2;
  Tag<int**> t;
  Tag<int>& ti = t;
  if (ti.tag() != 2 || t.depth() != 102) return 3;
  Tag<char*> c;
  Tag<char>& tc = c;
  return tc.tag() == 41 ? 0 : 4;
}
