// #285, spec-only. Array and class member bindings share the syntax. Incomplete or empty tuple_size leaves member binding ([dcl.struct.bind]).
struct Crate {
  int tag;
  unsigned bins[3];
  char mark;
};

struct Shelf {
  int level;
};

struct Parcel {
  int weight;
};

namespace std {
template<class T> struct tuple_size;
template<> struct tuple_size<Crate> {};
template<> struct tuple_size<Shelf>;
template<> struct tuple_size<Parcel> { static constexpr unsigned value = 1; };
}

int main() {
  unsigned values[3]{7, 11, 13};
  auto& [first, middle, last] = values;
  if (&first != &values[0] || &middle != &values[1] || &last != &values[2]) return 1;
  middle = 17;
  if (values[1] != 17) return 2;

  Crate crate{19, {23, 29, 31}, 'q'};
  auto& [tag, bins, mark] = crate;
  if (&tag != &crate.tag || &bins != &crate.bins || &mark != &crate.mark) return 3;
  if (tag != 19 || bins[0] != 23 || bins[1] != 29 || bins[2] != 31 || mark != 'q') return 4;
  bins[2] = 37;
  if (crate.bins[2] != 37) return 5;

  Shelf shelf{41};
  auto& [level] = shelf;
  if (&level != &shelf.level || level != 41) return 6;

  Parcel parcel{43};
  const auto& [weight] = parcel;
  if (&weight != &parcel.weight || weight != 43) return 7;
}
