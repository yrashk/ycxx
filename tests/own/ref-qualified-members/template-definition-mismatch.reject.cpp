// [dcl.meaning.general], [temp.mem.func]
template <class T>
struct Wrap {
  int select() &;
};

template <class T>
int Wrap<T>::select() && { return 1; } //! error
