// [class.access.general], [temp.arg.template]: a template argument still names the private member.
template <template <class> class Make> struct Apply {
  using Type = Make<int>;
};

class Base {
  template <class T> using Hidden = T*;
};

struct Derived : Base {
  using Type = Apply<Hidden>::Type;  //! error
};
