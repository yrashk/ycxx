// [class.access.general]
class Other { static constexpr int value = 8; };
struct Box {
  static constexpr int value = Other::value; //! error
};
