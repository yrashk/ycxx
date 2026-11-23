// [stmt.ambig], [dcl.init]
namespace N { using T = int; }
int main() {
  typename N::T (seed)(13);
  return seed != 13;
}
