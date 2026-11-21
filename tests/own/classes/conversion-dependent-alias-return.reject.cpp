// #339: the conversion-type-id supplies the conversion function's return
// type ([class.conv.fct]).
template<class T>
struct Invalid {
  using value_type = T;
  value_type operator value_type() const; //! error
};
