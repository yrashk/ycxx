// θ step 2 (#280), spec-only: a closure type without a lambda-capture has a defaulted default constructor
// and copy assignment; closure objects are copied with their captured state
// ([expr.prim.lambda.closure]).
int main() {
  auto plain = [](int x) { return x + 1; };
  decltype(plain) other;
  if (other(1) != 2) return 1;
  other = plain;
  int k = 4;
  auto cap = [k] { return k; };
  auto copy = cap;
  k = 40;
  if (copy() != 4) return 2;
  auto c = [n = 1]() mutable { return n++; };
  c();
  auto d = c;
  if (c() != 2 || d() != 2) return 3;
  return 0;
}
