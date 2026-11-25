// #306 [class.nest]: a member class declaration does not make its polymorphic enclosing class
// incomplete; the member class is defined after the enclosing class, deriving from a polymorphic
// base (the shape of std::ios_base::failure).
struct Error {
  virtual ~Error();
  virtual int code() const { return 1; }
};
Error::~Error() {}

class B {
public:
  class failure;
  int get() const { return x; }
  virtual ~B();
  B() {}
  int x = 0;
};
B::~B() {}

class B::failure : public Error {
public:
  explicit failure(int c) : c_(c) {}
  ~failure() override;
  int code() const override { return c_; }
private:
  int c_;
};
B::failure::~failure() {}

int main() {
  B b;
  B::failure f(5);
  const Error& e = f;
  return b.get() + e.code() - 5;
}
