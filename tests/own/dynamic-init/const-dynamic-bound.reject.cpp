// ι (#94), spec-only: a `const int` with a dynamic initializer is not a constant expression, so it
// cannot be a namespace-scope array bound or a template argument, while a `const int` with a
// constant initializer can ([expr.const], [dcl.array], [temp.arg.nontype]).
int f() { return 3; }

const int n = f();
const int k = 3;

int fine[k];
int bad[n]; //! error

template<int N> struct Size {
  int data[N];
};

Size<k> ok;
Size<n> wrong; //! error

int main() { return fine[0] + ok.data[0]; }
