// ζ c2 (#88): a template-id that leaves out an argument for a parameter without a default is
// ill-formed: Pair<> supplies nothing for T, which has no default, though U has one
// ([temp.names], [temp.arg.general], [temp.param]).
template<class T, class U = long>
struct Pair {
  T first;
  U second;
};

Pair<int> ok{1, 2L};
int count(Pair<>& p); //! error
