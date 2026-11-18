// δ s1 (#86): more initializers than members or elements ([dcl.init.aggr]).
struct Pair { int a; int b; };
struct Outer { Pair p; int n; };

int main() {
  Pair x = {1, 2, 3}; //! error
  int arr[2] = {1, 2, 3}; //! error
  Outer o = {{1, 2, 3}, 4}; //! error
  return x.a + arr[0] + o.n;
}
