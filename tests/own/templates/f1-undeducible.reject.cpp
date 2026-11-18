// ζ f1 (#88): a type template parameter that appears only in the return type is not deduced
// from the call, has no default and is not given explicitly: deduction fails
// ([temp.deduct.general], [temp.deduct.call], [temp.arg.explicit]).
template<class R, class T>
R convert(T x) { return x; }

int main() {
  long l = convert(1); //! error
  return 0;
}
