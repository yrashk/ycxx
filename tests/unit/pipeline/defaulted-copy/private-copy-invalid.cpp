#include "private-copy.hpp"

void copy_outside(const PrivateCopy& source) {
    PrivateCopy copy(source);  // expected-error: selected copy constructor is private
}
