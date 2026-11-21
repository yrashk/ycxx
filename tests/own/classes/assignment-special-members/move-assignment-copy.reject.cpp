#include "move-assignment.hpp"

void copy_const(const MoveAssigned& source) {
    MoveAssigned copied(source);  //! error: declared move assignment deletes implicit copy construction
}

void copy_mutable(MoveAssigned& source) {
    MoveAssigned copied(source);  //! error: mutable lvalue still requires the deleted copy constructor
}
