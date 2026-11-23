// [class.member.lookup]: incompatible inherited types make lookup ambiguous.
struct Left { using T = char; };
struct Right { using T = short; };
struct Combined : Left, Right {
  using Published = T; //! error
};
