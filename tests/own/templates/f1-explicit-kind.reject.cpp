// ζ f1 (#88): an explicit template argument must match the kind of its parameter: a type given
// for a non-type parameter leaves no viable specialization ([temp.arg.general],
// [temp.arg.explicit], [temp.deduct.general]).
template<int N>
int times(int x) { return x * N; }

int main() {
  return times<int>(3); //! error
}
