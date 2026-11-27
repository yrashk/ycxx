// [over.match.funcs], [over.match.viable]
template <class T>
struct Wrap {
  T value{};
  T& get() & { return value; }
};

int use() { return Wrap<int>{}.get(); } //! error
