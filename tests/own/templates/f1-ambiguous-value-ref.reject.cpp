// ζ f1 (#88): f(T) and f(const T&) are not ordered: partial ordering replaces a reference by the
// type it refers to and drops top-level cv-qualifiers, so each deduces from the other; with
// equally good conversions the call is ambiguous ([temp.deduct.partial], [temp.func.order],
// [over.match.best]).
template<class T>
int f(T) { return 1; }
template<class T>
int f(const T&) { return 2; }

int main() {
  int i = 0;
  return f(i); //! error
}
