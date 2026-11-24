// [class.access.general], [class.access.base], [temp.arg.template]: derived classes can name
// protected aliases. The declaring class can name its private alias and pass it through a parameter.
template <template <class> class Make> struct Apply {
  using Type = Make<int>;
};

template <template <class> class Make> struct Forward {
  using Type = typename Apply<Make>::Type;
};

class Base {
  template <class T> using Hidden = T*;

protected:
  template <class T> using Pointer = T*;

public:
  using Direct = Hidden<int>;
  using Passed = Forward<Hidden>::Type;
};

struct Derived : Base {
  using Direct = Pointer<int>;
  using Passed = Apply<Pointer>::Type;
};

template <class Tag> struct Pattern : Base {
  using Direct = Pointer<int>;
  using Passed = Apply<Pointer>::Type;
};

template <class T> constexpr bool int_pointer = false;
template <> constexpr bool int_pointer<int*> = true;

static_assert(int_pointer<Base::Direct> && int_pointer<Base::Passed>);
static_assert(int_pointer<Derived::Direct> && int_pointer<Derived::Passed>);
static_assert(int_pointer<Pattern<int>::Direct> && int_pointer<Pattern<int>::Passed>);
