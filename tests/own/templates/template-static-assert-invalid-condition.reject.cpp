// #241: deferring a false assertion preserves the condition's validity rules.
int runtime_value;

template<class T> void nonconstant() {
  static_assert(runtime_value); //! error
}

template<class T> void undeclared() {
  static_assert(missing_name); //! error
}
