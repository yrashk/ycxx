// ζ c2 (#88): the argument for a non-type template parameter must be a converted constant
// expression; a variable that is not constexpr or const-initialized is not one
// ([temp.arg.nontype], [expr.const]).
template<int N>
struct Arr {
  int data[N];
};

int main() {
  constexpr int k = 2;
  Arr<k> ok{};
  int n = 2;
  Arr<n> bad{}; //! error
  return ok.data[0];
}
