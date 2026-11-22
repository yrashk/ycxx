namespace rvalue_ref {
struct A {};
struct Left : A {};
struct Right : A {};
struct Other {};
struct Value : Left, Right, Other {
  friend constexpr int operator-(A&&) { return 1; }
  friend constexpr long operator-(Other&&) { return 2; }
};
using Result = decltype(-Value{}); //! error
}

namespace const_ref {
struct A {};
struct Left : A {};
struct Right : A {};
struct Other {};
struct Value : Left, Right, Other {
  friend constexpr int operator-(const A&) { return 1; }
  friend constexpr long operator-(const Other&) { return 2; }
};
using Result = decltype(-Value{}); //! error
}
