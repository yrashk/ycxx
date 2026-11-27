// Local classes (#117), spec-only: a local class's member functions must be defined in the class;
// at namespace scope the local class name is not visible, so `L::f` cannot name the member
// ([class.local], [basic.scope.block]).
int use() {
  struct L {
    int f();
  };
  return 0;
}

int L::f() { return 1; } //! error

int main() { return use(); }
