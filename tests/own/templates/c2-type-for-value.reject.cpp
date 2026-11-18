// ζ c2 (#88): a type given as the argument for a non-type template parameter is ill-formed: the
// argument must be a constant expression of the parameter's type ([temp.arg.general],
// [temp.arg.nontype], [temp.names]).
template<int N>
struct Arr {
  int data[N];
};

Arr<4> ok{};
Arr<int> bad{}; //! error
