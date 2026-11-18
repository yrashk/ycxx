// ζ c2 (#88): a value given as the argument for a type template parameter is ill-formed: the
// argument must be a type-id ([temp.arg.general], [temp.arg.type], [temp.names]).
template<class T>
struct Box {
  T v;
};

Box<int> ok{0};
Box<3> bad{}; //! error
