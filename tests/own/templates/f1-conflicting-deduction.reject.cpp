// ζ f1 (#88): T deduced as int from one argument and as long from another: deduction fails and
// there is no viable function ([temp.deduct.call], [temp.deduct.type], [over.match.viable]).
template<class T>
T larger(T a, T b) {
  if (a < b) return b;
  return a;
}

int main() {
  return larger(1, 2L); //! error
}
