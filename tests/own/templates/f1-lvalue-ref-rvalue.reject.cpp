// ζ f1 (#88): T& is not a forwarding reference: from the rvalue 5, T is deduced as int and the
// non-const lvalue reference int& cannot bind to it, so the specialization is not viable
// ([temp.deduct.call], [dcl.init.ref], [over.match.viable]).
template<class T>
void twice(T& r) { r = r * 2; }

int main() {
  twice(5); //! error
  return 0;
}
