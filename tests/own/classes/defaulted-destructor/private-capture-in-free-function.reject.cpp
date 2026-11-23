// #157 [class.access.general], [expr.prim.lambda.capture], [class.dtor].
struct Reading {
  int value;

private:
  ~Reading() = default;
};

int read_copy(const Reading& source) {
  auto saved = [source] { return source.value; }; //! error
  return saved();
}
