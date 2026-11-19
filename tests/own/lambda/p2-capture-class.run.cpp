// θ step 2 (#280), spec-only: capturing an object of class type with trivial members, by copy (the
// closure gets its own copy) and by reference ([expr.prim.lambda.capture]).
struct Point { int x; int y; };

int main() {
  Point p{1, 2};
  auto byCopy = [p] { return p.x * 10 + p.y; };
  auto byRef = [&p] { return p.x * 10 + p.y; };
  p.x = 3;
  if (byCopy() != 12) return 1;
  if (byRef() != 32) return 2;
  auto move = [&p](int dx) { p.x += dx; };
  move(4);
  if (p.x != 7) return 3;
  auto local = [p]() mutable { p.y = 100; return p.y; };
  if (local() != 100 || p.y != 2) return 4;
  return 0;
}
