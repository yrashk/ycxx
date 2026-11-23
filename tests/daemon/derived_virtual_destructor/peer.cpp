#include "layout.hpp"

B* expected_base = nullptr;
extern const char sentinel[] = "base field";
unsigned base_calls = 0;
unsigned base_matches = 0;

B::~B() {
  ++base_calls;
  if (this == expected_base && name_ == sentinel && name_[0] == 'b')
    ++base_matches;
}

// The dynamic type is supplied by the other translation unit.
void delete_base(B* object) {
  delete object;
}
