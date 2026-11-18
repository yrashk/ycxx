// δ s4 (#86): an int does not convert implicitly to an enumeration, a scoped enumeration does not
// convert implicitly to an integer, two different enumerations do not mix, and an unqualified
// scoped enumerator is not found ([dcl.enum], [conv.prom], [conv.integral], [expr.eq]).
enum Color { red, green };
enum Shape { circle, square };
enum class Mode : int { off, on };

int as_int(int v) { return v; }

int main() {
  Color c = 1; //! error
  Color d = circle; //! error
  int i = Mode::on; //! error
  int j = as_int(Mode::off); //! error
  Mode m = off; //! error
  bool b = Mode::on == 1; //! error
  Color ok = red;
  int fine = ok;
  return i + j + fine + b;
}
