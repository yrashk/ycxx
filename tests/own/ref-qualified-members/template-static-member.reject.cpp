// [dcl.fct]/6
template <class T>
struct Wrap {
  static int select() &&; //! error
};
