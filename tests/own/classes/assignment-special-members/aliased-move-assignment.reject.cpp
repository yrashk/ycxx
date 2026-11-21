#include "aliased-move-assignment.hpp"

void require_copy(const AliasedMove& source) {
    AliasedMove copied(source);  //! error: void-returning aliased cv move assignment deletes implicit copy
}
