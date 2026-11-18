// ζ c2 (#88): a template-id with more template arguments than the class template has parameters
// is ill-formed ([temp.names], [temp.arg.general]).
template<class T, int N>
struct Arr {
  T data[N];
};

Arr<int, 3> ok{};
Arr<int, 3, 4> bad{}; //! error
