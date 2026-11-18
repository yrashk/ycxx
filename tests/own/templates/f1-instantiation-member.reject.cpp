// ζ f1 (#88): a template body that is fine for a class argument is ill-formed when instantiated
// with int: member access on a non-class type, diagnosed inside the template body
// ([temp.inst], [temp.res], [expr.ref]).
struct Point {
  int x;
};

template<class T>
int get_x(const T& t) {
  return t.x; //! error
}

int main() {
  Point p{1};
  return get_x(p) + get_x(2);
}
