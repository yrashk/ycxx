// Local classes (#117), spec-only: a class nested in a local class is a local class; it may be
// declared in the local class and defined in the nearest enclosing block scope
// ([class.local], [class.nest]).
int main() {
  struct Outer {
    struct In { int a; };
    struct Later;
    Later* p;
    In in;
  };
  struct Outer::Later { int b; };
  Outer::Later l{5};
  Outer o{&l, {6}};
  if (o.p->b + o.in.a != 11) return 1;
  return 0;
}
