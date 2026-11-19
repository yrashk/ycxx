// ι (#291), from the review: a static local of a class whose default member initializer is not a
// constant is initialized on the first pass ([stmt.dcl]/3, [class.base.init]).
// XFAIL: ycxx default member initializers that are not constants (#118)
// EXIT: 0
int g() { return 3; }
struct D { int y = g(); };
int f() { static D d; return d.y; }
int main() { return f() == 3 ? 0 : 1; }
