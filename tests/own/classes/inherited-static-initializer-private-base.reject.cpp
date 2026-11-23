// [class.access.base]: inaccessible inheritance does not resume lookup in the namespace.
constexpr int value = 5;
struct Base { static constexpr int value = 11; };
struct Middle : private Base {};
struct Derived : Middle {
  inline static int result = value;  //! error
};
