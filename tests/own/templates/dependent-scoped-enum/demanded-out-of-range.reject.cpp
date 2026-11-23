// [temp.inst], [dcl.enum]
template<int Value> struct Register {
  enum class Reading : unsigned char { sample = Value }; //! error
};

void use_reading() {
  (void)Register<-1>::Reading::sample;
}
