// ζ f1 (#88): const T&& is not a forwarding reference: from the lvalue i, T is deduced as int
// and the rvalue reference const int&& cannot bind to an lvalue ([temp.deduct.call],
// [dcl.init.ref]).
template<class T>
int take(const T&& x) { return x; }

int main() {
  int i = 1;
  return take(i); //! error
}
