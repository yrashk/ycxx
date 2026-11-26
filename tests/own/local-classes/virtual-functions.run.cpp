// Local classes (#117), spec-only: a local class can derive from a namespace-scope class and
// override its virtual functions; calls through a base reference dispatch to the local class
// ([class.virtual], [class.local], [class.derived]).
struct Shape {
  virtual int area() const = 0;
  virtual int sides() const { return 0; }
  virtual ~Shape() = default;
};

int measure(const Shape& s) { return s.area() * 10 + s.sides(); }

int main() {
  int destroyed = 0;
  {
    struct Square : Shape {
      int side;
      int* dtor_count;
      Square(int s, int* d) : side(s), dtor_count(d) {}
      int area() const override { return side * side; }
      int sides() const override { return 4; }
      ~Square() override { ++*dtor_count; }
    };
    struct Blob : Shape {
      int area() const override { return 1; }
    };
    Square sq(3, &destroyed);
    Blob b;
    if (measure(sq) != 94) return 1;
    if (measure(b) != 10) return 2;
    Shape* ps = &sq;
    if (ps->sides() != 4) return 3;
  }
  if (destroyed != 1) return 4;
  return 0;
}
