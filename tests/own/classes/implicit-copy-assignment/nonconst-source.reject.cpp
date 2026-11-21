#include "nonconst-member.hpp"

void assign_from_const(Whole& destination, const Whole& source) {
    destination = source;  //! error
}
