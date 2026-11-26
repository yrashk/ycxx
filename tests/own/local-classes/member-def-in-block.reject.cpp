// Local classes (#117), spec-only: a member function of a local class declared in the class cannot
// be defined later in the block; function definitions do not appear at block scope
// ([class.local], [dcl.fct.def.general]).
int main() {
  struct L {
    int f();
  };
  int L::f() { return 1; } //! error
  return 0;
}
