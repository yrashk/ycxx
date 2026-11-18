// δ s3 (#86): calling a deleted function, a deleted constructor and a deleted copy constructor
// ([dcl.fct.def.delete]).
int f(int) { return 0; }
int f(long) = delete;

struct Unique {
  int id;
  Unique(int i) : id(i) {}
  Unique(const Unique&) = delete;
  Unique(char) = delete;
};

int main() {
  int a = f(1);
  int b = f(1L); //! error
  Unique u(1);
  Unique v = u; //! error
  Unique w('c'); //! error
  return a + b + v.id + w.id;
}
