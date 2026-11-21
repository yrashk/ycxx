// δ s1 (#86): a member that does not exist, `.` on a pointer, `->` on a non-pointer
// ([expr.ref], [class.member.lookup]).
struct Point { int x; int y; };

int main() {
  Point p = {1, 2};
  Point* q = &p;
  int a = p.z; //! error
  int b = q.x; //! error
  int c = p->y; //! error
  bool d = p->y < 3; //! error
  q->w = 3; //! error
  return a + b + c + d;
}
