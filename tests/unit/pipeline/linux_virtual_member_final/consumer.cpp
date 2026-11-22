#include "root.hpp"

namespace final_probe {
int base_slot(const B* object, int value) {
    return object->score(value);
}

int derived_slot(const D& object, int value) {
    return object.score(value);
}

int qualified_base(const B& object, int value) {
    return object.B::score(value);
}

int other_slot(const B* object, int value) {
    return object->marker(value);
}
}
