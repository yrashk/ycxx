// δ s3 (#86): no viable constructor: wrong arity, wrong type, and a missing default
// constructor once another constructor is declared ([over.match.ctor], [class.default.ctor]).
struct Point {
  int x, y;
  Point(int a, int b) : x(a), y(b) {}
};

int main() {
  Point a(1); //! error
  Point b(1, 2, 3); //! error
  Point c("one", 2); //! error
  Point d; //! error
  Point ok(1, 2);
  return ok.x;
}
