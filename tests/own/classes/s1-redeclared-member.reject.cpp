// δ s1 (#86): a data member declared twice, and a class defined twice
// ([class.mem.general], [basic.def.odr]).
struct Point {
  int x;
  int y;
  long x; //! error
};

struct Twice { int a; };
struct Twice { int a; }; //! error

int main() { return 0; }
