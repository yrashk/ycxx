// ζ c2 (#88): specializations with different arguments are unrelated types: a Box<long>& does not
// bind to a Box<int>, even though int converts to long ([temp.type], [dcl.init.ref]).
template<class T>
struct Box {
  T v;
};

Box<int> i{1};
Box<int>& ok = i;
Box<long>& bad = i; //! error
