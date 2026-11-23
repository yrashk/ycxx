// [class.access.general]: lookup finds the private member before access is checked.
constexpr int value = 5;
class Base { static constexpr int value = 11; };
struct Derived : Base {
  inline static int result = value;  //! error
};
