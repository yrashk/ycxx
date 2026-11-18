// ζ f1 (#88): a non-type template parameter used in no function parameter, with no default and
// no explicit argument, cannot be deduced ([temp.deduct.general], [temp.arg.explicit]).
template<int N>
int times(int x) { return x * N; }

int main() {
  return times(3); //! error
}
