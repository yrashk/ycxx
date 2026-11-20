// The corresponding overload determines inherited linkage ([basic.scope.scope], #455).
namespace CFirst {
extern "C" int f(int);
long f(long x) { return x + 3; }
int f(int x) { return x + 2; }
}
namespace CxxFirst {
long g(long x) { return x + 4; }
extern "C" int g(int);
int g(int x) { return x + 5; }
}
namespace Unrelated {
int f(int x) { return x + 6; }
int g(int x) { return x + 7; }
}
int main() {
  return CFirst::f(40) == 42 && CFirst::f(40L) == 43 &&
         CxxFirst::g(40L) == 44 && CxxFirst::g(40) == 45 &&
         Unrelated::f(40) == 46 && Unrelated::g(40) == 47 ? 0 : 1;
}
