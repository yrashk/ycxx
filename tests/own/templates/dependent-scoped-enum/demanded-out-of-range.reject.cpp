// [temp.inst] instantiates the whole enum definition when an enumerator is used.
template<int Value> struct Register {
  enum class Reading : unsigned char { first = 0, second = Value }; //! error
};

void use_reading() {
  (void)Register<-1>::Reading::first;
}
