#include "base.hpp"

int make_and_dispatch(int value) {
    D object;
    object.field = value;
    return dispatch(object);
}
