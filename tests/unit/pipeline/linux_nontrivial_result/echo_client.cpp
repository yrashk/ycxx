#include "result.hpp"

ResultHandle witness_result(bool* echoed, ResultObservation* observation, int value);

int main() {
    ResultObservation observation = {};
    bool echoed = false;
    {
        ResultHandle handle = witness_result(&echoed, &observation, 219);
        if (!echoed) {
            return 1;
        }
        if (observation.constructions != 1 || observation.destructions != 0 ||
            observation.constructed_at != &handle || handle.value != 219) {
            return 2;
        }
    }
    if (observation.destructions != 1 || observation.released_value != 219 ||
        !observation.released_at_construction_address) {
        return 3;
    }
    return 0;
}
