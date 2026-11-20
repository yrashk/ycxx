// C++ namespace functions retain distinct identities ([basic.link], #446).
namespace A { int distinct(int x) { return x + 2; } }
namespace B { int distinct(int x) { return x + 3; } }
int main() {
  return A::distinct(40) == 42 && B::distinct(40) == 43 &&
         &A::distinct != &B::distinct ? 0 : 1;
}
