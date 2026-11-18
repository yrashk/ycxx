// δ s2 (#86): `this` outside a non-static member function, a non-static member function called
// without an object, a non-static data member used in a static member function
// ([expr.prim.this], [class.mfct.non.static], [class.static.mfct], [expr.prim.id.general]).
struct Widget {
  int size;
  int area() { return size * size; }
  static int make() {
    return size; //! error
  }
  static Widget* me() {
    return this; //! error
  }
};

int free_function() {
  return this->size; //! error
}

int main() {
  int a = Widget::area(); //! error
  return a;
}
