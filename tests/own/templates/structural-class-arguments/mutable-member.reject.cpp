// #317: a mutable data member prevents a class from being structural ([temp.param]).
struct MutableKey {
  mutable int value;
};

template<MutableKey Value> //! error
struct Entry {};
