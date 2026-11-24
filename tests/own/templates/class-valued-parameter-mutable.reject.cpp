// #317, [temp.param]/11-12: a structural class has no mutable non-static data members.
struct Bad {
  int count;
  mutable bool enabled;
};

template<Bad V> struct Rejected {}; //! error
