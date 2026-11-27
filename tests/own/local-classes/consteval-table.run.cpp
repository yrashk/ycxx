// #117: the shapes of libycxx's cerrno and ctype tables: a consteval function's unnamed local
// class array, and a local class with an array member returned through a deduced return type into
// a constant, using a block-scope alias ([class.local], [expr.const], [dcl.spec.auto]).
enum class code { a = 1, b = 2 };

consteval bool codes_match() {
  using enum code;
  const struct {
    code c;
    int v;
  } table[] = {{a, 1}, {b, 2}};
  for (const auto& t : table)
    if (static_cast<int>(t.c) != t.v) return false;
  return true;
}
static_assert(codes_match());

consteval auto make_masks() {
  using mask = unsigned char;
  struct masks {
    mask m[4];
  } t{};
  for (int i = 0; i < 4; ++i) t.m[i] = static_cast<mask>(i * 3);
  return t;
}
inline constexpr auto masks = make_masks();

int main() { return masks.m[3] == 9 ? 0 : 1; }
