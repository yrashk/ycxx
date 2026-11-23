// [class.member.lookup]: a type declaration contributes the type it designates.
// XFAIL: gcc GCC 16.2 diagnoses distinct inherited aliases for the same type as ambiguous.
struct Left { using Width = char; };
struct Right { typedef char Width; };
struct Config : Left, Right {
  static constexpr auto width = sizeof(Width);
};
static_assert(Config::width == sizeof(char));
