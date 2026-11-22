// EXIT: 0
// [dcl.init.list], [dcl.init.aggr], [dcl.constexpr], [class.static.data]

struct Point {
  int x;
  int y;
};

class Holder {
public:
  static constexpr Point position{7, -11};
};

const Point* volatile escaped_position;

int main() {
  escaped_position = &Holder::position;
  const Point* position = escaped_position;
  if (position->x != 7)
    return 1;
  if (position->y != -11)
    return 2;
  return 0;
}
