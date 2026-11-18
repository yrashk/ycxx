// δ s2 (#86): an out-of-class definition of a member function that the class does not declare,
// and a call of a member function that does not exist ([class.mfct], [expr.ref]).
struct Shape {
  int sides;
  int count() const;
};

int Shape::count() const { return sides; }
int Shape::perimeter() const { return 0; } //! error

int main() {
  Shape s = {3};
  return s.area(); //! error
}
