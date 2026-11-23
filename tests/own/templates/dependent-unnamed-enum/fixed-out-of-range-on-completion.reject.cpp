// [temp.inst] instantiates the unscoped enum definition when completing the class.
template<int Value> struct Slot {
  enum : unsigned char { first = 0, invalid = Value }; //! error
};

static_assert(sizeof(Slot<-1>) > 0);
