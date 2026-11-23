// [class.member.lookup]: the distinct declarations have incomparable base subobjects.
constexpr int value = 5;
struct Left { static constexpr int value = 11; };
struct Right { static constexpr int value = 13; };
struct Derived : Left, Right {
  inline static int result = value;  //! error
};
