// ι (#300): a by-value parameter of class type with a default argument: each call that uses
// the default constructs a new object, and the default can call a constructor with its own
// arguments or name a global object ([dcl.fct.default]/1, /9, [expr.call]/5).
// The global's dynamic initializer itself uses a default argument ([basic.start.dynamic], #94).
// EXIT: 0
int constructed = 0;
int default_heights = 0;

int unit_height() { return ++default_heights; }

struct Rect {
  int w, h;
  Rect(int w, int h = unit_height()) : w(w), h(h) { ++constructed; }
  Rect(const Rect& o) : w(o.w), h(o.h) { ++constructed; }
  int area() const { return w * h; }
};

Rect unit(1);

int area(Rect r = Rect(2, 3)) { return r.area(); }
int scaled(int k, Rect r = unit) { return k * r.area(); }
int grow(Rect r = Rect(1, 1)) {
  r.w += 10;  // changes this call's parameter only
  return r.w;
}

int main() {
  constructed = 0;
  if (area() != 6) return 1;
  if (area() != 6) return 2;
  if (constructed != 2) return 3;   // one object per call (initialized directly from the prvalue)
  if (area(Rect(4, 5)) != 20) return 4;
  if (scaled(3) != 3) return 5;
  unit.w = 2;
  if (scaled(3) != 6) return 6;     // the global is copied at each call
  if (unit.w != 2) return 7;
  if (grow() != 11 || grow() != 11) return 8;
  if (default_heights != 1) return 9;
  return 0;
}
