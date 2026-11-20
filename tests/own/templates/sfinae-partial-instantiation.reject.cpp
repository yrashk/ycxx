// #263: matching succeeds, then the selected partial specialization's member
// declaration fails during instantiation ([temp.deduct.general], [temp.inst]).
template<class T, class U> struct Pick {};
template<class T> struct Pick<T, typename T::result> {
  typename T::missing member; //! error
};

struct Valid { using result = int; };
Pick<Valid, int> value;
