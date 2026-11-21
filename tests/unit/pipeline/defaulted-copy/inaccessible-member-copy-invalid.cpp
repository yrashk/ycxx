#include "inaccessible-member-copy.hpp"

void try_copy(const W& w) {
    W copy(w);  // expected-error: W's copy constructor is deleted because the member copy is private
}
