// [class.member.lookup]: different inherited types give an invalid declaration set.
struct Left { using Width = char; };
struct Right { using Width = int; };
struct Config : Left, Right {
  static constexpr auto width = sizeof(Width);  //! error
};
