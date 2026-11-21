#include "constructor-template-shapes.hpp"

int copy_lvalues(R& mutable_source, const R& const_source) {
    R first(mutable_source);
    R second(const_source);
    return first.value + second.value;
}
