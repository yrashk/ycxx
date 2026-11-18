// #133: `typedef class-specifier declarators;` with named and unnamed classes and enumerations;
// an unnamed one has the typedef-name for linkage purposes ([dcl.typedef]/9), and a variable
// declared after an enumeration's braces is a variable ([dcl.enum]).
// EXIT: 0
typedef struct Pt { int x, y; } Point, *PointP;
typedef struct { int a; char b; } Anon;
typedef enum { red, green = 5 } Color;
typedef enum Shape { round, square } ShapeT;
int area(PointP p) { return p->x * p->y; }
enum class K { p, q } kk;
enum L { m } ll;
enum F { F1, F2 } fv = F2;  // past the braces, F2 has type F ([dcl.enum]/5)
int main() {
  Point p{2, 3};
  Anon an{4, 'z'};
  Color c = green;
  ShapeT s = square;
  struct Pt q = p;
  return area(&q) + an.a + c + s - 16 + (kk == K::p ? 0 : 1) + ll + m + fv - 1;
}
