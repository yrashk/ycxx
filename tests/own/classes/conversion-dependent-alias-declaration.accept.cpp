// #339: a dependent conversion function can be declared without a body,
// and its declaration can precede another member declaration ([class.conv.fct]).
template<class T>
struct Converter {
  using value_type = T;
  constexpr operator value_type() const noexcept;
  value_type get() const;
};
