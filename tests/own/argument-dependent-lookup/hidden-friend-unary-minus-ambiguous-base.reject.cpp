struct A {};
struct Left : A {};
struct Right : A {};
struct Other {};
struct Value : Left, Right, Other {
  friend constexpr int operator-(A&) { return 1; }
  friend constexpr int operator-(Other&) { return 2; }
};
constexpr int use() { Value v; return -v; } //! error
