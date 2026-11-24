// [class.access.general], [temp.alias]: naming an inherited private alias template is ill-formed.
class Base {
  template <class T> using Hidden = T*;
};

struct Derived : Base {
  using Type = Hidden<int>;  //! error
};
