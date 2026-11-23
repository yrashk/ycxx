// #157 [class.access.general], [expr.prim.lambda.capture], [class.dtor].
struct Reading {
  int value;

private:
  ~Reading() = default;

public:
  int read_copy(const Reading& source) const {
    auto saved = [source] { return source.value; };
    return saved();
  }
};
