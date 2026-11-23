// #157 [expr.static.cast], [expr.context], [conv.rval], [class.dtor].
struct Reading {
  int value;
private:
  ~Reading() = default;
};

void discard_copy(const Reading& source) {
  static_cast<void>(static_cast<Reading>(source)); //! error
}
