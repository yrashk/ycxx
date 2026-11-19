// ι (#313): an explicit type conversion to a pointer or reference to function is the target of
// an overload set: `static_cast`, a C-style cast and a functional cast through an alias
// ([over.over], [expr.static.cast], [expr.cast], [expr.type.conv]).
// EXIT: 0
int f(int) { return 1; }
int f(long) { return 2; }
int f(short) { return 3; }

using fl = int (*)(long);
using rs = int (&)(short);

int main() {
  if (static_cast<int (*)(long)>(f)(0) != 2) return 1;
  if (static_cast<int (*)(int)>(&f)(0) != 1) return 2;
  if (((int (*)(short))f)(0) != 3) return 3;
  if (((int (*)(int))&f)(0) != 1) return 4;
  if (fl(f)(0) != 2) return 5;
  if (static_cast<rs>(f)(0) != 3) return 6;
  if (static_cast<int (&)(int)>(f)(0) != 1) return 7;
  // The cast picks a different member than an ordinary call with the same argument would.
  if (static_cast<int (*)(long)>(f)(1) == f(1)) return 8;
  return 0;
}
