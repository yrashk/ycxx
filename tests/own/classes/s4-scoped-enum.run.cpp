// δ s4 (#86): scoped enumerations: `enum class` and `enum struct`, qualified enumerators,
// a fixed underlying type, comparison of enumerators with each other, scoped enumerators in
// different enumerations with the same name, as members and parameters, initialization from a
// value of the underlying type by list-initialization, `using enum`
// ([dcl.enum], [enum.udecl], [dcl.init.list]).
// EXIT: 0
enum class Dir : unsigned char { north, east, south, west };
enum struct Axis { horizontal, vertical };
enum class Size { small = 1, medium = 4, large = 16 };
enum class Unit { small, large };  // no clash with Size::small

Dir turn_right(Dir d) {
  if (d == Dir::north) return Dir::east;
  if (d == Dir::east) return Dir::south;
  if (d == Dir::south) return Dir::west;
  return Dir::north;
}

Axis axis_of(Dir d) {
  return d == Dir::north || d == Dir::south ? Axis::vertical : Axis::horizontal;
}

struct Robot {
  Dir facing;
  int x, y;
  void forward() {
    if (facing == Dir::north) ++y;
    else if (facing == Dir::south) --y;
    else if (facing == Dir::east) ++x;
    else --x;
  }
};

int main() {
  Robot r = {Dir::north, 0, 0};
  r.forward();
  r.facing = turn_right(r.facing);
  r.forward();
  r.forward();
  r.facing = turn_right(turn_right(r.facing));
  r.forward();
  if (r.x != 1 || r.y != 1) return 1;
  if (r.facing != Dir::west) return 2;
  if (axis_of(r.facing) != Axis::horizontal) return 3;
  if (!(Dir::north < Dir::west)) return 4;
  if (Size::medium <= Size::small) return 5;
  Dir d{2};  // list-initialization from the underlying type
  if (d != Dir::south) return 6;
  Size s = Size::large;
  if (s == Size::small) return 7;
  Unit u = Unit::large;
  if (u != Unit::large) return 8;
  using enum Axis;
  if (axis_of(Dir::east) != horizontal) return 9;
  return 0;
}
