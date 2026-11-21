// #285, spec-only. A complete tuple_size class with a private value selects the tuple-like case ([dcl.struct.bind]).
struct Crate {
  int tag;
  unsigned bins[3];
  char mark;
};

namespace std {
template<class T> struct tuple_size;
template<> class tuple_size<Crate> {
  static constexpr unsigned value = 3;
};
}

void bind(Crate& crate) {
  auto& [tag, bins, mark] = crate; //! error
}
