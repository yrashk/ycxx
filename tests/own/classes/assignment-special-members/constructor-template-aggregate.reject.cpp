#include "constructor-template-shapes.hpp"

void try_aggregate() {
    R object{7};  //! error: the constructor template prevents aggregate initialization
}
