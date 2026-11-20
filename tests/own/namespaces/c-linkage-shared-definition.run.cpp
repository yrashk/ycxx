// C-linkage declarations in distinct namespaces name one function ([dcl.link], #446).
namespace A { extern "C" int same_function(int); }
namespace B { extern "C" int same_function(int); }
extern "C" int same_function(int x) { return x + 2; }
int main() {
  using Pointer = decltype(&A::same_function);
  Pointer volatile via_a = &A::same_function;
  Pointer volatile via_b = &B::same_function;
  Pointer volatile global = &::same_function;
  return A::same_function(40) == 42 && B::same_function(40) == 42 &&
         via_a == via_b && via_b == global ? 0 : 1;
}
