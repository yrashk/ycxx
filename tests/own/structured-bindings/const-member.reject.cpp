// #285, spec-only. An ordinary member bound through a const object cannot be assigned ([dcl.struct.bind]).
struct Dial {
  int setting;
};

void adjust(const Dial& dial) {
  auto& [setting] = dial;
  setting = 53; //! error
}
