// ζ c2 (#88): when a specialization must be complete, its class is instantiated, and with it the
// declarations of all its members: the data member's type T::type is ill-formed for int, so
// defining a Holder<int> variable is an error at that member, while a pointer to Holder<int> was
// not ([temp.inst]/2, [temp.inst]/3, [temp.res], [temp.dep.type]).
template<class T>
struct Holder {
  typename T::type member; //! error
};

Holder<int>* fine = nullptr;
Holder<int> instantiated;
