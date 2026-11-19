// θ step 3 (#281), spec-only: the function call operator template of a template lambda can be named
// with explicit template arguments, including a non-type template parameter that no function
// parameter deduces ([expr.prim.lambda.closure], [temp.arg.explicit], [temp.names]).
// XFAIL: ycxx explicit template arguments for a member function template, #347
int main() {
  auto get = []<int N>() { return N; };
  if (get.template operator()<3>() != 3) return 1;
  if (get.operator()<-7>() != -7) return 2;
  auto conv = []<class T>(long v) { return static_cast<T>(v) + 0; };
  if (conv.template operator()<char>(321L) != static_cast<char>(321L) + 0) return 3;
  if (conv.template operator()<int>(321L) != 321) return 4;
  auto partly = []<class T, class U>(U u) { return static_cast<T>(u); };
  if (partly.template operator()<long>(5) != 5L) return 5;
  if (sizeof(partly.template operator()<char>(5)) != 1) return 6;
  return 0;
}
