// [over.match.funcs], [over.match.viable]
template <class T>
struct Wrap {
  T value{};
  T take() && { return value; }
};

int use(Wrap<int>& wrap) { return wrap.take(); } //! error
