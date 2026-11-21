#include "constructor-template-shapes.hpp"

void require_copy(const RWithMoveAssignment& source) {
    RWithMoveAssignment copied(source);  //! error: genuine move assignment deletes implicit copy
}
