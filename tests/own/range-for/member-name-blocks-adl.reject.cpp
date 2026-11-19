// ι (#298): finding both member names selects member calls even if one name is a type alias or
// enumerator. Member lookup accounts for inherited names and hiding
// ([stmt.ranged]/1.3.2, [class.member.lookup]).
namespace items {
struct Range {
  using begin = int;
  int* end();
};

struct Derived : Range {};

struct EnumRange {
  enum Marker { begin = 1 };
  int* end();
};

struct CallableBase {
  int* begin();
  int* end();
};

struct HidesBegin : CallableBase {
  using begin = int;
};

int* begin(Range& r);
int* end(Range& r);
int* begin(Derived& r);
int* end(Derived& r);
int* begin(EnumRange& r);
int* end(EnumRange& r);
int* begin(HidesBegin& r);
int* end(HidesBegin& r);
}

using items::begin;
using items::end;

void direct(items::Range& r) {
  for (int x : r) {} //! error
}

void inherited(items::Derived& r) {
  for (int x : r) {} //! error
}

void enumerator(items::EnumRange& r) {
  for (int x : r) {} //! error
}

void hides_begin(items::HidesBegin& r) {
  for (int x : r) {} //! error
}
